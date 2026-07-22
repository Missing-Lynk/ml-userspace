/** @file linkstate.c @brief See linkstate.h. */
#include "linkstate.h"

#include "../channel/osd_channel.h"

#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../../../ml-shared/mlm.h"

/* No telemetry-seam traffic for this long = air gone. ml-linkd's own TX_LOST fires at 5 s of :10000
 * silence; this is the backstop for when ml-linkd itself stops (its socket goes quiet, no TX_LOST).
 */
#define LINK_STALE_MS 6000

static uint32_t g_last_seen_ms;   /* monotonic ms of the last air-liveness datagram; 0 = never/lost */
static uint32_t g_pipeline_state; /* last MLM_T_STATE from ml-pipeline; defaults to MLM_STATE_IDLE (0) */
static int      g_pipeline_seen;  /* a real MLM_T_STATE has arrived (else g_pipeline_state is the default) */
static uint32_t g_pb_flags;       /* state flags (MLM_STATE_F_*) from the last MLM_T_STATE */
static uint32_t g_state_ms;       /* monotonic ms of the last MLM_T_STATE; freshness gate for the flags */
static uint32_t g_pb_pos_ms;      /* playback position (ms) */
static uint32_t g_pb_dur_ms;      /* playback duration (ms) */

static const osd_channel_cb_t *g_osd_cb; /* registered sink for MLM_T_STATUS's raw 0x09/0x11 frames */
static void *g_osd_ctx;

/* Local baseband link metrics from ml-linkd's MLM_T_LINKINFO; MLM_LINKINFO_NONE until one arrives. */
static int g_channel = MLM_LINKINFO_NONE;
static int g_snr_db = MLM_LINKINFO_NONE;
static int g_distance_m = MLM_LINKINFO_NONE;
static int g_standby;   /* air is in standby (MLM_LINKINFO_F_STANDBY): quad disarmed + standby armed */
static int g_throughput_kbps;   /* measured PHY link throughput from MLM_T_LINKINFO; 0 = unknown / no link */

/* Air encoder self-report from ml-pipeline's MLM_T_FRAMESTATS (SEI BR/QP). Fresh at ~60 Hz while
 * video flows; g_framestats_ms gates staleness so the OSD blanks when the feed stops. */
static int g_sei_br_kbps;
static int g_sei_qp;
static uint32_t g_framestats_ms;

/* Last RF channel scan from ml-linkd's MLM_T_SCAN; g_scan_gen increments per scan (0 = none yet). */
static struct mlm_scan g_scan;
static unsigned g_scan_gen;

/* Bind progress from ml-linkd's MLM_T_LINK. g_binding is set between BINDING and BIND_OK/FAIL;
 * g_bind_gen increments once per completed bind so the HUD can fire the result cue exactly once. */
static int g_binding;
static unsigned g_bind_gen;
static int g_bind_ok;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

int linkstate_open(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_TELEMETRY_SOCK, sizeof addr.sun_path - 1);
    unlink(MLM_TELEMETRY_SOCK);
    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

void linkstate_poll(int fd)
{
    if (fd < 0) {
        return;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        uint8_t buf[1024];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < (ssize_t) sizeof(struct mlm_hdr)) {
            continue;
        }

        struct mlm_hdr hdr;
        memcpy(&hdr, buf, sizeof hdr);
        if (hdr.magic != MLM_MAGIC) {
            continue;
        }

        if (hdr.type == MLM_T_STATUS) {
            g_last_seen_ms = now_ms();   /* air telemetry republished by linkd: the air unit is live */
            if (g_osd_cb != NULL) {
                /* the record payload is the raw :10000 status frame; feed it to the OSD dispatch so
                 * voltage/version reach the HUD exactly as they would off the UDP bench channel */
                osd_channel_dispatch(buf + sizeof hdr, (int) (n - (ssize_t) sizeof hdr),
                                     g_osd_cb, g_osd_ctx);
            }
        } else if (hdr.type == MLM_T_LINK
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_link))) {
            struct mlm_link link;
            memcpy(&link, buf + sizeof hdr, sizeof link);
            if (link.state == MLM_LINK_TX_LOST) {
                g_last_seen_ms = 0;   /* explicit loss: down at once, before the staleness timeout */
            } else if (link.state == MLM_LINK_PARAMS_ACKED || link.state == MLM_LINK_SESSION_RESTART) {
                g_last_seen_ms = now_ms();
            } else if (link.state == MLM_LINK_BINDING) {
                g_binding = 1;
            } else if (link.state == MLM_LINK_BIND_OK || link.state == MLM_LINK_BIND_FAIL) {
                g_binding = 0;
                g_bind_ok = (link.state == MLM_LINK_BIND_OK);
                g_bind_gen++;
            }
        } else if (hdr.type == MLM_T_LINKINFO
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_linkinfo))) {
            struct mlm_linkinfo info;   /* local baseband metrics for the air-unit System OSD */
            memcpy(&info, buf + sizeof hdr, sizeof info);
            g_channel = info.channel;
            g_snr_db = info.snr_db;
            g_distance_m = info.distance_m;
            g_standby = (info.flags & MLM_LINKINFO_F_STANDBY) != 0;
            g_throughput_kbps = (int) info.throughput_kbps;
        } else if (hdr.type == MLM_T_FRAMESTATS
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_framestats))) {
            struct mlm_framestats fs;   /* air encoder self-report (SEI BR/QP) from ml-pipeline */
            memcpy(&fs, buf + sizeof hdr, sizeof fs);
            g_sei_br_kbps = (int) fs.br_kbps;
            g_sei_qp = (int) fs.qp;
            g_framestats_ms = now_ms();
        } else if (hdr.type == MLM_T_STATE
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_state))) {
            struct mlm_state st;   /* ml-pipeline's current mode (idle / recording / playback) */
            memcpy(&st, buf + sizeof hdr, sizeof st);
            g_pipeline_state = st.state;
            g_pipeline_seen = 1;
            g_pb_flags = st.flags;
            g_state_ms = now_ms();
            g_pb_pos_ms = st.pos_ms;
            g_pb_dur_ms = st.dur_ms;
        } else if (hdr.type == MLM_T_SCAN
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_scan))) {
            memcpy(&g_scan, buf + sizeof hdr, sizeof g_scan);   /* channel table for the channel screen */
            g_scan_gen++;
        }
    }
}

unsigned linkstate_scan(struct mlm_scan *out)
{
    if (out != NULL && g_scan_gen != 0) {
        *out = g_scan;
    }

    return g_scan_gen;
}

void linkstate_set_osd_cb(const osd_channel_cb_t *cb, void *ctx)
{
    g_osd_cb = cb;
    g_osd_ctx = ctx;
}

int linkstate_pipeline_state(void)
{
    return (int) g_pipeline_state;
}

bool linkstate_has_pipeline_state(void)
{
    return g_pipeline_seen;
}

int linkstate_playback(int *paused, unsigned *pos_ms, unsigned *dur_ms)
{
    if (paused) {
        *paused = (g_pb_flags & MLM_STATE_F_PAUSED) != 0;
    }

    if (pos_ms) {
        *pos_ms = g_pb_pos_ms;
    }

    if (dur_ms) {
        *dur_ms = g_pb_dur_ms;
    }

    return g_pipeline_state == MLM_STATE_PLAYBACK;
}

bool linkstate_is_playback_ended(void)
{
    return g_pipeline_state == MLM_STATE_PLAYBACK && (g_pb_flags & MLM_STATE_F_ENDED) != 0;
}

bool linkstate_is_playback_rendering(void)
{
    return g_pipeline_state == MLM_STATE_PLAYBACK && (g_pb_flags & MLM_STATE_F_RENDERING) != 0;
}

bool linkstate_is_video_live(void)
{
    return (g_pb_flags & MLM_STATE_F_VIDEO_LIVE) != 0
        && g_state_ms != 0 && (uint32_t) (now_ms() - g_state_ms) < 2500;
}

bool linkstate_is_rtsp_on(void)
{
    return (g_pb_flags & MLM_STATE_F_RTSP) != 0;
}

bool linkstate_is_airunit_connected(void)
{
    return g_last_seen_ms != 0 && (uint32_t) (now_ms() - g_last_seen_ms) < LINK_STALE_MS;
}

int linkstate_channel(void)
{
    return g_channel;
}

int linkstate_snr_db(void)
{
    return g_snr_db;
}

int linkstate_distance_m(void)
{
    return g_distance_m;
}

bool linkstate_is_standby(void)
{
    return g_standby;
}

bool linkstate_is_binding(void)
{
    return g_binding != 0;
}

unsigned linkstate_bind_result(int *ok)
{
    if (ok != NULL) {
        *ok = g_bind_ok;
    }

    return g_bind_gen;
}

int linkstate_throughput_kbps(void)
{
    return g_throughput_kbps;
}

bool linkstate_sei_brqp(int *br_kbps, int *qp)
{
    if (g_framestats_ms == 0 || (uint32_t) (now_ms() - g_framestats_ms) >= 2500) {
        return false;
    }

    if (br_kbps != NULL) {
        *br_kbps = g_sei_br_kbps;
    }

    if (qp != NULL) {
        *qp = g_sei_qp;
    }

    return true;
}

void linkstate_close(int fd)
{
    if (fd >= 0) {
        close(fd);
        unlink(MLM_TELEMETRY_SOCK);
    }
}
