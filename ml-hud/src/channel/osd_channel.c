/** @file osd_channel.c @brief See osd_channel.h. */
#include "osd_channel.h"

#include "../../../ml-shared/mlm.h"

#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read a little-endian u32/u16 at a byte offset (the wire is LE regardless of host). */
static uint32_t read_u32(const unsigned char *p, int off)
{
    return (uint32_t) p[off] | ((uint32_t) p[off + 1] << 8)
         | ((uint32_t) p[off + 2] << 16) | ((uint32_t) p[off + 3] << 24);
}

static uint16_t read_u16(const unsigned char *p, int off)
{
    return (uint16_t) ((uint16_t) p[off] | ((uint16_t) p[off + 1] << 8));
}

int osd_decode_header(const unsigned char *frame, int len, osd_header_t *out)
{
    if (len < OSD10K_HEADER_LEN) {
        return -1;
    }

    out->type = read_u32(frame, OSD10K_OFF_TYPE);
    out->ts_us = read_u32(frame, OSD10K_OFF_TS);

    uint32_t payload_len = read_u32(frame, OSD10K_OFF_PAYLOAD_LEN);
    if ((int) (OSD10K_HEADER_LEN + payload_len) > len) {
        payload_len = (uint32_t) (len - OSD10K_HEADER_LEN);   /* trust the wire length no further */
    }
    out->payload_len = payload_len;

    return 0;
}

int osd_decode_periodic(const unsigned char *payload, int payload_len, osd_periodic_t *out)
{
    if (payload_len < OSD10K_PERIODIC_OFF_VOLTAGE_MV + 2) {
        return -1;
    }

    out->voltage_mV = read_u16(payload, OSD10K_PERIODIC_OFF_VOLTAGE_MV);
    return 0;
}

int osd_decode_version(const unsigned char *payload, int payload_len, osd_version_t *out)
{
    if (payload_len < OSD10K_VERSION_OFF_LINK_B + 1) {
        return -1;
    }

    memcpy(out->hw, payload + OSD10K_VERSION_OFF_HW, 16);
    out->hw[16] = '\0';

    memcpy(out->fw, payload + OSD10K_VERSION_OFF_FW, 16);
    out->fw[16] = '\0';

    out->voltage_mV = read_u16(payload, OSD10K_VERSION_OFF_VOLTAGE_MV);
    out->air_temp_c = read_u16(payload, OSD10K_VERSION_OFF_TEMP_C);
    out->link_b = payload[OSD10K_VERSION_OFF_LINK_B];

    return 0;
}

int osd_channel_open(void)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, MLM_OSD_SOCK, sizeof(sa.sun_path) - 1);
    mkdir(MLM_RUN_DIR, 0755);
    unlink(MLM_OSD_SOCK);
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int osd_channel_poll(int fd, const osd_channel_cb_t *cb, void *ctx, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        return -1;
    }

    if (pr == 0) {
        return 0;
    }

    unsigned char buf[65536];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < (ssize_t) sizeof(struct mlm_hdr)) {
        return n < 0 ? 0 : 1;
    }

    struct mlm_hdr h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != MLM_MAGIC || (h.type != MLM_T_MSP && h.type != MLM_T_STATUS)) {
        return 1;   /* a record arrived, just not an OSD one */
    }

    osd_channel_dispatch(buf + sizeof(h), (int) (n - (ssize_t) sizeof(h)), cb, ctx);
    return 1;
}

void osd_channel_dispatch(const unsigned char *frame, int len, const osd_channel_cb_t *cb, void *ctx)
{
    osd_header_t header;
    if (osd_decode_header(frame, len, &header) != 0) {
        return;
    }

    const unsigned char *payload = frame + OSD10K_HEADER_LEN;
    int payload_len = (int) header.payload_len;

    if (header.type == OSD10K_MSG_OSD) {
        if (cb->on_osd != NULL) {
            cb->on_osd(ctx, payload, payload_len);
        }
    } else if (header.type == OSD10K_MSG_VERSION) {
        osd_version_t v;
        if (osd_decode_version(payload, payload_len, &v) == 0 && cb->on_version != NULL) {
            cb->on_version(ctx, &header, &v);
        }
    } else if (header.type == OSD10K_MSG_PERIODIC) {
        osd_periodic_t p;
        if (osd_decode_periodic(payload, payload_len, &p) == 0 && cb->on_periodic != NULL) {
            cb->on_periodic(ctx, &header, &p);
        }
    }
}

void osd_channel_close(int fd)
{
    if (fd < 0) {
        return;
    }

    close(fd);
    unlink(MLM_OSD_SOCK);
}
