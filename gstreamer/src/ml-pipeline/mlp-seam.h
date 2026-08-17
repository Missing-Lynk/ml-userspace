/**
 * @file mlp-seam.h
 * @brief Cross-fade over the overlap band that two independently decoded tiles both carry.
 *
 * Free of glib and GStreamer, so the kernel builds standalone against the equivalence test
 * in userspace/tests/seam-blend.c.
 */
#ifndef MLP_SEAM_H
#define MLP_SEAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Luma rows the two tiles both carry. Every split-capable entry of the vendor's pattern table
 * uses this value at every interior tile boundary, for 1, 2, 3 and 4 tiles and at every
 * resolution. seam_blend_supported() rejects any other band height, because the kernel's
 * divide by the row count is a compile-time shift.
 */
#define SEAM_OVER        32

/** Band height of the luma plane: the whole band. */
#define SEAM_ROWS_LUMA   SEAM_OVER

/** Band height of each chroma plane: half the band, the planes being half-height. */
#define SEAM_ROWS_CHROMA (SEAM_OVER / 2)

/**
 * @brief Whether seam_blend_plane() can serve a band of this height.
 * @param rows  Rows in the plane's band.
 * @return true for the luma and chroma band heights, false for anything else.
 */
bool seam_blend_supported(int rows);

/**
 * @brief Fill in the per-row cross-fade weights for a band.
 *
 * weights[row] is the weight given to the current tile; the previous tile takes
 * rows - weights[row]. weights[0] is 0 and weights[rows - 1] is rows, so the band opens on
 * the previous tile alone and closes on the current tile alone.
 *
 * @param weights  Output buffer of at least @p rows entries.
 * @param rows     Rows in the plane's band.
 */
void seam_weight_table(uint8_t *weights, int rows);

/**
 * @brief Cross-fade one plane of the band, the previous tile fading into the current one.
 *
 * @p dst may alias @p prev, which is what blending in place inside the composite does: output
 * row r depends only on input row r, so the two are never assumed to be distinct.
 *
 * @param dst          Destination band, @p rows rows of @p width bytes.
 * @param dst_stride   Destination row stride in bytes.
 * @param prev         Previous tile's copy of the band.
 * @param prev_stride  Its row stride in bytes.
 * @param cur          Current tile's copy of the band.
 * @param cur_stride   Its row stride in bytes.
 * @param width        Bytes per row.
 * @param rows         Rows in the band; must satisfy seam_blend_supported().
 */
void seam_blend_plane(uint8_t *dst, int dst_stride,
                      const uint8_t *prev, int prev_stride,
                      const uint8_t *cur, int cur_stride,
                      int width, int rows);

#endif /* MLP_SEAM_H */
