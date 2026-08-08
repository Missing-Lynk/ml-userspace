/**
 * @file mav-bufs.c
 * @brief dma-heap tile pool: allocation, wrapping, recycling and the tile push path.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/** Bracket CPU writes to a dma-heap buffer so the encoder's DMA sees them (start=1 before the
 * write, start=0 after to flush to DDR). */
void air_dmabuf_sync(int fd, int start)
{
    struct dma_buf_sync s;

    s.flags = DMA_BUF_SYNC_WRITE | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END);
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
}

/** Allocate one dma-heap buffer of @p len bytes; returns the fd or -1. Prefers a non-mmz heap
 * (the wave5 encoder's own working buffers use mmz) unless ML_AIR_HEAP names one. */
static int air_heap_alloc(gsize len)
{
    struct dma_heap_allocation_data a;
    const char *want = getenv("ML_AIR_HEAP");
    char path[280];
    int hfd = -1;

    memset(&a, 0, sizeof a);
    a.len = len;
    a.fd_flags = O_RDWR | O_CLOEXEC;

    if (want != NULL && want[0] != '\0') {
        snprintf(path, sizeof path, "/dev/dma_heap/%s", want);
        hfd = open(path, O_RDWR | O_CLOEXEC);
    }

    if (hfd < 0) {
        DIR *d = opendir("/dev/dma_heap");
        struct dirent *de;

        if (d == NULL) {
            perror("ml-air-video: /dev/dma_heap (CONFIG_DMABUF_HEAPS_CMA)");
            return -1;
        }

        /* First pass: any heap that is not mmz. */
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || strcmp(de->d_name, "mmz") == 0) {
                continue;
            }

            snprintf(path, sizeof path, "/dev/dma_heap/%s", de->d_name);
            hfd = open(path, O_RDWR | O_CLOEXEC);
            if (hfd >= 0) {
                break;
            }
        }

        /* Fallback: whatever exists (mmz). */
        if (hfd < 0) {
            rewinddir(d);
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') {
                    continue;
                }

                snprintf(path, sizeof path, "/dev/dma_heap/%s", de->d_name);
                hfd = open(path, O_RDWR | O_CLOEXEC);
                if (hfd >= 0) {
                    break;
                }
            }
        }

        closedir(d);
    }

    if (hfd < 0) {
        return -1;
    }

    if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a) != 0) {
        perror("ml-air-video: DMA_HEAP_IOCTL_ALLOC");
        close(hfd);

        return -1;
    }

    close(hfd);
    return (int)a.fd;
}

/** Allocate and mmap a tile's dma-heap pool; returns the count obtained (>= AIR_POOL_MIN on ok).
 *
 * The tile buffers and the codec's own working buffers come from the same 32 MiB mmz pool,
 * because that is the only dma-heap the air unit exposes. Two tiles at four buffers is about
 * 12.9 MB of it, against roughly 13 MB the two encoder instances need, so the pool depth is a
 * real lever when the second instance cannot allocate its FBC buffers. ML_AIR_POOL trades
 * pipelining depth for headroom without a rebuild.
 */
int air_pool_init(struct air_tile *tile, int want)
{
    if (want < AIR_POOL_MIN) {
        want = AIR_POOL_MIN;
    }

    if (want > AIR_POOL_MAX) {
        want = AIR_POOL_MAX;
    }

    tile->freeq = g_async_queue_new();
    tile->pool_n = 0;

    for (int i = 0; i < want; i++) {
        int fd = air_heap_alloc(tile->buf_size);
        guint8 *map;

        if (fd < 0) {
            break;
        }

        map = mmap(NULL, tile->buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            break;
        }

        tile->pool[i].fd = fd;
        tile->pool[i].map = map;
        tile->pool[i].size = tile->buf_size;
        g_async_queue_push(tile->freeq, GINT_TO_POINTER(i + 1));
        tile->pool_n++;
    }

    return tile->pool_n;
}

struct air_recycle {
    struct air_tile *tile;
    int idx;
};

void air_recycle_notify(gpointer data)
{
    struct air_recycle *r = data;

    g_async_queue_push(r->tile->freeq, GINT_TO_POINTER(r->idx + 1));
    g_free(r);
}

/** Copy one tile out of the source I420 frame into a pool buffer laid out packed at the coded
 * height (chroma at stride*height), matching the wave5 encoder's plane math and the DVR feed. The
 * 16-row alignment lives only in the allocation size (slack at the tail), never in the offsets. */
void air_fill_tile(struct air_tile *tile, int idx, GstVideoFrame *src)
{
    guint8 *dst = tile->pool[idx].map;
    const int ys = AIR_COMP_W;            /* dst luma stride (packed) */
    const int cs = AIR_COMP_W / 2;        /* dst chroma stride (packed) */
    guint8 *d_y = dst;
    guint8 *d_cb = dst + (gsize)ys * tile->height;
    guint8 *d_cr = d_cb + (gsize)cs * (tile->height / 2);

    const guint8 *s_y = GST_VIDEO_FRAME_PLANE_DATA(src, 0);
    const guint8 *s_cb = GST_VIDEO_FRAME_PLANE_DATA(src, 1);
    const guint8 *s_cr = GST_VIDEO_FRAME_PLANE_DATA(src, 2);
    const int sy = GST_VIDEO_FRAME_PLANE_STRIDE(src, 0);
    const int scb = GST_VIDEO_FRAME_PLANE_STRIDE(src, 1);
    const int scr = GST_VIDEO_FRAME_PLANE_STRIDE(src, 2);

    air_dmabuf_sync(tile->pool[idx].fd, 1);

    /* luma: tile->height content rows from source row crop_y. */
    for (int row = 0; row < tile->height; row++) {
        memcpy(d_y + (gsize)row * ys, s_y + (gsize)(tile->crop_y + row) * sy, AIR_COMP_W);
    }

    /* chroma: height/2 content rows from source chroma row crop_y/2, packed after the luma. */
    for (int row = 0; row < tile->height / 2; row++) {
        memcpy(d_cb + (gsize)row * cs, s_cb + (gsize)(tile->crop_y / 2 + row) * scb, cs);
        memcpy(d_cr + (gsize)row * cs, s_cr + (gsize)(tile->crop_y / 2 + row) * scr, cs);
    }

    air_dmabuf_sync(tile->pool[idx].fd, 0);
}

/** Wrap pool buffer @p idx as a dmabuf GstBuffer with the aligned I420 video meta, tagged so its
 * index returns to the free queue on finalize. */
GstBuffer *air_wrap_tile(struct air_tile *tile, int idx, GstClockTime pts)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    gsize psize[3] = {
        (gsize)ys * tile->height, (gsize)cs * (tile->height / 2), (gsize)cs * (tile->height / 2)
    };
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, psize[0], psize[0] + psize[1], 0 };
    gint stride[GST_VIDEO_MAX_PLANES] = { ys, cs, cs, 0 };
    GstBuffer *buf = gst_buffer_new();
    struct air_recycle *r = g_new(struct air_recycle, 1);

    /* Three memories, one per plane, into the single dma-heap allocation at its plane offsets: this
     * makes gst-v4l2 present a multi-planar format so the wave5 encoder reads each plane at its own
     * offset (matches the DVR's zero-copy encoder feed). */
    for (int plane = 0; plane < 3; plane++) {
        GstMemory *mem = gst_dmabuf_allocator_alloc(g_dmabuf_alloc, dup(tile->pool[idx].fd),
                                                    tile->pool[idx].size);

        gst_memory_resize(mem, offset[plane], psize[plane]);
        gst_buffer_append_memory(buf, mem);
    }

    GST_BUFFER_PTS(buf) = pts;
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
                                   AIR_COMP_W, tile->height, 3, offset, stride);

    r->tile = tile;
    r->idx = idx;
    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), g_recycle_quark, r, air_recycle_notify);

    return buf;
}

/** Push one ready buffer per active tile into its encoder. @p buf is read only for active tiles,
 * and ownership of every entry transfers here whether or not the push succeeds.
 *
 * Takes GstBuffers rather than pool indices because the two source paths build them differently:
 * the pattern and benchmark paths wrap a filled dma-heap buffer, the camera path shares three
 * ranges of a capture buffer with no copy at all. Both then want the same push, the same counters
 * and the same bring-up order.
 *
 * Holds the staggered, ordered bring-up: on the first frame, feed tile 1 (552 lines) and wait
 * for its first encoded output, then tile 0 (560 lines). The order is what matters: the
 * firmware hangs bringing the 552-height instance up after the 560-height instance exists,
 * while 560-after-552 is clean.
 */
void air_push_frame(GstBuffer **buf)
{
    if (g_stagger && !g_primed && g_tile[0].active && g_tile[1].active) {
        /* Tile 1 (the 552-line tile) is brought up first; ML_AIR_ORDER=01 reverses. */
        const char *order_env = getenv("ML_AIR_ORDER");
        int order[2] = { 1, 0 };

        if (order_env != NULL && strcmp(order_env, "01") == 0) {
            order[0] = 0;
            order[1] = 1;
        }

        for (int i = 0; i < 2; i++) {
            struct air_tile *tile = &g_tile[order[i]];
            int waited;

            if (gst_app_src_push_buffer(tile->src, buf[order[i]]) != GST_FLOW_OK) {
                tile->lost++;
            } else {
                tile->pushed++;
            }

            for (waited = 0; waited < 300 && tile->done < 1; waited++) {
                g_usleep(10000);
            }

            g_printerr("[ml-air-video] stagger: tile %d first output %s\n",
                       tile->chn, tile->done >= 1 ? "OK" : "TIMEOUT");
        }

        g_primed = 1;
        return;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            if (gst_app_src_push_buffer(g_tile[i].src, buf[i]) != GST_FLOW_OK) {
                g_tile[i].lost++;
            } else {
                g_tile[i].pushed++;
            }
        }
    }
}

/** Wrap one pool buffer per active tile and push the frame. */
void air_push_pool_frame(const int *idx, GstClockTime pts)
{
    GstBuffer *buf[AIR_NCHN] = { NULL, NULL };

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            buf[i] = air_wrap_tile(&g_tile[i], idx[i], pts);
        }
    }

    air_push_frame(buf);
}

/** Reserve one free pool buffer for every active tile, writing indices into @p idx. Returns 0
 * and counts a drop if any active tile has none, returning what was taken so the pair stays
 * aligned. */
int air_reserve_pair(int *idx)
{
    gpointer p[AIR_NCHN];
    int i;

    for (i = 0; i < AIR_NCHN; i++) {
        p[i] = g_tile[i].active ? g_async_queue_try_pop(g_tile[i].freeq) : NULL;
    }

    for (i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && p[i] == NULL) {
            for (int j = 0; j < AIR_NCHN; j++) {
                if (p[j] != NULL) {
                    g_async_queue_push(g_tile[j].freeq, p[j]);
                }
            }
            g_tile[i].dropped++;

            return 0;
        }
    }

    for (i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            idx[i] = GPOINTER_TO_INT(p[i]) - 1;
        }
    }

    return 1;
}
