/*
 * ml-aed decision law.
 *
 *      delta = luma_target - current_luma
 *      top index with delta > 0: settled, no step
 *      |delta| <= 5: settled
 *      else step = truncf((50/256) * 77.893997 * log10f(target / current))
 *           zero step with a nonzero log takes sign(log) and arms a one-
 *           decision skip; the index clamps to [1, 365]
 *
 * Metering: the 36x16 grid decimates to 9x8 by sampling each block's top-left
 * zone, each sampled zone is a Bayer population average, and the mean of those
 * 72 is the metered luma. Replaying the vendor's per-cell buffers gives
 * 38.347222 against its recorded current_luma of 38.347221.
 *
 * The luma target is a five-knot curve over exp_index, linearly interpolated
 * and clamped outside the knots. Both vendor captures sit past the last knot,
 * target 41.
 */
#include <math.h>

#include <stdint.h>
#include <stdlib.h>

#include "ml-aed-core.h"

uint32_t mlaed_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rro_count(const uint8_t *rro, unsigned int col,
              unsigned int row, unsigned int channel)
{
    return mlaed_get_le32(rro + col * RRO_COL_STRIDE + (row * 4 + channel) * 4);
}

static uint32_t rro_sum(const uint8_t *rro, unsigned int col, unsigned int row,
            unsigned int channel)
{
    return mlaed_get_le32(rro + col * RRO_COL_STRIDE + RRO_BLOCK +
                  (row * 4 + channel) * 4);
}

/*
 * Zone luma: a Bayer population average, not a BT.601 luma, so it needs no
 * red-versus-blue assignment. Channel mean is sum / (count + 1); the four are
 * weighted 512, 1024 on the green pair, 512, then shifted right by 11 (blob
 * 0xba5ec, shift at 0xba5f8).
 *
 * The greens are summed at full width rather than pre-averaged with a
 * truncating shift: the pre-averaged form reproduces 68 of the vendor's 72
 * cells and every miss is one low, the cost of dropping the green pair's odd
 * bit.
 */
static float zone_luma(const uint8_t *rro, unsigned int col, unsigned int row)
{
    uint32_t mean[4];

    for (unsigned int ch = 0; ch < 4; ch++) {
        uint32_t count = rro_count(rro, col, row, ch);

        mean[ch] = rro_sum(rro, col, row, ch) / (count + 1);
    }

    return (float)((512 * mean[0] + 512 * mean[1] +
            512 * mean[2] + 512 * mean[3]) >> 11);
}

/*
 * The grid is decimated, not averaged: rows 0, 2, ... 14 and columns 0, 4,
 * ... 32, so each 9x8 cell is the top-left zone of its 4x2 block. The vendor's
 * per-zone weight table is uniform, so its weighted mean and this plain mean
 * over the 72 are the same number.
 */
float ae_metered_luma(const uint8_t *rro)
{
    float total = 0.0f;

    for (unsigned int br = 0; br < BIN_ROWS; br++) {
        for (unsigned int bc = 0; bc < BIN_COLS; bc++) {
            total += zone_luma(rro, bc * 4, br * 2);
        }
    }

    return total / (float)(BIN_COLS * BIN_ROWS);
}

/* current_luma = min(metered * scale, 255); the vendor scale reads 1.0. */
float ae_current_luma(float metered)
{
    return metered > 255.0f ? 255.0f : metered;
}

int ae_luma_target(const struct ae_tuning *t, int exp_index)
{
    const typeof(t->curve[0]) *ae_target_curve = t->curve;

    if (exp_index <= ae_target_curve[0].index) {
        return ae_target_curve[0].target;
    }

    if (exp_index >= ae_target_curve[MLAED_TARGET_CURVE_COUNT - 1].index) {
        return ae_target_curve[MLAED_TARGET_CURVE_COUNT - 1].target;
    }

    for (unsigned int i = 1; i < MLAED_TARGET_CURVE_COUNT; i++) {
        if (exp_index < ae_target_curve[i].index) {
            int x0 = ae_target_curve[i - 1].index;
            int y0 = ae_target_curve[i - 1].target;
            int x1 = ae_target_curve[i].index;
            int y1 = ae_target_curve[i].target;

            return y0 + (y1 - y0) * (exp_index - x0) / (x1 - x0);
        }
    }

    return ae_target_curve[MLAED_TARGET_CURVE_COUNT - 1].target;
}

int ae_decide(const struct ae_tuning *t, struct ae_state *st, float current_luma)
{
    float target = (float)ae_luma_target(t, st->exp_index);
    float delta = target - current_luma;
    float log_term;
    int step;

    if (st->exp_index >= t->index_max && delta > 0.0f) {
        st->settle_counter++;

        return 0;
    }

    if (fabsf(delta) <= t->tolerance) {
        st->settle_counter++;

        return 0;
    }

    if (st->skip_countdown != 0) {
        st->skip_countdown--;

        return 0;
    }

    log_term = log10f(target / current_luma);
    step = (int)truncf(t->damping * t->log_ladder * log_term);
    if (step == 0 && log_term != 0.0f) {
        step = log_term > 0.0f ? 1 : -1;
        st->skip_countdown = AE_MIN_STEP_SKIP;
    }

    st->settle_counter = 0;
    st->exp_index += step;

    if (st->exp_index < t->index_min) {
        st->exp_index = t->index_min;
    }

    if (st->exp_index > t->index_max) {
        st->exp_index = t->index_max;
    }

    return step;
}

/*
 * gain(code) = 2^(code >> 4) * (16 + (code & 0xf)) / 16. Live capture: table
 * gain 15.383, sensor code 0x3e = 15.0. 0x5f is the hardware ceiling; higher
 * codes wedge the sensor.
 */
unsigned int ae_sensor_gain_code(uint32_t gain_q8)
{
    unsigned int best = 0;

    for (unsigned int code = 0; code <= 0x5f; code++) {
        uint32_t q8 = (256u << (code >> 4)) * (16 + (code & 0xf)) / 16;

        if (q8 <= gain_q8) {
            best = code;
        }
    }

    return best;
}

int ae_tone_scalar_q8(const struct ae_tuning *t, int exp_index, float current_luma)
{
    double ratio;
    double target;
    double scalar;
    double ev_here;
    double ev_prev;
    int table_index;

    if (t->table_len < 2 || current_luma <= 0.0f) {
        return exp_index << 8;
    }

    /*
     * The step ratio is taken backwards so it stays defined at the last table
     * entry, which is where the lens-covered capture sits: at the ceiling there
     * is no next entry, and that is exactly the point where the luma error is
     * large and the correction matters.
     */
    table_index = exp_index;

    if (table_index < 1) {
        table_index = 1;
    }

    if ((size_t)table_index >= t->table_len) {
        table_index = (int)t->table_len - 1;
    }

    ev_here = (double)t->table[table_index].gain_q8 *
              (double)t->table[table_index].line_count;
    ev_prev = (double)t->table[table_index - 1].gain_q8 *
              (double)t->table[table_index - 1].line_count;

    if (ev_prev <= 0.0 || ev_here <= ev_prev) {
        return exp_index << 8;
    }

    ratio = ev_here / ev_prev;
    target = (double)ae_luma_target(t, exp_index);

    if (target <= 0.0) {
        return exp_index << 8;
    }

    scalar = (double)exp_index + log(target / (double)current_luma) / log(ratio);

    if (scalar < 0.0) {
        scalar = 0.0;
    }

    if (scalar > AE_TONE_SCALAR_MAX) {
        scalar = AE_TONE_SCALAR_MAX;
    }

    return (int)floor(scalar) << 8;
}

/*
 * The vendor rounds the half-period count two ways and picks between them at run time: to nearest
 * when the working gain has headroom (at least twice the table entry's), truncating otherwise.
 * Truncating shortens the exposure, so the compensation raises gain, which is always available;
 * rounding up needs headroom that may not be there. At the point ml-aed actuates, the working gain
 * IS the table entry's, so the threshold never passes and this always truncates. The branch is
 * kept because the comparison is the vendor's and a future manual-gain path would reach it.
 */
static double flicker_periods(double exposure_s, double half_period, int have_headroom)
{
    double n = exposure_s / half_period;

    return have_headroom ? floor(n + 0.5) : floor(n);
}

int ae_flicker_snap(int mains_hz, unsigned int line_ns, uint32_t *lines, uint32_t *gain_q8)
{
    double half_period;
    double exposure;
    double line_s;
    double snapped;
    double n;
    uint32_t new_lines;

    if (mains_hz != 50 && mains_hz != 60) {
        return 0;
    }

    if (!line_ns || !*lines || !*gain_q8) {
        return 0;
    }

    line_s = (double)line_ns / 1e9;
    exposure = (double)*lines * line_s;
    half_period = 1.0 / (2.0 * mains_hz);

    /*
     * The vendor's early-out. Below one half-period there is no whole number of periods to snap
     * to that is not zero, and lengthening the exposure is not this function's job.
     */
    if (exposure <= half_period) {
        return 0;
    }

    n = flicker_periods(exposure, half_period, 0);

    if (n < 1.0) {
        return 0;
    }

    snapped = n * half_period;
    new_lines = (uint32_t)(snapped / line_s);

    if (!new_lines || new_lines == *lines) {
        return 0;
    }

    /*
     * Gain carries the exposure that was removed. The product is held on the requested exposure,
     * not on the truncated line count, so the residual of that truncation is not amplified.
     */
    double compensated = (double)*gain_q8 * exposure / snapped;

    if (compensated > (double)UINT32_MAX) {
        return 0;
    }

    *lines = new_lines;
    *gain_q8 = (uint32_t)(compensated + 0.5);

    return 1;
}

void ae_health_update(struct ae_health *h, int step, float current_luma, int target)
{
    h->decisions++;

    if (step) {
        h->moves++;
    }

    h->err_sum += fabsf(current_luma - (float)target);
}

unsigned int ae_health_hold_pct(const struct ae_health *h)
{
    if (!h->decisions) {
        return 100;
    }

    return 100 - (h->moves * 100 / h->decisions);
}

float ae_health_mean_err(const struct ae_health *h)
{
    if (!h->decisions) {
        return 0.0f;
    }

    return h->err_sum / (float)h->decisions;
}

int ae_banding_parse(const char *text)
{
    int hz;

    if (text[0] == '\0' || text[0] == '\n') {
        return 50;
    }

    hz = atoi(text);

    return hz == 50 || hz == 60 ? hz : 0;
}
