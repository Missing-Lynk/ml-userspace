/* ml-pipeline osdburn: DVR OSD burn-in - cache the HUD's rendered BTFL OSD cells
 * (MLM_CMD_OSD_CELL) as YUV spans and overwrite them into each recorded composite.
 */
#include "ml-pipeline.h"

/* A patch pixel is opaque at alpha >= 128 (the HUD's binary-alpha draw rule). */
#define OSD_OPAQUE(a) ((a) >= 128)

/* BT.601 limited-range RGB -> YUV, the composite's own encoding (wave5 I420). */
static inline guint8 rgb_y(int r, int g, int b)
{
    return (guint8) (16 + ((66 * r + 129 * g + 25 * b + 128) >> 8));
}

static inline guint8 rgb_u(int r, int g, int b)
{
    return (guint8) (128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
}

static inline guint8 rgb_v(int r, int g, int b)
{
    return (guint8) (128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
}

/* Free one cached cell. Caller holds c->osd_lock. */
static void cell_free(struct ctx *c, struct osd_cell *cell)
{
    if (cell->px) {
        g_free(cell->px);
        g_free(cell->spans);
        cell->px = NULL;
        cell->spans = NULL;
        cell->nspans = 0;
        c->osd_ncells--;
    }
}

/* Append the opaque runs of one plane row as spans: @p op/@p val are the row's opacity and
 * pixel values (@p n samples), @p dst0 the composite offset of the row's first sample.
 */
static void row_spans(GArray *spans, GByteArray *px, const guint8 *op, const guint8 *val,
                      int n, guint32 dst0)
{
    for (int i = 0; i < n; ) {
        if (!op[i]) {
            i++;
            continue;
        }

        int i0 = i;
        while (i < n && op[i]) {
            i++;
        }

        struct osd_span sp = { dst0 + (guint32) i0, px->len, (guint32) (i - i0) };
        g_byte_array_append(px, val + i0, (guint) (i - i0));
        g_array_append_val(spans, sp);
    }
}

/* One MLM_CMD_OSD_CELL frame off ctrl.sock: validate, convert the RGBA patch to Y/U/V bytes +
 * opaque spans with absolute composite offsets, and install it in the cell cache. The conversion
 * runs once per cell CHANGE (main-loop thread); the per-frame burn is only the spans' memcpys.
 * A header-only frame clears the cell; the MLM_OSD_CLEAR_ALL sentinel clears the whole cache.
 */
void osd_burn_cell(struct ctx *c, const guint8 *frame, gssize len)
{
    struct mlm_osd_cell hdr;

    if (len < (gssize) sizeof hdr) {
        return;
    }

    memcpy(&hdr, frame, sizeof hdr);
    if (hdr.row == MLM_OSD_CLEAR_ALL && hdr.col == MLM_OSD_CLEAR_ALL) {
        osd_burn_clear(c);
        return;
    }

    if (hdr.row >= MLM_OSD_ROWS || hdr.col >= MLM_OSD_COLS) {
        return;
    }

    struct osd_cell *cell = &c->osd_cells[hdr.row][hdr.col];

    if (len == (gssize) sizeof hdr) {           /* emptied cell */
        pthread_mutex_lock(&c->osd_lock);
        cell_free(c, cell);
        pthread_mutex_unlock(&c->osd_lock);

        return;
    }

    int x = hdr.x, y = hdr.y, w = hdr.w, h = hdr.h;

    if (w <= 0 || h <= 0 || w > MLM_OSD_CELL_WMAX || h > MLM_OSD_CELL_HMAX
        || x + w > COMP_W || y + h > COMP_H
        || len != (gssize) (sizeof hdr + (gsize) w * h * 4)) {
        return;
    }

    const guint8 *rgba = frame + sizeof hdr;

    /* Luma: per-pixel Y + opacity. */
    guint8 ly[MLM_OSD_CELL_WMAX * MLM_OSD_CELL_HMAX];
    guint8 lop[MLM_OSD_CELL_WMAX * MLM_OSD_CELL_HMAX];

    for (int i = 0; i < w * h; i++) {
        const guint8 *p = rgba + (gsize) i * 4;

        lop[i] = OSD_OPAQUE(p[3]);
        ly[i] = lop[i] ? rgb_y(p[0], p[1], p[2]) : 0;
    }

    /* Chroma: one sample per composite 2x2 luma block the patch touches (the patch rect may
     * start/end at odd coordinates, so a border block can be partly video). A sample goes
     * opaque when the glyph covers at least half of its in-patch pixels and takes their
     * averaged U/V; border video pixels sharing the block get the glyph's chroma, a half-res
     * fringe the black outline makes invisible.
     */
    int cx0 = x / 2, cy0 = y / 2;
    int cw = (x + w - 1) / 2 - cx0 + 1;
    int ch = (y + h - 1) / 2 - cy0 + 1;
    guint8 cu[(MLM_OSD_CELL_WMAX / 2 + 1) * (MLM_OSD_CELL_HMAX / 2 + 1)];
    guint8 cv[(MLM_OSD_CELL_WMAX / 2 + 1) * (MLM_OSD_CELL_HMAX / 2 + 1)];
    guint8 cop[(MLM_OSD_CELL_WMAX / 2 + 1) * (MLM_OSD_CELL_HMAX / 2 + 1)];

    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            int tot = 0, opq = 0, su = 0, sv = 0;

            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int lx = (cx0 + cx) * 2 + dx - x;
                    int lyr = (cy0 + cy) * 2 + dy - y;

                    if (lx < 0 || lx >= w || lyr < 0 || lyr >= h) {
                        continue;
                    }

                    tot++;
                    if (lop[lyr * w + lx]) {
                        const guint8 *p = rgba + ((gsize) lyr * w + lx) * 4;

                        opq++;
                        su += rgb_u(p[0], p[1], p[2]);
                        sv += rgb_v(p[0], p[1], p[2]);
                    }
                }
            }

            int i = cy * cw + cx;
            cop[i] = (tot > 0 && 2 * opq >= tot);
            cu[i] = cop[i] ? (guint8) (su / opq) : 0;
            cv[i] = cop[i] ? (guint8) (sv / opq) : 0;
        }
    }

    /* Pack spans: luma rows, then U rows, then V rows (all absolute composite offsets). */
    GArray *spans = g_array_new(FALSE, FALSE, sizeof(struct osd_span));
    GByteArray *px = g_byte_array_new();

    for (int j = 0; j < h; j++) {
        row_spans(spans, px, lop + (gsize) j * w, ly + (gsize) j * w, w,
                  (guint32) ((y + j) * COMP_LSTRIDE + x));
    }

    for (int cy = 0; cy < ch; cy++) {
        row_spans(spans, px, cop + (gsize) cy * cw, cu + (gsize) cy * cw, cw,
                  (guint32) (COMP_UOFF + (cy0 + cy) * COMP_CSTRIDE + cx0));
    }

    for (int cy = 0; cy < ch; cy++) {
        row_spans(spans, px, cop + (gsize) cy * cw, cv + (gsize) cy * cw, cw,
                  (guint32) (COMP_VOFF + (cy0 + cy) * COMP_CSTRIDE + cx0));
    }

    guint nspans = spans->len;
    guint8 *pxbuf = g_byte_array_free(px, nspans == 0);   /* keep the data unless empty */
    struct osd_span *spbuf = (struct osd_span *) g_array_free(spans, nspans == 0);

    pthread_mutex_lock(&c->osd_lock);
    cell_free(c, cell);
    if (nspans > 0) {                   /* an all-transparent patch just leaves the cell empty */
        cell->px = pxbuf;
        cell->spans = spbuf;
        cell->nspans = (int) nspans;
        c->osd_ncells++;
    }
    pthread_mutex_unlock(&c->osd_lock);
}

/* Drop every cached cell (record stop, or the HUD's clear-all on a burn-gate edge). */
void osd_burn_clear(struct ctx *c)
{
    pthread_mutex_lock(&c->osd_lock);
    for (int r = 0; r < MLM_OSD_ROWS; r++) {
        for (int col = 0; col < MLM_OSD_COLS; col++) {
            cell_free(c, &c->osd_cells[r][col]);
        }
    }
    pthread_mutex_unlock(&c->osd_lock);
}

/* Burn the cached cells into one composite: memcpy each cell's opaque spans into the CPU map.
 * Write-only (WC-safe) and bounded by the spans built above. Called from slot_push under
 * comp_lock, BEFORE the buffer's flush + rec_push, so the existing ml_dmabuf_sync covers these
 * writes on a cached-CMA fallback heap too.
 */
void osd_burn_apply(struct ctx *c, guint8 *map)
{
    if (c->osd_ncells == 0) {
        return;
    }

    pthread_mutex_lock(&c->osd_lock);
    for (int r = 0; r < MLM_OSD_ROWS; r++) {
        for (int col = 0; col < MLM_OSD_COLS; col++) {
            const struct osd_cell *cell = &c->osd_cells[r][col];

            for (int i = 0; i < cell->nspans; i++) {
                const struct osd_span *sp = &cell->spans[i];

                memcpy(map + sp->dst, cell->px + sp->src, sp->len);
            }
        }
    }
    pthread_mutex_unlock(&c->osd_lock);
}
