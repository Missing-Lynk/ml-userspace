/*
 * Read ml-aed's operating parameters out of the sensor tuning blob, the file the ISP driver
 * request_firmware()s. Offsets come from ml-aed-blob.h.
 *
 * Named spans are pread individually: the AE payload is under 3 KiB against 859 KiB for the blob.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ml-aed-tuning.h"

static int read_span(int fd, void *dst, size_t len, off_t off)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = pread(fd, (char *)dst + got, len - got, off + (off_t)got);

        if (n <= 0) {
            return n == 0 ? -EIO : -errno;
        }

        got += (size_t)n;
    }

    return 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The log ladder is an IEEE-754 single. Through a union, not a pointer cast: aliasing. */
static float le_f32(const uint8_t *p)
{
    union {
        uint32_t u;
        float f;
    } v = { .u = le32(p) };

    return v.f;
}

int ae_tuning_load(struct ae_tuning *t, const char *path)
{
    uint8_t buf[MLAED_TARGET_CURVE_COUNT * MLAED_TARGET_CURVE_STRIDE];
    uint8_t word[4];
    struct stat st;
    int fd, ret;

    memset(t, 0, sizeof(*t));

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }

    /* Size-checked the way the driver checks it. */
    if (fstat(fd, &st) < 0) {
        ret = -errno;
        close(fd);

        return ret;
    }

    if (st.st_size != (off_t)MLAED_TUNING_SIZE) {
        close(fd);

        return -EINVAL;
    }

    t->table = calloc(MLAED_EXPOSURE_TABLE_COUNT, sizeof(*t->table));
    if (!t->table) {
        close(fd);

        return -ENOMEM;
    }

    ret = read_span(fd, buf, sizeof(buf), MLAED_TARGET_CURVE_OFF);
    for (unsigned int i = 0; !ret && i < MLAED_TARGET_CURVE_COUNT; i++) {
        t->curve[i].index = (int)le32(buf + i * MLAED_TARGET_CURVE_STRIDE);
        t->curve[i].target = (int)le32(buf + i * MLAED_TARGET_CURVE_STRIDE + 4);
    }

    if (!ret) {
        ret = read_span(fd, word, 4, MLAED_DAMPING_OFF);
        t->damping = (float)le32(word) / 256.0f;
    }

    if (!ret) {
        ret = read_span(fd, word, 4, MLAED_TOLERANCE_OFF);
        t->tolerance = (float)le32(word);
    }

    if (!ret) {
        ret = read_span(fd, word, 4, MLAED_LOG_LADDER_OFF);
        t->log_ladder = le_f32(word);
    }

    /* 366 x {gain Q8, line_count}, one span. */
    if (!ret) {
        size_t len = MLAED_EXPOSURE_TABLE_COUNT * MLAED_EXPOSURE_TABLE_STRIDE;
        uint8_t *raw = malloc(len);

        if (!raw) {
            ret = -ENOMEM;
        } else {
            ret = read_span(fd, raw, len, MLAED_EXPOSURE_TABLE_OFF);
            for (unsigned int i = 0; !ret && i < MLAED_EXPOSURE_TABLE_COUNT; i++) {
                t->table[i].gain_q8 = le32(raw + i * MLAED_EXPOSURE_TABLE_STRIDE);
                t->table[i].line_count = le32(raw + i * MLAED_EXPOSURE_TABLE_STRIDE + 4);
            }

            free(raw);
        }
    }

    close(fd);
    if (ret) {
        ae_tuning_free(t);

        return ret;
    }

    t->table_len = MLAED_EXPOSURE_TABLE_COUNT;
    t->index_min = 1;
    t->index_max = (int)t->table_len - 1;

    return 0;
}

void ae_tuning_free(struct ae_tuning *t)
{
    free(t->table);
    t->table = NULL;
    t->table_len = 0;
}
