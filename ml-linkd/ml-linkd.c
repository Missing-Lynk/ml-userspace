/*
 * ml-linkd - the RF link daemon.
 *
 * Persistent, reconnecting keeper of the AR8030 downlink. Two roles, selected by --role:
 *
 *   rx (default), the goggle side - this file and the ml-rx-* modules:
 *     - Link FSM on /dev/artosyn_sdio: WAIT_DEV -> ASSOC (22 verbatim frames) ->
 *       SETTLE (~2.5 s port73+ff02 only) -> OPEN (2 SET frames + TX power) ->
 *       STEADY (vendor cadence forever: port0c ~24 Hz, port73 ~6 Hz, ff02 ~3.4 Hz).
 *     - ml-rx-reader.c deframes every chip reply and hands it to the module that owns it;
 *       ml-rx-udp.c runs the :20001/:10000 protocol, the consumer READY gate and the HUD command
 *       surface; ml-rx-chan.c owns the channel table, tuning and the link metrics; ml-rx-bind.c
 *       owns the pair sequence.
 *
 *   air (--role air), the air-unit side - ml-linkd-air.c and the ml-air-* modules: reads the
 *   battery voltage and SoC temperature over IIO, transmits the :10000 status frames to the goggle
 *   over sdio0, and answers the :20001 identity probe. Association is chip-autonomous from the
 *   insmod config.
 *
 * Scope: the RF link ONLY. No module loading (ml-rf-bringup does bring-up; NEVER
 * warm-reload artosyn_sdio), no process supervision.
 * Static binary, runs on a bare slot-B boot.
 *
 * SAFETY: userspace only, sends exactly the frames the vendor stack sends; slot B only.
 * Usage: ml-linkd [-d /dev/artosyn_sdio] [--role air|rx] [--fc-tty /dev/ttyS1] [--no-gate] [-v]
 */
#define _GNU_SOURCE                       /* pthread_timedjoin_np */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../ml-shared/mlm.h"
#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-rx.h"
#include "ml-rx-chan.h"
#include "ml-rx-bind.h"
#include "ml-rx-reader.h"
#include "ml-rx-udp.h"

/* TAG, LOCAL_ADDR, AIR_ADDR, HELLO_PORT, PARAMS_PORT, HELLO_LEN, PKT_MAX, UDP_TICK_US are shared
 * with the air role and live in ml-linkd.h; AIR_LOSS_MS, OPEN_STEP_US and OPEN_RETRY_EVERY are
 * shared with the RX modules and live in ml-rx.h. */
#define DEV_NODE         "/dev/artosyn_sdio"

/* frame pacing (us) */
#define ASSOC_STEP_US    20000
#define SETTLE_STEP_US   167000
#define STEADY_STEP_US   42000
#define WRITE_WAIT_MS    100              /* bounded wait for the chip's cmd TX queue to drain */
#define WRITE_POLL_MS    5                /* POLLOUT retry step inside that budget */

#define SETTLE_TICKS     15               /* ~2.5 s of SETTLE */
#define ALIVE_EVERY      720              /* STEADY ticks between alive lines (~30 s) */
#define POLL_LINK_EVERY  4                /* Get1V1Info every Nth STEADY tick (~6 Hz) */
#define FF02_EVERY       7                /* ff02 every Nth STEADY tick (~3.4 Hz) */
#define LINKINFO_EVERY   24               /* publish MLM_T_LINKINFO every Nth STEADY tick (~1 Hz) */
#define SCAN_TABLE_EVERY 48               /* republish the seeded channel table every Nth tick (~2 s) */
#define SEQ_START        0x15             /* initial poll sequence number */

atomic_int g_run = 1;                       /* shared with the air role (ml-linkd.h) */
int g_verbose;                              /* shared with the air role (ml-linkd.h) */
int g_fd = -1;                              /* /dev/artosyn_sdio */
int g_no_gate;
int g_scan_probe;                           /* --scan-probe: hexdump the raw GetScanResult + Get1V1Info replies */
static int g_role_air;                      /* --role air: transmit-side (air unit), not the RX goggle */

atomic_int g_steady;                        /* FSM reached STEADY */
atomic_int g_hs_done;                       /* :20001 3-way done */
atomic_int g_params_acked;                  /* the air answered our params poll this session */
atomic_int g_air_lost;                      /* >5 s :10000 silence flagged */
atomic_int g_ready;                         /* consumer READY (heartbeat fresh) */
atomic_int g_video_confirmed;               /* consumer reported frames_seen after our ACK */
atomic_int g_standby_state;                 /* air's LIVE work-mode from SetStandyMode (0x12): 1 = standby */
atomic_long g_last_telem_ms;                /* last :10000 RX */

static void on_sig(int sig)
{
    (void)sig;
    ml_request_stop();
}

long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* MLM producer (telemetry.sock / osd.sock; drop on error, never block). */

/* one unconnected AF_UNIX dgram socket */
static int g_mlm;

void mlm_pub(const char *path, uint16_t type, const void *payload, size_t n)
{
    struct sockaddr_un dst = { .sun_family = AF_UNIX };
    uint8_t buf[sizeof(struct mlm_hdr) + PKT_MAX];
    struct mlm_hdr hdr = { .magic = MLM_MAGIC, .type = type, .flags = 0 };

    if (g_mlm < 0 || n > sizeof(buf) - sizeof hdr) {
        return;
    }

    memcpy(buf, &hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, payload, n);
    strncpy(dst.sun_path, path, sizeof dst.sun_path - 1);
    sendto(g_mlm, buf, sizeof hdr + n, MSG_DONTWAIT, (struct sockaddr *)&dst, sizeof dst);
}

/* Append the transition to the flight-session log on the SD card, if ml-logd is running. The
 * session dir path lives in /run/ml-log.dir (ml-logd is the sole writer); absent = no card or
 * logger off, so we skip silently and keep only the stdout line. Events are rare, so open/close
 * per call is fine. The uptime_ms column shares ml-logd's CLOCK_MONOTONIC axis for correlation.
 */
static void rf_log(uint32_t state, const char *what)
{
    char dir[256], path[300];
    FILE *dir_fp, *log_fp;

    dir_fp = fopen("/run/ml-log.dir", "r");
    if (!dir_fp) {
        return;
    }

    if (!fgets(dir, sizeof dir, dir_fp)) {
        fclose(dir_fp);
        return;
    }

    fclose(dir_fp);
    dir[strcspn(dir, "\n")] = '\0';
    if (dir[0] == '\0') {
        return;
    }

    snprintf(path, sizeof path, "%s/rf.log", dir);
    log_fp = fopen(path, "a");
    if (!log_fp) {
        return;
    }

    fprintf(log_fp, "%ld %u %s\n", now_ms(), state, what);
    fclose(log_fp);
}

void link_event_aux(uint32_t state, uint32_t aux, const char *what)
{
    struct mlm_link link_msg = { .state = state, .aux = aux };

    printf(TAG " link: %s\n", what);
    fflush(stdout);
    mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_LINK, &link_msg, sizeof link_msg);
    rf_log(state, what);
}

void link_event(uint32_t state, const char *what)
{
    link_event_aux(state, 0, what);
}

/* OPEN: the RX-chain power set from the vendor's RX_Init burst, 23 dBm. The 5 GET read-backs and
 * the buggy "set remote info" frame from the original capture are dropped (GETs configure nothing;
 * full decode + the drop rationale in re/notes/rf-open-sequence-decode.md). The capture's channel
 * select is not replayed either: its index is only valid in the race channel mode, so the channel
 * is chosen from the chip's own band by rx_chan_open instead. seq is the captured RPC sequence id,
 * replayed as-is. */
#define OPEN_RX_POWER_SEQ 0xcbf
#define OPEN_RX_POWER_DBM 0x17

int send_frame(const uint8_t *frame, int n, const char *tag)
{
    struct pollfd pfd = { .fd = g_fd, .events = POLLOUT };
    int waited_ms;

    for (waited_ms = 0; waited_ms <= WRITE_WAIT_MS; waited_ms += WRITE_POLL_MS) {
        ssize_t written = write(g_fd, frame, (size_t)n);

        if (written == n) {
            if (waited_ms > 0 && g_verbose) {
                fprintf(stderr, TAG " tx %s accepted after %d ms of backpressure\n",
                        tag, waited_ms);
            }

            if (g_verbose) {
                fprintf(stderr, TAG " tx %s (%d B)\n", tag, n);
            }

            return 0;
        }

        if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
            pfd.revents = 0;
            poll(&pfd, 1, WRITE_POLL_MS);
            continue;
        }

        fprintf(stderr, TAG " write(%s)=%zd (%s)\n", tag, written, strerror(errno));
        return -1;
    }

    fprintf(stderr, TAG " write(%s) not accepted in %d ms, frame dropped\n", tag, WRITE_WAIT_MS);
    return -1;
}

/* ASSOC: association bring-up, chip-bound (NOT air commands):
 *   6x ch0xff/port1 + 2x ch0xff/port0        link-management init pings
 *   1x ch05 (4-byte zero payload)             opens the ch05 chip-log channel (the reader drains it)
 *   14x ch03/op01, port 0x0e..0x00 (skip 5)   the association countdown -> chip "rpc sta=1"
 * 20 ms spacing throughout, matching the captured vendor cadence.
 */
static void assoc_bringup(void)
{
    uint8_t frame[32];
    static const uint8_t log_open[4] = { 0 };

    for (int i = 0; i < 6 && ml_should_run(); i++) {
        send_frame(frame, bb_build_frame(frame, BB_LINK, 0, 0, 0x01, 0, NULL, 0), "assoc");
        usleep(ASSOC_STEP_US);
    }

    for (int i = 0; i < 2 && ml_should_run(); i++) {
        send_frame(frame, bb_build_frame(frame, BB_LINK, 0, 0, 0x00, 0, NULL, 0), "assoc");
        usleep(ASSOC_STEP_US);
    }

    send_frame(frame, bb_build_frame(frame, BB_LOG, 0, 0, 0x00, 0, log_open, 4), "assoc-log");
    usleep(ASSOC_STEP_US);

    for (int port = 0x0e; port >= 0 && ml_should_run(); port--) {
        if (port == 0x05) {
            continue;
        }

        send_frame(frame, bb_build_frame(frame, BB_ASSOC, 0x01, 0, (uint8_t)port, 0, NULL, 0), "assoc-cd");
        usleep(ASSOC_STEP_US);
    }
}
/* SETTLE: ~2.5 s of Get1V1Info + ff02 ONLY (an early GetTime poll wedges the air). */
static void rx_settle(const uint8_t *ff02, uint32_t *seq_link)
{
    for (int i = 0; i < SETTLE_TICKS && ml_should_run(); i++) {
        uint8_t poll[19];

        bb_get(poll, GET_1V1INFO, (*seq_link)++);
        send_frame(poll, 19, "get-1v1");
        send_frame(ff02, 19, "ff02");
        usleep(SETTLE_STEP_US);
    }
}

/* OPEN: choose the channel from the chip's band, set the RX chain's power, then the goggle's TX
 * power and auto-adjust. The channel comes first, as in the capture, but it is now chosen rather
 * than replayed: the band has to be read off the chip before an index means anything. */
static void rx_open(uint8_t *frame, uint32_t *seq_link, uint32_t *seq_video)
{
    rx_chan_open(frame, seq_link);

    send_frame(frame, bb_set_power(frame, RF_RX, OPEN_RX_POWER_DBM, OPEN_RX_POWER_SEQ), "rx-power");
    usleep(OPEN_STEP_US);

    /* 23 dBm */
    send_frame(frame, bb_set_power(frame, RF_TX, 0x17, (*seq_video)++), "tx-power");
    usleep(20000);

    send_frame(frame, bb_set_power_auto(frame, 1, (*seq_video)++), "tx-power-auto");
    usleep(20000);
}

/* STEADY: the vendor cadence forever (GetTime ~24 Hz, Get1V1Info ~6 Hz, ff02 ~3.4 Hz), plus the
 * queued HUD work. This thread is the only bb-socket TX owner, so the retune, the sweep and the
 * bind all run here rather than on the thread that received the request. */
static void rx_steady(uint8_t *frame, const uint8_t *ff02, uint32_t *seq_link, uint32_t *seq_video)
{
    unsigned long ticks = 0;

    rx_state_set(&g_steady, 1);
    link_event(MLM_LINK_ASSOCIATED, "bring-up done, steady cadence");
    while (ml_should_run()) {
        uint8_t poll[19];

        bb_get(poll, GET_TIME, (*seq_video)++);
        send_frame(poll, 19, "get-time");

        rx_chan_service(frame, seq_link);
        rx_bind_service(frame, seq_link);

        if (ticks % POLL_LINK_EVERY == 0) {
            bb_get(poll, GET_1V1INFO, (*seq_link)++);
            send_frame(poll, 19, "get-1v1");
        }

        if (ticks % FF02_EVERY == 0) {
            send_frame(ff02, 19, "ff02");
        }

        /* Publish the local baseband link metrics for the HUD's System OSD (~1 Hz). Channel is a
         * config fact we always know; SNR/distance go stale on air loss, so blank them then and let
         * the HUD dim the whole air-unit side off its own connection state. */
        if (ticks % LINKINFO_EVERY == 0) {
            int air_lost = rx_state_get(&g_air_lost);
            int standby_state = rx_state_get(&g_standby_state);
            struct mlm_linkinfo info = {
                .channel = rx_chan_index(),
                .snr_db = air_lost ? MLM_LINKINFO_NONE : rx_chan_snr_db(),
                .distance_m = air_lost ? MLM_LINKINFO_NONE : rx_chan_distance_m(),
                .flags = (!air_lost && standby_state) ? MLM_LINKINFO_F_STANDBY : 0,
                /* rx_throughput (Get1V1Info +0x0c) = measured PHY link throughput. We publish it RAW.
                 * The vendor OSD shows this same field but divides it by a HARDCODED 6 in standby
                 * (AR_MID_GET_REALTIME_SYS_INFO param_2[7] = uVar3 / 6, gated on the RcStatus standby
                 * low byte) - a fixed display fudge, not a real measurement, so we deliberately do
                 * NOT replicate it. Our number is the honest link throughput in both states. */
                .throughput_kbps = air_lost ? 0 : (uint32_t)rx_chan_throughput_kbps(),
            };

            mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_LINKINFO, &info, sizeof info);
        }

        /* Keep the seeded channel table available to a HUD that started late (measured=0, no sweep). */
        if (ticks % SCAN_TABLE_EVERY == 0) {
            rx_chan_table_publish();
        }

        usleep(STEADY_STEP_US);

        /* Every state this reports already logged its own transition through link_event, so the
         * periodic form is a -v convenience for watching a session, not news. */
        if (++ticks % ALIVE_EVERY == 0 && g_verbose) {
            printf(TAG " alive hs=%d ready=%d acked=%d video=%d air_lost=%d thr_kbps=%d\n",
                   rx_state_get(&g_hs_done), rx_state_get(&g_ready),
                   rx_state_get(&g_params_acked), rx_state_get(&g_video_confirmed),
                   rx_state_get(&g_air_lost),
                   rx_chan_throughput_kbps());
            fflush(stdout);
        }
    }
}

/* The reader may be blocked in read() on the device; a SIGTERM directed at the thread interrupts it
 * with EINTR (SA_RESTART is off) and it exits on the g_run check. Re-send until the join lands: a
 * signal that arrives just before the thread re-enters read() is consumed without unblocking it. */
static void reader_join(pthread_t reader_th)
{
    for (;;) {
        struct timespec deadline;

        pthread_kill(reader_th, SIGTERM);
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        if (pthread_timedjoin_np(reader_th, NULL, &deadline) == 0) {
            return;
        }
    }
}

/* The RX (goggle) role: open the device, start the reader and UDP threads, then run the FSM. */
static int rx_main(const char *node)
{
    pthread_t reader_th, udp_th;
    uint8_t frame[64], ff02[19];
    uint32_t seq_link = SEQ_START, seq_video = SEQ_START;
    int rc;

    /* WAIT_DEV: ml-rf-bringup must have run; retry until the node appears. Log on the first miss
     * and then every OPEN_RETRY_EVERY tries, so a link down for a while does not flood the log. */
    for (unsigned tries = 0; ml_should_run() && (g_fd = open(node, O_RDWR | O_NONBLOCK)) < 0; tries++) {
        if (tries == 0 || (tries % OPEN_RETRY_EVERY) == 0) {
            fprintf(stderr, TAG " open(%s): %s, retrying\n", node, strerror(errno));
        }

        sleep(1);
    }

    if (!ml_should_run()) {
        return 0;
    }

    /* The FSM stages below are steps of one sequence whose outcome is the ASSOCIATED link event, so
     * they are -v. A stage that fails says so on its own. */
    if (g_verbose) {
        printf(TAG " opened %s%s\n", node, g_no_gate ? " (gate disabled)" : "");
        fflush(stdout);
    }

    rc = pthread_create(&reader_th, NULL, rx_reader_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, TAG " reader thread: %s\n", strerror(rc));
        close(g_fd);
        g_fd = -1;
        return 1;
    }

    rc = pthread_create(&udp_th, NULL, rx_udp_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, TAG " UDP thread: %s\n", strerror(rc));
        ml_request_stop();
        reader_join(reader_th);
        close(g_fd);
        g_fd = -1;
        return 1;
    }

    /* ASSOC: association bring-up (chip-bound), then build the reusable ff02 heartbeat frame */
    if (g_verbose) {
        printf(TAG " ASSOC (chip bring-up)\n");
    }

    assoc_bringup();
    bb_build_frame(ff02, BB_LINK, 0, 0, 0x02, 0, NULL, 0);

    if (g_verbose) {
        printf(TAG " SETTLE (Get1V1Info + ff02 only, ~2.5 s)\n");
    }

    rx_settle(ff02, &seq_link);

    if (g_verbose) {
        printf(TAG " OPEN (bb-socket setup + TX power)\n");
    }

    rx_open(frame, &seq_link, &seq_video);

    rx_steady(frame, ff02, &seq_link, &seq_video);

    ml_request_stop();
    reader_join(reader_th);
    pthread_join(udp_th, NULL);
    close(g_fd);
    g_fd = -1;

    return 0;
}

int main(int argc, char **argv)
{
    const char *node = DEV_NODE;
    const char *hw_version = NULL;
    const char *fc_tty = NULL;
    enum ml_rate_mode rate_mode = ML_RATE_OFF;
    enum ml_power_mode power_mode = ML_POWER_OFF;
    struct sigaction sa;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            node = argv[++i];
        } else if (!strcmp(argv[i], "--hw-version") && i + 1 < argc) {
            hw_version = argv[++i];
        } else if (!strcmp(argv[i], "--fc-tty") && i + 1 < argc) {
            fc_tty = argv[++i];
        } else if (!strcmp(argv[i], "--no-gate")) {
            g_no_gate = 1;
        } else if (!strcmp(argv[i], "--scan-probe")) {
            g_scan_probe = 1;
        } else if (!strcmp(argv[i], "--rate-adapt")) {
            rate_mode = ML_RATE_ON;
        } else if (!strcmp(argv[i], "--rate-probe")) {
            rate_mode = ML_RATE_PROBE;
        } else if (!strcmp(argv[i], "--power-adapt")) {
            power_mode = ML_POWER_ON;
        } else if (!strcmp(argv[i], "--power-probe")) {
            power_mode = ML_POWER_PROBE;
        } else if (!strcmp(argv[i], "--role") && i + 1 < argc) {
            i++;
            if (!strcmp(argv[i], "air")) {
                g_role_air = 1;
            } else if (!strcmp(argv[i], "rx")) {
                g_role_air = 0;
            } else {
                fprintf(stderr, "ml-linkd: unknown role '%s' (want air|rx)\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "-v")) {
            g_verbose = 1;
        } else {
            fprintf(stderr,
                    "usage: ml-linkd [-d /dev/artosyn_sdio] [--role air|rx] [--hw-version STR] "
                    "[--fc-tty /dev/ttyS1] [--no-gate] [--scan-probe] "
                    "[--rate-adapt|--rate-probe] [--power-adapt|--power-probe] [-v]\n");
            return 2;
        }
    }

    /* no SA_RESTART: a signal must interrupt the blocking device read (EINTR) so shutdown
     * cannot hang in the reader thread; glibc signal() would install the handler restarting */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Air unit (--role air): UDP telemetry TX on sdio0, handled entirely in ml-linkd-air.c. */
    if (g_role_air) {
        air_main(hw_version, fc_tty, rate_mode, power_mode);
        return 0;
    }

    g_mlm = socket(AF_UNIX, SOCK_DGRAM, 0);
    rx_main(node);

    if (g_mlm >= 0) {
        close(g_mlm);
    }

    printf(TAG " clean stop\n");
    return 0;
}
