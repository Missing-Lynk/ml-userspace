#include "ml-pipeline.h"

/* Path of the heap the last successful allocation came from. */
static char g_heap_name[280];

/* Allocate one contiguous dma-buf; returns its fd.
 *
 * Heap choice matters for correctness and placement. `mmz` (ml_mmzheap.ko) is the no-map
 * WC carveout shared with the wave5 codec pool: CPU writes land in DDR directly, so what we
 * compose is what the (non-snooping) DC fetches, and composite stops competing with the HUD
 * overlay, DRM and driver DMA for the small CMA. `default_cma_region` hands out CACHED CPU
 * mappings (its legacy alias `reserved` is the SAME CMA, not a carveout), so a CPU-blitted
 * composite can sit in L2 while the DC scans stale DDR - clean in every CPU dump, garbage on
 * the panel. Prefer mmz; the scan falls back to a CMA heap when it is absent; ML_HEAP overrides.
 */
static int heap_alloc_pref(const char *pref, gsize len, char *name_out, gsize name_len)
{
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    struct dirent *de;
    DIR *d;
    int hfd = -1;
    char path[280];

    snprintf(path, sizeof path, "/dev/dma_heap/%s", pref);
    hfd = open(path, O_RDWR | O_CLOEXEC);
    if (hfd < 0) {
        d = opendir("/dev/dma_heap");

        if (!d) {
            perror("ml-pipeline: /dev/dma_heap (CONFIG_DMABUF_HEAPS_CMA)");
            return -1;
        }

        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') {
                continue;
            }

            snprintf(path, sizeof path, "/dev/dma_heap/%s", de->d_name);
            hfd = open(path, O_RDWR | O_CLOEXEC);
            if (hfd >= 0) {
                break;
            }
        }

        closedir(d);
    }

    if (hfd < 0) {
        return -1;
    }

    if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a)) {
        perror("ml-pipeline: DMA_HEAP_IOCTL_ALLOC");
        close(hfd);
        return -1;
    }

    close(hfd);

    g_strlcpy(name_out, path, name_len);

    return a.fd;
}

int ml_heap_alloc(gsize len)
{
    const char *pref = getenv("ML_HEAP") ? getenv("ML_HEAP") : "mmz";
    int fd = heap_alloc_pref(pref, len, g_heap_name, sizeof g_heap_name);

    if (fd >= 0) {
        static int logged;

        if (g_verbose && !logged++) {
            fprintf(stderr, "ml-pipeline: composite heap = %s\n", g_heap_name);
        }
    }

    return fd;
}

/* Bracket CPU writes to a CMA buffer so the non-snooping DC sees them (start=1 before the
 * blit, start=0 after to flush to DDR).
 */
void ml_dmabuf_sync(int fd, int start)
{
    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_WRITE |
                                       (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

/* Invalidate a cached dma-buf so a CPU read sees what a device DMA'd into it.
 *
 * The bracket ml_dmabuf_sync() opens is DMA_BUF_SYNC_WRITE, which the dma-buf ioctl maps to
 * DMA_TO_DEVICE: dma_sync_sgtable_for_cpu with that direction invalidates nothing, so it
 * covers CPU writes and not CPU reads. SYNC_READ maps to DMA_FROM_DEVICE, which is the
 * invalidate. Whole-buffer, because the ABI has no range; on the write-combine mmz heap
 * begin_cpu_access is not implemented and this is a no-op.
 */
void ml_dmabuf_invalidate(int fd)
{
    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };

    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

/* Allocate the composite pool adaptively: the heap is shared (mmz with the codec, or CMA with
 * everything) and each buffer is ~3.1 MB, so grab as many as it yields up to COMP_POOL, stopping
 * at the first failure. Need COMP_MIN to run without constant starvation; fewer than COMP_POOL just means the
 * starve-drop path fires under heavy inter-decoder skew (counted in comp_starve). The display
 * side alone can hold 4 (prev + front + pending + next, late retirement in drm_flip_handler),
 * so the floor leaves at least one slot free for the compositor.
 */
#define COMP_MIN 5
gboolean comp_pool_init(struct ctx *c)
{
    c->comp_alloc = gst_dmabuf_allocator_new();
    c->comp_free = g_async_queue_new();
    c->comp_n = 0;

    /* Staging FIRST: it is 1.6 MB and the non-dmabuf tile's whole DMA path hangs off it -
     * allocated after the pool, a full heap silently downgraded tile 1 to per-frame CPU
     * blits (the pool adapts to whatever is left; staging cannot).
     */
    c->stage_fd = ml_heap_alloc(COMP_TILE_SIZE);
    if (c->stage_fd >= 0) {
        c->stage_map = mmap(NULL, COMP_TILE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, c->stage_fd, 0);
        if (c->stage_map == MAP_FAILED) {
            close(c->stage_fd);
            c->stage_fd = -1;
            c->stage_map = NULL;
        }
    }

    /* The band scratch is 180 KiB at a fixed size, claimed here ahead of the pool. A failure
     * drops the mode to SEAM_SPLIT.
     */
    seam_scratch_init(c);

    /* DVR 720p scaler-dst pool next (rec_hw_init; a no-op without /dev/arscaler): it must claim
     * its ~5.4 MiB while there still is CMA to claim - steady state runs ~0.3 MiB free - and the
     * composite grab below adapts to whatever remains, so the budget closes itself.
     */
    rec_hw_init(c);

    /* Cap the pool: the default heap is the SHARED mmz pool, and the composite allocates
     * before the two decoders claim their ~52 MiB of the same MMZ pool, so an uncapped
     * greedy grab would starve wave5. ML_COMP_MAX bounds it (ml-video-up sets 10 -> ~31 MiB,
     * leaving room for wave5 + DVR encoder). Default COMP_POOL for the standalone/CMA case.
     */
    int cap = getenv("ML_COMP_MAX") ? atoi(getenv("ML_COMP_MAX")) : COMP_POOL;
    if (cap < COMP_MIN) {
        cap = COMP_MIN;
    }

    if (cap > COMP_POOL) {
        cap = COMP_POOL;
    }

    /* COMP_ALLOC, not COMP_SIZE: the encoder's dmabuf import demands its 16-row-aligned
     * sizeimage; the content layout stays COMP_SIZE (the tail is padding). It is also the
     * largest size inside the heap's 768-page alignment slot.
     */
    gsize buf_len = COMP_ALLOC;

    for (int i = 0; i < cap; i++) {
        int fd = ml_heap_alloc(buf_len);
        guint8 *m;

        if (fd < 0) {
            break;                      /* pool exhausted/fragmented - use what we have */
        }

        m = mmap(NULL, buf_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            break;
        }

        c->comp_pool[i].fd = fd;
        c->comp_pool[i].map = m;
        g_async_queue_push(c->comp_free, GINT_TO_POINTER(i + 1));   /* +1: 0 == queue-empty */
        c->comp_n++;
    }
    if (g_verbose) {
        fprintf(stderr, "ml-pipeline: composite pool = %d x %d KiB (%d MiB)\n",
                c->comp_n, COMP_SIZE / 1024, c->comp_n * COMP_SIZE / (1024 * 1024));
    }

    /* Derive the input-gate bounds from the actual yield: the display side holds up to 4
     * buffers (prev/front/pending/next), the rest is the pairing window; split it between
     * tolerated decoded skew and dec-0 in-flight depth, minus a safety margin.
     */
    {
        int window = c->comp_n - 4 - 2;
        if (window < 4) {
            window = 4;
        }

        c->skew_max = window / 2;
        c->inflight_max = window - c->skew_max;
        if (g_verbose) {
            fprintf(stderr, "ml-pipeline: input gate: skew_max=%d inflight_max=%d\n",
                    c->skew_max, c->inflight_max);
        }
    }

    return c->comp_n >= COMP_MIN;
}

/* A GstBuffer finalize hook carries a {ctx, idx} so the compbuf returns to the free-list the
 * moment kmssink drops its ref.
 */
struct comp_ret { struct ctx *c; int idx; };
static void comp_on_finalize(gpointer user, GstMiniObject *obj)
{
    struct comp_ret *h = user;

    (void)obj;
    /* DEBUG (ML_NO_RECYCLE): never return the buffer to the pool. The pipeline then composes
     * comp_n frames onto unique, never-reused buffers and freezes on the last - a pristine
     * buffer the DC displays without any subsequent DMA overwrite. If that frozen frame is clean
     * on the panel, the panel garbage is the buffer-reuse-during-scanout race.
     */
    if (!getenv("ML_NO_RECYCLE")) {
        g_async_queue_push(h->c->comp_free, GINT_TO_POINTER(h->idx + 1));
    }
    g_free(h);
}

/* Claim a free compbuf and wrap it as a DMA_DRM/YU12 dmabuf GstBuffer (the layout kmssink
 * scans out zero-copy). Returns NULL if the pool is momentarily exhausted. Sets *idx_out to
 * the claimed index (the caller stores it to blit into comp_pool[idx].map and to flush
 * comp_pool[idx].fd at push).
 */
GstBuffer *comp_get(struct ctx *c, int *idx_out)
{
    gpointer v = g_async_queue_try_pop(c->comp_free);
    int idx;
    struct compbuf *cb;
    GstBuffer *b;
    GstMemory *mem;
    struct comp_ret *h;
    gsize offs[GST_VIDEO_MAX_PLANES] = { 0, COMP_UOFF, COMP_VOFF };
    gint strd[GST_VIDEO_MAX_PLANES] = { COMP_LSTRIDE, COMP_CSTRIDE, COMP_CSTRIDE };

    if (!v) {
        return NULL;
    }

    idx = GPOINTER_TO_INT(v) - 1;
    cb = &c->comp_pool[idx];
    ml_dmabuf_sync(cb->fd, 1);              /* begin CPU write access */

    b = gst_buffer_new();
    mem = gst_dmabuf_allocator_alloc(c->comp_alloc, dup(cb->fd), COMP_ALLOC);
    gst_buffer_append_memory(b, mem);
    gst_buffer_add_video_meta_full(b, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_I420, COMP_W, COMP_H, 3, offs, strd);

    h = g_malloc(sizeof *h);
    h->c = c;
    h->idx = idx;
    gst_mini_object_weak_ref(GST_MINI_OBJECT(b), comp_on_finalize, h);

    *idx_out = idx;

    return b;
}

/* Copy one decoded tile into the I420 composite at luma row dst_row. Dimensions and dst_row are
 * even, so chroma stays aligned. The 32-row overlap is harmless: tile 1's top rows overwrite
 * tile 0's identical bottom rows. Input may be I420 (copy chroma) or NV12 (deinterleave UV).
 */
void blit_tile(guint8 *out, const struct tileview *t, int dst_row)
{
    const int oys = COMP_LSTRIDE, ocs = COMP_CSTRIDE;
    guint8 *oY = out;
    guint8 *oU = oY + oys * COMP_H;
    guint8 *oV = oU + ocs * (COMP_H / 2);
    int w = (t->w > COMP_W) ? COMP_W : t->w;

    int h = t->h;
    if (dst_row + h > COMP_H) {
        h = COMP_H - dst_row;
    }

    for (int r = 0; r < h; r++) {
        memcpy(oY + (dst_row + r) * oys, t->y + r * t->ys, w);
    }

    if (!t->nv12) {
        for (int r = 0; r < h / 2; r++) {
            memcpy(oU + (dst_row / 2 + r) * ocs, t->u + r * t->us, w / 2);
            memcpy(oV + (dst_row / 2 + r) * ocs, t->v + r * t->vs, w / 2);
        }
    } else {
        for (int r = 0; r < h / 2; r++) {
            const guint8 *uv = t->u + r * t->us;
            guint8 *du = oU + (dst_row / 2 + r) * ocs;
            guint8 *dv = oV + (dst_row / 2 + r) * ocs;
            for (int col = 0; col < w / 2; col++) {
                du[col] = uv[2 * col];
                dv[col] = uv[2 * col + 1];
            }
        }
    }
}

/* DMA variant of blit_tile: copy the tile into the composite dmabuf via the AXI
 * DMA engine (ml_dmablit), off-CPU. Because the composite now uses PACKED strides matching the
 * decoder (COMP_LSTRIDE == COMP_W, COMP_CSTRIDE == COMP_W/2), a packed I420 source makes each
 * plane one contiguous src->dst block - 3 copies in one submit (the vendor's fusion shape,
 * re/notes/fusion-two-tile-rendering.md §2). Returns FALSE (caller CPU-blits) for NV12, a
 * non-dmabuf or row-padded tile, a non-packed composite, or on any submit failure. Caller holds
 * c->comp_lock. dst_row is even, so all offsets/lengths stay 4-byte aligned (ml_dmablit requires it).
 */
gboolean blit_tile_dma(struct ctx *c, int dst_fd, const struct tileview *t, int dst_row,
                       int src_row)
{
    if (c->dmablit_fd < 0 || t->fd < 0 || t->nv12) {
        return FALSE;
    }

    /* contiguous-copy preconditions: no row padding on the source, packed composite */
    if (t->ys != t->w || t->us != t->w / 2 || t->vs != t->w / 2 || t->w != COMP_W) {
        return FALSE;
    }

    if (COMP_LSTRIDE != COMP_W || COMP_CSTRIDE != COMP_W / 2) {
        return FALSE;
    }

    int h = t->h - src_row;
    if (dst_row + h > COMP_H) {
        h = COMP_H - dst_row;
    }

    if (h <= 0 || src_row < 0 || src_row % 2) {
        return FALSE;
    }

    guint32 sy = (guint32)(t->yoff + (gsize)src_row * t->ys);
    guint32 su = (guint32)(t->uoff + (gsize)(src_row / 2) * t->us);
    guint32 sv = (guint32)(t->voff + (gsize)(src_row / 2) * t->vs);

    struct ml_dmablit_req req = { .dst_fd = dst_fd, .n = 3 };
    req.copy[0] = (struct ml_dmablit_copy){ t->fd, sy,
                  (guint32)(dst_row * COMP_LSTRIDE), (guint32)(COMP_LSTRIDE * h) };
    req.copy[1] = (struct ml_dmablit_copy){ t->fd, su,
                  (guint32)(COMP_UOFF + (dst_row / 2) * COMP_CSTRIDE), (guint32)(COMP_CSTRIDE * (h / 2)) };
    req.copy[2] = (struct ml_dmablit_copy){ t->fd, sv,
                  (guint32)(COMP_VOFF + (dst_row / 2) * COMP_CSTRIDE), (guint32)(COMP_CSTRIDE * (h / 2)) };

    if (ioctl(c->dmablit_fd, ML_DMABLIT_SUBMIT, &req) != 0) {
        if (!c->dmablit_warned) {
            fprintf(stderr, "ml-pipeline: ml_dmablit submit failed (%s); falling back to CPU blit\n",
                    strerror(errno));
            c->dmablit_warned = TRUE;
        }

        return FALSE;
    }

    return TRUE;
}

/* Pack an I420 tile (possibly strided) into a contiguous stride==width layout at `out`. */
static void pack_tile(guint8 *out, const struct tileview *t, int h)
{
    int w = t->w;
    guint8 *oY = out, *oU = out + (gsize)w * h, *oV = oU + (gsize)(w / 2) * (h / 2);

    for (int r = 0; r < h; r++) {
        memcpy(oY + (gsize)r * w, t->y + (gsize)r * t->ys, w);
    }

    for (int r = 0; r < h / 2; r++) {
        memcpy(oU + (gsize)r * (w / 2), t->u + (gsize)r * t->us, w / 2);
        memcpy(oV + (gsize)r * (w / 2), t->v + (gsize)r * t->vs, w / 2);
    }
}

/* Compose a NON-dmabuf tile without ever CPU-writing the composite: CPU-pack it into our own
 * staging dmabuf, flush THAT buffer to DDR, then DMA staging -> composite. Because the composite
 * only ever receives DMA writes this way (tile 0 direct, tile 1 via staging), there is no CPU/DMA
 * cache mix on it - the structural fix the mixed CPU-blit could not achieve with any flush. This
 * mirrors the vendor's all-DMA fusion (both tiles DMA'd into the fused buffer). I420 packed only;
 * returns FALSE (caller CPU-blits) for NV12 / non-packed / any failure. Caller holds c->comp_lock.
 */
gboolean blit_tile_staged(struct ctx *c, int dst_fd, const struct tileview *t, int dst_row)
{
    if (c->dmablit_fd < 0 || c->stage_fd < 0 || t->nv12) {
        return FALSE;
    }

    if (t->w != COMP_W || COMP_LSTRIDE != COMP_W || COMP_CSTRIDE != COMP_W / 2) {
        return FALSE;
    }

    int w = t->w, h = t->h;
    if (dst_row + h > COMP_H) {
        h = COMP_H - dst_row;
    }

    if (h <= 0) {
        return FALSE;
    }

    pack_tile(c->stage_map, t, h);                  /* CPU write into the staging buffer only */
    ioctl(c->dmablit_fd, ML_DMABLIT_FLUSH, &c->stage_fd);   /* clean staging -> DDR for the DMA read */

    guint32 sU = (guint32)w * h, sV = sU + (guint32)(w / 2) * (h / 2);
    struct ml_dmablit_req req = { .dst_fd = dst_fd, .n = 3 };
    req.copy[0] = (struct ml_dmablit_copy){ c->stage_fd, 0,
                  (guint32)(dst_row * COMP_LSTRIDE), (guint32)(COMP_LSTRIDE * h) };
    req.copy[1] = (struct ml_dmablit_copy){ c->stage_fd, sU,
                  (guint32)(COMP_UOFF + (dst_row / 2) * COMP_CSTRIDE), (guint32)(COMP_CSTRIDE * (h / 2)) };
    req.copy[2] = (struct ml_dmablit_copy){ c->stage_fd, sV,
                  (guint32)(COMP_VOFF + (dst_row / 2) * COMP_CSTRIDE), (guint32)(COMP_CSTRIDE * (h / 2)) };

    return ioctl(c->dmablit_fd, ML_DMABLIT_SUBMIT, &req) == 0;
}

/* Select the seam handling from ML_SEAM once, before comp_pool_init: the seam modes are what
 * make seam_scratch_init claim its 180 KiB, and that lands ahead of the pool.
 */
void seam_mode_init(struct ctx *c)
{
    const char *v = getenv("ML_SEAM");
    int mode = v ? atoi(v) : SEAM_OFF;

    if (mode < SEAM_OFF || mode > SEAM_BLEND) {
        mode = SEAM_OFF;
    }

    c->seam_mode = (enum seam_mode)mode;
    if (c->seam_mode != SEAM_OFF) {
        fprintf(stderr, "ml-pipeline: seam mode %d (%s)\n", mode,
                mode == SEAM_SPLIT ? "split geometry" : "split geometry + cross-fade");
    }
}

/* Byte offsets of one ring region of the band scratch. */
#define BAND_Y_OFF(r)   ((guint32)((r) * COMP_BAND_SIZE))
#define BAND_U_OFF(r)   (BAND_Y_OFF(r) + COMP_BAND_YSIZE)
#define BAND_V_OFF(r)   (BAND_U_OFF(r) + COMP_BAND_CSIZE)

/* Which ring region a slot uses. Slots are a fixed array, so a slot maps to the same region for
 * the pair's lifetime; the PTS stamp is what makes a collision detectable.
 */
static int band_region(struct ctx *c, const struct comp_slot *sl)
{
    return (int)((sl - c->slot) % COMP_BAND_SLOTS);
}

/* Cache maintenance over one range of a dmabuf, via ml_dmablit. Returns FALSE when the running
 * module predates the ioctl, which is what seam_scratch_init's probe tests for.
 */
static gboolean band_cache(struct ctx *c, int fd, guint32 off, guint32 len, guint32 op)
{
    struct ml_dmablit_cache rq = { .fd = fd, .off = off, .len = len, .op = op };

    return ioctl(c->dmablit_fd, ML_DMABLIT_CACHE, &rq) == 0;
}

/* The band occupies three disjoint ranges of the composite, one per plane. */
static gboolean band_cache_all(struct ctx *c, int fd, guint32 op)
{
    guint32 y = (guint32)((gsize)c->seam_top * COMP_LSTRIDE);
    guint32 u = (guint32)(COMP_UOFF + (gsize)(c->seam_top / 2) * COMP_CSTRIDE);
    guint32 v = (guint32)(COMP_VOFF + (gsize)(c->seam_top / 2) * COMP_CSTRIDE);

    return band_cache(c, fd, y, COMP_BAND_YSIZE, op) &&
           band_cache(c, fd, u, COMP_BAND_CSIZE, op) &&
           band_cache(c, fd, v, COMP_BAND_CSIZE, op);
}

/* Allocate the ring tile 1's band is staged into, and probe for the ranged cache ioctl.
 *
 * A CMA heap, named explicitly rather than through ml_heap_alloc's write-combine mmz
 * preference, so the cross-fade reads the staged band cached. Blending straight out of the
 * decoder's write-combine buffer measured 4 ms per frame.
 */
void seam_scratch_init(struct ctx *c)
{
    char heap[280] = "";

    c->seam_scratch_fd = -1;
    c->seam_scratch_map = NULL;
    c->seam_ranged = FALSE;
    for (int i = 0; i < COMP_BAND_SLOTS; i++) {
        c->seam_stamp[i] = GST_CLOCK_TIME_NONE;
    }

    if (c->seam_mode != SEAM_BLEND) {
        return;
    }

    c->seam_scratch_fd = heap_alloc_pref("default_cma_region", COMP_BAND_ALL, heap, sizeof heap);
    if (c->seam_scratch_fd >= 0) {
        c->seam_scratch_map = mmap(NULL, COMP_BAND_ALL, PROT_READ | PROT_WRITE, MAP_SHARED,
                                   c->seam_scratch_fd, 0);
        if (c->seam_scratch_map == MAP_FAILED) {
            close(c->seam_scratch_fd);
            c->seam_scratch_fd = -1;
            c->seam_scratch_map = NULL;
        }
    }

    if (c->seam_scratch_fd < 0) {
        fprintf(stderr, "ml-pipeline: seam scratch alloc failed - cross-fade off\n");
        c->seam_mode = SEAM_SPLIT;
        return;
    }

    c->seam_scratch_cached = strcmp(heap, "/dev/dma_heap/mmz") != 0;

    /* Probe on the scratch itself: a clean of the whole ring is harmless whatever the answer. */
    c->seam_ranged = c->dmablit_fd >= 0 &&
                     band_cache(c, c->seam_scratch_fd, 0, COMP_BAND_ALL, ML_DMABLIT_CLEAN);
    if (!c->seam_ranged) {
        fprintf(stderr, "ml-pipeline: ml_dmablit has no ranged cache op - cross-fade off "
                        "(load the current ml_dmablit.ko)\n");
        c->seam_mode = SEAM_SPLIT;
        return;
    }

    fprintf(stderr, "ml-pipeline: seam scratch = %d KiB on %s (%s), ranged cache ops\n",
            COMP_BAND_ALL / 1024, heap, c->seam_scratch_cached ? "cached" : "write-combine");
}

/* Stage TILE 1's copy of the band out of its live decoder buffer: one submit, three whole-plane
 * blocks. Only tile 1 needs this - tile 0's copy of the band is already in the composite, put
 * there by its own body DMA - so the cross-fade costs exactly one extra submit per frame.
 *
 * Runs in tile 1's own callback because its decoder buffer is released when that callback
 * returns and the split geometry writes those rows to no composite row. Caller holds
 * c->comp_lock.
 */
gboolean seam_band_capture(struct ctx *c, struct comp_slot *sl, const struct tileview *t, int ch)
{
    if (c->seam_mode != SEAM_BLEND || ch != 1 || c->seam_scratch_fd < 0 || !c->seam_geom) {
        return FALSE;
    }

    /* blit_tile_dma's preconditions, for the same reason: three whole-plane blocks only hold
     * for an unpadded full-width I420 dmabuf.
     */
    if (c->dmablit_fd < 0 || t->fd < 0 || t->nv12 || t->w != COMP_W ||
        t->ys != t->w || t->us != t->w / 2 || t->vs != t->w / 2 || t->h < TILE_OVER) {
        return FALSE;
    }

    int region = band_region(c, sl);
    struct ml_dmablit_req req = { .dst_fd = c->seam_scratch_fd, .n = 3 };

    req.copy[0] = (struct ml_dmablit_copy){ t->fd, (guint32)t->yoff,
                  BAND_Y_OFF(region), COMP_BAND_YSIZE };
    req.copy[1] = (struct ml_dmablit_copy){ t->fd, (guint32)t->uoff,
                  BAND_U_OFF(region), COMP_BAND_CSIZE };
    req.copy[2] = (struct ml_dmablit_copy){ t->fd, (guint32)t->voff,
                  BAND_V_OFF(region), COMP_BAND_CSIZE };

    if (ioctl(c->dmablit_fd, ML_DMABLIT_SUBMIT, &req) != 0) {
        return FALSE;
    }

    c->seam_stamp[region] = sl->pts;

    return TRUE;
}

/* Derive the split geometry from the tile heights the decoders actually produce. The tile
 * count and heights are negotiated, so the identity h0 + h1 - TILE_OVER == COMP_H is a
 * runtime condition: on anything else the split stays off and every tile takes the overwrite
 * placement. Caller holds c->comp_lock.
 */
void seam_geom_update(struct ctx *c, int ch, const struct tileview *t)
{
    if (c->seam_mode == SEAM_OFF || t->h <= 0) {
        return;
    }

    /* Re-check the shape every frame. A session that changes resolution is absorbed by the
     * PTS-epoch continuation without a pipeline rebuild, so this is what clears the latched
     * heights and re-derives the split point.
     */
    if (c->tile_h[ch] != t->h || c->tile_w[ch] != t->w) {
        c->tile_h[ch] = t->h;
        c->tile_w[ch] = t->w;
        c->seam_geom = FALSE;
    }

    if (c->seam_geom || c->tile_h[0] == 0 || c->tile_h[1] == 0) {
        return;
    }

    if (c->tile_h[0] + c->tile_h[1] - TILE_OVER != COMP_H ||
        c->tile_w[0] != COMP_W || c->tile_w[1] != COMP_W ||
        c->tile_h[0] <= TILE_OVER || c->tile_h[1] <= TILE_OVER ||
        c->tile_h[0] % 2 || c->tile_h[1] % 2 || TILE_OVER % 2) {
        if (!c->seam_warned) {
            c->seam_warned = TRUE;
            fprintf(stderr, "ml-pipeline: seam off, tiles %dx%d + %dx%d do not overlap by %d "
                    "into %d rows\n", c->tile_w[0], c->tile_h[0], c->tile_w[1], c->tile_h[1],
                    TILE_OVER, COMP_H);
        }

        return;
    }

    c->seam_h0 = c->tile_h[0];
    c->seam_top = c->seam_h0 - TILE_OVER;
    c->seam_geom = TRUE;

    if (g_verbose) {
        fprintf(stderr, "ml-pipeline: seam band = composite rows %d..%d\n",
                c->seam_top, c->seam_top + TILE_OVER - 1);
    }
}

/* Cross-fade the band, in place in the composite.
 *
 * Tile 0's copy of those rows is already there from its own body DMA; tile 1's was staged into
 * the scratch by seam_band_capture. So the only DMA this design adds is that one capture: the
 * blend reads the composite band directly and writes the result back over it, and both the
 * invalidate before and the clean after are scoped to the band's three plane ranges - about
 * 90 KB of cache work, against 3 MB for a whole-buffer sync.
 *
 * This is the one CPU write into the composite that coexists with DMA writes to it: the band
 * rows are cleaned before the buffer is pushed and no DMA targets them afterwards, which is
 * what the rest of this file's flushing rules ask for.
 *
 * Runs at pair completion under c->comp_lock, before the OSD burn, so a glyph over the band
 * survives.
 */
gboolean seam_blend_band(struct ctx *c, struct comp_slot *sl)
{
    if (c->seam_mode != SEAM_BLEND || !c->seam_geom || !c->seam_ranged || sl->cbi < 0) {
        return FALSE;
    }

    if (!sl->seam_ok[0] || !sl->seam_ok[1]) {
        c->seam_skip_geom++;
        return FALSE;
    }

    int region = band_region(c, sl);

    /* A pair whose tile 1 arrived first holds its region until tile 0 lands, and another pair's
     * tile 1 can claim the same region in between. The stamp catches that; the ring depth sets
     * how often it happens.
     */
    if (c->seam_stamp[region] != sl->pts) {
        c->seam_skip_ring++;
        return FALSE;
    }

    int fd = c->comp_pool[sl->cbi].fd;
    guint8 *comp = c->comp_pool[sl->cbi].map;
    guint8 *scratch = c->seam_scratch_map;
    gint64 t0;

    /* Tile 0's body DMA and tile 1's capture have both completed - ML_DMABLIT_SUBMIT blocks -
     * so both copies are in DDR while the CPU's view of them may not be.
     */
    t0 = g_get_monotonic_time();
    if (!band_cache_all(c, fd, ML_DMABLIT_INVALIDATE)) {
        c->seam_skip_cache++;
        return FALSE;
    }

    if (c->seam_scratch_cached &&
        !band_cache(c, c->seam_scratch_fd, BAND_Y_OFF(region), COMP_BAND_SIZE, ML_DMABLIT_INVALIDATE)) {
        c->seam_skip_cache++;
        return FALSE;
    }

    {
        guint64 us = (guint64)(g_get_monotonic_time() - t0);

        c->ns_inv += us;
        if (us > c->mx_inv) {
            c->mx_inv = us;
        }
    }

    guint8 *band_y = comp + (gsize)c->seam_top * COMP_LSTRIDE;
    guint8 *band_u = comp + COMP_UOFF + (gsize)(c->seam_top / 2) * COMP_CSTRIDE;
    guint8 *band_v = comp + COMP_VOFF + (gsize)(c->seam_top / 2) * COMP_CSTRIDE;

    t0 = g_get_monotonic_time();
    seam_blend_plane(band_y, COMP_LSTRIDE, band_y, COMP_LSTRIDE, scratch + BAND_Y_OFF(region), COMP_LSTRIDE,
                     COMP_W, SEAM_ROWS_LUMA);
    seam_blend_plane(band_u, COMP_CSTRIDE, band_u, COMP_CSTRIDE, scratch + BAND_U_OFF(region), COMP_CSTRIDE,
                     COMP_W / 2, SEAM_ROWS_CHROMA);
    seam_blend_plane(band_v, COMP_CSTRIDE, band_v, COMP_CSTRIDE, scratch + BAND_V_OFF(region), COMP_CSTRIDE,
                     COMP_W / 2, SEAM_ROWS_CHROMA);
    {
        guint64 us = (guint64)(g_get_monotonic_time() - t0);

        c->ns_blend += us;
        if (us > c->mx_blend) {
            c->mx_blend = us;
        }
    }

    /* The clean is the step that makes the blend visible to the display controller, which does
     * not snoop. An unchecked failure here leaves the band dirty in cache: this frame scans the
     * pre-blend rows out of DDR, and the dirty lines land later, over whatever DMA has written
     * into this pooled buffer by then. Retry once, then give up on the pair and say so.
     */
    t0 = g_get_monotonic_time();
    if (!band_cache_all(c, fd, ML_DMABLIT_CLEAN) &&
        !band_cache_all(c, fd, ML_DMABLIT_CLEAN)) {
        if (!c->seam_clean_warned) {
            c->seam_clean_warned = TRUE;
            fprintf(stderr, "ml-pipeline: seam clean failed (%s) - band left unflushed\n",
                    strerror(errno));
        }

        c->seam_skip_cache++;

        return FALSE;
    }
    {
        guint64 us = (guint64)(g_get_monotonic_time() - t0);

        c->ns_bandwb += us;
        if (us > c->mx_bandwb) {
            c->mx_bandwb = us;
        }
    }

    return TRUE;
}
