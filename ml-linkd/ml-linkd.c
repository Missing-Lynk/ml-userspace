/*
 * ml-linkd - the RF link daemon.
 *
 * Persistent, reconnecting keeper of the AR8030 downlink:
 *
 *   - Link FSM on /dev/artosyn_sdio: WAIT_DEV -> ASSOC (22 verbatim frames) ->
 *     SETTLE (~2.5 s port73+ff02 only) -> OPEN (2 SET frames + TX power) ->
 *     STEADY (vendor cadence forever: port0c ~24 Hz, port73 ~6 Hz, ff02 ~3.4 Hz).
 *   - UDP :20001 3-way hello (stops after hs_done, vendor fidelity) and :10000
 *     params handshake (type1 request at the vendor's 2 s rate, type3 ACK on the
 *     air's type2 reply) + telemetry drain.
 *   - Consumer-ready gate: the type1 request is NOT sent until a consumer declared
 *     READY on link.sock (MLM_T_READY heartbeat, 6 s liveness window), so the
 *     one-and-only IDR at FrameId 0 is emitted while the decoder is listening on
 *     :10001. --no-gate for bench use.
 *   - Telemetry publish: MSP DisplayPort (type 0x10) -> MLM_T_MSP on osd.sock,
 *     binary status (0x09/0x11) -> MLM_T_STATUS on telemetry.sock, link state
 *     changes -> MLM_T_LINK on telemetry.sock. MSG_DONTWAIT, drop on error.
 *   - Air-loss detection: >5 s of :10000 silence in STEADY = air lost (reset the
 *     handshake state, keep the bb-socket cadence; the chip re-associates
 *     autonomously); on return, re-handshake behind the READY gate.
 *
 * Scope: the RF link ONLY. No module loading (ml-rf-bringup does bring-up; NEVER
 * warm-reload artosyn_sdio), no process supervision, no binding yet.
 * Static binary, runs on a bare slot-B boot.
 *
 * SAFETY: userspace only, sends exactly the frames the vendor stack sends; slot B only.
 * Usage: ml-linkd [-d /dev/artosyn_sdio] [--no-gate] [-v]
 */
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
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../ml-shared/mlm.h"
#include "bb-cmd.h"

#define TAG              "[ml-linkd]"
#define DEV_NODE         "/dev/artosyn_sdio"

#define LOCAL_ADDR       "10.0.0.1"
#define AIR_ADDR         "10.0.0.100"
#define HELLO_PORT       20001            /* :20001 3-way hello */
#define PARAMS_PORT      10000            /* :10000 params handshake + telemetry */

/* service intervals and timeouts (ms) */
#define READY_WINDOW_MS  6000             /* consumer heartbeat liveness window */
#define AIR_LOSS_MS      5000             /* :10000 silence in STEADY => air lost */
#define HELLO_IVL_MS     300              /* :20001 hello cadence (~3 Hz) */
#define PARAMS_IVL_MS    2000             /* :10000 request cadence (vendor 2 s) */
#define STANDBY_IVL_MS   2000             /* :10000 SetTranParm re-send cadence while armed (~2 s) */
#define ACK_GRACE_MS     6000             /* re-poll grace after a type3 ACK */
#define LED_ASSERT_MS    1000             /* LED re-assert cadence (~1 Hz) */
#define LED_BREATHE_MS   3000             /* breathe-red period */

/* frame pacing (us) */
#define ASSOC_STEP_US    20000
#define SETTLE_STEP_US   167000
#define OPEN_STEP_US     60000
#define STEADY_STEP_US   42000
#define UDP_TICK_US      50000            /* 20 Hz service tick */
#define READ_IDLE_US     2000

#define SETTLE_TICKS     15               /* ~2.5 s of SETTLE */
#define ALIVE_EVERY      720              /* STEADY ticks between alive lines (~30 s) */
#define OPEN_RETRY_EVERY 30               /* WAIT_DEV: log every Nth failed open */
#define POLL_LINK_EVERY  4                /* Get1V1Info + GetDistance every Nth STEADY tick (~6 Hz) */
#define FF02_EVERY       7                /* ff02 every Nth STEADY tick (~3.4 Hz) */
#define LINKINFO_EVERY   24               /* publish MLM_T_LINKINFO every Nth STEADY tick (~1 Hz) */
#define SEQ_START        0x15             /* initial poll sequence number */

/* The baseband channel index we tune the local RX to at OPEN (SET_CHNIDX). Single source of truth:
 * used both in OPEN_SEQ and when publishing the channel for the HUD's System OSD. */
#define OPEN_CHNIDX      5

/* Baseband GET reply field offsets. Replies arrive on the request channel BB_GET (0x01), port =
 * selector. Get1V1Info (0x73) carries a linear SNR at +0x06 (dB = 10*log10(raw/36); zero until a
 * video link is up); GetDistanceResult (0x05) a u32 at +0 (0xFFFFFFFF = no ranging fix). */
#define REPLY_CH         0x01
#define V1V1_OFF_SNR     0x06
#define DIST_OFF_RESULT  0x00
#define DIST_NONE        0xFFFFFFFFu   /* GetDistance result when no ranging fix (bench / no GPS) */

/* :10000 params-handshake message types (LE u32, byte 0) */
#define MP_REQUEST       1
#define MP_REPLY         2
#define MP_ACK           3
/* :10000 air-to-goggle telemetry types */
#define AIR_MSP          0x10             /* MSP DisplayPort canvas */
#define AIR_STATUS_A     0x09
#define AIR_STATUS_B     0x11
#define AIR_STANDBY      0x12             /* SetStandyMode: air's work-mode sync (body[0]: 1=standby) */
#define STANDBY_OFF_MODE 20               /* body offset 0 = datagram offset 20 (u32 work-mode) */
#define STANDBY_MODE_ON  1                /* work-mode 1 = standby (0 = normal, 2 = airscrew/armed) */

/* STB_EVENT_ACK (msg 0x1b): the goggle->air ack that COMPLETES the air's standby entry. Without it the
 * air holds at full fps - it hard-gates its fps/power drop on receiving this ack (air StbThread checks
 * handle+0x188, set only by StbAck / FSM case 0x1b). Stock sends one per standby transition. Empty body:
 * just the 24-byte header with msg_type 0x1b + timestamp. Verified by HW capture (slota-airconfig
 * `1b0000...`) and static RE (AR_LOWDELAY_RX_SYSCTRL_StbThread @004392a8; air StbAck @42327 gates fps
 * @40566). */
#define MP_STBACK        0x1b
#define STBACK_LEN       24               /* header only, no body */

/* :10000 SetTranParm (msg 0x0D) - the air's TX-power + standby-arm datagram. Byte layout is the
 * HW-confirmed vendor tuple (see plans/rf-air-config.md): a 34-byte payload, 10-byte body at payload
 * offset 20. body[0] = TX power dBm, body[1] = 0x04 (const), body[8] = u8StandbyModeEn. We send the
 * known-good tuple verbatim and vary only power (body[0]) and standby (body[8]); fabricating other
 * RF bytes can reboot the goggle, so nothing else is touched. */
#define MP_SETTRANPARM   0x0D
#define STP_LEN          34               /* full :10000 SetTranParm payload */
#define STP_OFF_BODY     20               /* body starts here (0x0D uses the [20..23] word) */
#define STP_OFF_POWER    (STP_OFF_BODY + 0)  /* body[0]: TX power in dBm */
#define STP_OFF_CONST    (STP_OFF_BODY + 1)  /* body[1]: 0x04, constant in every captured frame */
#define STP_OFF_STANDBY  (STP_OFF_BODY + 8)  /* body[8]: u8StandbyModeEn (0/1) */
#define STP_BODY_LEN     0x0A             /* header length field (payload off 16) */

/* HW-confirmed mW -> dBm values for SetTranParm body[0] (plans/rf-air-config.md §2). Only these three
 * levels are valid; a fabricated dBm can reboot the goggle, so map_power_dbm returns -1 for anything
 * else and the command is dropped. */
enum air_tx_dbm {
    AIR_TX_DBM_25  = 0x0e,   /* 25 mW  */
    AIR_TX_DBM_100 = 0x14,   /* 100 mW */
    AIR_TX_DBM_200 = 0x17,   /* 200 mW */
};
#define AIR_TX_DBM AIR_TX_DBM_100         /* vendor MID default, sent until the HUD commands a level */

/* datagram buffer sizes */
#define PKT_MAX          600              /* receive buffer */
#define HELLO_LEN        520              /* :20001 hello datagram */
#define PARAMS_LEN       24               /* :10000 params message */

static volatile int g_run = 1;
static int g_fd = -1;                       /* /dev/artosyn_sdio */
static int g_verbose;
static int g_no_gate;

/* handshake/link state shared between the FSM tick (main) and the UDP thread.
 * All plain ints/timestamps, single writer per field, so volatile is enough. */
static volatile int g_steady;               /* FSM reached STEADY */
static volatile int g_hs_done;              /* :20001 3-way done */
static volatile int g_params_acked;         /* type3 ACK sent this session */
static volatile int g_air_lost;             /* >5 s :10000 silence flagged */
static volatile int g_ready;                /* consumer READY (heartbeat fresh) */
static volatile int g_video_confirmed;      /* consumer reported frames_seen after our ACK */
static volatile long g_last_telem_ms;       /* last :10000 RX */
static volatile long g_last_ready_ms;       /* last MLM_T_READY heartbeat */
static volatile long g_last_ack_ms;         /* last type3 ACK sent */

/* Local baseband link metrics, parsed from the GET replies in the reader thread and published for
 * the HUD's System OSD. Single writer (reader), single reader (main publish); MLM_LINKINFO_NONE
 * until a reply lands. */
static volatile int g_snr_db = MLM_LINKINFO_NONE;
static volatile int g_distance_m = MLM_LINKINFO_NONE;

/* Air-unit RF config the HUD has commanded (MLM_T_RFCMD on link.sock). -1 = never commanded, so
 * nothing is pushed to the air until the HUD asserts it; the HUD re-asserts on every link-up edge.
 * ml-linkd re-sends the SetTranParm on a steady cadence while linked, so no per-change latch is
 * needed - a toggle or a returning air unit is picked up by the next tick. */
static volatile int g_standby_arm = -1;     /* 0/1 = HUD-commanded u8StandbyModeEn, -1 = unknown */
static volatile int g_power_dbm = -1;        /* HUD-commanded TX power (dBm) for SetTranParm body[0], -1 = unset */
static volatile int g_standby_state;        /* air's LIVE work-mode from SetStandyMode (0x12): 1 = in standby */

/* Map a HUD-commanded mW level to the air's SetTranParm dBm byte; -1 rejects anything not captured. */
static int map_power_dbm(uint32_t mw)
{
    switch (mw) {
        case 25: {
            return AIR_TX_DBM_25;
        } break;

        case 100: {
            return AIR_TX_DBM_100;
        } break;

        case 200: {
            return AIR_TX_DBM_200;
        } break;

        default: {
            return -1;
        } break;
    }
}

static void on_sig(int sig)
{
    (void)sig;
    g_run = 0;
}

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* Absolute ChnIdx -> displayed "CH N" (1..16): number = ChnIdx - 1, shown directly, with 0 -> 16. */
static int chnidx_to_display(int chnidx)
{
    int number = chnidx - 1;

    return number == 0 ? 16 : number;
}

/* MLM producer (telemetry.sock / osd.sock; drop on error, never block). */

/* one unconnected AF_UNIX dgram socket */
static int g_mlm;

static void mlm_pub(const char *path, uint16_t type, const void *payload, size_t n)
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

static void link_event(uint32_t state, const char *what)
{
    struct mlm_link link_msg = { .state = state };

    printf(TAG " link: %s\n", what);
    fflush(stdout);
    mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_LINK, &link_msg, sizeof link_msg);
    rf_log(state, what);
}

/* Drive the status LED (ml-ledd) off link state: breathe red while there is no video,
 * solid green once the params handshake is acked. Sent on the edges for latency and
 * re-asserted ~1 Hz (led_assert) so a late/restarted ml-ledd reconverges.
 */
static void led_cmd(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms)
{
    struct mlm_led led = { .mode = mode, .r = r, .g = g, .b = b, .period_ms = period_ms };

    mlm_pub(MLM_LED_SOCK, MLM_T_LED, &led, sizeof led);
}

static void led_assert(void)
{
    if (g_params_acked && !g_air_lost) {
        /* link up, video flowing */
        led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);
    } else {
        /* no usable link yet */
        led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
    }
}

/* One frame of the captured socket-open sequence. */
struct bb_frame {
    uint8_t channel;
    uint8_t opcode;
    uint8_t slot;
    uint8_t port;
    uint32_t seq;               /* RPC sequence counter, replayed as captured */
    const uint8_t *payload;
    int plen;
};

/* OPEN: the two SET config frames from the vendor's RX_Init burst - select channel + set RX power.
 * The 5 GET read-backs and the buggy "set remote info" frame from the original capture are dropped
 * (GETs configure nothing; full decode + the drop rationale in re/notes/rf-open-sequence-decode.md).
 * These two SETs are the proven minimal OPEN: validated end-to-end on a cold chip, video streams.
 * seq is the captured RPC sequence id, replayed as-is.
 */
static const struct bb_frame OPEN_SEQ[] = {
    { BB_SET, 0, 0, SET_CHNIDX, 6,     (const uint8_t[]){ 0x02, OPEN_CHNIDX }, 2 },  /* select channel index */
    { BB_SET, 0, 0, SET_POWER,  0xcbf, (const uint8_t[]){ RF_RX, 0x17 }, 2 },  /* RX chain, 23 dBm */
};
static const int OPEN_SEQ_N = sizeof(OPEN_SEQ) / sizeof(OPEN_SEQ[0]);

static int send_frame(const uint8_t *frame, int n, const char *tag)
{
    ssize_t written = write(g_fd, frame, n);
    if (written != n) {
        fprintf(stderr, TAG " write(%s)=%zd (%s)\n", tag, written, strerror(errno));
        return -1;
    }

    if (g_verbose) {
        fprintf(stderr, TAG " tx %s (%d B)\n", tag, n);
    }

    return 0;
}

/* reader thread: chip log off bb-socket ch05. */

static char g_chiplog[16384];
static int g_chiplog_n;

static void *reader(void *arg)
{
    uint8_t buf[8192];

    (void)arg;
    while (g_run) {
        ssize_t n = read(g_fd, buf, sizeof buf);

        if (n <= 0) {
            if (!g_run) {
                break;
            }

            usleep(READ_IDLE_US);
            continue;
        }

        for (ssize_t i = 0; i + 18 < n; i++) {
            int plen;

            if (buf[i] != 0xAA) {
                continue;
            }

            plen = buf[i + 1] | (buf[i + 2] << 8);
            if (plen < 0 || i + 18 + plen >= n) {
                continue;
            }

            if (buf[i + 18 + plen] != 0xBB) {
                continue;
            }

            const uint8_t *payload = buf + i + 18;

            if (buf[i + 5] == 0x05 || (buf[i + 5] == 0x03 && buf[i + 8] == 0x06)) {
                for (int k = 0; k < plen && g_chiplog_n < (int)sizeof(g_chiplog) - 1; k++) {
                    char ch = buf[i + 18 + k];

                    if (ch == '\n' || ch == '\r') {
                        if (g_chiplog_n) {
                            g_chiplog[g_chiplog_n] = 0;
                            /* ch05 chip log is a verbose RF-debug stream (a down link spews it fast
                             * enough to fill the log tmpfs), so forward it only under -v. */
                            if (g_verbose) {
                                printf("[chip] %s\n", g_chiplog);
                            }
                            g_chiplog_n = 0;
                        }
                    } else if (ch >= 32 && ch < 127) {
                        g_chiplog[g_chiplog_n++] = ch;
                    }
                }
                fflush(stdout);
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_1V1INFO && plen >= V1V1_OFF_SNR + 2) {
                /* Get1V1Info reply: linear SNR at +0x06 -> dB. Empty inter-poll replies (raw 0, seen
                 * when video is idle or during a brief Tx-link blip) keep the last good value so the
                 * OSD does not flicker to No Link; a real link loss blanks it via the air_lost gate.
                 */
                unsigned raw = payload[V1V1_OFF_SNR] | (payload[V1V1_OFF_SNR + 1] << 8);
                if (raw > 0) {
                    g_snr_db = (int)lroundf(10.0f * log10f((float)raw / 36.0f));
                }

                if (g_verbose) {
                    printf(TAG " 1v1info snr_raw=%u snr_db=%d\n", raw, g_snr_db);
                    fflush(stdout);
                }
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_DISTANCE && plen >= DIST_OFF_RESULT + 4) {
                /* GetDistanceResult reply: u32 ranging result at +0, in metres per the vendor OSD.
                 * 0xFFFFFFFF = no ranging fix (the bench value); publish NONE so the OSD shows a
                 * placeholder. A real value awaits a live ranging flight.
                 */
                unsigned d = payload[DIST_OFF_RESULT] | (payload[DIST_OFF_RESULT + 1] << 8)
                           | (payload[DIST_OFF_RESULT + 2] << 16) | (payload[DIST_OFF_RESULT + 3] << 24);

                g_distance_m = d == DIST_NONE ? MLM_LINKINFO_NONE : (int)d;
                if (g_verbose) {
                    printf(TAG " distance=%d m (raw 0x%08x)\n", g_distance_m, d);
                    fflush(stdout);
                }
            }
            i += 18 + plen;
        }
    }

    return NULL;
}

/* UDP thread: :20001 hello/ack, :10000 handshake + telemetry, link.sock gate. */

static void *udp_thread(void *arg)
{
    int hello_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int params_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int link_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    int one = 1;
    struct sockaddr_in local, air_hello, air_params;
    struct sockaddr_un link_un = { .sun_family = AF_UNIX };
    uint8_t hello[HELLO_LEN], params_req[PARAMS_LEN], params_ack[PARAMS_LEN];
    long last_hello = 0, last_req = 0, last_led = 0, last_stp = 0;

    (void)arg;
    setsockopt(hello_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(params_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    inet_pton(AF_INET, LOCAL_ADDR, &local.sin_addr);
    local.sin_port = htons(HELLO_PORT);
    if (bind(hello_sock, (struct sockaddr *)&local, sizeof local) < 0) {
        fprintf(stderr, TAG " bind :%d: %s (is sdio0 up as %s?)\n",
                HELLO_PORT, strerror(errno), LOCAL_ADDR);
    }

    local.sin_port = htons(PARAMS_PORT);
    if (bind(params_sock, (struct sockaddr *)&local, sizeof local) < 0) {
        fprintf(stderr, TAG " bind :%d: %s\n", PARAMS_PORT, strerror(errno));
    }

    memset(&air_hello, 0, sizeof air_hello);
    air_hello.sin_family = AF_INET;
    inet_pton(AF_INET, AIR_ADDR, &air_hello.sin_addr);
    air_params = air_hello;
    air_hello.sin_port = htons(HELLO_PORT);
    air_params.sin_port = htons(PARAMS_PORT);

    /* link.sock: the READY-gate seam; linkd binds, consumers send heartbeats */
    mkdir(MLM_RUN_DIR, 0755);
    unlink(MLM_LINK_SOCK);
    strncpy(link_un.sun_path, MLM_LINK_SOCK, sizeof link_un.sun_path - 1);
    if (bind(link_sock, (struct sockaddr *)&link_un, sizeof link_un) < 0) {
        fprintf(stderr, TAG " bind %s: %s\n", MLM_LINK_SOCK, strerror(errno));
    }

    memset(hello, 0, sizeof hello);
    memset(params_req, 0, sizeof params_req);
    params_req[0] = MP_REQUEST;
    memset(params_ack, 0, sizeof params_ack);
    params_ack[0] = MP_ACK;

    while (g_run) {
        long now = now_ms();
        uint32_t stamp_us;
        struct timespec t;
        uint8_t rx[PKT_MAX];
        ssize_t n;

        clock_gettime(CLOCK_MONOTONIC, &t);
        stamp_us = (uint32_t)(t.tv_sec * 1000000ULL + t.tv_nsec / 1000);

        /* link.sock datagrams: consumer READY heartbeats + HUD RF commands (nonblocking drain) */
        while ((n = recv(link_sock, rx, sizeof rx, MSG_DONTWAIT)) > 0) {
            struct mlm_hdr *hdr = (struct mlm_hdr *)rx;

            if ((size_t)n < sizeof *hdr || hdr->magic != MLM_MAGIC) {
                continue;
            }

            /* HUD -> air config: record the desired state; the periodic sender below pushes it. */
            if (hdr->type == MLM_T_RFCMD
                && (size_t)n >= sizeof *hdr + sizeof(struct mlm_rfcmd)) {
                struct mlm_rfcmd *rc = (struct mlm_rfcmd *)(rx + sizeof *hdr);
                if (rc->cmd == MLM_RF_SET_STANDBY) {
                    int arm = rc->arg ? 1 : 0;
                    if (arm != g_standby_arm) {
                        printf(TAG " rfcmd: standby arm=%d\n", arm);
                        fflush(stdout);
                    }
                    g_standby_arm = arm;
                } else if (rc->cmd == MLM_RF_SET_POWER) {
                    int dbm = map_power_dbm(rc->arg);
                    if (dbm < 0) {
                        fprintf(stderr, TAG " rfcmd: ignoring bad power %u mW\n", rc->arg);
                    } else {
                        if (dbm != g_power_dbm) {
                            printf(TAG " rfcmd: power %u mW (0x%02x dBm)\n", rc->arg, dbm);
                            fflush(stdout);
                        }
                        g_power_dbm = dbm;
                    }
                }
                continue;
            }

            if (hdr->type != MLM_T_READY
                || (size_t)n < sizeof *hdr + sizeof(struct mlm_ready)) {
                continue;
            }

            if (!g_ready) {
                g_ready = 1;
                printf(TAG " consumer READY\n");
                fflush(stdout);
            }

            g_last_ready_ms = now;
            if (((struct mlm_ready *)(rx + sizeof *hdr))->frames_seen
                && g_params_acked && !g_video_confirmed) {
                g_video_confirmed = 1;
                printf(TAG " consumer confirms frames arriving; type1 poll stops\n");
                fflush(stdout);
            }
        }

        if (g_ready && now - g_last_ready_ms > READY_WINDOW_MS) {
            g_ready = 0;
            printf(TAG " consumer READY lost (heartbeat timeout)\n");
            fflush(stdout);
        }

        /* :20001 hello until the 3-way is done (vendor goes quiet after) */
        if (!g_hs_done && now - last_hello >= HELLO_IVL_MS) {
            sendto(hello_sock, hello, sizeof hello, 0,
                   (struct sockaddr *)&air_hello, sizeof air_hello);
            last_hello = now;
        }

        while ((n = recvfrom(hello_sock, rx, sizeof rx, MSG_DONTWAIT, NULL, NULL)) > 0) {
            if (rx[0] == 1) {               /* air's type-1 identity: echo the 2-byte ACK diff */
                uint8_t ack[PKT_MAX];

                memcpy(ack, rx, n);
                ack[0] = 0x02;
                if (n > 5) {
                    ack[5] = 0x00;
                }

                sendto(hello_sock, ack, n, 0, (struct sockaddr *)&air_hello, sizeof air_hello);
                if (!g_hs_done) {
                    g_hs_done = 1;
                    printf(TAG " :20001 3-way done (air identity %zd B, type2 ACK sent)\n", n);
                    fflush(stdout);
                }
            }
        }

        /* :10000 params request: READY-gated, vendor 2 s rate, quiet once the consumer
         * confirmed frames, grace after each type3 ACK before re-polling */
        if ((g_no_gate || g_ready) && g_hs_done && !g_video_confirmed
            && (!g_params_acked || now - g_last_ack_ms > ACK_GRACE_MS)
            && now - last_req >= PARAMS_IVL_MS) {
            memcpy(params_req + 8, &stamp_us, 4);
            sendto(params_sock, params_req, sizeof params_req, 0,
                   (struct sockaddr *)&air_params, sizeof air_params);
            last_req = now;

            if (g_verbose) {
                fprintf(stderr, TAG " tx MEDIA_PARAMS_REQUEST\n");
            }
        }

        /* drain :10000: params reply + telemetry/OSD; all of it counts as air liveness */
        while ((n = recvfrom(params_sock, rx, sizeof rx, MSG_DONTWAIT, NULL, NULL)) > 0) {
            uint32_t msg_type;

            g_last_telem_ms = now;
            if (g_air_lost) {
                g_air_lost = 0;
                link_event(MLM_LINK_SESSION_RESTART, "TX unit returned, re-handshaking");
                led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
                /* the periodic sender re-applies the standby-arm on its own once the air is back */
            }

            if ((size_t)n < 4) {
                continue;
            }

            memcpy(&msg_type, rx, 4);       /* LE u32 msg type, common to both families */
            if (msg_type == MP_REPLY) {     /* MEDIA_PARAMS reply -> type3 ACK, video starts */
                memcpy(params_ack + 8, &stamp_us, 4);
                sendto(params_sock, params_ack, sizeof params_ack, 0,
                       (struct sockaddr *)&air_params, sizeof air_params);
                g_last_ack_ms = now;

                if (!g_params_acked) {
                    g_params_acked = 1;
                    link_event(MLM_LINK_PARAMS_ACKED, "MEDIA_PARAMS acked, video should start");
                    led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);
                }
            } else if (msg_type == AIR_MSP) {
                mlm_pub(MLM_OSD_SOCK, MLM_T_MSP, rx, n);
            } else if (msg_type == AIR_STATUS_A || msg_type == AIR_STATUS_B) {
                mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_STATUS, rx, n);
            } else if (msg_type == AIR_STANDBY && n >= STANDBY_OFF_MODE + 4) {
                /* the air reports its live work-mode on every change; latch it for the HUD icon */
                uint32_t wm;
                memcpy(&wm, rx + STANDBY_OFF_MODE, 4);
                g_standby_state = (wm == STANDBY_MODE_ON);
                if (g_verbose) {
                    fprintf(stderr, TAG " standby work-mode=%u\n", wm);
                }

                /* Ack a standby entry so the air actually drops to its low-power fps - it gates that
                 * drop on this 0x1b and holds full fps until it arrives (see MP_STBACK). The air only
                 * emits 0x12 on a work-mode change, so one ack per standby-mode report matches stock. */
                if (wm == STANDBY_MODE_ON) {
                    uint8_t ack[STBACK_LEN];
                    memset(ack, 0, sizeof ack);
                    ack[0] = MP_STBACK;
                    memcpy(ack + 8, &stamp_us, 4);
                    sendto(params_sock, ack, sizeof ack, 0,
                           (struct sockaddr *)&air_params, sizeof air_params);
                    if (g_verbose) {
                        fprintf(stderr, TAG " tx StbAck (0x1b)\n");
                    }
                }
            }
        }

        /* air-loss watch (only meaningful once telemetry has flowed in STEADY) */
        if (g_steady && !g_air_lost && g_last_telem_ms
            && now - g_last_telem_ms > AIR_LOSS_MS) {
            g_air_lost = 1;
            g_hs_done = 0;                  /* re-arm the :20001 3-way for the next session */
            g_params_acked = 0;
            g_video_confirmed = 0;
            link_event(MLM_LINK_TX_LOST, "no :10000 traffic for 5 s");
            led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
        }

        /* Keep the air's TX power + standby-arm asserted while it is reachable. Both ride this one
         * SetTranParm (body[0] = TX power dBm, body[8] = u8StandbyModeEn). The air only enters standby
         * when the arm bit is set AND its own RC link is disarmed, and it re-evaluates on every frame,
         * so we re-send on a steady cadence - a single frame can race association or be dropped. Send
         * once either has been commanded by the HUD; the other falls back to a safe default (vendor
         * 100 mW / disarmed) so a fabricated byte never reaches the air. */
        if ((g_standby_arm >= 0 || g_power_dbm >= 0)
            && g_hs_done && !g_air_lost && now - last_stp >= STANDBY_IVL_MS) {
            uint8_t stp[STP_LEN];
            uint8_t power   = (g_power_dbm >= 0) ? (uint8_t) g_power_dbm : AIR_TX_DBM;
            uint8_t standby = (g_standby_arm >= 0) ? (uint8_t) g_standby_arm : 0;

            memset(stp, 0, sizeof stp);
            stp[0] = MP_SETTRANPARM;
            stp[16] = STP_BODY_LEN;
            memcpy(stp + 8, &stamp_us, 4);
            stp[STP_OFF_POWER]   = power;
            stp[STP_OFF_CONST]   = 0x04;
            stp[STP_OFF_STANDBY] = standby;
            sendto(params_sock, stp, sizeof stp, 0,
                   (struct sockaddr *)&air_params, sizeof air_params);
            last_stp = now;

            if (g_verbose) {
                fprintf(stderr, TAG " tx SetTranParm standby=%d power=0x%02x\n", standby, power);
            }
        }

        /* re-assert the LED ~1 Hz so a late-started/restarted ml-ledd reconverges;
         * also paints breathe-red from the first tick, before any link is up */
        if (now - last_led > LED_ASSERT_MS) {
            last_led = now;
            led_assert();
        }

        usleep(UDP_TICK_US);
    }

    close(hello_sock);
    close(params_sock);
    close(link_sock);
    unlink(MLM_LINK_SOCK);

    return NULL;
}

/* ASSOC: association bring-up, chip-bound (NOT air commands):
 *   6x ch0xff/port1 + 2x ch0xff/port0        link-management init pings
 *   1x ch05 (4-byte zero payload)             opens the ch05 chip-log channel (reader() drains it)
 *   14x ch03/op01, port 0x0e..0x00 (skip 5)   the association countdown -> chip "rpc sta=1"
 * 20 ms spacing throughout, matching the captured vendor cadence.
 */
static void assoc_bringup(void)
{
    uint8_t frame[32];
    static const uint8_t log_open[4] = { 0 };

    for (int i = 0; i < 6 && g_run; i++) {
        send_frame(frame, bb_build_frame(frame, BB_LINK, 0, 0, 0x01, 0, NULL, 0), "assoc");
        usleep(ASSOC_STEP_US);
    }

    for (int i = 0; i < 2 && g_run; i++) {
        send_frame(frame, bb_build_frame(frame, BB_LINK, 0, 0, 0x00, 0, NULL, 0), "assoc");
        usleep(ASSOC_STEP_US);
    }

    send_frame(frame, bb_build_frame(frame, BB_LOG, 0, 0, 0x00, 0, log_open, 4), "assoc-log");
    usleep(ASSOC_STEP_US);

    for (int port = 0x0e; port >= 0 && g_run; port--) {
        if (port == 0x05) {
            continue;
        }

        send_frame(frame, bb_build_frame(frame, BB_ASSOC, 0x01, 0, (uint8_t)port, 0, NULL, 0), "assoc-cd");
        usleep(ASSOC_STEP_US);
    }
}

/* main: the link FSM + steady bb-socket cadence. */

int main(int argc, char **argv)
{
    const char *node = DEV_NODE;
    pthread_t reader_th, udp_th;
    uint8_t frame[64], ff02[19];
    uint32_t seq_link = SEQ_START, seq_video = SEQ_START;
    unsigned long ticks = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            node = argv[++i];
        } else if (!strcmp(argv[i], "--no-gate")) {
            g_no_gate = 1;
        } else if (!strcmp(argv[i], "-v")) {
            g_verbose = 1;
        } else {
            fprintf(stderr, "usage: ml-linkd [-d /dev/artosyn_sdio] [--no-gate] [-v]\n");
            return 2;
        }
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    g_mlm = socket(AF_UNIX, SOCK_DGRAM, 0);

    /* WAIT_DEV: ml-rf-bringup must have run; retry until the node appears. Log on the first miss
     * and then every OPEN_RETRY_EVERY tries, so a link down for a while does not flood the log. */
    for (unsigned tries = 0; g_run && (g_fd = open(node, O_RDWR)) < 0; tries++) {
        if (tries == 0 || (tries % OPEN_RETRY_EVERY) == 0) {
            fprintf(stderr, TAG " open(%s): %s, retrying\n", node, strerror(errno));
        }

        sleep(1);
    }

    if (!g_run) {
        return 0;
    }

    printf(TAG " opened %s%s\n", node, g_no_gate ? " (gate disabled)" : "");

    pthread_create(&reader_th, NULL, reader, NULL);
    pthread_create(&udp_th, NULL, udp_thread, NULL);

    /* ASSOC: association bring-up (chip-bound), then build the reusable ff02 heartbeat frame */
    printf(TAG " ASSOC (chip bring-up)\n");
    assoc_bringup();
    bb_build_frame(ff02, BB_LINK, 0, 0, 0x02, 0, NULL, 0);

    /* SETTLE: ~2.5 s of Get1V1Info + ff02 ONLY (early GetTime poll wedges the air) */
    printf(TAG " SETTLE (Get1V1Info + ff02 only, ~2.5 s)\n");
    for (int i = 0; i < SETTLE_TICKS && g_run; i++) {
        uint8_t poll[19];

        bb_get(poll, GET_1V1INFO, seq_link++);
        send_frame(poll, 19, "get-1v1");
        send_frame(ff02, 19, "ff02");
        usleep(SETTLE_STEP_US);
    }

    /* OPEN: the two SET config frames (select channel + RX power), then goggle TX power + auto-adjust. */
    printf(TAG " OPEN (bb-socket setup + TX power)\n");
    for (int i = 0; i < OPEN_SEQ_N && g_run; i++) {
        const struct bb_frame *bf = &OPEN_SEQ[i];

        send_frame(frame, bb_build_frame(frame, bf->channel, bf->opcode, bf->slot, bf->port,
                                         bf->seq, bf->payload, bf->plen), "open");
        usleep(OPEN_STEP_US);
    }

    send_frame(frame, bb_set_power(frame, RF_TX, 0x17, seq_video++), "tx-power");          /* 23 dBm */
    usleep(20000);

    send_frame(frame, bb_set_power_auto(frame, 1, seq_video++), "tx-power-auto");
    usleep(20000);

    /* STEADY: vendor cadence forever (GetTime ~24 Hz, Get1V1Info ~6 Hz, ff02 ~3.4 Hz) */
    g_steady = 1;
    link_event(MLM_LINK_ASSOCIATED, "bring-up done, steady cadence");
    while (g_run) {
        uint8_t poll[19];

        bb_get(poll, GET_TIME, seq_video++);
        send_frame(poll, 19, "get-time");

        if (ticks % POLL_LINK_EVERY == 0) {
            bb_get(poll, GET_1V1INFO, seq_link++);
            send_frame(poll, 19, "get-1v1");
            bb_get(poll, GET_DISTANCE, seq_link++);
            send_frame(poll, 19, "get-distance");
        }

        if (ticks % FF02_EVERY == 0) {
            send_frame(ff02, 19, "ff02");
        }

        /* Publish the local baseband link metrics for the HUD's System OSD (~1 Hz). Channel is a
         * config fact we always know; SNR/distance go stale on air loss, so blank them then and let
         * the HUD dim the whole air-unit side off its own connection state. */
        if (ticks % LINKINFO_EVERY == 0) {
            struct mlm_linkinfo info = {
                .channel = chnidx_to_display(OPEN_CHNIDX),
                .snr_db = g_air_lost ? MLM_LINKINFO_NONE : g_snr_db,
                .distance_m = g_air_lost ? MLM_LINKINFO_NONE : g_distance_m,
                .flags = (!g_air_lost && g_standby_state) ? MLM_LINKINFO_F_STANDBY : 0,
            };

            mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_LINKINFO, &info, sizeof info);
        }

        usleep(STEADY_STEP_US);
        if (++ticks % ALIVE_EVERY == 0) {
            printf(TAG " alive hs=%d ready=%d acked=%d video=%d air_lost=%d\n",
                   g_hs_done, g_ready, g_params_acked, g_video_confirmed, g_air_lost);
            fflush(stdout);
        }
    }

    g_run = 0;
    pthread_join(reader_th, NULL);
    pthread_join(udp_th, NULL);
    close(g_fd);

    if (g_mlm >= 0) {
        close(g_mlm);
    }

    printf(TAG " clean stop\n");
    return 0;
}
