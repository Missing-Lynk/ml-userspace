#include "ml-pipeline.h"

/*
 * Display phase pacing (ML_PACE=1): pins the submit-to-latch wait low by trimming the
 * panel's pixel clock, the open equivalent of the vendor lowdelay loop (libmpp
 * display_fre_adjust_proc).
 *
 * The VO's refresh is free-running against the RF source. With the rates matched the
 * phase between "frame ready" and the next vsync latch freezes wherever it happened to
 * land (0..16 ms of dead wait, drifting over minutes); mismatched, the phase sweeps and
 * every beat period the display repeats a frame. Both defects fall to one controller: a
 * servo on the measured wait. Wait above target -> raise pclk a fraction so the latch
 * edges walk earlier; below -> lower it. At equilibrium the wait sits at the target and
 * the panel rate has converged onto the source rate, with no explicit source-rate
 * estimate.
 *
 * The controller is PI. The boot pixel clock sits ~3 % above the source-matched rate
 * (62.08 Hz panel vs ~60.3 Hz source), far more than a proportional term bounded by
 * sane phase errors can command, so an integral term walks the servo's base rate onto
 * the matched rate (anti-windup: no integration while railed) while the proportional
 * term steers phase. Full-scale phase error (16 ms) commands ~0.16 % of rate
 * proportionally; the base integrates at 1/8 of that per second. Commands and base are
 * clamped to the sysfs attribute's own 141.1-155 MHz range.
 *
 * Actuator: artosyn_vo's /sys/.../pclk_hz (kernel-side slew ~0.049 %/step, one step per
 * frame, so a write is glitch-free). Sensor: the flip handler records the wait
 * (event time minus pending_since) for every completion; the 1 Hz tick takes the 5th
 * PERCENTILE of the interval's waits: low enough to guard the early tail against missed
 * latches, robust to single-frame outliers (an absolute minimum servo parks the median a
 * full vsync out when frame-ready jitter is wide). No video (fewer than PACE_MIN_FLIPS clean flips) parks the loop and
 * restores the boot rate so the next session starts from stock.
 */

#define PACE_SYSFS       "/sys/bus/platform/devices/8810000.vo/pclk_hz"
#define PACE_TARGET_US   3500       /* steady-state 5th-percentile submit-to-latch wait */
#define PACE_DEAD_US     1200       /* deadband: no correction inside target +- this */
#define PACE_NWAIT       64         /* waits kept per interval for the percentile */
#define PACE_MIN_FLIPS   30         /* clean flips per second to consider video live */
#define PACE_GAIN_DIV    25000      /* err/GAIN_DIV = fractional P command (16 ms -> 0.064 %) */
#define PACE_INTEG_DIV   8          /* integral rate = P authority / this, per second */
#define PACE_HZ_MIN      141100000  /* the sysfs attribute's accepted range */
#define PACE_HZ_MAX      155000000

void pace_flip(struct ctx *c, gint64 wait_us)
{
    if (!c->pace_on || wait_us <= 0) {
        return;
    }

    /* Called under disp_lock from the flip handler. */
    if (c->pace_n < PACE_NWAIT) {
        c->pace_w[c->pace_n] = wait_us;
    }

    c->pace_n++;
}

static int pace_cmp(const void *a, const void *b)
{
    gint64 d = *(const gint64 *)a - *(const gint64 *)b;

    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

static gboolean pace_tick(gpointer u)
{
    struct ctx *c = u;

    /* File playback flips at the clip's own rate (e.g. 30 fps), so the interval's waits do
     * not measure the 60 Hz latch phase, and the flip count sits right at PACE_MIN_FLIPS -
     * the servo then alternates between acting on garbage phase data and parking at the
     * boot rate, commanding ~3 % pclk transitions that slew for ~a second mid-scanout. The
     * DC's address latch does not survive that: the panel freezes on one frame while flips
     * keep completing (HW-observed; pace off plays the same clip correctly). Hold the
     * clock untouched for the whole playback; the servo resumes on return to live.
     */
    if (c->pb_active) {
        pthread_mutex_lock(&c->disp_lock);
        c->pace_n = 0;   /* discard the interval's samples */
        pthread_mutex_unlock(&c->disp_lock);
        return G_SOURCE_CONTINUE;
    }

    gint64 w[PACE_NWAIT];

    pthread_mutex_lock(&c->disp_lock);
    int n = c->pace_n;
    if (n > PACE_NWAIT) {
        n = PACE_NWAIT;
    }

    memcpy(w, c->pace_w, (size_t)n * sizeof w[0]);
    c->pace_n = 0;
    pthread_mutex_unlock(&c->disp_lock);

    gint64 lo_us = 0;
    if (n > 0) {
        qsort(w, (size_t)n, sizeof w[0], pace_cmp);
        lo_us = w[n / 20];   /* 5th percentile: early tail without single-frame outliers */
    }

    long cmd;
    if (n < PACE_MIN_FLIPS) {
        /* No steady video: park at the boot rate so stock behaviour returns. The
         * converged hold rate is kept for the next session.
         */
        cmd = c->pace_base_hz;
    } else {
        long err_us = (long)(lo_us - PACE_TARGET_US);

        /* Deadband: the P action itself slews phase ~1-2 ms/s, so correcting inside the
         * noise floor turns the servo into the disturbance (a sustained limit cycle whose
         * low swing clips the latch, one repeated frame per period). Inside the band the
         * clock holds and phase only random-walks with true drift.
         */
        if (err_us > -PACE_DEAD_US && err_us < PACE_DEAD_US) {
            err_us = 0;
        }

        /* Asymmetric clamp. In a sawtooth (rate a hair high: wait shrinks until a frame
         * clips the latch and wraps a full vsync), the post-wrap seconds read a large
         * wait and would drive the integrator UP, sustaining the cycle. A large positive
         * error is therefore untrustworthy (wrap artifact) and capped; a negative error
         * (clip imminent) keeps full weight, so the equilibrium settles below the clip
         * point. A genuinely slow panel still corrects at the capped rate.
         */
        if (err_us > 2000) {
            err_us = 2000;
        }

        long p = (long)((gint64)c->pace_hold_hz * err_us / 1000 / PACE_GAIN_DIV);

        /* Integrate the base toward the matched rate; freeze while railed. */
        if (c->pace_hold_hz > PACE_HZ_MIN && c->pace_hold_hz < PACE_HZ_MAX) {
            c->pace_hold_hz += p / PACE_INTEG_DIV;
        }

        if (c->pace_hold_hz < PACE_HZ_MIN) {
            c->pace_hold_hz = PACE_HZ_MIN;
        } else if (c->pace_hold_hz > PACE_HZ_MAX) {
            c->pace_hold_hz = PACE_HZ_MAX;
        }

        cmd = c->pace_hold_hz + p;
        if (cmd < PACE_HZ_MIN) {
            cmd = PACE_HZ_MIN;
        } else if (cmd > PACE_HZ_MAX) {
            cmd = PACE_HZ_MAX;
        }
    }

    if (cmd != c->pace_cmd_hz) {
        char buf[24];
        int len = snprintf(buf, sizeof buf, "%ld\n", cmd);

        if (pwrite(c->pace_fd, buf, (size_t)len, 0) == len) {
            c->pace_cmd_hz = cmd;
        }
    }

    if (c->pace_dbg && n >= PACE_MIN_FLIPS) {
        fprintf(stderr, "ml-pipeline: pace n=%d lo=%lldus hold=%ld cmd=%ld\n",
                n, (long long)lo_us, c->pace_hold_hz, cmd);
    }

    return G_SOURCE_CONTINUE;
}

void pace_init(struct ctx *c)
{
    const char *env = getenv("ML_PACE");

    if (!env) {
        return;
    }

    c->pace_fd = open(PACE_SYSFS, O_RDWR | O_CLOEXEC);
    if (c->pace_fd < 0) {
        perror("ml-pipeline: pace: " PACE_SYSFS);
        return;
    }

    char buf[24] = { 0 };
    if (pread(c->pace_fd, buf, sizeof buf - 1, 0) <= 0) {
        close(c->pace_fd);
        c->pace_fd = -1;
        return;
    }

    c->pace_base_hz = atol(buf);
    c->pace_cmd_hz = c->pace_base_hz;

    /* ML_PACE=<hz> seeds the servo base at a known matched rate (the phase error
     * saturates at one vsync, so integrating down from the ~3 %-high boot rate would
     * take minutes; seeding leaves the integrator only source drift to track).
     * ML_PACE=1 starts from the boot rate.
     */
    long seed = atol(env);
    c->pace_hold_hz = (seed >= PACE_HZ_MIN && seed <= PACE_HZ_MAX) ? seed
                                                                   : c->pace_base_hz;
    c->pace_dbg = getenv("ML_PACE_DBG") != NULL;
    c->pace_on = 1;
    g_timeout_add_seconds(1, pace_tick, c);
    fprintf(stderr, "ml-pipeline: pace: on, base %ld Hz, hold %ld Hz, target %d us\n",
            c->pace_base_hz, c->pace_hold_hz, PACE_TARGET_US);
}
