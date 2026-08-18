/*
 * ml-aed: userspace auto-exposure for the air-unit camera. AE only; AWB is gated off by the
 * shipped tuning and the fixed-focus module leaves af_stats disabled.
 *
 * The kernel owns the interrupt path, the statistics ping-pong and the derived register stages.
 * This daemon reads statistics and writes the operating point back through module parameters.
 * The decision law is ml-aed-core.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ml-aed-core.h"

/* Default for a hand-run; the per-board init script passes --tuning. */
#define ML_AED_TUNING_FALLBACK "/lib/firmware/artosyn/nt99235-tuning-preview-fpv.bin"

#define STATS_RAW_PATH  "/sys/kernel/debug/ar-isp/stats_raw"
#define LADDER_ARM_PATH "/sys/kernel/debug/ar-isp/ladders"
#define SENSOR_EXPOSURE "/sys/module/nt99235/parameters/exposure"
#define SENSOR_GAIN     "/sys/module/nt99235/parameters/gain"
#define ISP_RNR_GAIN    "/sys/module/ar_isp/parameters/rnr_gain"
#define ISP_LNR_GAIN    "/sys/module/ar_isp/parameters/lnr_gain"
#define ISP_DE3D_GAIN   "/sys/module/ar_isp/parameters/de3d_gain"
#define ISP_CFA_GAIN    "/sys/module/ar_isp/parameters/cfa_gain"
#define ISP_CNF_GAIN    "/sys/module/ar_isp/parameters/cnf_gain"
#define ISP_TONE_SCALAR "/sys/module/ar_isp/parameters/tone_scalar"
#define TONE_ARM_PATH   "/sys/kernel/debug/ar-isp/tone"

/*
 * cm and cm2 are absent on purpose. They key on the trigger scalar, axis
 * 0..550, not the 1..2048 gain the five ladders above take; writing the gain
 * into them selects row 0 where the vendor sat on row 1. Both stay at the
 * driver default, which pins the vendor's traced operating point.
 */

/*
 * Poll interval while the driver reports no statistics, and how many of those
 * polls separate the "still waiting" lines. Frames land at 60/s, so 100 ms
 * picks the first flip up promptly without spinning; 300 polls is one line per
 * 30 s, which keeps a camera that never streams visible in the log without
 * filling the 8 MiB /var/log tmpfs.
 */
#define STATS_POLL_US       100000
#define STATS_POLL_REPORT   300

struct ae_opts {
    int start_index;
    int max_step;               /* 0 = no clamp (vendor behaviour) */
    int floor_index;
    int ceil_index;
    int dry_run;
    int decisions;              /* stop after N decisions, 0 = run forever */
    int no_ladders;
    int tone;                   /* drive gamma and DRC from the trigger scalar */
    int verbose;
};

static int write_int(const char *path, int value)
{
    char buf[16];
    int fd, len, ret = 0;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -errno;
    }

    len = snprintf(buf, sizeof(buf), "%d\n", value);
    if (write(fd, buf, len) != len) {
        ret = -errno;
    }

    close(fd);

    return ret;
}

/*
 * Apply one table entry: sensor exposure and gain (each write commits inside
 * the sensor's group hold), the ladder abscissas in Q8, then the ladder-only
 * re-apply. The sensor driver clamps exposure at vts - 2 = 1123, two lines
 * under the table's 1125 ceiling; accepted as a 0.2% parity nit.
 */
static int ae_actuate(const struct ae_tuning *tune, const struct ae_opts *opts,
              int exp_index)
{
    const struct mlaed_exp_entry *e = &tune->table[exp_index];
    unsigned int code = ae_sensor_gain_code(e->gain_q8);
    int ret;

    if (opts->dry_run) {
        return 0;
    }

    ret = write_int(SENSOR_EXPOSURE, (int)e->line_count);
    if (ret) {
        return ret;
    }

    ret = write_int(SENSOR_GAIN, (int)code);
    if (ret) {
        return ret;
    }

    if (opts->no_ladders) {
        return 0;
    }

    ret = write_int(ISP_RNR_GAIN, (int)e->gain_q8);
    if (!ret) {
        ret = write_int(ISP_LNR_GAIN, (int)e->gain_q8);
    }

    if (!ret) {
        ret = write_int(ISP_DE3D_GAIN, (int)e->gain_q8);
    }

    if (!ret) {
        ret = write_int(ISP_CFA_GAIN, (int)e->gain_q8);
    }

    if (!ret) {
        ret = write_int(ISP_CNF_GAIN, (int)e->gain_q8);
    }

    if (!ret) {
        ret = write_int(LADDER_ARM_PATH, 1);
    }

    return ret;
}

/*
 * The trigger scalar, written every decision rather than every step.
 *
 * It moves with the luma error, not only with the index, so a pinned index does
 * not mean a pinned scalar: with the lens covered the index sits at its ceiling
 * while the scalar keeps climbing, and that saturated region is where it stops
 * tracking the index at all. The driver rebuilds the tone pages only when the
 * scalar crosses a band edge, so writing it every decision costs two writes and
 * not a rebuild.
 */
static int ae_actuate_tone(const struct ae_tuning *tune,
               const struct ae_opts *opts, int exp_index, float luma)
{
    static int last = -1;
    int q8;
    int ret;

    if (opts->dry_run || !opts->tone) {
        return 0;
    }

    q8 = ae_tone_scalar_q8(tune, exp_index, luma);

    /*
     * Only on a change. The scalar is truncated to whole counts, so a settled
     * scene repeats the same value and there is nothing for the driver to
     * reselect; writing it anyway would re-enter the page rebuild every frame
     * for no result. The driver skips an unchanged selection too, but the
     * cheaper place to stop is here, before the two syscalls.
     */
    if (q8 == last) {
        return 0;
    }

    ret = write_int(ISP_TONE_SCALAR, q8);
    if (ret) {
        return ret;
    }

    ret = write_int(TONE_ARM_PATH, 1);
    if (ret) {
        return ret;
    }

    last = q8;

    return 0;
}

/*
 * One coherent stats_raw snapshot. The driver returns EAGAIN before the first
 * flip and the two sequence words differ if a flip landed mid-copy; both
 * retry. Returns the flip sequence, or negative errno.
 */
static int read_stats(uint8_t *buf)
{
    int tries;

    for (tries = 0; tries < 5; tries++) {
        int fd = open(STATS_RAW_PATH, O_RDONLY);
        ssize_t got = 0, n;

        if (fd < 0) {
            return -errno;
        }

        while (got < STATS_RAW_SIZE) {
            n = read(fd, buf + got, STATS_RAW_SIZE - got);
            if (n <= 0) {
                break;
            }

            got += n;
        }

        close(fd);
        if (got == STATS_RAW_SIZE &&
            mlaed_get_le32(buf) == mlaed_get_le32(buf + STATS_RAW_SIZE - 4)) {
            return (int)mlaed_get_le32(buf);
        }

        usleep(2000);
    }

    return -EAGAIN;
}

static int run_loop(const struct ae_tuning *tune, const struct ae_opts *opts)
{
    struct ae_state st = {
        .exp_index = opts->start_index,
    };
    uint8_t *buf = malloc(STATS_RAW_SIZE);
    uint32_t last_seq = 0;
    unsigned int waited = 0;
    int have_seq = 0, decided = 0, ret;

    if (!buf) {
        return 1;
    }

    /*
     * Actuate before the first decision, so the hardware and this loop agree
     * on the operating point. Without it a restart inherits what the previous
     * run left on the sensor and reports an index that is not in effect:
     * measured live, a second run starting at 317 against a sensor still at
     * 326. Reading the state back instead cannot work, because the gain code
     * is quantised and many indices share one code.
     */
    ret = ae_actuate(tune, opts, st.exp_index);
    if (ret) {
        fprintf(stderr, "ml-aed: initial actuate: %s\n", strerror(-ret));
        free(buf);

        return 1;
    }

    while (!opts->decisions || decided < opts->decisions) {
        int seq = read_stats(buf);
        float luma;
        int step, prev;

        /*
         * EAGAIN is the driver reporting no completed frame to hand over, not
         * a failure: it holds before the first flip after stream-on, and again
         * after any reconfigure, which republishes the statistics buffers and
         * clears the valid flag. Waiting is the only correct response. Exiting
         * leaves the camera at a fixed exposure until the next boot with the
         * link, the encoder and every counter healthy, so nothing else reports
         * the loss.
         */
        if (seq == -EAGAIN) {
            if (!(waited % STATS_POLL_REPORT)) {
                fprintf(stderr, "ml-aed: waiting for statistics%s\n",
                        waited ? " (still)" : "");
            }

            waited++;
            usleep(STATS_POLL_US);
            continue;
        }

        if (seq < 0) {
            fprintf(stderr, "ml-aed: stats_raw: %s\n", strerror(-seq));
            free(buf);

            return 1;
        }

        if (waited) {
            fprintf(stderr, "ml-aed: statistics live after %u ms\n",
                    waited * (STATS_POLL_US / 1000));
            waited = 0;
        }

        if (have_seq && (uint32_t)seq == last_seq) {
            usleep(2000);
            continue;
        }

        last_seq = (uint32_t)seq;
        have_seq = 1;

        luma = ae_current_luma(ae_metered_luma(buf + 4));
        prev = st.exp_index;
        ae_decide(tune, &st, luma);
        if (opts->max_step && abs(st.exp_index - prev) > opts->max_step) {
            st.exp_index = st.exp_index > prev ?
                prev + opts->max_step : prev - opts->max_step;
        }

        if (st.exp_index < opts->floor_index) {
            st.exp_index = opts->floor_index;
        }

        if (opts->ceil_index && st.exp_index > opts->ceil_index) {
            st.exp_index = opts->ceil_index;
        }

        step = st.exp_index - prev;
        decided++;

        if (step || opts->verbose) {
            printf("seq %u luma %.3f target %d index %d step %d settle %u%s\n",
                   (unsigned int)seq, (double)luma,
                   ae_luma_target(tune, st.exp_index), st.exp_index,
                   step, st.settle_counter,
                   opts->dry_run ? " (dry)" : "");
            fflush(stdout);
        }

        if (opts->tone) {
            int ret = ae_actuate_tone(tune, opts, st.exp_index, luma);

            if (ret) {
                fprintf(stderr, "ml-aed: actuate tone: %s\n", strerror(-ret));
                free(buf);

                return 1;
            }
        }

        if (step) {
            int ret = ae_actuate(tune, opts, st.exp_index);

            if (ret) {
                fprintf(stderr, "ml-aed: actuate: %s\n", strerror(-ret));
                free(buf);

                return 1;
            }
        }
    }

    free(buf);

    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: ml-aed [options]\n"
        "  --start-index N   initial exp_index (default 317, the boot recipe)\n"
        "  --max-step N      clamp a decision to N indices (low authority)\n"
        "  --floor N         never go below index N\n"
        "  --ceil N          never go above index N\n"
        "  --decisions N     stop after N decisions\n"
        "  --dry-run         decide and log, never write\n"
        "  --no-ladders      actuate the sensor only\n"
        "  --tone            also drive gamma, DRC, cm and cm2 from the trigger scalar\n"
        "                    (off by default: its producer is unproven)\n"
        "  --tuning PATH     sensor tuning blob (default: the board's firmware path)\n"
        "  --verbose         log settled decisions too\n");
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "start-index", required_argument, NULL, 's' },
        { "max-step", required_argument, NULL, 'm' },
        { "floor", required_argument, NULL, 'f' },
        { "ceil", required_argument, NULL, 'c' },
        { "decisions", required_argument, NULL, 'n' },
        { "dry-run", no_argument, NULL, 'd' },
        { "no-ladders", no_argument, NULL, 'L' },
        { "tone", no_argument, NULL, 'T' },
        { "tuning", required_argument, NULL, 'T' + 128 },
        { "verbose", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };
    struct ae_opts opts = {
        .start_index = 317,
        .floor_index = 1,
    };
    const char *tuning_path = ML_AED_TUNING_FALLBACK;
    struct ae_tuning tune;
    int c, ret;

    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 's': {
            opts.start_index = atoi(optarg);
        } break;

        case 'm': {
            opts.max_step = atoi(optarg);
        } break;

        case 'f': {
            opts.floor_index = atoi(optarg);
        } break;

        case 'c': {
            opts.ceil_index = atoi(optarg);
        } break;

        case 'n': {
            opts.decisions = atoi(optarg);
        } break;

        case 'd': {
            opts.dry_run = 1;
        } break;

        case 'L': {
            opts.no_ladders = 1;
        } break;

        case 'T': {
            opts.tone = 1;
        } break;

        case 'T' + 128: {
            tuning_path = optarg;
        } break;

        case 'v': {
            opts.verbose = 1;
        } break;

        default: {
            usage();

            return 1;
        } break;
        }
    }

    /*
     * The blob is the source for every AE constant, so it is loaded before anything is validated
     * against it. There is no fallback: rootfs/build.sh refuses to build a rootfs without the
     * file, and compiled-in values would be exactly the stale configuration this removes.
     */
    ret = ae_tuning_load(&tune, tuning_path);
    if (ret) {
        fprintf(stderr, "ml-aed: %s: %s\n", tuning_path,
            ret == -EINVAL ? "not the size a tuning blob must be" : strerror(-ret));

        return 1;
    }

    if (opts.start_index < tune.index_min || opts.start_index > tune.index_max) {
        fprintf(stderr, "ml-aed: --start-index must be %d..%d\n",
            tune.index_min, tune.index_max);
        ae_tuning_free(&tune);

        return 1;
    }

    ret = run_loop(&tune, &opts);
    ae_tuning_free(&tune);

    return ret;
}
