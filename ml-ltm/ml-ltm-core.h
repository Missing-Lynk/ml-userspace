/*
 * ml-ltm-core: the vendor's per-frame LTM/CLAHE page computation, decoded.
 *
 * Pure: no file descriptors, no clock, no globals. A caller does the I/O (read the histogram
 * from the ISP's ltm_hist, read the current page back from ltm_page, write the new page to
 * ltm_page). tests/ltm-page.c drives this on the host.
 *
 * The algorithm is the vendor's `gtm_algo_process_optimation_with_neo_v2` Path A, recovered in
 * plans/done/au-ltm-page-algorithm.md and out/au-ltm-neo-v2/report.md. Per tile: fold the 256-bin
 * histogram to 128 (S1), normalise by the accumulation count (S2), apply the per-bin sloped clip
 * limit (S4/S5), redistribute the clipped mass (S6), then the CDF scaled to the page range,
 * temporally blended against the PREVIOUS published page (S9). The blend against the previous page,
 * not an identity ramp, is what makes the page a temporal IIR and is why the driver double-buffers.
 *
 * The arithmetic here is fully grounded in the vendor library (every stage decoded from
 * neo_v2). Two inputs, the clip-shaping tables LUT1840 (gathered by histogram count) and TBL1864
 * (128 entries by bin position, both u16), are mmap'd from /dev/ar_sys on the vendor; where the
 * vendor uploads them FROM (a library table, a tuning-blob field, or the ISP kernel) is under
 * investigation in out/au-ltm-tables/. This core takes them as an input struct so it stays
 * source-agnostic, but the driver must fill that struct from a library/blob-derived generator,
 * NOT from a device capture. Until that source is pinned this producer cannot ship grounded.
 */
#ifndef ML_LTM_CORE_H
#define ML_LTM_CORE_H

#include <stdint.h>

/* Page geometry, mirroring the driver's ar-isp-regs.h. */
#define LTM_TILES        64
#define LTM_HIST_BINS    256        /* per tile, in ltm_hist */
#define LTM_SAMPLES      128        /* per tile, in the page (the folded histogram) */
#define LTM_PAGE_STRIDE  0x100      /* bytes per tile in the page (128 u16 + gap) */
#define LTM_PAGE_WORDS   (LTM_TILES * (LTM_PAGE_STRIDE / 2))
#define LTM_SAMPLE_MAX   0x3ff

/*
 * The runtime parameters neo_v2 reads from its input struct. On the shipped 1920x1080 config the
 * folded tile sum is 8040 = 120 * 67, so pixels_per_tile is 8040 with accum 1. clip_frac and blend
 * are runtime-smoothed on the vendor; a producer reads them from the AE/LTM context or pins them.
 */
struct ltm_params {
    int width;
    int height;
    int tiles_x;
    int tiles_y;
    int bayer;          /* input+32; halved before use */
    int accum;          /* input+36; >= 1 */
    float clip_frac;    /* input+44 */
    float blend;        /* input+48; 0..1, the temporal weight on the fresh curve */
    int clip_off;       /* input+88 */
};

/*
 * The clip-shaping tables, device data. lut is indexed by a folded histogram count (up to the tile
 * sum), so it must cover [0, max_count]; tbl is 128 entries by bin position. Element type u16.
 */
struct ltm_tables {
    const uint16_t *lut;        /* LUT1840, length lut_len */
    unsigned int lut_len;
    const uint16_t *tbl;        /* TBL1864, length LTM_SAMPLES */
};

/*
 * pixels_per_tile, the way neo_v2 computes it (0x28a160): bh = bayer/2, then
 * (width/tiles_x/bh) * (height/tiles_y/bh) / accum, all integer, truncating.
 */
int ltm_pixels_per_tile(const struct ltm_params *p);

/*
 * The frame-0 page seed the vendor latches before the first blend: every tile the identity ramp
 * floor(pos * 1023 / 127). Matches the driver's default identity page, so a producer that starts
 * from the driver default needs no separate seed.
 */
void ltm_seed_identity(uint16_t *page);

/*
 * One frame. hist is LTM_TILES*LTM_HIST_BINS u16 (the ltm_hist buffer). prev is the currently
 * published page (LTM_PAGE_WORDS u16, the driver's read of ltm_page); out is the new page to
 * publish (same layout). prev and out may not alias. Returns 0, or -1 if a table is too short for
 * the histogram counts it is asked to index.
 */
int ltm_page_compute(const uint16_t *hist, const struct ltm_params *p,
                     const struct ltm_tables *t, const uint16_t *prev, uint16_t *out);

#endif /* ML_LTM_CORE_H */
