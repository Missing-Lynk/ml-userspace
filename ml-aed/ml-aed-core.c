/*
 * ml-aed decision law. See ml-aed-core.h for what this half is and is not.
 *
 * Decision law:
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

#include "ml-aed-core.h"

/* The five-knot luma target curve over exp_index, blob 0xba560. */
static const struct {
    int index;
    int target;
} ae_target_curve[] = {
    { 20, 54 }, { 80, 54 }, { 120, 52 }, { 150, 49 }, { 200, 41 },
};

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

int ae_luma_target(int exp_index)
{
    if (exp_index <= ae_target_curve[0].index) {
        return ae_target_curve[0].target;
    }

    if (exp_index >= ae_target_curve[AE_TARGET_KNOTS - 1].index) {
        return ae_target_curve[AE_TARGET_KNOTS - 1].target;
    }

    for (int i = 1; i < AE_TARGET_KNOTS; i++) {
        if (exp_index < ae_target_curve[i].index) {
            int x0 = ae_target_curve[i - 1].index;
            int y0 = ae_target_curve[i - 1].target;
            int x1 = ae_target_curve[i].index;
            int y1 = ae_target_curve[i].target;

            return y0 + (y1 - y0) * (exp_index - x0) / (x1 - x0);
        }
    }

    return ae_target_curve[AE_TARGET_KNOTS - 1].target;
}

int ae_decide(struct ae_state *st, float current_luma)
{
    float target = (float)ae_luma_target(st->exp_index);
    float delta = target - current_luma;
    float log_term;
    int step;

    if (st->exp_index >= AE_INDEX_MAX && delta > 0.0f) {
        st->settle_counter++;

        return 0;
    }

    if (fabsf(delta) <= AE_TOLERANCE) {
        st->settle_counter++;

        return 0;
    }

    if (st->skip_countdown != 0) {
        st->skip_countdown--;

        return 0;
    }

    log_term = log10f(target / current_luma);
    step = (int)truncf(AE_DAMPING * AE_LOG_LADDER * log_term);
    if (step == 0 && log_term != 0.0f) {
        step = log_term > 0.0f ? 1 : -1;
        st->skip_countdown = AE_MIN_STEP_SKIP;
    }

    st->settle_counter = 0;
    st->exp_index += step;

    if (st->exp_index < AE_INDEX_MIN) {
        st->exp_index = AE_INDEX_MIN;
    }

    if (st->exp_index > AE_INDEX_MAX) {
        st->exp_index = AE_INDEX_MAX;
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

int ae_tone_scalar_q8(int exp_index)
{
    return exp_index << 8;
}
