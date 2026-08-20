/*
 * ml-ltm-core: the vendor's neo_v2 Path-A LTM/CLAHE page arithmetic. See ml-ltm-core.h.
 *
 * Every stage is transcribed from the decoded vendor function (out/au-ltm-neo-v2/report.md,
 * section 9). The two clip-shaping tables are inputs; where they come from is the caller's
 * problem (see the header) and does not change the arithmetic here.
 */
#include <stdint.h>

#include "ml-ltm-core.h"

static int ilog2_u(unsigned int v)
{
    int r = 0;

    while (v > 1) {
        v >>= 1;
        r++;
    }

    return r;
}

int ltm_pixels_per_tile(const struct ltm_params *p)
{
    int bh = p->bayer / 2;
    int w, h, accum;

    if (bh < 1) {
        bh = 1;
    }

    w = (p->width / p->tiles_x) / bh;
    h = (p->height / p->tiles_y) / bh;
    accum = p->accum < 1 ? 1 : p->accum;

    return (w * h) / accum;
}

void ltm_seed_identity(uint16_t *page)
{
    /*
     * The frame-0 seed the vendor latches before the first blend (Path-A configure branch
     * 0x2896b4): every tile the ramp floor(pos * 1023 / 127). Identical to the driver's default
     * identity page, so a producer starting from the driver default needs no separate seed.
     */
    for (unsigned int t = 0; t < LTM_TILES; t++) {
        for (unsigned int i = 0; i < LTM_SAMPLES; i++) {
            page[t * LTM_SAMPLES + i] =
                (uint16_t)((unsigned int)i * LTM_SAMPLE_MAX / (LTM_SAMPLES - 1));
        }
    }
}

int ltm_page_compute(const uint16_t *hist, const struct ltm_params *p,
                     const struct ltm_tables *t, const uint16_t *prev, uint16_t *out)
{
    int ppt = ltm_pixels_per_tile(p);
    int inv16k = ppt > 0 ? 16384 / ppt : 0;              /* integer division, per the vendor */
    int clip_base = (int)((float)ppt * p->clip_frac);
    int w_new = (int)(p->blend * 4096.0f);               /* Q12 on the fresh curve */
    int w_old = (int)((1.0f - p->blend) * 4096.0f) << 4; /* Q16 on the previous page */
    uint16_t h1[LTM_TILES * LTM_SAMPLES];

    /* S1: fold the 256-bin histogram to 128 per tile. */
    for (unsigned int tile = 0; tile < LTM_TILES; tile++) {
        for (unsigned int i = 0; i < LTM_SAMPLES; i++) {
            h1[tile * LTM_SAMPLES + i] =
                (uint16_t)(hist[tile * LTM_HIST_BINS + 2 * i] +
                           hist[tile * LTM_HIST_BINS + 2 * i + 1]);
        }
    }

    /* S2: normalise by the accumulation count, one global carry across the whole buffer. */
    if (p->accum > 1) {
        int sh = ilog2_u((unsigned int)p->accum);
        int msk = (1 << sh) - 1;
        int carry = 0;

        for (unsigned int k = 0; k < LTM_TILES * LTM_SAMPLES; k++) {
            int v = h1[k] + carry;

            carry = v & msk;
            h1[k] = (uint16_t)(v >> sh);
        }
    }

    for (unsigned int tile = 0; tile < LTM_TILES; tile++) {
        uint16_t *bin = &h1[tile * LTM_SAMPLES];
        const uint16_t *pv = &prev[tile * LTM_SAMPLES];
        uint16_t *ov = &out[tile * LTM_SAMPLES];
        int clipped_total = 0;
        int residual, step, rem, cdf;

        /*
         * S4/S5: per-bin sloped clip limit, then clamp the bin to it. p is the table product;
         * the limit computation is carried in 64 bits because (clip_base - clip_off) can be
         * negative and the table product large. The exact vendor width past 32 bits is not
         * pinned (out/au-ltm-neo-v2/report.md hole), so a bit-exact match needs the real tables
         * and a same-frame check; the shifts and operand order here are what was decoded.
         */
        for (unsigned int i = 0; i < LTM_SAMPLES; i++) {
            unsigned int idx = bin[i];
            int64_t prod, lim;

            if (idx >= t->lut_len) {
                return -1;
            }

            prod = (int64_t)t->lut[idx] * t->tbl[i];
            lim = (((int64_t)p->clip_off << 12) +
                   (int64_t)(clip_base - p->clip_off) * prod) >> 12;
            if (lim < bin[i]) {
                bin[i] = (uint16_t)lim;
            }

            clipped_total += bin[i];
        }

        /* S6: redistribute the clipped mass evenly, the remainder one count each to the low bins. */
        residual = ppt - clipped_total;
        step = residual >> 7;                            /* truncating divide by 128 */
        rem = residual - step * 128;
        if (rem < 0) {
            rem = -rem;
        }

        for (unsigned int i = 0; i < LTM_SAMPLES; i++) {
            bin[i] = (uint16_t)(bin[i] + step);
        }

        for (int i = 0; i < rem && i < (int)LTM_SAMPLES; i++) {
            bin[i] = (uint16_t)(bin[i] + 1);
        }

        /*
         * S9: the CDF scaled to the page range, temporally blended against the previous page.
         * fresh = cdf * (16384/ppt); the blend's Q12 weight and the >>16 together bring it to
         * cdf*1024/ppt (full scale ~1024). This IIR against prev is why the driver double-buffers.
         */
        cdf = 0;
        for (unsigned int i = 0; i < LTM_SAMPLES; i++) {
            uint16_t fresh;
            uint32_t acc;

            cdf += bin[i];
            fresh = (uint16_t)((unsigned int)(cdf * inv16k) & 0xffff);
            acc = (uint32_t)pv[i] * (uint32_t)w_old +
                  (uint32_t)fresh * (uint32_t)w_new;
            ov[i] = (uint16_t)(acc >> 16);
        }
    }

    return 0;
}
