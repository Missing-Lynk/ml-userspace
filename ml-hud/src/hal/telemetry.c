/** @file telemetry.c @brief Implementation; see telemetry.h */
#include "telemetry.h"
#include "board.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>

static int g_cell_count;   /* latched after the first valid reading */

static int read_int_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    char buffer[32];
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (count <= 0) {
        return -1;
    }

    buffer[count] = '\0';
    return atoi(buffer);
}

/* Read a 64-bit unsigned counter file (netdev byte counters exceed 2^31 within a session). Returns
 * -1 on any failure so the caller can distinguish "no reading" from a genuine value. */
static long long read_u64_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    char buffer[32];
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (count <= 0) {
        return -1;
    }

    buffer[count] = '\0';
    return strtoll(buffer, NULL, 10);
}

static uint32_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* Incoming-video bitrate: the SDIO netdev RX byte counter differentiated over wall time. The counter
 * is monotonic per boot, so a smaller reading than last means it was reset (driver reload) - we skip
 * that tick and re-baseline. The first call only baselines (no interval yet). */
static void read_bitrate(const board_profile_t *board, telemetry_t *out)
{
    static long long last_bytes = -1;
    static uint32_t  last_ms;

    long long bytes = read_u64_file(board->sdio_rx_bytes_path);
    uint32_t now = mono_ms();
    if (bytes < 0) {
        last_bytes = -1;   /* source gone: re-baseline when it returns */
        return;
    }

    uint32_t elapsed = now - last_ms;
    if (last_bytes >= 0 && bytes >= last_bytes && elapsed >= 250) {
        out->have_bitrate = 1;
        out->bitrate_mbps = (float) ((bytes - last_bytes) * 8) / ((double) elapsed * 1000.0);
    }

    last_bytes = bytes;
    last_ms = now;
}

/** @brief Estimate the pack cell count from the pack voltage. */
/* Assumes ~3.7V nominal per cell, clamped to the board's supported cell range. A discharged pack is
 * ambiguous, so this relies on detection happening near plug-in (like stock FPV goggles).
 */
static int detect_cell_count(float pack_volts)
{
    const board_profile_t *board = board_current();
    int cells = (int)(pack_volts / 3.7f + 0.5f);
    if (cells < board->battery_cell_min) {
        cells = board->battery_cell_min;
    }

    if (cells > board->battery_cell_max) {
        cells = board->battery_cell_max;
    }

    return cells;
}

void telemetry_read(telemetry_t *out)
{
    out->have_battery = 0;
    out->have_sdcard = 0;
    out->pack_volts = 0.0;
    out->cell_count = 0;
    out->sd_free_gb = 0.0;
    out->have_temp = 0;
    out->temp_c = 0;
    out->have_bitrate = 0;
    out->bitrate_mbps = 0.0f;

    const board_profile_t *board = board_current();

    /* The driver's PROCESSED node reports calibrated ADC-pin millivolts (per-channel gain+offset);
     * the board divider turns that into pack volts.
     */
    int pin_millivolts = read_int_file(board->battery_adc_input_path);
    if (pin_millivolts >= 0) {
        float pack_volts = (float)(pin_millivolts * board->battery_divider / 1000.0);
        if (g_cell_count == 0 && pack_volts >= board->battery_cell_min * 3.0f) {
            g_cell_count = detect_cell_count(pack_volts);
            fprintf(stderr, "telemetry: detected %dS pack (%.2fV, ~%.2fV/cell)\n",
                    g_cell_count, pack_volts, pack_volts / g_cell_count);
        }
        out->have_battery = 1;
        out->pack_volts = pack_volts;
        out->cell_count = g_cell_count;
    }

    struct statvfs filesystem;
    if (statvfs(board->sdcard_mount, &filesystem) == 0) {
        out->have_sdcard = 1;
        out->sd_free_gb = (float)((double)filesystem.f_bavail * (double)filesystem.f_frsize / 1e9);
    }

    int temp_c = read_int_file(board->temp_path);   /* the file already holds whole degC */
    if (temp_c >= 0) {
        out->have_temp = 1;
        out->temp_c = temp_c;
    }

    read_bitrate(board, out);
}
