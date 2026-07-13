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

static const osd_channel_cb_t *g_osd_cb; /* registered sink for MLM_T_STATUS's raw 0x09/0x11 frames */
static void *g_osd_ctx;

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
            }
        } else if (hdr.type == MLM_T_STATE
                   && n >= (ssize_t) (sizeof hdr + sizeof(struct mlm_state))) {
            struct mlm_state st;   /* ml-pipeline's current mode (recording, later playback) */
            memcpy(&st, buf + sizeof hdr, sizeof st);
            g_pipeline_state = st.state;
        }
    }
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

int linkstate_airunit_connected(void)
{
    return g_last_seen_ms != 0 && (uint32_t) (now_ms() - g_last_seen_ms) < LINK_STALE_MS;
}

void linkstate_close(int fd)
{
    if (fd >= 0) {
        close(fd);
        unlink(MLM_TELEMETRY_SOCK);
    }
}
