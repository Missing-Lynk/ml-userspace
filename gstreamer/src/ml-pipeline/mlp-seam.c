/**
 * @file mlp-seam.c
 * @brief Cross-fade over the overlap band that two independently decoded tiles both carry.
 *
 * Per band row the output is round((prev * (N - weight) + cur * weight) / N), with N the row
 * count of the plane's band, so each row is a fixed linear blend of the two tiles' copies of
 * that row and the band walks from one tile to the other.
 *
 * N is 32 for luma and 16 for chroma, both powers of two, so the divide is a rounding
 * narrowing shift and the accumulator never leaves 16 bits: prev * (N - weight) + cur * weight
 * peaks at 255 * N = 8160, and the rounding addend N/2 is the one vrshrn already applies.
 * Nothing widens to 32 bits and nothing saturates.
 */
#include "mlp-seam.h"

#include <arm_neon.h>

/**
 * @brief Blend one row of the band.
 *
 * The rounding narrow is spelled out as add-then-shift rather than vrshrn_n_u16, whose shift
 * is an immediate: that would force @p shift to be a literal, and so either a macro or one
 * copy of this loop per band height. vshlq_u16 takes its count from a vector, so a plain
 * parameter serves both planes and the function still compiles unoptimised. The two forms are
 * arithmetically identical, and seam-blend.c proves it over the whole input space.
 */
static void blend_row(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, int width,
                      int prev_weight, int cur_weight, int shift)
{
    const uint8x8_t prev_lanes = vdup_n_u8((uint8_t)prev_weight);
    const uint8x8_t cur_lanes = vdup_n_u8((uint8_t)cur_weight);
    const unsigned round_addend = 1u << (shift - 1);
    const uint16x8_t round_lanes = vdupq_n_u16((uint16_t)round_addend);
    const int16x8_t shift_lanes = vdupq_n_s16((int16_t)-shift);
    int i;

    for (i = 0; i + 16 <= width; i += 16) {
        uint8x16_t prev_px = vld1q_u8(prev + i);
        uint8x16_t cur_px = vld1q_u8(cur + i);
        uint16x8_t acc_low = vmull_u8(vget_low_u8(prev_px), prev_lanes);
        uint16x8_t acc_high = vmull_u8(vget_high_u8(prev_px), prev_lanes);

        acc_low = vmlal_u8(acc_low, vget_low_u8(cur_px), cur_lanes);
        acc_high = vmlal_u8(acc_high, vget_high_u8(cur_px), cur_lanes);
        acc_low = vshlq_u16(vaddq_u16(acc_low, round_lanes), shift_lanes);
        acc_high = vshlq_u16(vaddq_u16(acc_high, round_lanes), shift_lanes);
        vst1q_u8(dst + i, vcombine_u8(vmovn_u16(acc_low), vmovn_u16(acc_high)));
    }

    /* Continues where the vector loop stopped; it must not restart at 0. dst may alias prev,
     * so a second pass over a blended pixel would blend it again.
     */
    for (; i < width; i++) {
        unsigned acc = prev[i] * (unsigned)prev_weight + cur[i] * (unsigned)cur_weight;

        dst[i] = (uint8_t)((acc + round_addend) >> shift);
    }
}

bool seam_blend_supported(int rows)
{
    return rows == SEAM_ROWS_LUMA || rows == SEAM_ROWS_CHROMA;
}

/*
 * The weight climbs by one per row but skips the value just past the midpoint, so the ramp
 * spans the whole 0..rows range in exactly rows steps and both ends land on a whole tile. A
 * plain i would end at rows - 1 and leave a step where the band meets the current tile.
 */
void seam_weight_table(uint8_t *weights, int rows)
{
    for (int i = 0; i < rows; i++) {
        weights[i] = (uint8_t)((i <= rows / 2) ? i : i + 1);
    }
}

void seam_blend_plane(uint8_t *dst, int dst_stride,
                      const uint8_t *prev, int prev_stride,
                      const uint8_t *cur, int cur_stride,
                      int width, int rows)
{
    uint8_t weights[SEAM_ROWS_LUMA];
    int shift = rows == SEAM_ROWS_LUMA ? 5 : 4;     /* log2(rows); the divide by N */

    if (!seam_blend_supported(rows)) {
        return;
    }

    seam_weight_table(weights, rows);

    for (int i = 0; i < rows; i++) {
        int cur_weight = weights[i];
        int prev_weight = rows - cur_weight;
        uint8_t *dst_row = dst + (size_t)i * dst_stride;
        const uint8_t *prev_row = prev + (size_t)i * prev_stride;
        const uint8_t *cur_row = cur + (size_t)i * cur_stride;

        blend_row(dst_row, prev_row, cur_row, width, prev_weight, cur_weight, shift);
    }
}
