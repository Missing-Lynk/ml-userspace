/**
 * @file ml-air-video.c
 * @brief Air-unit synthetic video transmitter for the RF downlink on UDP :10001.
 *
 * Drives a 1920x1080 videotestsrc into the vendor's two-tile split (top 1920x560, bottom
 * 1920x552, 32-row overlap), encodes each tile as an independent H.265 elementary stream, and
 * sends every access unit to the goggle wrapped in the :10001 video_packet_header (see vph.h).
 * The two tiles of one source frame share a FrameId and are distinguished by ChnIndex, exactly
 * as the goggle's ml-pipeline receiver expects. Swapping videotestsrc for the camera later
 * changes only the source element.
 *
 * Each tile is copied into a dma-heap buffer as packed I420 at its coded height (chroma at
 * stride*height, matching the wave5 encoder's plane math and the DVR feed; the allocation is
 * padded to the 16-aligned height only at the tail) and fed by dmabuf-import, so the coded
 * height stays the vendor-exact 560/552.
 *
 * The two encoder instances are brought up staggered and in a fixed order: tile 1 (the
 * 552-line tile) runs its whole create -> seq-init -> framebuffer-registration -> first-frame
 * sequence first, then tile 0 (560 lines). The order is load-bearing: the wave5 firmware
 * deterministically hangs (VCPU watchdog) bringing up the 552-height instance after the
 * 560-height instance exists, while 560-after-552 is clean (HW-confirmed on the AU; 552 is a
 * multiple of the codec's 8-row step but not of 16).
 *
 * Configuration (environment):
 *   ML_AIR_DST        goggle address              (default 10.0.0.1)
 *   ML_AIR_PORT       goggle video port           (default 10001)
 *   ML_AIR_FPS        frame rate                  (default 15)
 *   ML_AIR_PATTERN    videotestsrc pattern        (default ball)
 *   ML_AIR_CAMERA     ar-cvisp node, e.g. /dev/video2: use the sensor instead
 *                     of the synthetic pattern (needs ar-cvisp depth >= 3)
 *   ML_AIR_ENC        encoder element + props     (default v4l2h265enc dmabuf-import, large GOP)
 *   ML_AIR_HEAP       dma_heap name for tile bufs (default: first non-mmz heap, else any)
 *   ML_AIR_DUMP       prefix: also write <prefix>_tileN.h265
 *   ML_AIR_NOTX       encode (and dump) without transmitting
 *   ML_AIR_VERBOSE    log one line per second when set
 *   ML_AIR_ONLY       0|1: encode only that tile (single encoder instance, diagnostic)
 *   ML_AIR_FULL       encode one full 1920x1080 frame instead of tiles (diagnostic)
 *   ML_AIR_SAMEH      run both tiles at 560 rows (diagnostic)
 *   ML_AIR_NO_STAGGER concurrent (racy) encoder bring-up (diagnostic)
 *   ML_AIR_ORDER      01: bring tile 0 up first (diagnostic; known to hang the firmware)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <glib-unix.h>

#include "vph.h"

/** Composite frame geometry (both tiles report this Resolution). */
#define AIR_COMP_W   1920
#define AIR_COMP_H   1080

/** Number of tiles / encode channels. */
#define AIR_NCHN     2

/** dma-heap tile buffers per channel (filled/in-encoder/spare). Allocated at startup; the pool
 * shrinks to whatever the heap yields, down to a floor of AIR_POOL_MIN. */
#define AIR_POOL_MAX 4
#define AIR_POOL_MIN 2

/** Send buffer capacity: the largest a single UDP datagram can be (the kernel IP-fragments it). */
#define AIR_TX_MAX   65507

/* dma-heap allocation + CPU-access sync UAPI (defined locally, as in ml-pipeline). */
struct dma_heap_allocation_data { guint64 len; guint32 fd; guint32 fd_flags; guint64 heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
struct dma_buf_sync { guint64 flags; };
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

/** One pre-allocated dma-heap tile buffer (kept for process lifetime). */
struct air_buf {
    int fd;
    guint8 *map;
    gsize size;
};

/** Per-tile state: the encoder input (appsrc + dma-heap pool) and the transmit side (appsink). */
struct air_tile {
    int chn;                       /* ChnIndex 0 or 1 */
    int active;                    /* 1 unless ML_AIR_ONLY selects the other tile */
    int crop_y;                    /* source row this tile starts at */
    int height;                    /* coded/display height (560 or 552) */
    int alloc_h;                   /* physical rows = ALIGN(height,16) */
    gsize buf_size;                /* dma-heap buffer size (stride*alloc_h*3/2) */

    GstAppSrc *src;                /* encoder input (fed dma-heap I420 buffers) */
    struct air_buf pool[AIR_POOL_MAX];
    int pool_n;
    GAsyncQueue *freeq;            /* free pool indices, stored as (idx+1) */

    int sock;                      /* shared UDP socket */
    struct sockaddr_in dst;        /* goggle :10001 */
    int fps;
    guint32 resolution;            /* composite Resolution word */
    guint8 *txbuf;                 /* per-channel send buffer (off the thread stack) */
    int dumpfd;                    /* ML_AIR_DUMP raw stream, or -1 */
    volatile guint64 sent;
    volatile guint64 dropped;      /* frames skipped because no free buffer */
};

static struct air_tile g_tile[AIR_NCHN];
static GMainLoop *g_loop;
static GstAllocator *g_dmabuf_alloc;
static GstVideoInfo g_src_info;    /* source 1920x1080 I420 layout */
static GQuark g_recycle_quark;
static int g_verbose;
static int g_notx;
static int g_stagger;              /* ML_AIR_STAGGER: serialize the two encoder bring-ups */
static int g_primed;               /* set once the staggered bring-up completed */

/** Bracket CPU writes to a dma-heap buffer so the encoder's DMA sees them (start=1 before the
 * write, start=0 after to flush to DDR). */
static void air_dmabuf_sync(int fd, int start)
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
static int air_pool_init(struct air_tile *t)
{
    const char *pool_env = getenv("ML_AIR_POOL");
    int want = (pool_env != NULL && *pool_env != '\0') ? atoi(pool_env) : AIR_POOL_MAX;

    if (want < AIR_POOL_MIN) {
        want = AIR_POOL_MIN;
    }
    if (want > AIR_POOL_MAX) {
        want = AIR_POOL_MAX;
    }

    t->freeq = g_async_queue_new();
    t->pool_n = 0;

    for (int i = 0; i < want; i++) {
        int fd = air_heap_alloc(t->buf_size);
        guint8 *map;

        if (fd < 0) {
            break;
        }

        map = mmap(NULL, t->buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            break;
        }

        t->pool[i].fd = fd;
        t->pool[i].map = map;
        t->pool[i].size = t->buf_size;
        g_async_queue_push(t->freeq, GINT_TO_POINTER(i + 1));
        t->pool_n++;
    }

    return t->pool_n;
}

/** Return a pool index to the free queue when its GstBuffer is finalized (encoder done with it). */
struct air_recycle {
    struct air_tile *tile;
    int idx;
};

static void air_recycle_notify(gpointer data)
{
    struct air_recycle *r = data;

    g_async_queue_push(r->tile->freeq, GINT_TO_POINTER(r->idx + 1));
    g_free(r);
}

/** Copy one tile out of the source I420 frame into a pool buffer laid out packed at the coded
 * height (chroma at stride*height), matching the wave5 encoder's plane math and the DVR feed. The
 * 16-row alignment lives only in the allocation size (slack at the tail), never in the offsets. */
static void air_fill_tile(struct air_tile *t, int idx, GstVideoFrame *src)
{
    guint8 *dst = t->pool[idx].map;
    const int ys = AIR_COMP_W;            /* dst luma stride (packed) */
    const int cs = AIR_COMP_W / 2;        /* dst chroma stride (packed) */
    guint8 *d_y = dst;
    guint8 *d_cb = dst + (gsize)ys * t->height;
    guint8 *d_cr = d_cb + (gsize)cs * (t->height / 2);

    const guint8 *s_y = GST_VIDEO_FRAME_PLANE_DATA(src, 0);
    const guint8 *s_cb = GST_VIDEO_FRAME_PLANE_DATA(src, 1);
    const guint8 *s_cr = GST_VIDEO_FRAME_PLANE_DATA(src, 2);
    const int sy = GST_VIDEO_FRAME_PLANE_STRIDE(src, 0);
    const int scb = GST_VIDEO_FRAME_PLANE_STRIDE(src, 1);
    const int scr = GST_VIDEO_FRAME_PLANE_STRIDE(src, 2);

    air_dmabuf_sync(t->pool[idx].fd, 1);

    /* luma: t->height content rows from source row crop_y. */
    for (int r = 0; r < t->height; r++) {
        memcpy(d_y + (gsize)r * ys, s_y + (gsize)(t->crop_y + r) * sy, AIR_COMP_W);
    }

    /* chroma: height/2 content rows from source chroma row crop_y/2, packed after the luma. */
    for (int r = 0; r < t->height / 2; r++) {
        memcpy(d_cb + (gsize)r * cs, s_cb + (gsize)(t->crop_y / 2 + r) * scb, cs);
        memcpy(d_cr + (gsize)r * cs, s_cr + (gsize)(t->crop_y / 2 + r) * scr, cs);
    }

    air_dmabuf_sync(t->pool[idx].fd, 0);
}

/** Wrap pool buffer @p idx as a dmabuf GstBuffer with the aligned I420 video meta, tagged so its
 * index returns to the free queue on finalize. */
static GstBuffer *air_wrap_tile(struct air_tile *t, int idx, GstClockTime pts)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    gsize psize[3] = {
        (gsize)ys * t->height, (gsize)cs * (t->height / 2), (gsize)cs * (t->height / 2)
    };
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, psize[0], psize[0] + psize[1], 0 };
    gint stride[GST_VIDEO_MAX_PLANES] = { ys, cs, cs, 0 };
    GstBuffer *buf = gst_buffer_new();
    struct air_recycle *r = g_new(struct air_recycle, 1);

    /* Three memories, one per plane, into the single dma-heap allocation at its plane offsets: this
     * makes gst-v4l2 present a multi-planar format so the wave5 encoder reads each plane at its own
     * offset (matches the DVR's zero-copy encoder feed). */
    for (int p = 0; p < 3; p++) {
        GstMemory *mem = gst_dmabuf_allocator_alloc(g_dmabuf_alloc, dup(t->pool[idx].fd),
                                                    t->pool[idx].size);

        gst_memory_resize(mem, offset[p], psize[p]);
        gst_buffer_append_memory(buf, mem);
    }

    GST_BUFFER_PTS(buf) = pts;
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
                                   AIR_COMP_W, t->height, 3, offset, stride);

    r->tile = t;
    r->idx = idx;
    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), g_recycle_quark, r, air_recycle_notify);
    return buf;
}

/** Source callback: split one 1920x1080 frame into the two aligned tiles and push each into its
 * encoder. Both tiles carry the same PTS, so their encoded FrameIds match. Skips a frame whole
 * if either tile has no free pool buffer, keeping the pair aligned. */
static GstFlowReturn air_on_src(GstAppSink *sink, gpointer user)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *sbuf;
    GstVideoFrame frame;
    GstClockTime pts;
    gpointer p[AIR_NCHN];
    int idx[AIR_NCHN];

    (void)user;
    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    sbuf = gst_sample_get_buffer(sample);
    if (sbuf == NULL || !gst_video_frame_map(&frame, &g_src_info, sbuf, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    pts = GST_BUFFER_PTS(sbuf);

    /* Reserve a free buffer for every active tile; if any active tile has none, keep the pair
     * aligned by returning what we took and dropping the whole frame. */
    for (int i = 0; i < AIR_NCHN; i++) {
        p[i] = g_tile[i].active ? g_async_queue_try_pop(g_tile[i].freeq) : NULL;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && p[i] == NULL) {
            for (int j = 0; j < AIR_NCHN; j++) {
                if (p[j] != NULL) {
                    g_async_queue_push(g_tile[j].freeq, p[j]);
                }
            }
            g_tile[i].dropped++;
            gst_video_frame_unmap(&frame);
            gst_sample_unref(sample);

            return GST_FLOW_OK;
        }
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            idx[i] = GPOINTER_TO_INT(p[i]) - 1;
            air_fill_tile(&g_tile[i], idx[i], &frame);
        }
    }
    gst_video_frame_unmap(&frame);

    /* Staggered, ordered bring-up: on the first frame, feed tile 1 (552 lines) and wait for
     * its first encoded output, then tile 0 (560 lines). The order is what matters: the
     * firmware hangs bringing the 552-height instance up after the 560-height instance
     * exists, while 560-after-552 is clean. */
    if (g_stagger && !g_primed && g_tile[0].active && g_tile[1].active) {
        /* Tile 1 (the 552-line tile) is brought up first; ML_AIR_ORDER=01 reverses. */
        const char *order_env = getenv("ML_AIR_ORDER");
        int order[2] = { 1, 0 };

        if (order_env != NULL && strcmp(order_env, "01") == 0) {
            order[0] = 0;
            order[1] = 1;
        }

        for (int o = 0; o < 2; o++) {
            struct air_tile *t = &g_tile[order[o]];
            int waited;

            gst_app_src_push_buffer(t->src, air_wrap_tile(t, idx[order[o]], pts));
            for (waited = 0; waited < 300 && t->sent < 1; waited++) {
                g_usleep(10000);
            }
            g_printerr("[ml-air-video] stagger: tile %d first output %s\n",
                       t->chn, t->sent >= 1 ? "OK" : "TIMEOUT");
        }

        g_primed = 1;
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            gst_app_src_push_buffer(g_tile[i].src, air_wrap_tile(&g_tile[i], idx[i], pts));
        }
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/** Encoder-output callback: frame one tile access unit and send it to the goggle. */
static GstFlowReturn air_on_enc(GstAppSink *sink, gpointer user)
{
    struct air_tile *t = user;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *buf;
    GstMapInfo map;
    guint32 frame_id;
    guint32 is_idr;
    guint32 ts_ms;
    size_t len;

    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    buf = gst_sample_get_buffer(sample);
    if (buf == NULL || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    if (GST_BUFFER_PTS_IS_VALID(buf)) {
        frame_id = (guint32)gst_util_uint64_scale(GST_BUFFER_PTS(buf), t->fps, GST_SECOND);
    } else {
        frame_id = 0;
    }

    is_idr = GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT) ? 0 : 1;
    ts_ms = (guint32)((guint64)frame_id * 1000u / (guint32)t->fps);

    if (t->dumpfd >= 0) {
        if (write(t->dumpfd, map.data, map.size) != (ssize_t)map.size) {
            /* best-effort capture */
        }
    }

    if (!g_notx) {
        len = vph_build(t->txbuf, AIR_TX_MAX, (guint32)t->chn, is_idr, frame_id, ts_ms,
                        t->resolution, map.data, (guint32)map.size);
        if (len > 0) {
            if (sendto(t->sock, t->txbuf, len, MSG_DONTWAIT,
                       (struct sockaddr *)&t->dst, sizeof t->dst) == (ssize_t)len) {
                t->sent++;
            }
        }
    } else {
        t->sent++;
    }

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static gboolean air_on_signal(gpointer user)
{
    (void)user;
    if (g_loop != NULL) {
        g_main_loop_quit(g_loop);
    }

    return G_SOURCE_REMOVE;
}

static gboolean air_on_tick(gpointer user)
{
    (void)user;
    if (g_verbose) {
        g_printerr("[ml-air-video] tx chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT
                   " dropped=%" G_GUINT64_FORMAT "\n",
                   g_tile[0].sent, g_tile[1].sent, g_tile[0].dropped);
    }

    return G_SOURCE_CONTINUE;
}

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : dflt;
}

static gboolean air_on_bus(GstBus *bus, GstMessage *msg, gpointer user)
{
    (void)bus;
    (void)user;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;

        gst_message_parse_error(msg, &err, &dbg);
        g_printerr("[ml-air-video] error from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        if (dbg != NULL) {
            g_printerr("[ml-air-video] debug: %s\n", dbg);
        }

        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(g_loop);
    } break;

    case GST_MESSAGE_EOS: {
        g_printerr("[ml-air-video] end of stream\n");
        g_main_loop_quit(g_loop);
    } break;

    default: {
    } break;
    }

    return TRUE;
}

/** Build one per-tile encode pipeline: appsrc -> encoder -> h265parse -> appsink. Returns the
 * pipeline and stores the tile's appsrc + appsink, or NULL on failure. */
static GstElement *air_build_encoder(struct air_tile *t, const char *enc, int fps, GstBus **bus_out)
{
    char desc[1024];
    GError *err = NULL;
    GstElement *pipe;
    GstElement *el;
    GstAppSinkCallbacks cbs;

    snprintf(desc, sizeof desc,
             "appsrc name=in is-live=true format=time do-timestamp=false "
             "caps=video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1 ! "
             "%s ! h265parse ! video/x-h265,stream-format=byte-stream,alignment=au ! "
             "appsink name=out sync=false max-buffers=8 drop=false",
             AIR_COMP_W, t->height, fps, enc);

    pipe = gst_parse_launch(desc, &err);
    if (pipe == NULL) {
        g_printerr("[ml-air-video] encoder %d build failed: %s\n", t->chn, err ? err->message : "?");
        if (err != NULL) {
            g_error_free(err);
        }

        return NULL;
    }

    el = gst_bin_get_by_name(GST_BIN(pipe), "in");
    t->src = GST_APP_SRC(el);

    el = gst_bin_get_by_name(GST_BIN(pipe), "out");
    memset(&cbs, 0, sizeof cbs);
    cbs.new_sample = air_on_enc;
    gst_app_sink_set_callbacks(GST_APP_SINK(el), &cbs, t, NULL);
    gst_object_unref(el);

    *bus_out = gst_element_get_bus(pipe);
    return pipe;
}

int main(int argc, char **argv)
{
    const char *dst = env_or("ML_AIR_DST", "10.0.0.1");
    int port = atoi(env_or("ML_AIR_PORT", "10001"));
    int fps = atoi(env_or("ML_AIR_FPS", "15"));
    const char *pattern = env_or("ML_AIR_PATTERN", "ball");
    const char *camera = getenv("ML_AIR_CAMERA");
    const char *enc = env_or("ML_AIR_ENC",
                             "v4l2h265enc output-io-mode=dmabuf-import "
                             "extra-controls=\"controls,video_gop_size=65535\"");
    const char *dump = env_or("ML_AIR_DUMP", NULL);
    char desc[512];
    GError *err = NULL;
    GstElement *src_pipe;
    GstElement *enc_pipe[AIR_NCHN];
    GstBus *bus;
    GstAppSinkCallbacks cbs;
    GstElement *el;
    struct sockaddr_in dstaddr;
    int sock;

    g_verbose = (getenv("ML_AIR_VERBOSE") != NULL);
    g_notx = (getenv("ML_AIR_NOTX") != NULL);
    /* Staggered encoder bring-up is the default: concurrent instance creation while the other
     * encoder has frames in flight races the wave5 firmware (corrupt output or a VCPU watchdog,
     * HW-confirmed). ML_AIR_NO_STAGGER restores the concurrent bring-up for diagnostics. */
    g_stagger = (getenv("ML_AIR_NO_STAGGER") == NULL);
    gst_init(&argc, &argv);

    g_recycle_quark = g_quark_from_static_string("air-recycle");
    g_dmabuf_alloc = gst_dmabuf_allocator_new();
    gst_video_info_set_format(&g_src_info, GST_VIDEO_FORMAT_I420, AIR_COMP_W, AIR_COMP_H);

    memset(&dstaddr, 0, sizeof dstaddr);
    dstaddr.sin_family = AF_INET;
    dstaddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, dst, &dstaddr.sin_addr) != 1) {
        g_printerr("[ml-air-video] bad destination address: %s\n", dst);
        return 1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("[ml-air-video] socket");
        return 1;
    }

    /* Tile geometry: tile 0 = top 560 rows, tile 1 = bottom 552 rows (32-row overlap). Each tile is
     * packed I420 at its coded height (chroma at stride*height); the dma-heap buffer is allocated at
     * the 16-aligned height so its total size meets the wave5 raw-buffer minimum, slack at the tail. */
    g_tile[0].chn = 0;
    g_tile[0].crop_y = 0;
    g_tile[0].height = 560;
    g_tile[1].chn = 1;
    g_tile[1].crop_y = 528;
    g_tile[1].height = 552;

    /* ML_AIR_ONLY=0|1 encodes just that tile, isolating a single wave5 encoder instance from the
     * two-instance concurrency (diagnostic: does tile 1 corrupt because of its 552 geometry or
     * because it is the second concurrent /dev/video1 context?). Unset = both tiles. */
    {
        const char *only = getenv("ML_AIR_ONLY");

        g_tile[0].active = (only == NULL || only[0] == '\0' || only[0] == '0');
        g_tile[1].active = (only == NULL || only[0] == '\0' || only[0] == '1');
    }

    /* ML_AIR_SAMEH runs both concurrent instances at the SAME geometry (both 560) to test whether
     * the dual-instance corruption is specific to mismatched (560 vs 552) geometry or affects any
     * two concurrent encodes. tile1 then covers source rows 520..1079. */
    if (getenv("ML_AIR_SAMEH") != NULL) {
        g_tile[1].crop_y = 520;
        g_tile[1].height = 560;
    }

    /* ML_AIR_FULL encodes a single full-frame 1920x1080 tile (diagnostic: does one standard-size
     * encoder instance work at all, independent of the two-tile split?). Overrides ML_AIR_ONLY. */
    if (getenv("ML_AIR_FULL") != NULL) {
        g_tile[0].chn = 0;
        g_tile[0].crop_y = 0;
        g_tile[0].height = AIR_COMP_H;
        g_tile[0].active = 1;
        g_tile[1].active = 0;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        struct air_tile *t = &g_tile[i];

        if (!t->active) {
            continue;
        }

        t->alloc_h = (t->height + 15) & ~15;
        t->buf_size = (gsize)AIR_COMP_W * t->alloc_h * 3 / 2;
        t->sock = sock;
        t->dst = dstaddr;
        t->fps = fps;
        t->resolution = vph_resolution(AIR_COMP_W, AIR_COMP_H);
        t->txbuf = g_malloc(AIR_TX_MAX);
        t->dumpfd = -1;
        t->sent = 0;
        t->dropped = 0;

        if (air_pool_init(t) < AIR_POOL_MIN) {
            g_printerr("[ml-air-video] tile %d: dma-heap pool alloc failed (need %d, got %d)\n",
                       i, AIR_POOL_MIN, t->pool_n);
            return 1;
        }

        if (dump != NULL) {
            char path[300];

            snprintf(path, sizeof path, "%s_tile%d.h265", dump, i);
            t->dumpfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (t->dumpfd >= 0) {
                g_printerr("[ml-air-video] dumping tile %d -> %s\n", i, path);
            }
        }
    }

    /* Per-tile encode pipelines (one v4l2h265enc instance each). */
    for (int i = 0; i < AIR_NCHN; i++) {
        GstBus *ebus = NULL;

        if (!g_tile[i].active) {
            enc_pipe[i] = NULL;
            continue;
        }

        enc_pipe[i] = air_build_encoder(&g_tile[i], enc, fps, &ebus);
        if (enc_pipe[i] == NULL) {
            close(sock);
            return 1;
        }

        gst_bus_add_watch(ebus, air_on_bus, NULL);
        gst_object_unref(ebus);
    }

    /*
     * Source pipeline: full 1080 frames into an appsink, which the tile
     * splitter reads through gst_video_frame_map, so a source stride wider
     * than the width costs nothing here.
     *
     * ML_AIR_CAMERA names the ar-cvisp capture node and swaps the synthetic
     * source for the sensor. The camera runs at its own rate, which is
     * higher than the link budget allows, so videorate drops to ML_AIR_FPS
     * rather than the encoders being asked to swallow 60 fps: two tiles at
     * 15 fps is what the transmit path was validated at. ar-cvisp must be
     * loaded with depth 3 or more, because depth 1 re-arms one buffer per
     * frame and the encoder then reads it mid-overwrite.
     */
    if (camera != NULL) {
        snprintf(desc, sizeof desc,
                 "v4l2src device=%s io-mode=mmap ! "
                 "video/x-raw,format=I420,width=%d,height=%d ! "
                 "videorate drop-only=true ! "
                 "video/x-raw,framerate=%d/1 ! "
                 "appsink name=src sync=false max-buffers=4 drop=true",
                 camera, AIR_COMP_W, AIR_COMP_H, fps);
    } else {
        snprintf(desc, sizeof desc,
                 "videotestsrc is-live=true pattern=%s ! "
                 "video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1 ! "
                 "appsink name=src sync=false max-buffers=4 drop=true",
                 pattern, AIR_COMP_W, AIR_COMP_H, fps);
    }

    src_pipe = gst_parse_launch(desc, &err);
    if (src_pipe == NULL) {
        g_printerr("[ml-air-video] source build failed: %s\n", err ? err->message : "?");
        close(sock);
        return 1;
    }

    el = gst_bin_get_by_name(GST_BIN(src_pipe), "src");
    memset(&cbs, 0, sizeof cbs);
    cbs.new_sample = air_on_src;
    gst_app_sink_set_callbacks(GST_APP_SINK(el), &cbs, NULL, NULL);
    gst_object_unref(el);

    bus = gst_element_get_bus(src_pipe);
    gst_bus_add_watch(bus, air_on_bus, NULL);
    gst_object_unref(bus);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, air_on_signal, NULL);
    g_unix_signal_add(SIGTERM, air_on_signal, NULL);
    g_timeout_add_seconds(1, air_on_tick, NULL);

    if (g_notx) {
        g_printerr("[ml-air-video] %dx%d @ %d fps, two H.265 tiles, encode-only (no transmit)\n",
                   AIR_COMP_W, AIR_COMP_H, fps);
    } else {
        g_printerr("[ml-air-video] %dx%d @ %d fps, two H.265 tiles -> %s:%d\n",
                   AIR_COMP_W, AIR_COMP_H, fps, dst, port);
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_element_set_state(enc_pipe[i], GST_STATE_PLAYING);
        }
    }
    gst_element_set_state(src_pipe, GST_STATE_PLAYING);

    g_main_loop_run(g_loop);

    gst_element_set_state(src_pipe, GST_STATE_NULL);
    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_app_src_end_of_stream(g_tile[i].src);
            gst_element_set_state(enc_pipe[i], GST_STATE_NULL);
        }
    }

    g_printerr("[ml-air-video] stopped (chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT ")\n",
               g_tile[0].sent, g_tile[1].sent);

    gst_object_unref(src_pipe);
    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_object_unref(enc_pipe[i]);
        }
    }
    g_main_loop_unref(g_loop);
    close(sock);
    return 0;
}
