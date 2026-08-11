/*
 * ml-msp.c - Betaflight MSPv1 client for the air-unit FC UART.
 */
#define _GNU_SOURCE
#include "ml-msp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MSP_API_VERSION     0x02
#define MSP_STATUS          0x65
#define MSP_BATTERY_STATE   0x82
#define MSP_DISPLAYPORT     0xb6

/* Both intervals are the vendor's, from libvtxfc's poller; the battery poll is deliberately 0.2 Hz.
 * ML_MSP_FRESH_MS is sized against this cadence, so the two move together. Polling faster is a
 * parked beyond-vendor improvement, see plans/beyond-vendor-backlog.md. */
#define MSP_STATUS_MS       200
#define MSP_BATTERY_MS      5000

#define MSP_MAX_PAYLOAD     255
#define MSP_FRAME_OVERHEAD  6

#define MSP_DP_WRITE_STRING 3
#define MSP_DP_DRAW_SCREEN  4
#define MSP_DP_CLEAR_SCREEN 2

#define LOG_TAG             "ml-msp"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int set_raw_115200(int fd)
{
    struct termios tio;

    memset(&tio, 0, sizeof tio);
    tio.c_cflag = CLOCAL | CREAD | CS8;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);

    if (tcflush(fd, TCIFLUSH) != 0) {
        return -1;
    }

    return tcsetattr(fd, TCSANOW, &tio);
}

void ml_msp_init(struct ml_msp *msp, ml_msp_canvas_cb canvas_cb, void *canvas_ctx)
{
    memset(msp, 0, sizeof *msp);
    msp->fd = -1;
    msp->canvas_cb = canvas_cb;
    msp->canvas_ctx = canvas_ctx;
}

int ml_msp_open(struct ml_msp *msp, const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) {
        return -1;
    }

    if (set_raw_115200(fd) != 0) {
        close(fd);
        return -1;
    }

    msp->fd = fd;
    return 0;
}

void ml_msp_close(struct ml_msp *msp)
{
    if (msp->fd >= 0) {
        close(msp->fd);
        msp->fd = -1;
    }
}

static int send_request(struct ml_msp *msp, uint8_t cmd)
{
    uint8_t frame[MSP_FRAME_OVERHEAD] = { '$', 'M', '<', 0, cmd, cmd };
    ssize_t n = write(msp->fd, frame, sizeof frame);

    if (n == (ssize_t)sizeof frame) {
        return 0;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 1;
    }

    return -1;
}

static void records_clear(struct ml_msp *msp)
{
    msp->records_len = 0;
    msp->record_count = 0;
}

static void msp_drop_fd(struct ml_msp *msp, const char *what)
{
    if (msp->fd >= 0) {
        fprintf(stderr, LOG_TAG ": %s: %s, closing FC UART\n", what, strerror(errno));
        close(msp->fd);
        msp->fd = -1;
    }

    msp->rx_len = 0;
    records_clear(msp);
}

static int append_record(struct ml_msp *msp, const uint8_t *payload, uint8_t len)
{
    uint8_t text_len;
    size_t need;

    if (len < 4) {
        return -1;
    }

    text_len = (uint8_t)(len - 4);
    if (text_len > 0 && payload[4 + text_len - 1] == '\0') {
        text_len--;
    }

    need = 5 + text_len;
    if (msp->records_len + need > sizeof msp->records || msp->record_count == UINT8_MAX) {
        return -1;
    }

    msp->records[msp->records_len++] = payload[1];   /* row */
    msp->records[msp->records_len++] = payload[2];   /* col */
    msp->records[msp->records_len++] = text_len;
    msp->records[msp->records_len++] = payload[3];   /* attr */
    memcpy(msp->records + msp->records_len, payload + 4, text_len);
    msp->records_len += text_len;
    msp->record_count++;

    return 0;
}

static size_t pack_interleaved(uint8_t *dst, size_t dstsz, const uint8_t *src, size_t src_len)
{
    size_t d = 0;

    for (size_t i = 0; i < src_len; i += 2) {
        if (d >= dstsz) {
            return 0;
        }

        dst[d++] = src[i];
        if (i + 1 < src_len) {
            if (d + 1 >= dstsz) {
                return 0;
            }

            dst[d++] = src[i + 1];
            dst[d++] = 0xff;
        }
    }

    return d;
}

static void emit_canvas(struct ml_msp *msp)
{
    uint8_t plain[512];
    uint8_t packed[768];
    size_t p = 9;
    size_t record_start = 0;
    size_t packed_len;

    if (msp->canvas_cb == NULL || msp->record_count == 0) {
        records_clear(msp);
        return;
    }

    plain[0] = msp->record_count;
    write_be32(plain + 1, ++msp->canvas_seq);

    for (uint8_t i = 0; i < msp->record_count; i++) {
        uint8_t text_len = msp->records[record_start + 2];
        /* Declared length spans the sub-command, row, col, attr and text: 4 + text_len, for every
         * record including the last. The trailing chain byte is not counted. */
        uint8_t len = (uint8_t)(4 + text_len);
        size_t next = record_start + 4 + text_len;

        if (p + (i == 0 ? 1 : 0) + 2 + len > sizeof plain) {
            records_clear(msp);
            return;
        }

        if (i == 0) {
            plain[p++] = len;
        }

        plain[p++] = MSP_DISPLAYPORT;
        plain[p++] = MSP_DP_WRITE_STRING;
        plain[p++] = msp->records[record_start + 0];
        plain[p++] = msp->records[record_start + 1];
        plain[p++] = msp->records[record_start + 3];
        memcpy(plain + p, msp->records + record_start + 4, text_len);
        p += text_len;
        if (i + 1 < msp->record_count) {
            plain[p++] = (uint8_t)(4 + msp->records[next + 2]);
        }

        record_start = next;
    }

    write_be32(plain + 5, (uint32_t)p);
    packed_len = pack_interleaved(packed, sizeof packed, plain, p);
    if (packed_len > 0) {
        msp->canvas_cb(packed, packed_len, msp->canvas_ctx);
    }

    records_clear(msp);
}

static void process_displayport(struct ml_msp *msp, const uint8_t *payload, uint8_t len)
{
    if (len == 0) {
        return;
    }

    switch (payload[0]) {
    case MSP_DP_CLEAR_SCREEN: {
        records_clear(msp);
    } break;

    case MSP_DP_WRITE_STRING: {
        append_record(msp, payload, len);
    } break;

    case MSP_DP_DRAW_SCREEN: {
        emit_canvas(msp);
    } break;

    default: {
        break;
    }
    }
}

static void process_frame(struct ml_msp *msp, uint8_t cmd, const uint8_t *payload,
                          uint8_t len, long now_ms)
{
    if (cmd == MSP_STATUS && len >= 7) {
        msp->status.arm_flag = payload[6] & 1;
        msp->status.last_rx_ms = now_ms;
    } else if (cmd == MSP_BATTERY_STATE && len >= 11) {
        msp->status.mah_drawn_x10 = (uint16_t)(read_le16(payload + 4) * 10);
        msp->status.voltage_mv = (uint16_t)(read_le16(payload + 9) * 10);
        msp->status.last_rx_ms = now_ms;
        msp->status.last_battery_ms = now_ms;
    } else if (cmd == MSP_DISPLAYPORT) {
        process_displayport(msp, payload, len);
    } else if (cmd == MSP_API_VERSION) {
        msp->status.last_rx_ms = now_ms;
    }
}

/* One-shot latch for the MSP v2 sighting below, so a desynced link cannot flood the log. */
static int g_saw_msp_v2;

static void scan_frames(struct ml_msp *msp, long now_ms)
{
    size_t pos = 0;

    while (msp->rx_len - pos >= MSP_FRAME_OVERHEAD) {
        uint8_t len;
        size_t total;
        uint8_t checksum;

        if (msp->rx[pos] != '$' || msp->rx[pos + 1] != 'M' || msp->rx[pos + 2] != '>') {
            /* Everything that is not a v1 reply preamble is discarded a byte at a time. A literal
             * "$X" is MSP v2, which neither Betaflight nor INAV should ever send us: they push
             * DisplayPort as v1 unconditionally and answer a request in the version it arrived in,
             * and we only ever ask in v1. Report it once rather than silently dropping every frame,
             * because the symptom otherwise is a blank OSD and a frozen voltage, which reads as a
             * wiring fault. A v1 frame carrying command 255 is V2-over-V1 encapsulation, not this:
             * it matches the preamble above and is dropped harmlessly by process_frame. */
            if (msp->rx[pos] == '$' && msp->rx[pos + 1] == 'X' && !g_saw_msp_v2) {
                g_saw_msp_v2 = 1;
                fprintf(stderr, LOG_TAG ": MSP v2 ($X) frame on the FC UART, ignored; "
                                "this parser is v1 only\n");
            }

            pos++;
            continue;
        }

        len = msp->rx[pos + 3];
        total = (size_t)len + MSP_FRAME_OVERHEAD;
        if (msp->rx_len - pos < total) {
            break;
        }

        checksum = len ^ msp->rx[pos + 4];
        for (uint8_t i = 0; i < len; i++) {
            checksum ^= msp->rx[pos + 5 + i];
        }

        if (checksum != msp->rx[pos + total - 1]) {
            pos += 3;
            continue;
        }

        process_frame(msp, msp->rx[pos + 4], msp->rx + pos + 5, len, now_ms);
        pos += total;
    }

    if (pos > 0) {
        memmove(msp->rx, msp->rx + pos, msp->rx_len - pos);
        msp->rx_len -= pos;
    }
}

void ml_msp_service(struct ml_msp *msp, long now_ms)
{
    ssize_t n = 0;
    int rc;

    if (msp->fd < 0) {
        return;
    }

    while (msp->rx_len < sizeof msp->rx &&
           (n = read(msp->fd, msp->rx + msp->rx_len, sizeof msp->rx - msp->rx_len)) > 0) {
        msp->rx_len += (size_t)n;
        scan_frames(msp, now_ms);
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        msp_drop_fd(msp, "read");
        return;
    }

    if (msp->rx_len == sizeof msp->rx) {
        memmove(msp->rx, msp->rx + 3, msp->rx_len - 3);
        msp->rx_len -= 3;
    }

    if (now_ms >= msp->next_status_ms) {
        rc = send_request(msp, MSP_STATUS);
        if (rc < 0) {
            msp_drop_fd(msp, "write status request");
            return;
        }
        msp->next_status_ms = now_ms + MSP_STATUS_MS;
    }

    if (now_ms >= msp->next_battery_ms) {
        rc = send_request(msp, MSP_BATTERY_STATE);
        if (rc < 0) {
            msp_drop_fd(msp, "write battery request");
            return;
        }
        msp->next_battery_ms = now_ms + MSP_BATTERY_MS;
    }
}

/* Whether an FC is talking to us right now. `arm_flag` is only meaningful while this holds: with no
 * FC attached it reads 0, which is indistinguishable from a real disarm, so a caller that gates on
 * arm state has to know the difference. `last_rx_ms` is stamped by any MSP reply, so this covers
 * both a unit that never had an FC and one whose FC has gone away mid-session. */
int ml_msp_fc_present(const struct ml_msp *msp, long now_ms)
{
    return msp->status.last_rx_ms != 0 && now_ms - msp->status.last_rx_ms <= ML_MSP_FRESH_MS;
}

int ml_msp_fc_voltage_mv(const struct ml_msp *msp, long now_ms)
{
    /* 1800 is the vendor's 0x708, from AR_LOWDELAY_TX_SYSCTRL_GetVoltage. It is an OVERRIDE
     * THRESHOLD, not a plausibility floor: above it the FC value replaces the SoC ADC reading,
     * below it the ADC stands. Reading it as a floor is a mistake this project has already made
     * once and had to correct in the telemetry docs, so do not "fix" it into a clamp.
     *
     * The staleness test is ours, not the vendor's: theirs latches the last FC value forever, so a
     * disconnected FC freezes the displayed voltage instead of falling back. */
    if (msp->status.voltage_mv > 1800 &&
        now_ms - msp->status.last_battery_ms <= ML_MSP_FRESH_MS) {
        return msp->status.voltage_mv;
    }

    return -1;
}
