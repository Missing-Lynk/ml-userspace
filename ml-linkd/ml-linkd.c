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
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../ml-shared/mlm.h"
#include "bb-cmd.h"
#include "mp-cmd.h"

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
#define BIND_RETRY_MS    1000             /* unbound-socket rebind cadence */
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
#define POLL_LINK_EVERY  4                /* Get1V1Info every Nth STEADY tick (~6 Hz) */
#define FF02_EVERY       7                /* ff02 every Nth STEADY tick (~3.4 Hz) */
#define LINKINFO_EVERY   24               /* publish MLM_T_LINKINFO every Nth STEADY tick (~1 Hz) */
#define SEQ_START        0x15             /* initial poll sequence number */

/* The baseband channel index we tune the local RX to at OPEN (SET_CHNIDX). Single source of truth:
 * used both in OPEN_SEQ and when publishing the channel for the HUD's System OSD. */
#define OPEN_CHNIDX      5

/* Baseband GET reply field offsets. Replies arrive on the request channel BB_GET (0x01), port =
 * selector. Get1V1Info (0x73) carries a linear SNR at +0x06 (dB = 10*log10(raw/36); zero until a
 * video link is up) and the distance in metres at +0x08. */
#define REPLY_CH         0x01
#define V1V1_OFF_SNR     0x06

/* Get1V1Info +0x25 carries the chip's current working channel. The vendor's scan sweep clobbers this
 * byte to 0xff before each read and then refuses any reply whose +0x25 != the channel it just
 * selected (AR_MID_RX_WIRELESS_GET_SCAN_RESULT_IMPL, ar_lowdelay-full.txt:58393-58440). That gate
 * only makes sense because Get1V1Info keeps returning the PREVIOUS channel's cached snapshot for a
 * while after SelectChn - without it a sweep reads the active link's SNR on every channel.
 * The +0x25 = working-channel binding is inferred from that usage, not confirmed by a symbol. */
#define V1V1_OFF_CHAN    0x25

/* Get1V1Info +0x08 is the OSD distance: signed i32, metres, negative = no fix. The vendor's
 * AR_MID_GET_REALTIME_SYS_INFO (ar_lowdelay-full.txt:59482-59486) reads exactly this field,
 * clamps < 0 to 0 and truncates to u16; no scaling. GetDistanceResult (0x05) is NOT the source:
 * it has no call sites in the vendor stack and its u32 at +0 ticks at 1 kHz (a ms counter). */
#define V1V1_OFF_DIST    0x08

/* Get1V1Info +0x0c: measured PHY link throughput (rx_throughput), u32 LE kbps. This is link
 * capacity, NOT the encoder bitrate (HW-confirmed: constant ~20.9 Mbps while the encoded rate and
 * the netdev rate vary). The vendor OSD draws this same field as "Bitrate", dividing it by a
 * hardcoded 6 in standby - we publish it raw. Reads 0 until a video link is up, so it rides the
 * same raw>0 gate as SNR/distance. Offset HW-verified against a live 0x73 reply (be 51 = 20926). */
#define V1V1_OFF_THROUGHPUT 0x0c

/* Per-channel SNR sweep timings, from the vendor sweep (ar_lowdelay-full.txt:58393-58440). Per
 * channel it polls Get1V1Info every 10 ms until the reply's working channel matches (500 ms budget,
 * "timeout 500ms" in its own log), then settles 50 ms and takes one sample. A locked channel matches
 * almost at once, so the common cost is ~1 poll + the dwell; only dead channels burn the full
 * budget. Worst case over the 16 Race channels is ~8 s, which EXCEEDS AIR_LOSS_MS - the sweep blocks
 * the STEADY cadence, so a fully dead band re-runs the handshake afterwards. */
#define SWEEP_DWELL_MS   50               /* settle after the channel gate matches, before the sample */
#define SWEEP_LOCK_MS    500              /* budget for the reply's working channel to match */
#define SWEEP_GATE_US    10000            /* spacing between gate polls */
#define SWEEP_REPLY_MS   40               /* max wait for a single Get1V1Info reply */
#define SWEEP_SCAN_MS    300              /* max wait for the GetScanResult that seeds the table */

/* A first sample below this is taken again once. The value is the vendor's bucket-2 floor (raw 160,
 * ~6.5 dB): the level below which a channel has no usable link, so a healthy channel never pays the
 * extra reply. */
#define SWEEP_RETRY_RAW  160

/* GetScanResult raw reply layout (struct-of-arrays). count at [0]; freq[] u32 LE kHz at [4] (the full
 * channel table, up to 19); the valid-channel bitmap is the trailing u32 of the payload. Captured in
 * race mode (16-channel); the normal-mode payload layout is unverified.
 *
 * SCAN_OFF_SIGNAL is the reply's rssi[] (s32 LE, packed over the valid channels in table order):
 * ambient energy, not link quality. Kept as reference only - HW showed it pinned near the noise floor
 * (-104..-108 dBm) and unmoved by a locked air unit on the channel, so the tiles use the Get1V1Info
 * sweep instead. The array at [80] is always zero even with a live link. */
#define SCAN_OFF_COUNT   0
#define SCAN_OFF_FREQ    4
#define SCAN_OFF_SIGNAL  144
#define SCAN_FREQ_MHZ_MIN 5000        /* freq sanity gate (reject crossed-transaction junk) */
#define SCAN_FREQ_MHZ_MAX 6100

/* :10000 message types + goggle->air builders live in mp-cmd.h. These are the receive-parse details
 * for the air's SetStandyMode (0x12) work-mode word. */
#define STANDBY_OFF_MODE 20               /* work-mode u32 at datagram offset 20 (body offset 0) */
#define STANDBY_MODE_ON  1                /* work-mode 1 = standby (0 = normal, 2 = airscrew/armed) */

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

static volatile int g_run = 1;
static int g_fd = -1;                       /* /dev/artosyn_sdio */
static int g_verbose;
static int g_no_gate;
static int g_scan_probe;                    /* --scan-probe: hexdump the raw GetScanResult + Get1V1Info replies */

/* handshake/link state shared between the FSM tick (main) and the UDP thread.
 * All plain ints/timestamps, single writer per field, so volatile is enough. */
static volatile int g_steady;               /* FSM reached STEADY */
static volatile int g_hs_done;              /* :20001 3-way done */
static volatile int g_params_acked;         /* type3 ACK sent this session */
static volatile int g_air_lost;             /* >5 s :10000 silence flagged */
static volatile int g_ready;                /* consumer READY (heartbeat fresh) */
static volatile int g_video_confirmed;      /* consumer reported frames_seen after our ACK */

/* Video-stall watch. The air-loss watch only sees :10000 silence, but an air-side link bounce
 * (chip LinkDown/LinkUp) tears down and rebuilds the air's video path WITHOUT ever silencing its
 * telemetry - the session then sits "acked" forever with zero video (HW post-mortem 2026-07-19).
 * The consumer's READY heartbeat carries its raw :10001 datagram counter (mlm_ready.rx_pkts); when
 * that counter stops advancing mid-session, the media handshake is re-run. All owned by udp_thread. */
static uint32_t g_rx_pkts_last;             /* last heartbeat's counter value */
static long g_rx_pkts_change_ms;            /* when it last advanced */
static int g_rx_counting;                   /* counter has advanced at least once this session */
#define MEDIA_STALL_MS 6000                 /* 3 heartbeats of a frozen counter = video is dead */
static volatile long g_last_telem_ms;       /* last :10000 RX */
static volatile long g_last_ready_ms;       /* last MLM_T_READY heartbeat */
static volatile long g_last_ack_ms;         /* last type3 ACK sent */

/* Local baseband link metrics, parsed from the GET replies in the reader thread and published for
 * the HUD's System OSD. Single writer (reader), single reader (main publish); MLM_LINKINFO_NONE
 * until a reply lands. */
static volatile int g_snr_db = MLM_LINKINFO_NONE;
static volatile int g_distance_m = MLM_LINKINFO_NONE;
static volatile int g_throughput_kbps;      /* measured PHY link throughput (Get1V1Info +0x0c); 0 = no link */

/* Raw Get1V1Info sampling for the channel sweep. g_snr_db deliberately holds the last GOOD value
 * (raw 0 replies do not overwrite it, so the OSD does not flicker), which makes it unusable for the
 * sweep: a dead channel would silently inherit the previous channel's SNR. g_v1v1_seq increments on
 * EVERY reply including raw 0, so the sweep can wait for a fresh sample and tell "no lock" (raw 0)
 * apart from "no reply at all". Single writer (reader), single reader (main sweep). */
static volatile unsigned g_v1v1_seq;        /* bumped on every Get1V1Info reply */
static volatile int g_v1v1_raw;             /* raw linear SNR of the last reply (0 = no lock) */
static volatile int g_v1v1_chan = -1;       /* working channel (+0x25) of the last reply, -1 = absent */

/* Last Get1V1Info reply payload, for --scan-probe. The +0x25 working-channel offset the sweep gates
 * on is INFERRED, so the sweep dumps one reply per run: on a live link the byte holding the current
 * channel index must equal the tuned channel, which locates it without trusting the inference. */
#define V1V1_PAY_MAX     64
static uint8_t g_v1v1_pay[V1V1_PAY_MAX];
static volatile int g_v1v1_plen;

/* The parsed channel table, owned by the main STEADY thread. The reader parses a GetScanResult reply
 * into it and sets g_scan_ready; the sweep then fills in snr_db per channel and publishes it. Not
 * published from the reader: the SNR only exists after the main thread has visited each channel. */
static struct mlm_scan g_scan;
static volatile int g_scan_ready;           /* a GetScanResult reply has landed in g_scan */

/* The current band's valid-channel mask (the config JSON's chan_valid_bmp, echoed by the chip in
 * every GetScanResult reply). Its own global rather than a read of g_scan: udp_thread tests it to
 * reject off-band selects, and g_scan belongs to the main thread. 0 = not read back yet, in which
 * case the band is unknown and selects are allowed through rather than all rejected. */
static volatile uint32_t g_valid_bmp;

/* Air-unit RF config the HUD has commanded (MLM_T_RFCMD on link.sock). -1 = never commanded, so
 * nothing is pushed to the air until the HUD asserts it; the HUD re-asserts on every link-up edge.
 * ml-linkd re-sends the SetTranParm on a steady cadence while linked, so no per-change latch is
 * needed - a toggle or a returning air unit is picked up by the next tick. */
static volatile int g_standby_arm = -1;     /* 0/1 = HUD-commanded u8StandbyModeEn, -1 = unknown */
static volatile int g_power_dbm = -1;        /* HUD-commanded TX power (dBm) for SetTranParm body[0], -1 = unset */
static volatile int g_bitrate_mbps;         /* HUD-commanded bitrate (Mbps) for SetLdCfg bitrate_q, 0 = unset */
static volatile int g_standby_state;        /* air's LIVE work-mode from SetStandyMode (0x12): 1 = in standby */

/* Channel select: the HUD queues a retune here (udp_thread), the bb-socket owner (main STEADY loop)
 * issues it once and clears it back to -1. One-shot, NOT a latch: re-issuing SelectChn on a cadence
 * would retune the RX continuously. The select must come from the bb-socket TX thread only; issuing
 * it from udp_thread would race the steady poll and get lost (RE of the stock picker). */
static volatile int g_pending_chnidx = -1;  /* HUD-requested channel table index (0..18), -1 = none */
static volatile int g_cur_chnidx = OPEN_CHNIDX; /* channel the local RX is tuned to; tracks SelectChn for the OSD */
static volatile int g_pending_scan;         /* HUD requested a one-shot scan (MLM_RF_SCAN); STEADY fires it */

/* Camera/scale state the HUD has commanded (MLM_RF_SET_CAMERA / MLM_RF_SET_SCALE). Owned entirely
 * by udp_thread: the commands arrive on link.sock and the :10000 datagrams leave on params_sock,
 * both on that thread. Each commanded selector is marked pending and sent as one live SetCameraInfo
 * (0x0C) once the session is up; the pending set is re-armed from the commanded set on every
 * (re)association, because the SetLdCfg the air latches there resets its ISP to the association
 * defaults. Scale (zoom + aspect) rides its own SetScaleMode (0x15) with the same latching. */
static struct mp_camera g_cam = MP_CAMERA_DEFAULTS;  /* full ISP state; defaults = air cold boot */
static uint32_t g_cam_pending;              /* bit per MLM_CAM_* selector awaiting a send */
static uint32_t g_cam_commanded;            /* bit per selector the HUD has commanded (re-assert set) */
static int g_scale_aspect = -1;             /* 0 = 16:9, 1 = 4:3; -1 = never commanded */
static int g_scale_zoom_pct = 100;          /* zoom factor in percent (100 or 70) */
static int g_scale_pending;

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
        if (g_standby_state) {
            /* link up, air in standby */
            led_cmd(MLM_LED_BREATHE, 0xff, 0x50, 0x00, LED_BREATHE_MS);
        } else {
            /* link up, video flowing */
            led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);
        }
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

/* Dump a raw bb-socket frame (header + payload + trailer) as offset/hex/ascii rows to stdout, so a
 * bench capture can confirm an un-RE'd reply envelope byte-for-byte. Only reached under --scan-probe. */
static void hexdump_frame(const char *what, const uint8_t *frame, int n)
{
    printf(TAG " %s (%d B):\n", what, n);
    for (int off = 0; off < n; off += 16) {
        printf("  %04x: ", off);
        for (int k = 0; k < 16; k++) {
            if (off + k < n) {
                printf("%02x ", frame[off + k]);
            } else {
                printf("   ");
            }
        }

        printf(" |");
        for (int k = 0; k < 16 && off + k < n; k++) {
            uint8_t c = frame[off + k];

            putchar(c >= 32 && c < 127 ? c : '.');
        }

        printf("|\n");
    }

    fflush(stdout);
}

/* Parse a raw GetScanResult reply (struct-of-arrays, HW-decoded) into g_scan for the sweep. The chip
 * returns the full channel table (freq[]) regardless of Standard/Race mode; the trailing bitmap is
 * the current mode's valid mask. snr_db is left unset here: the reply carries no per-channel SNR
 * (its rssi[] at SCAN_OFF_SIGNAL is ambient energy that did not move even with a locked air unit on
 * the channel, so it cannot drive the tiles), and scan_sweep() measures it per channel instead. */
static void parse_scan(const uint8_t *pay, int plen)
{
    struct mlm_scan scan;
    uint32_t bmp;
    int count;

    if (plen < SCAN_OFF_FREQ + 4) {
        return;
    }

    count = pay[SCAN_OFF_COUNT];
    if (count > MLM_SCAN_MAX_CH) {
        count = MLM_SCAN_MAX_CH;
    }

    /* the valid-channel bitmap is the trailing u32 of the payload */
    bmp = (uint32_t)pay[plen - 4] | ((uint32_t)pay[plen - 3] << 8)
        | ((uint32_t)pay[plen - 2] << 16) | ((uint32_t)pay[plen - 1] << 24);

    memset(&scan, 0, sizeof scan);
    scan.valid_bmp = bmp;
    g_valid_bmp = bmp;
    scan.count = (uint8_t)count;
    scan.active_idx = (uint8_t)g_cur_chnidx;

    for (int i = 0; i < count; i++) {
        int foff = SCAN_OFF_FREQ + i * 4;
        uint32_t fkhz;
        uint16_t mhz;
        int valid;

        if (foff + 4 > plen) {
            break;
        }

        fkhz = (uint32_t)pay[foff] | ((uint32_t)pay[foff + 1] << 8)
             | ((uint32_t)pay[foff + 2] << 16) | ((uint32_t)pay[foff + 3] << 24);
        mhz = (uint16_t)(fkhz / 1000);
        valid = (bmp >> i) & 1;

        scan.chan[i].freq_mhz = (mhz >= SCAN_FREQ_MHZ_MIN && mhz <= SCAN_FREQ_MHZ_MAX) ? mhz : 0;
        scan.chan[i].index = (uint8_t)i;
        scan.chan[i].valid = (uint8_t)valid;
        scan.chan[i].snr_db = MLM_SCAN_SIGNAL_NONE;
        scan.chan[i].snr_raw = MLM_SCAN_RAW_NONE;
    }

    g_scan = scan;
    g_scan_ready = 1;
    if (g_verbose) {
        printf(TAG " scan: count=%d valid_bmp=0x%08x\n", count, bmp);
        fflush(stdout);
    }
}

/* @return the lowest channel index set in @bmp, or -1 if none is. */
static int first_valid_idx(uint32_t bmp)
{
    for (int i = 0; i < MLM_SCAN_MAX_CH; i++) {
        if ((bmp >> i) & 1) {
            return i;
        }
    }

    return -1;
}

/* Retune the RX onto the band if OPEN left it off it. OPEN_CHNIDX is a Race index, so a Normal-band
 * chip (chan_valid_bmp = 0x7, indices 0..2) comes up tuned outside its own valid set: the channel
 * grid then shows 3 tiles with the active one among none of them, and the OSD reports a channel the
 * band does not contain. The band is only known from the chip (the config JSON's chan_valid_bmp,
 * echoed in the GetScanResult reply), so it is read here rather than assumed.
 *
 * Runs once at the end of OPEN, before the air is associated: retuning here costs nothing, whereas
 * doing it later would drop a working link. Cheap - the raw GetScanResult does not sweep.
 */
static void tune_into_band(uint8_t *frame, uint32_t *seq_link)
{
    uint8_t poll[19];
    long t0;
    int first;

    g_scan_ready = 0;
    bb_get(poll, GET_SCAN_RESULT, (*seq_link)++);
    send_frame(poll, 19, "get-scan");
    for (t0 = now_ms(); !g_scan_ready; ) {
        if (now_ms() - t0 >= SWEEP_SCAN_MS) {
            printf(TAG " band: no GetScanResult reply, staying on ch%d\n", g_cur_chnidx);
            fflush(stdout);
            return;
        }

        usleep(5000);
    }

    if ((g_scan.valid_bmp >> g_cur_chnidx) & 1) {
        return;   /* already on a channel this band allows */
    }

    first = first_valid_idx(g_scan.valid_bmp);
    if (first < 0) {
        printf(TAG " band: valid_bmp=0x%08x has no channel, staying on ch%d\n",
               g_scan.valid_bmp, g_cur_chnidx);
        fflush(stdout);
        return;
    }

    printf(TAG " band: ch%d is outside valid_bmp=0x%08x, retuning to ch%d\n",
           g_cur_chnidx, g_scan.valid_bmp, first);
    fflush(stdout);

    send_frame(frame, bb_select_channel(frame, (uint8_t)first, (*seq_link)++), "band-retune");
    g_cur_chnidx = first;
    usleep(OPEN_STEP_US);
}

/* Send one Get1V1Info and wait for its reply. @return 1 on a fresh reply (g_v1v1_raw / g_v1v1_chan
 * updated), 0 on timeout. g_snr_db cannot serve here: it holds the last GOOD value by design, so it
 * cannot tell a dead channel from a missing reply. */
static int v1v1_poll(uint32_t *seq_link)
{
    uint8_t poll[19];
    unsigned seq0 = g_v1v1_seq;
    long t0;

    bb_get(poll, GET_1V1INFO, (*seq_link)++);
    send_frame(poll, 19, "sweep-1v1");

    for (t0 = now_ms(); g_v1v1_seq == seq0; ) {
        if (now_ms() - t0 >= SWEEP_REPLY_MS) {
            return 0;
        }

        usleep(2000);
    }

    return 1;
}

/* Measure one swept channel's raw SNR, gating on the reply's working channel (+0x25) so the sample
 * belongs to @p idx and not to the channel we just left: poll every SWEEP_GATE_US until the reply's
 * channel matches (SWEEP_LOCK_MS budget), then settle SWEEP_DWELL_MS and sample. Without the gate the
 * chip's cached snapshot makes every channel read as the active link's SNR, saturating the top bucket
 * everywhere. The gate does not fully close the race, so a low sample is re-read once (below).
 *
 * @return the raw linear SNR (>0), MLM_SCAN_RAW_NOLOCK when the chip never reports the channel
 * (the vendor's strength-0 timeout case), or MLM_SCAN_RAW_NONE when Get1V1Info stops replying. */
static int sweep_measure(uint32_t *seq_link, int idx)
{
    long t0 = now_ms();

    while (now_ms() - t0 < SWEEP_LOCK_MS) {
        if (!v1v1_poll(seq_link)) {
            return MLM_SCAN_RAW_NONE;
        }

        if (g_v1v1_chan == idx) {
            int raw;

            usleep(SWEEP_DWELL_MS * 1000);
            if (!v1v1_poll(seq_link)) {
                return MLM_SCAN_RAW_NONE;
            }

            raw = g_v1v1_raw;

            /* Re-read an implausibly low sample. The chip reports the new working channel at +0x25
             * before it has finished recomputing the SNR, so the gate does not fully close the race
             * and a single post-dwell read intermittently returns ~0 on a healthy channel (HW: ch9
             * read 5730, 5342, then 21 over three sweeps; the vendor, reading once after a bare
             * 50 ms, hits this ~5x more often and paints those tiles red). Keep the better of the
             * two: a genuinely dead channel reads ~0 twice, so this cannot mask one. */
            if (raw < SWEEP_RETRY_RAW) {
                usleep(SWEEP_DWELL_MS * 1000);
                if (v1v1_poll(seq_link) && g_v1v1_raw > raw) {
                    raw = g_v1v1_raw;
                }
            }

            return raw;
        }

        usleep(SWEEP_GATE_US);
    }

    return MLM_SCAN_RAW_NOLOCK;
}

/* One full channel scan for the HUD: seed the table from GetScanResult, then measure each valid
 * channel's SNR and publish it as MLM_T_SCAN. The raw reply carries no per-channel SNR, so the only
 * source is Get1V1Info read while tuned to each channel - the vendor does the same (FUN_0045c108).
 * The air unit follows the retune over the chip-to-chip management link, so each reading is the link
 * SNR actually achievable on that channel; with no air unit up, every channel reads no-lock.
 *
 * Visits the current mode's valid channels starting at the active one and wrapping around, so the
 * active channel is measured while its link is still up and is the last one left tuned. Video is
 * interrupted for the length of the sweep and resumes when the active channel is restored, so this
 * must stay a one-shot on an explicit HUD request - never a cadence. Runs on the main STEADY thread,
 * the only bb-socket TX owner; issuing these selects from another thread would race the poll. */
static void scan_sweep(uint8_t *frame, uint32_t *seq_link)
{
    uint8_t poll[19];
    int order[MLM_SCAN_MAX_CH];
    int n = 0;
    int restore = g_cur_chnidx;
    long t0;

    /* seed freq[] + the current mode's valid bitmap */
    g_scan_ready = 0;
    bb_get(poll, GET_SCAN_RESULT, (*seq_link)++);
    send_frame(poll, 19, "get-scan");
    for (t0 = now_ms(); !g_scan_ready; ) {
        if (now_ms() - t0 >= SWEEP_SCAN_MS) {
            printf(TAG " scan: no GetScanResult reply, sweep skipped\n");
            fflush(stdout);
            return;
        }

        usleep(5000);
    }

    /* Locate the working-channel byte on the active channel before retuning anywhere: any offset
     * holding restore is a candidate for the gate, and V1V1_OFF_CHAN must be among them. On a dead
     * link the whole struct reads zero and this proves nothing, so the link state is printed too. */
    if (g_scan_probe && v1v1_poll(seq_link)) {
        printf(TAG " scan: 1v1 on active ch%d (hs=%d air_lost=%d) plen=%d raw=%d\n", restore, g_hs_done,
               g_air_lost, g_v1v1_plen, g_v1v1_raw);
        for (int i = 0; i < g_v1v1_plen; i += 16) {
            printf(TAG " scan: 1v1[%02x]", i);
            for (int k = 0; k < 16 && i + k < g_v1v1_plen; k++) {
                printf(" %02x", g_v1v1_pay[i + k]);
            }

            printf("\n");
        }

        printf(TAG " scan: offsets holding the active channel (%d):", restore);
        for (int i = 0; i < g_v1v1_plen; i++) {
            if (g_v1v1_pay[i] == (uint8_t)restore) {
                printf(" +0x%02x", i);
            }
        }

        printf("   (gate uses +0x%02x)\n", V1V1_OFF_CHAN);
        fflush(stdout);
    }

    /* visit order: the valid channels, rotated to start at the active one */
    for (int k = 0; k < g_scan.count; k++) {
        int i = (restore + k) % g_scan.count;

        if (g_scan.chan[i].valid) {
            order[n++] = i;
        }
    }

    /* Without an air unit every channel would gate-timeout to NOLOCK anyway, at SWEEP_LOCK_MS each:
     * over the 16 Race channels that is ~8 s of blocked STEADY cadence, past AIR_LOSS_MS, which
     * would fake an air loss and tear down the handshake. Publish the same all-NOLOCK answer at
     * once instead - the table and bitmap are already seeded, only the readings are missing. */
    if (!g_hs_done || g_air_lost) {
        for (int k = 0; k < n; k++) {
            g_scan.chan[order[k]].snr_raw = MLM_SCAN_RAW_NOLOCK;
        }

        g_scan.active_idx = (uint8_t)restore;
        mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_SCAN, &g_scan, sizeof g_scan);
        printf(TAG " scan: no air unit (hs=%d air_lost=%d), %d channels reported unmeasured\n",
               g_hs_done, g_air_lost, n);
        fflush(stdout);
        return;
    }

    /* Force MCS 0 for the sweep, exactly as the vendor does (SetMcs(0) at ar_lowdelay-full.txt:58363,
     * SetMcsMode(1) restoring auto at :58503). This is not cosmetic: the vendor's buckets top out at
     * raw 1100 (~15 dB), yet under auto MCS the chip reports 4000-11500 on any healthy link, so every
     * tile saturates. The scan is the only place the vendor pins the rate, which is the one regime
     * difference that can explain the scale gap - plausibly because an SNR estimate read off the
     * coarse MCS-0 constellation cannot resolve high SNR and saturates into the bucketed range. */
    send_frame(frame, bb_set_mcs_mode(frame, MCS_MODE_MANUAL, (*seq_link)++), "sweep-mcs-manual");
    send_frame(frame, bb_set_mcs_value(frame, 0, (*seq_link)++), "sweep-mcs-0");

    for (int k = 0; k < n; k++) {
        int idx = order[k];
        int raw;

        /* the active channel is already tuned on the first visit */
        if (idx != g_cur_chnidx) {
            send_frame(frame, bb_select_channel(frame, (uint8_t)idx, (*seq_link)++), "sweep-sel");
            g_cur_chnidx = idx;
        }

        raw = sweep_measure(seq_link, idx);
        g_scan.chan[idx].snr_raw = (int16_t)raw;
        g_scan.chan[idx].snr_db = raw > 0
            ? (int16_t)lroundf(10.0f * log10f((float)raw / 36.0f))
            : MLM_SCAN_SIGNAL_NONE;

        /* One line per channel. chan is the reply's +0x25 working channel: it must equal the swept
         * index, and is the check that the inferred +0x25 binding is real - if it never matches,
         * every channel times out to NOLOCK and the gate is keyed on the wrong byte. */
        printf(TAG " scan: ch%-2d %u MHz raw=%-6d chan=%-3d snr=%d\n", idx, g_scan.chan[idx].freq_mhz,
               raw, g_v1v1_chan, g_scan.chan[idx].snr_db);
    }

    fflush(stdout);

    /* restore the active channel and hand the rate back to the chip: the link and video resume here.
     * Auto MCS must be restored on every exit or the link stays pinned to MCS 0 after a scan. */
    if (g_cur_chnidx != restore) {
        send_frame(frame, bb_select_channel(frame, (uint8_t)restore, (*seq_link)++), "sweep-restore");
        g_cur_chnidx = restore;
    }

    send_frame(frame, bb_set_mcs_mode(frame, MCS_MODE_AUTO, (*seq_link)++), "sweep-mcs-auto");

    g_scan.active_idx = (uint8_t)restore;
    mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_SCAN, &g_scan, sizeof g_scan);
    printf(TAG " scan: swept %d channels, active %d restored\n", n, restore);
    fflush(stdout);
}

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

                /* publish every sample (raw 0 included) for the sweep before the last-good filter,
                 * with the working channel the sample belongs to so the sweep can reject stale ones */
                int n = plen > V1V1_PAY_MAX ? V1V1_PAY_MAX : plen;

                g_v1v1_raw = (int)raw;
                g_v1v1_chan = plen > V1V1_OFF_CHAN ? (int)payload[V1V1_OFF_CHAN] : -1;
                memcpy(g_v1v1_pay, payload, (size_t)n);
                g_v1v1_plen = n;
                g_v1v1_seq++;

                if (raw > 0) {
                    g_snr_db = (int)lroundf(10.0f * log10f((float)raw / 36.0f));

                    /* distance rides the same populated reply: i32 at +0x08, metres, vendor-style
                     * clamp of negative (no fix) to 0. Empty inter-poll replies keep the last good
                     * value, same as SNR. */
                    if (plen >= V1V1_OFF_DIST + 4) {
                        int32_t dist = (int32_t)((uint32_t)payload[V1V1_OFF_DIST]
                                     | ((uint32_t)payload[V1V1_OFF_DIST + 1] << 8)
                                     | ((uint32_t)payload[V1V1_OFF_DIST + 2] << 16)
                                     | ((uint32_t)payload[V1V1_OFF_DIST + 3] << 24));

                        g_distance_m = dist < 0 ? 0 : (int)dist;
                    }

                    /* measured PHY link throughput rides the same populated reply: u32 kbps at
                     * +0x0c. Empty inter-poll replies keep the last good value, same as SNR. */
                    if (plen >= V1V1_OFF_THROUGHPUT + 4) {
                        g_throughput_kbps = (int)((uint32_t)payload[V1V1_OFF_THROUGHPUT]
                                     | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 1] << 8)
                                     | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 2] << 16)
                                     | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 3] << 24));
                    }
                }

                if (g_verbose) {
                    printf(TAG " 1v1info snr_raw=%u snr_db=%d dist_m=%d thr_kbps=%d\n", raw, g_snr_db,
                           g_distance_m, g_throughput_kbps);
                    fflush(stdout);
                }
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_SCAN_RESULT) {
                /* Seed the channel table for the sweep; --scan-probe also dumps the raw frame so the
                 * reply envelope can be re-checked on the bench (e.g. normal-mode layout). */
                if (g_scan_probe) {
                    hexdump_frame("get-scan reply", buf + i, 19 + plen);
                }

                parse_scan(payload, plen);
            }
            i += 18 + plen;
        }
    }

    return NULL;
}

/* UDP thread: :20001 hello/ack, :10000 handshake + telemetry, link.sock gate. */

/* Bind an AF_INET dgram socket to LOCAL_ADDR:port. */
static int bind_local(int sock, int port)
{
    struct sockaddr_in local;

    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    inet_pton(AF_INET, LOCAL_ADDR, &local.sin_addr);
    local.sin_port = htons(port);

    return bind(sock, (struct sockaddr *)&local, sizeof local);
}

static void *udp_thread(void *arg)
{
    int hello_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int params_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int link_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    int hello_bound = 0, params_bound = 0, link_bound = 0;
    unsigned bind_tries = 0;
    int one = 1;
    struct sockaddr_in air_hello, air_params;
    struct sockaddr_un link_un = { .sun_family = AF_UNIX };
    uint8_t hello[HELLO_LEN];
    long last_hello = 0, last_req = 0, last_led = 0, last_stp = 0, last_bind = -BIND_RETRY_MS;

    (void)arg;
    setsockopt(hello_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(params_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&air_hello, 0, sizeof air_hello);
    air_hello.sin_family = AF_INET;
    inet_pton(AF_INET, AIR_ADDR, &air_hello.sin_addr);
    air_params = air_hello;
    air_hello.sin_port = htons(HELLO_PORT);
    air_params.sin_port = htons(PARAMS_PORT);

    /* link.sock: the READY-gate seam; linkd binds, consumers send heartbeats */
    strncpy(link_un.sun_path, MLM_LINK_SOCK, sizeof link_un.sun_path - 1);

    memset(hello, 0, sizeof hello);

    while (g_run) {
        long now = now_ms();

        /* sdio0's 10.0.0.1 exists only after ml-rf-bringup configures the interface, so a bind can
         * fail at startup; retry until every socket is bound instead of running the whole session
         * on unbound sockets. The send sites below are gated on the bound flags: a sendto on an
         * unbound AF_INET socket would auto-bind it to an ephemeral port and make the real bind
         * fail forever. Log the first failed round and then every OPEN_RETRY_EVERY-th. */
        if ((!hello_bound || !params_bound || !link_bound) && now - last_bind >= BIND_RETRY_MS) {
            int log_this = (bind_tries == 0 || (bind_tries % OPEN_RETRY_EVERY) == 0);

            last_bind = now;
            bind_tries++;
            if (!hello_bound) {
                if (bind_local(hello_sock, HELLO_PORT) == 0) {
                    hello_bound = 1;
                    if (bind_tries > 1) {
                        printf(TAG " bind :%d ok\n", HELLO_PORT);
                        fflush(stdout);
                    }
                } else if (log_this) {
                    fprintf(stderr, TAG " bind :%d: %s (is sdio0 up as %s?), retrying\n",
                            HELLO_PORT, strerror(errno), LOCAL_ADDR);
                }
            }

            if (!params_bound) {
                if (bind_local(params_sock, PARAMS_PORT) == 0) {
                    params_bound = 1;
                    if (bind_tries > 1) {
                        printf(TAG " bind :%d ok\n", PARAMS_PORT);
                        fflush(stdout);
                    }
                } else if (log_this) {
                    fprintf(stderr, TAG " bind :%d: %s, retrying\n", PARAMS_PORT, strerror(errno));
                }
            }

            if (!link_bound) {
                mkdir(MLM_RUN_DIR, 0755);
                unlink(MLM_LINK_SOCK);
                if (bind(link_sock, (struct sockaddr *)&link_un, sizeof link_un) == 0) {
                    link_bound = 1;
                    if (bind_tries > 1) {
                        printf(TAG " bind %s ok\n", MLM_LINK_SOCK);
                        fflush(stdout);
                    }
                } else if (log_this) {
                    fprintf(stderr, TAG " bind %s: %s, retrying\n", MLM_LINK_SOCK, strerror(errno));
                }
            }
        }
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
                } else if (rc->cmd == MLM_RF_SET_BITRATE) {
                    /* Applied via SetLdCfg at association (the air latches it there), so a change
                     * takes effect on the next session; only the stock-menu levels are valid. */
                    if (rc->arg != 8 && rc->arg != 16 && rc->arg != 24) {
                        fprintf(stderr, TAG " rfcmd: ignoring bad bitrate %u Mbps\n", rc->arg);
                    } else {
                        if ((int)rc->arg != g_bitrate_mbps) {
                            printf(TAG " rfcmd: bitrate %u Mbps (next session)\n", rc->arg);
                            fflush(stdout);
                        }
                        g_bitrate_mbps = (int)rc->arg;
                    }
                } else if (rc->cmd == MLM_RF_SELECT_CHANNEL) {
                    /* Queue the retune for the bb-socket owner (main); issuing SelectChn from this
                     * thread would race the steady poll and get lost. Passed verbatim, no +1.
                     * The bound is the channel TABLE size: indices run 0..18, and Race's valid set is
                     * 3..18, so a 0..15 bound would reject CH16/17/18 (5420/5380/5340 MHz).
                     *
                     * The band is enforced here rather than left to the chip: the chip accepts an
                     * index outside its own chan_valid_bmp and simply tunes there (a Normal-band
                     * chip sat on Race ch5 until tune_into_band was added). The HUD re-asserts a
                     * saved channel on every link-up, so without this a channel saved under Race
                     * would retune a Normal-band chip straight back off its band. Rejecting leaves
                     * the band's first valid channel in place. */
                    if (rc->arg >= MLM_SCAN_MAX_CH) {
                        fprintf(stderr, TAG " rfcmd: ignoring bad channel index %u\n", rc->arg);
                    } else if (g_valid_bmp != 0 && !((g_valid_bmp >> rc->arg) & 1)) {
                        fprintf(stderr, TAG " rfcmd: channel %u outside band valid_bmp=0x%08x,"
                                " ignoring\n", rc->arg, g_valid_bmp);
                    } else {
                        printf(TAG " rfcmd: select channel %u\n", rc->arg);
                        fflush(stdout);
                        g_pending_chnidx = (int)rc->arg;
                    }
                } else if (rc->cmd == MLM_RF_SET_CAMERA) {
                    /* arg = (MLM_CAM_* selector << 16) | u16 value. Only the HW-captured selectors
                     * are accepted; values are bounds-checked (an ISP value is not an RF byte, but
                     * garbage still has no business on the wire). Applied live below. */
                    unsigned sel = rc->arg >> 16;
                    unsigned value = rc->arg & 0xffffu;
                    int ok = 1;

                    switch (sel) {
                        case MLM_CAM_EXPOSURE: {
                            /* 0 = auto; else manual with the exposure time in us (down to 1/10000,
                             * up to 1/30 - the stock page never leaves that range) */
                            ok = value == 0 || (value >= 100 && value <= 33333);
                            if (ok) {
                                g_cam.exposure_manual = value != 0;
                                if (value != 0) {
                                    g_cam.exposure_time = (uint16_t) value;
                                }
                            }
                        } break;

                        case MLM_CAM_SATURATION: {
                            ok = value <= 100;
                            if (ok) {
                                g_cam.saturation = (uint16_t) value;
                            }
                        } break;

                        case MLM_CAM_SHARPNESS: {
                            ok = value <= 100;
                            if (ok) {
                                g_cam.sharpness = (uint16_t) value;
                            }
                        } break;

                        case MLM_CAM_ROTATION: {
                            ok = value <= 1;
                            if (ok) {
                                g_cam.rotation = (uint16_t) value;
                            }
                        } break;

                        case MLM_CAM_NR3D: {
                            ok = value <= 1;
                            if (ok) {
                                g_cam.nr3d_en = (uint16_t) value;
                            }
                        } break;

                        default: {
                            ok = 0;
                        } break;
                    }

                    if (!ok) {
                        fprintf(stderr, TAG " rfcmd: ignoring bad camera sel=%u value=%u\n",
                                sel, value);
                    } else {
                        g_cam_commanded |= 1u << sel;
                        g_cam_pending |= 1u << sel;
                        if (g_verbose) {
                            printf(TAG " rfcmd: camera sel=%u value=%u\n", sel, value);
                            fflush(stdout);
                        }
                    }
                } else if (rc->cmd == MLM_RF_SET_SCALE) {
                    /* arg = (aspect << 16) | zoom_pct; only the two HW-captured zoom factors */
                    unsigned zoom_pct = rc->arg & 0xffffu;
                    if (zoom_pct != 100 && zoom_pct != 70) {
                        fprintf(stderr, TAG " rfcmd: ignoring bad zoom %u%%\n", zoom_pct);
                    } else {
                        g_scale_aspect = (rc->arg >> 16) & 1;
                        g_scale_zoom_pct = (int) zoom_pct;
                        g_scale_pending = 1;
                        if (g_verbose) {
                            printf(TAG " rfcmd: scale aspect=%d zoom=%d%%\n",
                                   g_scale_aspect, g_scale_zoom_pct);
                            fflush(stdout);
                        }
                    }
                } else if (rc->cmd == MLM_RF_SCAN) {
                    /* Queue a one-shot scan for the bb-socket owner (main); read-only, self-restores. */
                    g_pending_scan = 1;
                    if (g_verbose) {
                        printf(TAG " rfcmd: scan requested\n");
                        fflush(stdout);
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
            struct mlm_ready *ready = (struct mlm_ready *)(rx + sizeof *hdr);
            if (ready->frames_seen && g_params_acked && !g_video_confirmed) {
                g_video_confirmed = 1;
                printf(TAG " consumer confirms frames arriving\n");
                fflush(stdout);
            }

            /* feed the video-stall watch: note every advance of the consumer's datagram counter */
            if (ready->rx_pkts != g_rx_pkts_last) {
                g_rx_pkts_last = ready->rx_pkts;
                g_rx_pkts_change_ms = now;
                g_rx_counting = 1;
            }
        }

        /* Video-stall watch: the session is "acked" and video was confirmed flowing, but the
         * consumer's :10001 counter has frozen while the air's telemetry stays alive - the
         * air-side link bounce rebuilt its video path and now waits for a media handshake this
         * session already latched past. Re-arm the handshake: the ungated 2 s poll keeps running,
         * and the next 0x02 reply re-fires SetLdCfg + the type-3 ack (and the camera re-assert). */
        if (g_params_acked && g_video_confirmed && g_rx_counting && !g_air_lost
            && now - g_rx_pkts_change_ms > MEDIA_STALL_MS) {
            g_params_acked = 0;
            g_video_confirmed = 0;
            g_rx_counting = 0;
            link_event(MLM_LINK_SESSION_RESTART,
                       "video datagrams stalled, re-running the media handshake");
        }

        if (g_ready && now - g_last_ready_ms > READY_WINDOW_MS) {
            g_ready = 0;
            printf(TAG " consumer READY lost (heartbeat timeout)\n");
            fflush(stdout);
        }

        /* :20001 hello until the 3-way is done (vendor goes quiet after) */
        if (hello_bound && !g_hs_done && now - last_hello >= HELLO_IVL_MS) {
            sendto(hello_sock, hello, sizeof hello, MSG_DONTWAIT,
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

                sendto(hello_sock, ack, n, MSG_DONTWAIT, (struct sockaddr *)&air_hello, sizeof air_hello);
                if (!g_hs_done) {
                    g_hs_done = 1;
                    printf(TAG " :20001 3-way done (air identity %zd B, type2 ACK sent)\n", n);
                    fflush(stdout);
                }
            }
        }

        /* :10000 poll: a type-1 request every 2 s for the WHOLE session, matching the vendor's
         * ParamsTimerEvent (which never stops). The air replies 0x02 to each - harmless, no air-side
         * state change (RE of AR_FSM_TX_ProcessParamsRequest) - and those replies keep :10000 RX
         * fresh so the air-loss watch below only trips on a real baseband drop. We used to STOP this
         * poll once the consumer reported frames (g_video_confirmed); that let the air's :10000
         * telemetry go quiet and tripped a FALSE air-loss every ~5 s, so video cut out and re-
         * handshook in a ~10 s loop. Pre-media the poll is READY-gated so the air's first IDR lands
         * with a bound consumer; once acked it is the ungated keepalive (the air's 0x02 reply then
         * drives the type-3 ACK below, i.e. the vendor's steady-state 2 s :10000 datagram). */
        if (params_bound && g_hs_done && (g_no_gate || g_ready || g_params_acked)
            && now - last_req >= PARAMS_IVL_MS) {
            uint8_t frame[MP_HDR_LEN];
            sendto(params_sock, frame, mp_params_request(frame, stamp_us), MSG_DONTWAIT,
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
            if (msg_type == MP_REPLY) {     /* MEDIA_PARAMS reply -> [SetLdCfg] -> type3 ACK, video starts */
                /* Match the vendor's per-cycle sequence exactly: after the air's 0x02 reply the goggle
                 * sends SetLdCfg (0x0A), THEN the 0x03 ack that starts video (capture: 01 -> 02 -> 0a ->
                 * 03 -> 15). Sending it here - as part of the handshake, before video - is the vendor
                 * placement; sending it post-ack (mid-stream) reconfigured a live encoder and wedged the
                 * air. Sent whenever the HUD has commanded a lever it carries (TX power and/or bitrate;
                 * both stay at the captured base when unset). The !g_params_acked guard makes it fire
                 * ONCE per session (the first 0x02): the poll runs for the whole session, 0x02 replies
                 * arrive every 2 s, and re-sending SetLdCfg on each would be exactly that mid-stream
                 * reconfig. */
                if (!g_params_acked && (g_power_dbm >= 0 || g_bitrate_mbps > 0)) {
                    uint8_t cfg[MP_LDCFG_LEN];
                    uint8_t dbm = g_power_dbm >= 0 ? (uint8_t) g_power_dbm : 0;
                    /* standby: armed only when the HUD commanded it; never the base's 1 (an
                     * uncommanded first association must not arm the standby trap) */
                    sendto(params_sock, cfg,
                           mp_set_ld_cfg(cfg, dbm, (uint8_t) g_bitrate_mbps, g_standby_arm > 0,
                                         stamp_us),
                           MSG_DONTWAIT, (struct sockaddr *)&air_params, sizeof air_params);
                    /* Power is the lever the air honours; the blob's bitrate_q is stored-but-ignored
                     * on the air (HW-confirmed), so only power is worth noting. */
                    if (g_verbose) {
                        fprintf(stderr, TAG " tx SetLdCfg (power=0x%02x)\n", dbm);
                    }
                }

                uint8_t frame[MP_HDR_LEN];
                sendto(params_sock, frame, mp_params_ack(frame, stamp_us), MSG_DONTWAIT,
                       (struct sockaddr *)&air_params, sizeof air_params);
                g_last_ack_ms = now;

                if (!g_params_acked) {
                    g_params_acked = 1;
                    link_event(MLM_LINK_PARAMS_ACKED, "MEDIA_PARAMS acked, video should start");
                    led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);

                    /* The SetLdCfg the air just latched reset its ISP to the association defaults:
                     * re-arm every commanded camera selector (and the scale pair) so the live
                     * sender below re-applies the HUD's state on top. */
                    g_cam_pending = g_cam_commanded;
                    g_scale_pending = g_scale_aspect >= 0;
                }
            } else if (msg_type == MP_MSP) {
                mlm_pub(MLM_OSD_SOCK, MLM_T_MSP, rx, n);
            } else if (msg_type == MP_STATUS_A || msg_type == MP_STATUS_B) {
                mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_STATUS, rx, n);
            } else if (msg_type == MP_STANDBY && n >= STANDBY_OFF_MODE + 4) {
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
                    uint8_t frame[MP_HDR_LEN];
                    sendto(params_sock, frame, mp_stb_ack(frame, stamp_us), MSG_DONTWAIT,
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
        if (params_bound && (g_standby_arm >= 0 || g_power_dbm >= 0)
            && g_hs_done && !g_air_lost && now - last_stp >= STANDBY_IVL_MS) {
            uint8_t frame[MP_STP_LEN];
            uint8_t power   = (g_power_dbm >= 0) ? (uint8_t) g_power_dbm : AIR_TX_DBM;
            uint8_t standby = (g_standby_arm >= 0) ? (uint8_t) g_standby_arm : 0;

            sendto(params_sock, frame, mp_set_tran_parm(frame, power, standby, stamp_us), MSG_DONTWAIT,
                   (struct sockaddr *)&air_params, sizeof air_params);
            last_stp = now;

            if (g_verbose) {
                fprintf(stderr, TAG " tx SetTranParm standby=%d power=0x%02x\n", standby, power);
            }
        }

        /* (SetLdCfg is sent in the MEDIA_PARAMS 0x02-reply handler above, before the 0x03 ack, to match
         * the vendor's per-cycle 01->02->0a->03 sequence - not here.) */

        /* Live camera/scale pushes: one SetCameraInfo (0x0C) per pending selector plus one
         * SetScaleMode (0x15), the vendor's own live-set path (one datagram per menu change).
         * Gated on an established session: the air only applies ISP sets with a running encoder,
         * and anything commanded earlier stays pending until the ack above re-arms and lands here. */
        if (params_bound && g_params_acked && !g_air_lost) {
            while (g_cam_pending != 0) {
                unsigned sel = (unsigned) __builtin_ctz(g_cam_pending);
                uint8_t frame[MP_CAM_LEN];

                g_cam_pending &= g_cam_pending - 1;
                sendto(params_sock, frame, mp_set_camera_info(frame, sel, &g_cam, stamp_us),
                       MSG_DONTWAIT, (struct sockaddr *)&air_params, sizeof air_params);
                if (g_verbose) {
                    fprintf(stderr, TAG " tx SetCameraInfo sel=%u\n", sel);
                }
            }

            if (g_scale_pending) {
                uint8_t frame[MP_SCALE_LEN];

                g_scale_pending = 0;
                sendto(params_sock, frame,
                       mp_set_scale_mode(frame, g_scale_aspect == 1,
                                         (float) g_scale_zoom_pct / 100.0f, stamp_us),
                       MSG_DONTWAIT, (struct sockaddr *)&air_params, sizeof air_params);
                if (g_verbose) {
                    fprintf(stderr, TAG " tx SetScaleMode aspect=%d zoom=%d%%\n",
                            g_scale_aspect, g_scale_zoom_pct);
                }
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
    struct sigaction sa;
    uint8_t frame[64], ff02[19];
    uint32_t seq_link = SEQ_START, seq_video = SEQ_START;
    unsigned long ticks = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            node = argv[++i];
        } else if (!strcmp(argv[i], "--no-gate")) {
            g_no_gate = 1;
        } else if (!strcmp(argv[i], "--scan-probe")) {
            g_scan_probe = 1;
        } else if (!strcmp(argv[i], "-v")) {
            g_verbose = 1;
        } else {
            fprintf(stderr, "usage: ml-linkd [-d /dev/artosyn_sdio] [--no-gate] [--scan-probe] [-v]\n");
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

    /* OPEN_SEQ's channel is a Race index, so correct it before the air associates if the chip's
     * band does not contain it (Normal). */
    tune_into_band(frame, &seq_link);

    /* 23 dBm */
    send_frame(frame, bb_set_power(frame, RF_TX, 0x17, seq_video++), "tx-power");
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

        /* HUD-requested channel retune: issue it from this thread (the bb-socket TX owner), once,
         * then clear. The tune is async and the air follows transparently; g_cur_chnidx tracks it so
         * the published OSD channel stays correct. Clear before sending so a request that arrives
         * during the tune is not lost to the clear. */
        if (g_pending_chnidx >= 0) {
            int chnidx = g_pending_chnidx;

            g_pending_chnidx = -1;
            send_frame(frame, bb_select_channel(frame, (uint8_t)chnidx, seq_link++), "select-chn");
            g_cur_chnidx = chnidx;
            printf(TAG " selected channel %d\n", chnidx);
            fflush(stdout);
        }

        /* Run one channel sweep on a HUD request (MLM_RF_SCAN). Request-driven only: the sweep
         * retunes the RX across every valid channel and interrupts video for its duration, so the
         * HUD fires it on opening the channel screen and on an explicit refresh, never on a cadence.
         * It restores the active channel itself. */
        if (g_pending_scan) {
            g_pending_scan = 0;
            scan_sweep(frame, &seq_link);
        }

        if (ticks % POLL_LINK_EVERY == 0) {
            bb_get(poll, GET_1V1INFO, seq_link++);
            send_frame(poll, 19, "get-1v1");
        }

        if (ticks % FF02_EVERY == 0) {
            send_frame(ff02, 19, "ff02");
        }

        /* Publish the local baseband link metrics for the HUD's System OSD (~1 Hz). Channel is a
         * config fact we always know; SNR/distance go stale on air loss, so blank them then and let
         * the HUD dim the whole air-unit side off its own connection state. */
        if (ticks % LINKINFO_EVERY == 0) {
            struct mlm_linkinfo info = {
                .channel = g_cur_chnidx,
                .snr_db = g_air_lost ? MLM_LINKINFO_NONE : g_snr_db,
                .distance_m = g_air_lost ? MLM_LINKINFO_NONE : g_distance_m,
                .flags = (!g_air_lost && g_standby_state) ? MLM_LINKINFO_F_STANDBY : 0,
                /* rx_throughput (Get1V1Info +0x0c) = measured PHY link throughput. We publish it RAW.
                 * The vendor OSD shows this same field but divides it by a HARDCODED 6 in standby
                 * (AR_MID_GET_REALTIME_SYS_INFO param_2[7] = uVar3 / 6, gated on the RcStatus standby
                 * low byte) - a fixed display fudge, not a real measurement, so we deliberately do
                 * NOT replicate it. Our number is the honest link throughput in both states. */
                .throughput_kbps = g_air_lost ? 0 : (uint32_t)g_throughput_kbps,
            };

            mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_LINKINFO, &info, sizeof info);
        }

        usleep(STEADY_STEP_US);
        if (++ticks % ALIVE_EVERY == 0) {
            printf(TAG " alive hs=%d ready=%d acked=%d video=%d air_lost=%d thr_kbps=%d\n",
                   g_hs_done, g_ready, g_params_acked, g_video_confirmed, g_air_lost,
                   g_throughput_kbps);
            fflush(stdout);
        }
    }

    g_run = 0;

    /* The reader may be blocked in read() on the device; a SIGTERM directed at the thread
     * interrupts it with EINTR (SA_RESTART is off) and it exits on the g_run check. Re-send
     * until the join lands: a signal that arrives just before the thread re-enters read() is
     * consumed without unblocking it. */
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
            break;
        }
    }

    pthread_join(udp_th, NULL);
    close(g_fd);

    if (g_mlm >= 0) {
        close(g_mlm);
    }

    printf(TAG " clean stop\n");
    return 0;
}
