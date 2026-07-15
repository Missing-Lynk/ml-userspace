#include "ml-pipeline.h"

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
int ml_heap_alloc(gsize len)
{
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    struct dirent *de;
    DIR *d;
    int hfd = -1;
    const char *pref = getenv("ML_HEAP") ? getenv("ML_HEAP") : "mmz";
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

    {
        static int logged;
        if (!logged++) {
            fprintf(stderr, "ml-pipeline: composite heap = %s\n", path);
        }
    }

    return a.fd;
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
    /* Cap the pool: the default heap is the SHARED mmz pool, and the composite allocates
     * before the two decoders claim their ~52 MiB of the same 108 MiB pool, so an uncapped
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

    for (int i = 0; i < cap; i++) {
        int fd = ml_heap_alloc(COMP_SIZE);
        guint8 *m;

        if (fd < 0) {
            break;                      /* pool exhausted/fragmented - use what we have */
        }

        m = mmap(NULL, COMP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            break;
        }

        c->comp_pool[i].fd = fd;
        c->comp_pool[i].map = m;
        g_async_queue_push(c->comp_free, GINT_TO_POINTER(i + 1));   /* +1: 0 == queue-empty */
        c->comp_n++;
    }
    fprintf(stderr, "ml-pipeline: composite pool = %d x %d KiB (%d MiB)\n",
            c->comp_n, COMP_SIZE / 1024, c->comp_n * COMP_SIZE / (1024 * 1024));

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
        fprintf(stderr, "ml-pipeline: input gate: skew_max=%d inflight_max=%d\n",
                c->skew_max, c->inflight_max);
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
    mem = gst_dmabuf_allocator_alloc(c->comp_alloc, dup(cb->fd), COMP_SIZE);
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
gboolean blit_tile_dma(struct ctx *c, int dst_fd, const struct tileview *t, int dst_row)
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

    int h = t->h;
    if (dst_row + h > COMP_H) {
        h = COMP_H - dst_row;
    }

    if (h <= 0) {
        return FALSE;
    }

    struct ml_dmablit_req req = { .dst_fd = dst_fd, .n = 3 };
    req.copy[0] = (struct ml_dmablit_copy){ t->fd, (guint32)t->yoff,
                  (guint32)(dst_row * COMP_LSTRIDE), (guint32)(COMP_LSTRIDE * h) };
    req.copy[1] = (struct ml_dmablit_copy){ t->fd, (guint32)t->uoff,
                  (guint32)(COMP_UOFF + (dst_row / 2) * COMP_CSTRIDE), (guint32)(COMP_CSTRIDE * (h / 2)) };
    req.copy[2] = (struct ml_dmablit_copy){ t->fd, (guint32)t->voff,
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
