/**
 * @file seam-blend.c
 * @brief Host test: the seam cross-fade kernel against the vendor's arithmetic.
 *
 * Cross-built for aarch64 and run under qemu-user, because the kernel is NEON. It asserts:
 *
 *   1. the weight table reproduces the vendor's two sequences, including the midpoint skip,
 *   2. both band ends are whole tiles, so neither boundary of the band can step,
 *   3. the weights climb by one or two per row and never reverse,
 *   4. the shipped kernel is byte-identical to a scalar transcription of the vendor's
 *      expression over the entire 256 x 256 x rows input space, both planes,
 *   5. a NEON transcription that keeps the vendor's 32-bit widen, scale multiply and
 *      saturating narrow agrees with both over the same space,
 *   6. the accumulator never reaches the saturation the vendor's uqshrn would apply,
 *   7. a blend whose destination aliases the previous tile matches a non-aliased one,
 *   8. a black-into-white band produces the exact weight ramp on real geometry.
 *
 * Items 4 and 5 are what license the shipped kernel to drop the widen entirely: at a
 * 32-row overlap the vendor's scale of 8192/32 = 256 against a shift of 13 (12 for chroma)
 * is a divide by the plane's row count, and their rounding addend is the one vrshrn
 * already applies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arm_neon.h>

#include "../gstreamer/src/ml-pipeline/mlp-seam.h"

/* The vendor computes scale from the luma overlap for both planes. */
#define VENDOR_SCALE    (8192 / SEAM_OVER)
#define SHIFT_LUMA      13
#define SHIFT_CHROMA    12

/* One (prev, cur) pair per byte position, so a single row covers the whole input space. */
#define SPACE           (256 * 256)

struct plane {
    const char *name;
    int rows;
    unsigned shift;
};

static const struct plane planes[] = {
    { "luma",   SEAM_ROWS_LUMA,   SHIFT_LUMA   },
    { "chroma", SEAM_ROWS_CHROMA, SHIFT_CHROMA },
};

static int fail(const char *what)
{
    fprintf(stderr, "seam-blend FAIL: %s\n", what);

    return 1;
}

/* The vendor's expression, transcribed literally: accumulate into the rounding addend,
 * widen, multiply by the scale, narrow with saturation.
 */
static uint8_t reference_pixel(unsigned prev, unsigned cur, unsigned w, unsigned rows, unsigned shift)
{
    unsigned acc = prev * (rows - w) + cur * w + rows / 2;
    unsigned out = (acc * VENDOR_SCALE) >> shift;

    return (uint8_t)(out > 255 ? 255 : out);
}

/* The same expression in NEON, keeping the widen to 32 bits and the saturating narrows the
 * shipped kernel drops. Present only so the test can prove the two identical.
 */
#define VENDOR_BLEND_ROW(SHIFT)                                                     \
    do {                                                                            \
        for (int x = 0; x + 16 <= width; x += 16) {                                 \
            uint8x16_t vp = vld1q_u8(prev_row + x);                                 \
            uint8x16_t vc = vld1q_u8(cur_row + x);                                  \
            uint16x8_t lo = vmull_u8(vget_low_u8(vp), vec_prev);                    \
            uint16x8_t hi = vmull_u8(vget_high_u8(vp), vec_prev);                   \
            uint16x4_t out_lo, out_hi;                                              \
                                                                                    \
            lo = vaddq_u16(vmlal_u8(lo, vget_low_u8(vc), vec_cur), vec_round);       \
            hi = vaddq_u16(vmlal_u8(hi, vget_high_u8(vc), vec_cur), vec_round);      \
            out_lo = vqshrn_n_u32(vmull_u16(vget_low_u16(lo), vec_scale), SHIFT);   \
            out_hi = vqshrn_n_u32(vmull_u16(vget_high_u16(lo), vec_scale), SHIFT);  \
            vst1_u8(dst_row + x, vqmovn_u16(vcombine_u16(out_lo, out_hi)));         \
            out_lo = vqshrn_n_u32(vmull_u16(vget_low_u16(hi), vec_scale), SHIFT);   \
            out_hi = vqshrn_n_u32(vmull_u16(vget_high_u16(hi), vec_scale), SHIFT);  \
            vst1_u8(dst_row + x + 8, vqmovn_u16(vcombine_u16(out_lo, out_hi)));     \
        }                                                                           \
    } while (0)

static void vendor_blend_plane(uint8_t *dst, int dst_stride,
                               const uint8_t *prev, int prev_stride,
                               const uint8_t *cur, int cur_stride,
                               int width, int rows)
{
    uint8_t w[SEAM_ROWS_LUMA];
    uint16x4_t vec_scale = vdup_n_u16(VENDOR_SCALE);
    uint16x8_t vec_round = vdupq_n_u16((uint16_t)(rows / 2));

    seam_weight_table(w, rows);

    for (int r = 0; r < rows; r++) {
        uint8x8_t vec_cur = vdup_n_u8(w[r]);
        uint8x8_t vec_prev = vdup_n_u8((uint8_t)(rows - w[r]));
        const uint8_t *prev_row = prev + (long)r * prev_stride;
        const uint8_t *cur_row = cur + (long)r * cur_stride;
        uint8_t *dst_row = dst + (long)r * dst_stride;

        if (rows == SEAM_ROWS_LUMA) {
            VENDOR_BLEND_ROW(SHIFT_LUMA);
        } else {
            VENDOR_BLEND_ROW(SHIFT_CHROMA);
        }
    }
}

static int check_weights(void)
{
    static const uint8_t want_luma[SEAM_ROWS_LUMA] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
        16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    };
    static const uint8_t want_chroma[SEAM_ROWS_CHROMA] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16,
    };
    uint8_t w[SEAM_ROWS_LUMA];

    seam_weight_table(w, SEAM_ROWS_LUMA);
    if (memcmp(w, want_luma, sizeof want_luma) != 0) {
        return fail("luma weight sequence");
    }

    seam_weight_table(w, SEAM_ROWS_CHROMA);
    if (memcmp(w, want_chroma, sizeof want_chroma) != 0) {
        return fail("chroma weight sequence");
    }

    for (size_t p = 0; p < sizeof planes / sizeof planes[0]; p++) {
        int rows = planes[p].rows;

        seam_weight_table(w, rows);

        if (w[0] != 0 || w[rows - 1] != rows) {
            return fail("band ends are not whole tiles");
        }

        for (int r = 1; r < rows; r++) {
            int step = w[r] - w[r - 1];

            if (step < 1 || step > 2) {
                return fail("weight ramp reverses or jumps");
            }
        }
    }

    return 0;
}

/* Every (prev, cur) pair against every row of the band, for both planes. Input rows carry
 * stride 0, so all band rows read the same pair table and each output row isolates one
 * weight.
 */
static int check_equivalence(void)
{
    uint8_t *prev = malloc(SPACE);
    uint8_t *cur = malloc(SPACE);
    uint8_t *got = malloc((size_t)SPACE * SEAM_ROWS_LUMA);
    uint8_t *vendor = malloc((size_t)SPACE * SEAM_ROWS_LUMA);
    uint8_t w[SEAM_ROWS_LUMA];
    int rc = 0;

    if (!prev || !cur || !got || !vendor) {
        return fail("out of memory");
    }

    for (int i = 0; i < SPACE; i++) {
        prev[i] = (uint8_t)(i >> 8);
        cur[i] = (uint8_t)(i & 0xff);
    }

    for (size_t p = 0; p < sizeof planes / sizeof planes[0] && rc == 0; p++) {
        int rows = planes[p].rows;
        unsigned shift = planes[p].shift;

        seam_weight_table(w, rows);
        seam_blend_plane(got, SPACE, prev, 0, cur, 0, SPACE, rows);
        vendor_blend_plane(vendor, SPACE, prev, 0, cur, 0, SPACE, rows);

        for (int r = 0; r < rows && rc == 0; r++) {
            for (int i = 0; i < SPACE; i++) {
                uint8_t want = reference_pixel(prev[i], cur[i], w[r], (unsigned)rows, shift);
                size_t at = (size_t)r * SPACE + i;

                if (got[at] != want || vendor[at] != want) {
                    fprintf(stderr, "seam-blend: %s row %d prev %u cur %u: "
                            "reference %u, shipped %u, vendor-form %u\n",
                            planes[p].name, r, prev[i], cur[i], want, got[at], vendor[at]);
                    rc = fail("kernel differs from the vendor's arithmetic");
                    break;
                }
            }
        }
    }

    free(prev);
    free(cur);
    free(got);
    free(vendor);

    return rc;
}

/* The vendor's uqshrn saturates; the shipped kernel's vrshrn does not. They can only agree
 * if the pre-shift accumulator stays under the saturation point at its maximum input.
 */
static int check_no_saturation(void)
{
    for (size_t p = 0; p < sizeof planes / sizeof planes[0]; p++) {
        unsigned rows = (unsigned)planes[p].rows;
        unsigned acc = 255 * rows + rows / 2;

        if (acc > 0xffff) {
            return fail("accumulator leaves 16 bits");
        }

        if ((acc * VENDOR_SCALE) >> planes[p].shift > 255) {
            return fail("accumulator reaches saturation");
        }
    }

    return 0;
}

/* An in-place blend, which is what a band cross-faded inside the composite does. */
static int check_aliasing(void)
{
    enum { WIDTH = 1920 };
    static uint8_t band[SEAM_ROWS_LUMA][WIDTH];
    static uint8_t other[SEAM_ROWS_LUMA][WIDTH];
    static uint8_t apart[SEAM_ROWS_LUMA][WIDTH];

    for (int r = 0; r < SEAM_ROWS_LUMA; r++) {
        for (int x = 0; x < WIDTH; x++) {
            band[r][x] = (uint8_t)(x * 7 + r);
            other[r][x] = (uint8_t)(x * 13 + r * 5);
        }
    }

    seam_blend_plane(apart[0], WIDTH, band[0], WIDTH, other[0], WIDTH, WIDTH, SEAM_ROWS_LUMA);
    seam_blend_plane(band[0], WIDTH, band[0], WIDTH, other[0], WIDTH, WIDTH, SEAM_ROWS_LUMA);

    if (memcmp(band, apart, sizeof apart) != 0) {
        return fail("in-place blend differs from an out-of-place one");
    }

    return 0;
}

/* Real band geometry, one tile black and the other white, so every output byte is the row's
 * weight scaled to 0..255 and the ends are exactly the two tiles.
 */
static int check_ramp(void)
{
    enum { WIDTH = 1920 };
    static uint8_t black[SEAM_ROWS_LUMA][WIDTH];
    static uint8_t white[SEAM_ROWS_LUMA][WIDTH];
    static uint8_t out[SEAM_ROWS_LUMA][WIDTH];
    uint8_t w[SEAM_ROWS_LUMA];

    memset(black, 0x00, sizeof black);
    memset(white, 0xff, sizeof white);
    seam_weight_table(w, SEAM_ROWS_LUMA);
    seam_blend_plane(out[0], WIDTH, black[0], WIDTH, white[0], WIDTH, WIDTH, SEAM_ROWS_LUMA);

    for (int r = 0; r < SEAM_ROWS_LUMA; r++) {
        unsigned want = (255u * w[r] + SEAM_ROWS_LUMA / 2) / SEAM_ROWS_LUMA;

        for (int x = 0; x < WIDTH; x++) {
            if (out[r][x] != want) {
                return fail("black-into-white band is not the weight ramp");
            }
        }
    }

    if (out[0][0] != 0x00 || out[SEAM_ROWS_LUMA - 1][0] != 0xff) {
        return fail("band ends are not the source tiles");
    }

    return 0;
}

int main(void)
{
    if (check_weights() || check_no_saturation() || check_equivalence() ||
        check_aliasing() || check_ramp()) {
        return 1;
    }

    printf("seam-blend OK\n");

    return 0;
}
