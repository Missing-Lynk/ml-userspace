/**
 * @file ml-air-video.c
 * @brief Air-unit video transmitter for the RF downlink on UDP :10001.
 *
 * Drives a 1920x1080 source into the vendor's two-tile split (top 1920x560, bottom 1920x552,
 * 32-row overlap), encodes each tile as an independent H.265 elementary stream, and sends every
 * access unit to the goggle wrapped in the :10001 video_packet_header (see vph.h). The two tiles
 * of one source frame share a FrameId and are distinguished by ChnIndex, exactly as the goggle's
 * ml-pipeline receiver expects.
 *
 * There are two source paths and they feed the encoders differently.
 *
 * ML_AIR_CAMERA is the production path and copies nothing. The capture node is driven directly
 * (open, REQBUFS, EXPBUF, DQBUF), each plane of each buffer is wrapped in a GstMemory once, and a
 * frame becomes two GstBuffers of three gst_memory_share() views at the tile's row offset. The
 * offsets reach V4L2 as per-plane data_offset and the capture stride reaches it through the video
 * meta, which is what wave5_widen_src_stride() accepts. See the capture section below.
 *
 * videotestsrc is the pattern path and copies each tile into a dma-heap buffer as packed I420 at
 * its coded height (chroma at stride*height, matching the wave5 encoder's plane math and the DVR
 * feed; the allocation is padded to the 16-aligned height only at the tail). That copy is about
 * 6 MB per frame on one A53 and caps this path near 17 fps at 1080p. It is kept because it is the
 * configuration the end-to-end link was validated at, and because the benchmark reuses the same
 * dma-heap pool machinery.
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
 *   ML_AIR_BUFS       capture buffers to request  (default 8; camera path only)
 *   ML_AIR_INFLIGHT   capture buffers the encoders may hold at once (default 3)
 *   ML_AIR_COPY       0|1: feed that tile by copy instead of by sharing the capture buffer,
 *                     so the two encoder instances no longer read one allocation (camera only)
 *   ML_AIR_ENC        encoder element + props     (default v4l2h265enc dmabuf-import, large GOP)
 *   ML_AIR_BITRATE    per-tile encoder bitrate    (default 4000000, half the vendor total)
 *   ML_AIR_VBV        encoder VBV window in ms    (default derived from bitrate)
 *   ML_AIR_CTRL       live control socket          (default /run/missinglynk/air-video.sock)
 *                     commands: bitrate <bps> [vbv-ms], fps <fps>,
 *                     rate <bps> <fps> [vbv-ms]
 *   ML_AIR_HEAP       dma_heap name for tile bufs (default: first non-mmz heap, else any)
 *   ML_AIR_DUMP       prefix: also write <prefix>_tileN.h265
 *   ML_AIR_NOTX       encode (and dump) without transmitting
 *   ML_AIR_VERBOSE    log one line per second when set
 *   ML_AIR_ONLY       0|1: encode only that tile (single encoder instance, diagnostic)
 *   ML_AIR_FULL       encode one full 1920x1080 frame instead of tiles (diagnostic)
 *   ML_AIR_SAMEH      run both tiles at 560 rows (diagnostic)
 *   ML_AIR_NO_STAGGER concurrent (racy) encoder bring-up (diagnostic)
 *   ML_AIR_ORDER      01: bring tile 0 up first (diagnostic; known to hang the firmware)
 *
 * Throughput benchmark (ML_AIR_BENCH, no source pipeline at all):
 *   ML_AIR_BENCH      comma-separated tiers from static|bars|detail|noise, run back to back on
 *                     one encoder instance pair (e.g. static,bars,detail)
 *   ML_AIR_BENCH_SECS seconds per tier (default 10); the run ends when the last tier does
 *   ML_AIR_RING       ring length in frames (default 8; static ignores content variation)
 *   ML_AIR_BENCH_FREE resubmit as fast as buffers return instead of pacing at ML_AIR_FPS
 *
 * The benchmark exists to separate the encoder and RF ceiling from the capture path: a
 * pre-rendered ring touches no CVISP buffer, so it measures without the carveout resize the
 * zero-copy path needs. Rendering per frame is not viable (a 1080p field is about 3.1 MB of
 * writes, so 60 fps is roughly 187 MB/s into uncached memory, the same order as the tile copy
 * that caps the live path); rendering once and resubmitting costs nothing per frame.
 *
 * The three patterns bound different things and only the middle one decides anything:
 *   static  every ring buffer identical. After the first IRAP every block codes as skip, so
 *           this bounds the plumbing with the encoder near-idle. Necessary, not sufficient.
 *   bars    colour bars scrolled per frame plus a per-frame dither. The scroll alone is close
 *           to best case, because a global translation is exactly what motion estimation
 *           predicts; the dither is what stops every block resolving to a motion vector and
 *           no residual.
 *   detail  a low-passed noise texture sampled with a different horizontal velocity per band.
 *           Dense high-frequency structure so coefficients are expensive, and no global motion
 *           vector that predicts the frame, but still compressible. This is the worst case.
 *   noise   full entropy in every block. Diagnostic only: incompressible data expands under
 *           entropy coding, overflows any bitstream buffer bounded by the raw frame, and wedges
 *           the instance, so it measures the buffer rather than the encoder.
 *
 * A ring of 8 rather than an A/B flip is deliberate. Two frames alternating can be defeated by
 * reference reuse (bring-up reports fbc_buf_count 2), which would silently return every other
 * frame to the skip-coded best case.
 *
 * None of those four is watchable, and that is inherent rather than a defect. The ring holds
 * pool_n fixed images and the feeder takes whichever buffer the encoder released first, so the
 * displayed phase hops among pool_n values in completion order instead of advancing, and the two
 * tiles sit on unrelated phases so the seam between them does not line up. Judging any of them by
 * eye reads as corruption.
 *
 * `scroll` is the tier to look at. It renders every frame with the phase taken from the shared
 * frame counter, so motion is continuous and both tiles agree at the seam. That is a full CPU
 * pass per frame per tile, the same cost that caps the live path near 17 fps, so it measures
 * nothing: it exists to put a correct picture on the panel.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include "vph.h"

/** Composite frame geometry (both tiles report this Resolution). */
#define AIR_COMP_W   1920
#define AIR_COMP_H   1080
/* Pixels the `scroll` tier advances per frame. 8 divides 1920, so the pattern wraps exactly and
 * the loop has no discontinuity. */
#define AIR_SCROLL_PX 8

/** Number of tiles / encode channels. */
#define AIR_NCHN     2

/** Direct V4L2 encoder queues, camera path only. CAPTURE is MMAP and holds coded access units;
 * OUTPUT is DMABUF and never owns memory, so its count only bounds how many capture buffers can
 * be in the encoder at once and is sized above ML_AIR_INFLIGHT. */
#define AIR_ENC_CAP_BUFS 6
#define AIR_ENC_OUT_BUFS 8
/* An encoder holding source buffers this long has stopped making progress. Generous next to a
 * 16 ms frame period, because the cost of acting is the frames in flight and the cost of a false
 * positive at a lower threshold is a needless glitch. */
#define AIR_ENC_STALL_US (500 * 1000)
/* Recovery rebuilds the encoder instance, so a tile that needs it repeatedly is not recovering.
 * Retire it and keep the rest of the pipeline running rather than rebuild forever. */
#define AIR_ENC_MAX_RESTARTS 5

/** dma-heap tile buffers per channel (filled/in-encoder/spare). Allocated at startup; the pool
 * shrinks to whatever the heap yields, down to a floor of AIR_POOL_MIN. AIR_POOL_MAX bounds the
 * array only: the live path asks for AIR_POOL_DEF, and the benchmark asks for its ring length,
 * which is the only caller that wants more than a pipelining depth. */
#define AIR_POOL_MAX 16
#define AIR_POOL_DEF 4
#define AIR_POOL_MIN 2

/** Benchmark ring length when ML_AIR_RING is unset. Long enough that the period exceeds any
 * plausible reference window, so no frame can find an exact match and code as skip. */
#define AIR_RING_DEF 8

/** Send buffer capacity: the largest a single UDP datagram can be (the kernel IP-fragments it). */
#define AIR_TX_MAX   65507
#define AIR_VPH_HEADER 36
#define AIR_VPH_TAIL 4

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
    /* ML_AIR_COPY: this tile is copied out of the capture buffer instead of sharing it. The
     * diagnostic for the two-instance watchdog, and the fallback if sharing one capture buffer
     * between two encoder instances turns out to be what the firmware cannot take. */
    int copy;
    int alloc_h;                   /* physical rows = ALIGN(height,16) */
    gsize buf_size;                /* dma-heap buffer size (stride*alloc_h*3/2) */

    /* Direct V4L2 encoder, used by the camera path only. GStreamer takes the source
     * bytesperline from the caps at S_FMT (gstv4l2object.c, "format.fmt.pix_mp.plane_fmt[i]
     * .bytesperline = GST_VIDEO_INFO_PLANE_STRIDE"), so 1920-wide caps describe the 2048-pitch
     * capture buffer as 1920 and every row slips 128 bytes. Per-buffer video meta cannot fix
     * that: S_FMT is negotiated before any buffer exists. Stating the stride needs the ioctl. */
    int enc_fd;
    int enc_cap_n;
    guint8 *enc_cap_map[AIR_ENC_CAP_BUFS];
    gsize enc_cap_len[AIR_ENC_CAP_BUFS];
    int enc_out_n;
    struct air_cap_buf *enc_out_cb[AIR_ENC_OUT_BUFS];  /* index -> buffer held, NULL if free */
    guint32 enc_seq;               /* frame_id for emitted access units */
    gint64 enc_progress_us;        /* monotonic time of the last buffer this encoder returned */
    guint32 enc_restarts;          /* instance rebuilds performed */
    char enc_node[32];             /* node to re-open on recovery */
    int enc_fps;                   /* rate to re-apply on recovery */
    int enc_bitrate;               /* bitrate to re-apply on recovery */
    int enc_vbv;                   /* VBV window to re-apply on recovery */
    int fps_pending;               /* rate adopted on the next access unit, 0 when none */
    guint32 ts_base_id;            /* frame_id the VPH timestamp base was taken at */
    guint32 ts_base_ms;            /* VPH timestamp at ts_base_id */

    GstAppSrc *src;                /* encoder input (fed dma-heap I420 buffers) */
    guint8 *tex;                   /* detail tier's shared source texture, built once */
    struct air_buf pool[AIR_POOL_MAX];
    int pool_n;
    GAsyncQueue *freeq;            /* free pool indices, stored as (idx+1) */

    int sock;                      /* shared UDP socket */
    struct sockaddr_in dst;        /* goggle :10001 */
    int fps;
    guint32 resolution;            /* composite Resolution word */
    guint8 *txbuf;                 /* per-channel send buffer (off the thread stack) */
    int dumpfd;                    /* ML_AIR_DUMP raw stream, or -1 */
    volatile guint64 sent;         /* access units actually transmitted */
    volatile guint64 dropped;      /* frames skipped because no free buffer */
    volatile guint64 pushed;       /* frames handed to the encoder */
    /* Encoder output, counted whether or not it goes out on the wire. Kept apart from `sent`
     * because a transmit to an absent peer fails, and both the bring-up handshake and the
     * benchmark ask "did the encoder produce a frame", not "did the goggle receive one". */
    volatile guint64 done;
    volatile guint64 bytes;        /* encoded bytes out of the encoder */
    /* Transmit losses, split by cause and counted separately from `dropped` (which is ring-slot
     * starvation only). Both were previously silent: the frame vanished, `sent` simply did not
     * advance, and every other counter on both ends still read healthy.
     *
     * oversize is a hard protocol limit, not a tuning problem. One access unit goes in one UDP
     * datagram, so anything above AIR_TX_MAX - VPH overhead cannot be sent at all. Bytes per
     * frame scale inversely with frame rate at a fixed bitrate, so a rate low enough makes the
     * encoder's ordinary peaks uncarriable. */
    volatile guint64 tx_oversize;  /* access unit too large for one datagram */
    volatile guint64 tx_error;     /* sendto failed or sent short */
    int tx_errno;                  /* errno of the most recent tx_error */
    guint32 tx_maxlen;             /* largest access unit seen, for sizing the limit against */
    /* A frame lost between the encoder and this program: appsrc refused the input, or a pulled
     * sample could not be mapped. Rare, but the same failure class as tx_oversize was: without a
     * counter the frame is gone and every other number still reads healthy. */
    volatile guint64 lost;
};

/* Source frames discarded before any tile saw them: the live appsink could not be mapped. Not
 * per-tile, because the frame is dropped upstream of the split. */
static volatile guint64 g_src_lost;

static struct air_tile g_tile[AIR_NCHN];
static GMainLoop *g_loop;
static GstAllocator *g_dmabuf_alloc;
static GstVideoInfo g_src_info;    /* source 1920x1080 I420 layout */
static GQuark g_recycle_quark;
static int g_verbose;
static int g_notx;
/* Camera path drives the encoders through V4L2 directly instead of GStreamer, which is the only
 * way to state the capture stride. ML_AIR_GST=1 restores the GStreamer path for comparison. */
static int g_enc_direct;

/* The direct encoder backend is defined below the capture code but driven from inside it. */
struct air_cap_buf;
static int air_enc_queue(struct air_tile *t, struct air_cap_buf *cb);
static void air_enc_drain(struct air_tile *t);
static void air_enc_restart(struct air_tile *t);
static void air_enc_close(struct air_tile *t);
static int air_enc_held(const struct air_tile *t);
static void air_emit_au(struct air_tile *t, const guint8 *data, size_t size,
                        guint32 frame_id, guint32 is_idr);

static int g_stagger;              /* ML_AIR_STAGGER: serialize the two encoder bring-ups */
static int g_primed;               /* set once the staggered bring-up completed */
static int g_bench_free;           /* ML_AIR_BENCH_FREE: resubmit unpaced */
static volatile int g_bench_stop;  /* feeder thread exit flag */
static int g_bench_fps;            /* pacing rate and PTS base for the feeder */
static int g_bench_secs;           /* ML_AIR_BENCH_SECS: seconds per tier */
static char **g_bench_stages;      /* ML_AIR_BENCH split on commas */
static const char *g_bench_stage;  /* tier currently running, for the rate line */
static int g_ctrl_fd = -1;
static char g_ctrl_path[108];

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
static int air_pool_init(struct air_tile *t, int want)
{
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

/* BT.601 colour bars, left to right: white, yellow, cyan, green, magenta, red, blue, black.
 * Nominal studio-range values, not a conformance-checked SMPTE field; the benchmark cares about
 * the spectral content, not the colorimetry. */
static const guint8 air_bar_y[8] = { 235, 210, 170, 145, 106,  81,  41,  16 };
static const guint8 air_bar_u[8] = { 128,  16, 166,  54, 202,  90, 240, 128 };
static const guint8 air_bar_v[8] = { 128, 146,  16,  34, 222, 240, 110, 128 };

/** xorshift32. Deterministic so two runs of the same tier encode identical content. */
static guint32 air_rand(guint32 *s)
{
    guint32 x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;

    return x;
}

/** Render colour bars into a tile buffer, packed I420 at the coded height (chroma at
 * stride*height), the layout air_wrap_tile describes to the encoder.
 *
 * @p shift scrolls the bars horizontally. On its own that is close to best case, because a
 * global translation is what motion estimation is built to predict; @p dither perturbs luma by
 * a few LSB per pixel with a per-frame seed, which is what leaves a residual behind after the
 * motion vector and makes the tier represent something.
 */
static void air_render_bars(struct air_tile *t, guint8 *dst, int shift, int dither)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    guint8 *y = dst;
    guint8 *cb = dst + (gsize)ys * t->height;
    guint8 *cr = cb + (gsize)cs * (t->height / 2);
    guint32 s = 0x1234567u + (guint32)shift * 2654435761u + (guint32)t->chn;

    for (int r = 0; r < t->height; r++) {
        for (int c = 0; c < ys; c++) {
            int bar = ((c + shift) % AIR_COMP_W) * 8 / AIR_COMP_W;
            int v = air_bar_y[bar];

            if (dither) {
                v += (int)(air_rand(&s) & 7) - 3;
            }

            y[(gsize)r * ys + c] = (guint8)CLAMP(v, 16, 235);
        }
    }

    for (int r = 0; r < t->height / 2; r++) {
        for (int c = 0; c < cs; c++) {
            int bar = ((c * 2 + shift) % AIR_COMP_W) * 8 / AIR_COMP_W;

            cb[(gsize)r * cs + c] = air_bar_u[bar];
            cr[(gsize)r * cs + c] = air_bar_v[bar];
        }
    }
}

/** Build the detail tier's source texture: white noise low-passed with a 3x3 box.
 *
 * The blur is the whole point. Full-entropy content expands under entropy coding, so a frame of
 * it cannot fit a bitstream buffer bounded by the raw frame, and the encoder returns
 * WAVE5_SYSERR_VLC_BUF_FULL before any rate can be measured. That makes `noise` a measurement of
 * the buffer rather than of the encoder. Low-passing keeps dense high-frequency structure, which
 * is what costs coefficients, while leaving the frame compressible enough to code.
 */
static void air_make_texture(struct air_tile *t)
{
    const int w = AIR_COMP_W;
    const int h = t->height;
    guint8 *raw = g_malloc((gsize)w * h);
    guint32 s = 0x2545f491u ^ (guint32)t->chn;

    t->tex = g_malloc((gsize)w * h);

    for (gsize i = 0; i < (gsize)w * h; i++) {
        raw[i] = (guint8)(16 + (air_rand(&s) % 220));
    }

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int acc = 0;
            int n = 0;

            for (int dr = -1; dr <= 1; dr++) {
                int rr = r + dr;

                if (rr < 0 || rr >= h) {
                    continue;
                }

                for (int dc = -1; dc <= 1; dc++) {
                    int cc = c + dc;

                    if (cc < 0 || cc >= w) {
                        continue;
                    }

                    acc += raw[(gsize)rr * w + cc];
                    n++;
                }
            }

            t->tex[(gsize)r * w + c] = (guint8)(acc / n);
        }
    }

    g_free(raw);
}

/** Detail tier: the shared texture sampled with a different horizontal velocity per band.
 *
 * A single global scroll is close to best case, because one motion vector predicts the whole
 * frame. Eight bands moving at different speeds and in alternating directions leave no global
 * vector that works, so the encoder pays real residual across the picture while the content
 * itself stays codeable. This is the worst case that produces a number instead of a wedge.
 */
static void air_render_detail(struct air_tile *t, guint8 *dst, int frame)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    const int bands = 8;
    guint8 *y = dst;
    guint8 *cb = dst + (gsize)ys * t->height;
    guint8 *cr = cb + (gsize)cs * (t->height / 2);
    int band_h = t->height / bands;

    if (t->tex == NULL) {
        air_make_texture(t);
    }

    if (band_h < 1) {
        band_h = 1;
    }

    for (int r = 0; r < t->height; r++) {
        int b = r / band_h;
        int vel;
        int off;
        const guint8 *src = t->tex + (gsize)r * ys;

        if (b > bands - 1) {
            b = bands - 1;
        }

        vel = (b & 1) ? -(3 + b) : (3 + b);
        off = ((frame * vel) % AIR_COMP_W + AIR_COMP_W) % AIR_COMP_W;

        for (int c = 0; c < ys; c++) {
            y[(gsize)r * ys + c] = src[(c + off) % AIR_COMP_W];
        }
    }

    /* Chroma carries the same structure at a quarter of the excursion: a sensor's chroma is
     * smoother than its luma, and a full-amplitude chroma field would be harder than anything
     * the camera can produce. */
    for (int r = 0; r < t->height / 2; r++) {
        const guint8 *src = t->tex + (gsize)(r * 2) * ys;
        int off = (frame * 5) % AIR_COMP_W;

        for (int c = 0; c < cs; c++) {
            int v = (int)src[(c * 2 + off) % AIR_COMP_W] - 128;

            cb[(gsize)r * cs + c] = (guint8)(128 + (v / 4));
            cr[(gsize)r * cs + c] = (guint8)(128 - (v / 4));
        }
    }
}

/** Fill a tile buffer with pseudo-random bytes across the whole packed extent. Every block then
 * carries full spectral energy and no prediction mode is cheap.
 *
 * Kept as a diagnostic, not as the worst-case tier: it reliably overflows the bitstream buffer
 * and wedges the instance, so it yields no rate. Use `detail` for a worst case that measures.
 */
static void air_render_noise(struct air_tile *t, guint8 *dst, int frame)
{
    const gsize len = (gsize)AIR_COMP_W * t->height
                      + (gsize)(AIR_COMP_W / 2) * (t->height / 2) * 2;
    guint32 s = 0x9e3779b9u ^ ((guint32)frame * 2654435761u) ^ (guint32)t->chn;
    gsize i = 0;

    while (i + 4 <= len) {
        guint32 v = air_rand(&s);

        dst[i++] = (guint8)(16 + (v & 0xdf));
        dst[i++] = (guint8)(16 + ((v >> 8) & 0xdf));
        dst[i++] = (guint8)(16 + ((v >> 16) & 0xdf));
        dst[i++] = (guint8)(16 + ((v >> 24) & 0xdf));
    }
    while (i < len) {
        dst[i++] = (guint8)(16 + (air_rand(&s) & 0xdf));
    }
}

/** Render ring slot @p idx. `static` writes the same field into every slot, so the ring still
 * supplies pipelining depth while the content never changes; anything unrecognised is bars. */
static void air_render_one(struct air_tile *t, int idx, const char *pattern)
{
    air_dmabuf_sync(t->pool[idx].fd, 1);

    if (strcmp(pattern, "noise") == 0) {
        air_render_noise(t, t->pool[idx].map, idx);
    } else if (strcmp(pattern, "detail") == 0) {
        air_render_detail(t, t->pool[idx].map, idx);
    } else if (strcmp(pattern, "static") == 0) {
        air_render_bars(t, t->pool[idx].map, 0, 0);
    } else {
        air_render_bars(t, t->pool[idx].map, idx * 7, 1);
    }

    air_dmabuf_sync(t->pool[idx].fd, 0);
}

/** Render the whole ring. */
static void air_render_ring(struct air_tile *t, const char *pattern)
{
    for (int i = 0; i < t->pool_n; i++) {
        air_render_one(t, i, pattern);
    }
}

/* Tiers differ only in buffer content, so running them as separate processes would open a fresh
 * pair of wave5 instances for each. Instances after the first pair in a boot watchdog or produce
 * nothing at all (HW-confirmed, and reconfirmed the hard way: a third instance sat in PIC_RUN
 * emitting zero frames), which would corrupt exactly the measurement being taken. So the ring is
 * rewritten in place and one instance pair lives for the whole run.
 *
 * The rewrite happens slot by slot as the feeder reserves each one, not in a barrier at the tier
 * boundary. Draining the whole free queue first looks tidier but cannot finish: the encoder keeps
 * the most recently pushed buffer referenced until another replaces it, so one slot per tile
 * never comes back and carries the previous tier's content for the whole of the next one.
 * Renewing on reserve covers every slot, because a reserved slot is held exclusively.
 */

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
static void air_push_frame(GstBuffer **buf)
{
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

            if (gst_app_src_push_buffer(t->src, buf[order[o]]) != GST_FLOW_OK) {
                t->lost++;
            } else {
                t->pushed++;
            }
            for (waited = 0; waited < 300 && t->done < 1; waited++) {
                g_usleep(10000);
            }
            g_printerr("[ml-air-video] stagger: tile %d first output %s\n",
                       t->chn, t->done >= 1 ? "OK" : "TIMEOUT");
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
static void air_push_pool_frame(const int *idx, GstClockTime pts)
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
static int air_reserve_pair(int *idx)
{
    gpointer p[AIR_NCHN];

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

            return 0;
        }
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            idx[i] = GPOINTER_TO_INT(p[i]) - 1;
        }
    }

    return 1;
}

/** Benchmark feeder: resubmit the pre-rendered ring with no source pipeline and no per-frame CPU
 * work at all. Paced to ML_AIR_FPS by default, so a shortfall shows up as drops and the
 * sustainable rate is what the counters report; ML_AIR_BENCH_FREE instead waits for a buffer to
 * come back and pushes immediately, which measures the ceiling rather than a target. */
static gpointer air_bench_feed(gpointer user)
{
    const gint64 period = G_USEC_PER_SEC / g_bench_fps;
    guint64 n = 0;

    (void)user;

    for (int s = 0; g_bench_stages[s] != NULL && !g_bench_stop; s++) {
        gint64 next = g_get_monotonic_time();
        gint64 until = next + (gint64)g_bench_secs * G_USEC_PER_SEC;
        guint32 renewed[AIR_NCHN] = { 0, 0 };
        guint64 at_start[AIR_NCHN];
        guint64 seen[AIR_NCHN];
        gint64 progress = next;
        int need[AIR_NCHN];
        int scroll;

        for (int i = 0; i < AIR_NCHN; i++) {
            at_start[i] = g_tile[i].done;
            seen[i] = g_tile[i].done;
        }

        /* The first tier was rendered at startup; later tiers renew each slot on reserve. */
        for (int i = 0; i < AIR_NCHN; i++) {
            need[i] = (s > 0 && g_tile[i].active) ? g_tile[i].pool_n : 0;
        }

        g_bench_stage = g_bench_stages[s];
        scroll = strcmp(g_bench_stage, "scroll") == 0;

        if (scroll) {
            for (int i = 0; i < AIR_NCHN; i++) {
                need[i] = 0;
            }
        }

        g_printerr("[ml-air-video] bench: tier %s for %d s%s\n", g_bench_stage, g_bench_secs,
                   scroll ? " (visual tier: renders every frame, not a rate measurement)" : "");

        while (!g_bench_stop && g_get_monotonic_time() < until) {
            int idx[AIR_NCHN];

            if (g_bench_free) {
                /* Block on each active tile so the loop tracks buffer returns rather than
                 * spinning on try_pop; the pop is put straight back for air_reserve_pair. */
                gboolean starved = FALSE;

                for (int i = 0; i < AIR_NCHN; i++) {
                    if (g_tile[i].active) {
                        gpointer p = g_async_queue_timeout_pop(g_tile[i].freeq, 100000);

                        if (p == NULL) {
                            starved = TRUE;
                            break;
                        }
                        g_async_queue_push_front(g_tile[i].freeq, p);
                    }
                }

                if (starved) {
                    continue;
                }
            }

            if (air_reserve_pair(idx)) {
                if (scroll) {
                    /* Visual tier: phase comes from the shared frame counter, so both tiles
                     * render the same point in the scroll and the tile seam is continuous.
                     * Re-rendering every frame is the only way to get smooth motion; the ring
                     * tiers hold pool_n fixed phases and the feeder replays them in buffer
                     * completion order, which hops rather than scrolls and puts the two tiles
                     * on unrelated phases. That costs a CPU pass per frame per tile, so this
                     * tier measures nothing: use it to look at a picture, not to take a rate. */
                    for (int i = 0; i < AIR_NCHN; i++) {
                        if (g_tile[i].active) {
                            struct air_tile *t = &g_tile[i];

                            air_dmabuf_sync(t->pool[idx[i]].fd, 1);
                            /* No dither. It exists to defeat motion estimation so the ring tiers
                             * carry residual, which is the opposite of what a visual tier wants:
                             * perturbing every pixel every frame costs about 5x the bytes and
                             * pushes access units past the 65467 B datagram ceiling, where they
                             * are discarded and the picture breaks. A pure translation of flat
                             * bars is what keeps frames small enough to arrive. */
                            air_render_bars(t, t->pool[idx[i]].map,
                                            (int)((n * AIR_SCROLL_PX) % AIR_COMP_W), 0);
                            air_dmabuf_sync(t->pool[idx[i]].fd, 0);
                        }
                    }
                } else {
                    for (int i = 0; i < AIR_NCHN; i++) {
                        if (need[i] > 0 && !(renewed[i] & (1u << idx[i]))) {
                            air_render_one(&g_tile[i], idx[i], g_bench_stage);
                            renewed[i] |= 1u << idx[i];
                            need[i]--;
                        }
                    }
                }

                air_push_pool_frame(idx,
                                    gst_util_uint64_scale(n, GST_SECOND, (guint64)g_bench_fps));
                n++;
            }

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active && g_tile[i].done != seen[i]) {
                    seen[i] = g_tile[i].done;
                    progress = g_get_monotonic_time();
                }
            }

            if (!g_bench_free) {
                gint64 now;

                next += period;
                now = g_get_monotonic_time();
                if (next > now) {
                    g_usleep((gulong)(next - now));
                } else {
                    next = now;
                }
            }
        }

        /* Per-tier verdict. Some tiers are rate measurements and some are survival tests, and
         * the counters alone do not say which failed: a wedged encoder and a slow one both show
         * a low rate. Stall time since the last output separates them. */
        {
            double idle = (double)(g_get_monotonic_time() - progress) / G_USEC_PER_SEC;

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active) {
                    g_printerr("[ml-air-video] bench: tier %s tile %d: %" G_GUINT64_FORMAT
                               " frames, %" G_GUINT64_FORMAT " oversize, %" G_GUINT64_FORMAT
                               " tx errors, %" G_GUINT64_FORMAT " lost, largest AU %u B%s\n",
                               g_bench_stage, i, g_tile[i].done - at_start[i],
                               g_tile[i].tx_oversize, g_tile[i].tx_error, g_tile[i].lost,
                               g_tile[i].tx_maxlen, idle > 2.0 ? ", STALLED" : "");
                }
            }

            if (idle > 2.0) {
                g_printerr("[ml-air-video] bench: tier %s produced no output for %.1f s: FAIL\n",
                           g_bench_stage, idle);
            }
        }
    }

    /* All tiers done: end the run rather than idling, so a scripted benchmark terminates. */
    if (g_loop != NULL) {
        g_main_loop_quit(g_loop);
    }

    return NULL;
}

/*
 * Zero-copy camera capture.
 *
 * The capture node is driven directly rather than through v4l2src, for one reason: the buffer
 * count. The block completes a buffer only when another is queued, so every buffer the encoder
 * holds is one the rotation cannot use, and the count is what decides whether capture and encode
 * pipeline at all. REQBUFS takes it as an argument; gst-v4l2 derives it from a negotiation whose
 * inputs (the driver's V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, which this node does not implement, and
 * a downstream allocation query appsink does not answer) are not ours to set. Driving the node
 * also removes the appsink queue and the videorate element from the latency path.
 *
 * Each plane of each buffer is exported once with EXPBUF and wrapped in a GstMemory that lives
 * for the process. A frame is then two GstBuffers of three gst_memory_share() views into those
 * memories, at the tile's row offset. No allocation, no copy, no CPU read.
 *
 * The share carries its offset into V4L2 as the plane's data_offset, and the video meta's wider
 * stride makes gst-v4l2 re-run S_FMT on the encoder's OUTPUT queue at 2048 bytes, which is what
 * wave5_widen_src_stride() exists to accept.
 */
#define AIR_CAP_PLANES     3
#define AIR_CAP_BUFS_MAX   16
#define AIR_CAP_BUFS_DEF   8
/* Frames the encoders may hold at once. Each one is a buffer withheld from the rotation, and
 * also a frame of queueing ahead of the encoder, so this is a latency bound as much as a credit
 * bound. The rest of the pool stays with the driver. */
#define AIR_CAP_INFLIGHT_DEF 3

/** One capture buffer: its three exported planes, and how many tiles still reference it. */
struct air_cap_buf {
    GstMemory *mem[AIR_CAP_PLANES];
    guint8 *map[AIR_CAP_PLANES];   /* CPU view, only read by the ML_AIR_COPY path */
    /* The exported plane, kept as a bare fd as well as a GstMemory. The direct V4L2 encoder
     * path queues the fd itself, because it is the only way to state the capture stride to the
     * encoder: gstv4l2 takes the source bytesperline from the caps at S_FMT, so a 1920-wide
     * caps describes a 2048-pitch buffer as 1920 and every row slips 128 bytes. */
    int fd[AIR_CAP_PLANES];
    gsize len[AIR_CAP_PLANES];
    unsigned int index;
    gint refs;
};

static int g_cap_fd = -1;
static struct air_cap_buf g_cap[AIR_CAP_BUFS_MAX];
static int g_cap_n;
static int g_cap_inflight_max;
static int g_cap_fps;                   /* ML_AIR_FPS: 0 means take every frame the node gives */
static gint g_cap_inflight;
static guint32 g_cap_stride[AIR_CAP_PLANES];
static volatile int g_cap_stop;
static volatile guint64 g_cap_frames;   /* dequeued from the node */
static volatile guint64 g_cap_skipped;  /* returned unused: rate limit or no credit */

/** Hand one capture buffer back to the driver. */
static void air_cap_qbuf(struct air_cap_buf *cb)
{
    struct v4l2_plane planes[AIR_CAP_PLANES];
    struct v4l2_buffer b;

    /* Tile buffers outlive the node: the encoder pipelines are torn down after air_cap_close,
     * so the last releases arrive with the fd already gone. Not an error, just late. */
    if (g_cap_fd < 0) {
        return;
    }

    memset(planes, 0, sizeof planes);
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = cb->index;
    b.length = AIR_CAP_PLANES;
    b.m.planes = planes;

    if (ioctl(g_cap_fd, VIDIOC_QBUF, &b) != 0) {
        perror("[ml-air-video] capture QBUF");
    }
}

/** Last tile buffer sharing this capture buffer was finalized: the encoders are done reading it,
 * so it goes back to the rotation. Runs on whichever thread finalized the tile buffer. */
static void air_cap_release(gpointer data)
{
    struct air_cap_buf *cb = data;

    if (g_atomic_int_dec_and_test(&cb->refs)) {
        g_atomic_int_add(&g_cap_inflight, -1);
        air_cap_qbuf(cb);
    }
}

/** Byte offset of a tile's first row within a capture plane. */
static gsize air_cap_off(const struct air_tile *t, int plane)
{
    if (plane == 0) {
        return (gsize)t->crop_y * g_cap_stride[0];
    }

    return (gsize)(t->crop_y / 2) * g_cap_stride[plane];
}

/** Bytes of a capture plane a tile covers.
 *
 * The ALIGNED height, not the coded one. The encoder aligns its source geometry to 16 rows, so
 * s_fmt on a 552-row tile reports a 560-row plane, gst-v4l2 accumulates its plane offsets from
 * those sizeimages, and the alignment set on the video meta below computes 560-row plane sizes.
 * Sharing only 552 rows would make all three disagree with each other.
 *
 * The extra rows are read into the encoder's source buffer and never coded, because the coded
 * height stays the requested 552 (kernel patch 0280 item 6). Tile 1 then ends flush against the
 * end of the capture plane: 528 + 560 = 1088 rows, which is exactly what the block allocates.
 */
static gsize air_cap_len(const struct air_tile *t, int plane)
{
    if (plane == 0) {
        return (gsize)t->alloc_h * g_cap_stride[0];
    }

    return (gsize)(t->alloc_h / 2) * g_cap_stride[plane];
}

/** Build one tile's encoder input as three shared views of @p cb, with no copy.
 *
 * The video meta carries the capture strides, not the picture width. That is what makes
 * gst-v4l2 re-negotiate the encoder's OUTPUT format to a 2048-byte luma stride; without it the
 * encoder would read 1920-byte rows out of a 2048-byte-pitch frame and shear the picture.
 */
static GstBuffer *air_cap_share(struct air_tile *t, struct air_cap_buf *cb, GstClockTime pts)
{
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, 0, 0, 0 };
    gint stride[GST_VIDEO_MAX_PLANES] = { 0, 0, 0, 0 };
    GstVideoAlignment align;
    GstVideoMeta *meta;
    GstBuffer *buf = gst_buffer_new();
    gsize flat = 0;

    for (int p = 0; p < AIR_CAP_PLANES; p++) {
        gsize len = air_cap_len(t, p);

        gst_buffer_append_memory(buf, gst_memory_share(cb->mem[p], (gssize)air_cap_off(t, p),
                                                       (gssize)len));
        offset[p] = flat;
        stride[p] = (gint)g_cap_stride[p];
        flat += len;
    }

    GST_BUFFER_PTS(buf) = pts;
    meta = gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
                                          AIR_COMP_W, t->height, AIR_CAP_PLANES, offset, stride);

    /* The padding has to be stated, not just implied by the strides. A meta carrying offsets and
     * strides but no alignment fails its own consistency check: gst_video_meta_get_plane_size
     * recomputes the strides from the picture size with zero alignment, gets 1920/960/960 against
     * our 2048/1024/1024, and returns FALSE. gst_video_meta_get_plane_height then returns FALSE
     * without writing anything, and gst-v4l2 passes a zero padded_height into
     * gst_v4l2_object_match_buffer_layout, which drops back to the current format height and
     * never sets obj->align.padding_bottom.
     *
     * padding_right is in pixels: 2048 - 1920 = 128 gives luma stride 2048 and chroma 1024.
     * padding_bottom is the 16-row source alignment the encoder applies anyway, 8 rows on the
     * 552 tile and 0 on the 560 one. With both set the meta reproduces exactly the plane sizes
     * air_cap_len shares, so plane_height comes out as the aligned height and every number
     * gst-v4l2 derives matches what the driver reports.
     */
    gst_video_alignment_reset(&align);
    align.padding_right = g_cap_stride[0] - AIR_COMP_W;
    align.padding_bottom = (guint)(t->alloc_h - t->height);
    if (!gst_video_meta_set_alignment(meta, align)) {
        g_printerr("[ml-air-video] tile %d: video meta rejected padding %u right %u bottom\n",
                   t->chn, align.padding_right, align.padding_bottom);
    }

    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), g_recycle_quark, cb, air_cap_release);
    return buf;
}

/** Copy one tile out of the capture buffer into a pool buffer, packed at the coded height.
 *
 * The same layout air_fill_tile produces, so air_wrap_tile takes it unchanged. Reads come out of
 * a no-map coherent carveout and are therefore uncached, which is the whole cost: about 1.6 MB
 * per tile per frame, against zero for a share.
 */
static void air_cap_fill(struct air_tile *t, int idx, struct air_cap_buf *cb)
{
    guint8 *dst = t->pool[idx].map;
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    guint8 *d_y = dst;
    guint8 *d_cb = dst + (gsize)ys * t->height;
    guint8 *d_cr = d_cb + (gsize)cs * (t->height / 2);

    air_dmabuf_sync(t->pool[idx].fd, 1);

    for (int r = 0; r < t->height; r++) {
        memcpy(d_y + (gsize)r * ys,
               cb->map[0] + (gsize)(t->crop_y + r) * g_cap_stride[0], AIR_COMP_W);
    }

    for (int r = 0; r < t->height / 2; r++) {
        memcpy(d_cb + (gsize)r * cs,
               cb->map[1] + (gsize)(t->crop_y / 2 + r) * g_cap_stride[1], cs);
        memcpy(d_cr + (gsize)r * cs,
               cb->map[2] + (gsize)(t->crop_y / 2 + r) * g_cap_stride[2], cs);
    }

    air_dmabuf_sync(t->pool[idx].fd, 0);
}

/** Open the capture node, export every plane of every buffer, and start streaming.
 * Returns 0 on success. */
static int air_cap_open(const char *dev, int want)
{
    struct v4l2_requestbuffers req;
    struct v4l2_format fmt;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (want < 2) {
        want = 2;
    }
    if (want > AIR_CAP_BUFS_MAX) {
        want = AIR_CAP_BUFS_MAX;
    }

    g_cap_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (g_cap_fd < 0) {
        g_printerr("[ml-air-video] open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    /* The node's geometry is not negotiable (it is the geometry the block is configured for),
     * so this reads the format rather than proposing one, and the strides come from the driver
     * instead of from a constant here that could drift from it. */
    memset(&fmt, 0, sizeof fmt);
    fmt.type = type;
    if (ioctl(g_cap_fd, VIDIOC_G_FMT, &fmt) != 0) {
        g_printerr("[ml-air-video] %s G_FMT: %s\n", dev, strerror(errno));
        return -1;
    }

    if (fmt.fmt.pix_mp.num_planes != AIR_CAP_PLANES ||
        fmt.fmt.pix_mp.width != AIR_COMP_W || fmt.fmt.pix_mp.height != AIR_COMP_H) {
        g_printerr("[ml-air-video] %s is %ux%u in %u planes, expected %dx%d in %d\n",
                   dev, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
                   fmt.fmt.pix_mp.num_planes, AIR_COMP_W, AIR_COMP_H, AIR_CAP_PLANES);
        return -1;
    }

    for (int p = 0; p < AIR_CAP_PLANES; p++) {
        g_cap_stride[p] = fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
    }

    memset(&req, 0, sizeof req);
    req.count = (unsigned int)want;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_cap_fd, VIDIOC_REQBUFS, &req) != 0) {
        g_printerr("[ml-air-video] %s REQBUFS %d: %s\n", dev, want, strerror(errno));
        return -1;
    }

    if (req.count > AIR_CAP_BUFS_MAX) {
        req.count = AIR_CAP_BUFS_MAX;
    }
    g_cap_n = (int)req.count;

    for (int i = 0; i < g_cap_n; i++) {
        struct v4l2_plane planes[AIR_CAP_PLANES];
        struct v4l2_buffer b;
        struct air_cap_buf *cb = &g_cap[i];

        memset(planes, 0, sizeof planes);
        memset(&b, 0, sizeof b);
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned int)i;
        b.length = AIR_CAP_PLANES;
        b.m.planes = planes;
        if (ioctl(g_cap_fd, VIDIOC_QUERYBUF, &b) != 0) {
            g_printerr("[ml-air-video] QUERYBUF %d: %s\n", i, strerror(errno));
            return -1;
        }

        cb->index = (unsigned int)i;
        for (int p = 0; p < AIR_CAP_PLANES; p++) {
            struct v4l2_exportbuffer exp;

            memset(&exp, 0, sizeof exp);
            exp.type = type;
            exp.index = (unsigned int)i;
            exp.plane = (unsigned int)p;
            exp.flags = O_RDWR | O_CLOEXEC;
            if (ioctl(g_cap_fd, VIDIOC_EXPBUF, &exp) != 0) {
                g_printerr("[ml-air-video] EXPBUF %d plane %d: %s\n", i, p, strerror(errno));
                return -1;
            }

            cb->len[p] = planes[p].length;
            /* gst_dmabuf_allocator_alloc takes the fd, so the V4L2 path gets its own. */
            cb->fd[p] = dup(exp.fd);
            cb->mem[p] = gst_dmabuf_allocator_alloc(g_dmabuf_alloc, exp.fd, cb->len[p]);
            if (cb->fd[p] < 0) {
                g_printerr("[ml-air-video] dup(dmabuf) %d plane %d: %s\n", i, p, strerror(errno));
                return -1;
            }

            /* Only the ML_AIR_COPY path reads this. Mapped unconditionally because it costs one
             * mmap per plane at startup and nothing per frame, and because a tile switching to
             * the copy path mid-run would otherwise need the node re-opened. */
            cb->map[p] = mmap(NULL, cb->len[p], PROT_READ, MAP_SHARED, g_cap_fd,
                              (off_t)planes[p].m.mem_offset);
            if (cb->map[p] == MAP_FAILED) {
                cb->map[p] = NULL;
            }
        }

        air_cap_qbuf(cb);
    }

    /* A tile that runs off the end of a plane is rejected by the kernel at QBUF time, one frame
     * at a time and with nothing said about why. Check it once, here, where the numbers are. */
    for (int i = 0; i < AIR_NCHN; i++) {
        struct air_tile *t = &g_tile[i];

        if (!t->active) {
            continue;
        }

        for (int p = 0; p < AIR_CAP_PLANES; p++) {
            if (t->copy && g_cap[0].map[p] == NULL) {
                g_printerr("[ml-air-video] tile %d is ML_AIR_COPY but plane %d did not mmap\n",
                           t->chn, p);
                return -1;
            }

            if (air_cap_off(t, p) + air_cap_len(t, p) > g_cap[0].len[p]) {
                g_printerr("[ml-air-video] tile %d plane %d: rows %d..%d at stride %u needs "
                           "%" G_GSIZE_FORMAT " B of a %" G_GSIZE_FORMAT " B plane\n",
                           t->chn, p, t->crop_y, t->crop_y + t->height, g_cap_stride[p],
                           air_cap_off(t, p) + air_cap_len(t, p), g_cap[0].len[p]);
                return -1;
            }
        }
    }

    if (ioctl(g_cap_fd, VIDIOC_STREAMON, &type) != 0) {
        g_printerr("[ml-air-video] STREAMON: %s\n", strerror(errno));
        return -1;
    }

    g_printerr("[ml-air-video] camera %s: %d buffers, strides %u/%u/%u, up to %d in flight\n",
               dev, g_cap_n, g_cap_stride[0], g_cap_stride[1], g_cap_stride[2],
               g_cap_inflight_max);
    return 0;
}

/** Capture thread: dequeue, share into both tiles, push. A frame that is not wanted (rate limit)
 * or cannot be afforded (no credit) goes straight back to the driver, which is the cheapest
 * possible drop and keeps the rotation fed. */
static gpointer air_cap_feed(gpointer user)
{
    struct pollfd pfd[1 + AIR_NCHN];
    int enc_slot[AIR_NCHN];
    gint64 base_us = 0;
    gint64 due_us = 0;
    gint64 cap_err_us = 0;
    /* Silence is only the encoder's fault while frames are still arriving for it. A quiet camera
     * would otherwise read as a stalled encoder and buy a rebuild that fixes nothing. */
    gint64 last_cap_us = 0;
    int nfd;

    (void)user;

    /* Slot 0 is the camera; the direct path's encoders take the slots after it. The set is fixed
     * for the life of the thread because a tile cannot become active after start-up. */
    memset(pfd, 0, sizeof pfd);
    pfd[0].fd = g_cap_fd;
    pfd[0].events = POLLIN;
    nfd = 1;

    for (int i = 0; i < AIR_NCHN; i++) {
        enc_slot[i] = -1;

        if (g_enc_direct && g_tile[i].active && g_tile[i].enc_fd >= 0) {
            enc_slot[i] = nfd;
            pfd[nfd].fd = g_tile[i].enc_fd;
            pfd[nfd].events = POLLIN | POLLOUT;
            nfd++;
        }
    }

    while (!g_cap_stop) {
        struct v4l2_plane planes[AIR_CAP_PLANES];
        struct v4l2_buffer b;
        struct air_cap_buf *cb;
        GstBuffer *buf[AIR_NCHN] = { NULL, NULL };
        gpointer pool[AIR_NCHN] = { NULL, NULL };
        int cap_fps;
        gint64 period;
        gint64 ts_us;
        gint64 now_us;
        int nshare = 0;
        int starved = 0;

        for (int i = 0; i < nfd; i++) {
            pfd[i].revents = 0;
        }

        /* Polled rather than blocking in DQBUF, so the exit path does not depend on a frame
         * arriving to be noticed. */
        if (poll(pfd, nfd, 200) <= 0) {
            continue;
        }

        /* The encoders are in the same poll set as the camera, so a coded frame is collected on
         * its own readiness rather than waiting for the next capture frame, and output keeps
         * flowing if capture stalls. v4l2-mem2mem raises POLLOUT for a finished source buffer and
         * POLLIN for a finished coded one, and only ever from a done list, so this cannot spin.
         * Draining is also what lowers g_cap_inflight, which is why it must stay ahead of the
         * in-flight gate below: behind it, reaching the ceiling would deadlock the loop. */
        now_us = g_get_monotonic_time();

        for (int i = 0; i < AIR_NCHN; i++) {
            struct air_tile *t = &g_tile[i];

            if (enc_slot[i] < 0) {
                continue;
            }

            if (pfd[enc_slot[i]].revents & (POLLIN | POLLOUT)) {
                air_enc_drain(t);
            }

            /* Nothing here ends the stream. A stalled encoder costs the frames it is holding
             * and nothing else: an error on the fd, or silence for AIR_ENC_STALL_US while
             * source buffers are outstanding, cycles its OUTPUT queue and carries on. The
             * alternative is worse than a glitch, because the frames it holds are capture
             * frames and g_cap_inflight cannot fall until they are back. */
            if (pfd[enc_slot[i]].revents & (POLLERR | POLLNVAL)) {
                g_printerr("[ml-air-video] tile %d: encoder poll 0x%x, recovering\n",
                           t->chn, pfd[enc_slot[i]].revents);
                air_enc_restart(t);
            } else if (air_enc_held(t) > 0 && now_us - t->enc_progress_us > AIR_ENC_STALL_US &&
                       now_us - last_cap_us < AIR_ENC_STALL_US) {
                g_printerr("[ml-air-video] tile %d: encoder silent for %" G_GINT64_FORMAT " ms "
                           "holding %d frames, recovering\n",
                           t->chn, (now_us - t->enc_progress_us) / 1000, air_enc_held(t));
                air_enc_restart(t);
            } else {
                continue;
            }

            /* Recovery replaced the fd, or retired the tile. Either way the poll set is stale. */
            pfd[enc_slot[i]].fd = t->active ? t->enc_fd : -1;
        }

        if ((pfd[0].revents & POLLIN) == 0) {
            continue;
        }

        memset(planes, 0, sizeof planes);
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = AIR_CAP_PLANES;
        b.m.planes = planes;

        /* A failed dequeue costs this frame, not the stream. The loop goes back to poll and
         * picks up whenever the camera produces again; only g_cap_stop ends it. Reported at most
         * once a second so a persistent fault cannot flood a 32 MiB /tmp. */
        if (ioctl(g_cap_fd, VIDIOC_DQBUF, &b) != 0) {
            if (errno != EINTR && now_us - cap_err_us > G_USEC_PER_SEC) {
                cap_err_us = now_us;
                g_printerr("[ml-air-video] capture DQBUF: %s\n", strerror(errno));
            }

            continue;
        }

        cb = &g_cap[b.index];
        g_cap_frames++;
        last_cap_us = now_us;

        ts_us = (gint64)b.timestamp.tv_sec * G_USEC_PER_SEC + b.timestamp.tv_usec;
        if (base_us == 0) {
            base_us = ts_us;
            due_us = ts_us;
        }

        cap_fps = g_atomic_int_get(&g_cap_fps);
        period = cap_fps > 0 ? G_USEC_PER_SEC / cap_fps : 0;

        /* Rate limit against the driver's own timestamps rather than wall clock, so a frame is
         * judged by when it was captured. period is truncated down, so at a request equal to the
         * sensor rate every frame is due and nothing is dropped. */
        if (period > 0 && ts_us < due_us) {
            g_cap_skipped++;
            air_cap_qbuf(cb);
            continue;
        }

        if (g_atomic_int_get(&g_cap_inflight) >= g_cap_inflight_max) {
            g_cap_skipped++;
            air_cap_qbuf(cb);
            continue;
        }

        if (period > 0) {
            due_us += period;
            if (due_us < ts_us) {
                due_us = ts_us + period;
            }
        }

        /* Direct V4L2: the capture buffer is queued into each encoder as it stands, at the
         * tile's row offset, with no GstBuffer in between. */
        if (g_enc_direct) {
            int nq = 0;

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active) {
                    nq++;
                }
            }

            g_atomic_int_inc(&g_cap_inflight);
            g_atomic_int_set(&cb->refs, nq);

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active && air_enc_queue(&g_tile[i], cb) != 0) {
                    /* This encoder will never return the buffer, so drop its reference now. */
                    air_cap_release(cb);
                }
            }

            continue;
        }

        /* Copy tiles need a pool slot before anything is committed, because running out of one
         * halfway through a frame would leave the pair misaligned. Share tiles need nothing. */
        for (int i = 0; i < AIR_NCHN; i++) {
            if (!g_tile[i].active) {
                continue;
            }

            if (!g_tile[i].copy) {
                nshare++;
                continue;
            }

            pool[i] = g_async_queue_try_pop(g_tile[i].freeq);
            if (pool[i] == NULL) {
                starved = 1;
            }
        }

        if (starved) {
            for (int i = 0; i < AIR_NCHN; i++) {
                if (pool[i] != NULL) {
                    g_async_queue_push(g_tile[i].freeq, pool[i]);
                }
            }

            g_tile[0].dropped++;
            g_cap_skipped++;
            air_cap_qbuf(cb);
            continue;
        }

        /* Only the sharing tiles hold the capture buffer, so only they are counted. With every
         * tile copied nothing holds it and it goes straight back below. */
        if (nshare > 0) {
            g_atomic_int_inc(&g_cap_inflight);
            g_atomic_int_set(&cb->refs, nshare);
        }

        for (int i = 0; i < AIR_NCHN; i++) {
            GstClockTime pts = (GstClockTime)(ts_us - base_us) * GST_USECOND;

            if (!g_tile[i].active) {
                continue;
            }

            if (g_tile[i].copy) {
                int idx = GPOINTER_TO_INT(pool[i]) - 1;

                air_cap_fill(&g_tile[i], idx, cb);
                buf[i] = air_wrap_tile(&g_tile[i], idx, pts);
            } else {
                buf[i] = air_cap_share(&g_tile[i], cb, pts);
            }
        }

        if (nshare == 0) {
            air_cap_qbuf(cb);
        }

        air_push_frame(buf);
    }

    return NULL;
}

/** Stop the capture node. The exported memories are deliberately not freed: shares of them may
 * still be in an encoder queue, and the process is exiting. */
static void air_cap_close(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (g_cap_fd < 0) {
        return;
    }

    ioctl(g_cap_fd, VIDIOC_STREAMOFF, &type);
    close(g_cap_fd);
    g_cap_fd = -1;
}

static const char *env_or(const char *name, const char *dflt);

static int air_vbv_for_bitrate(int bitrate)
{
    guint64 limit = AIR_TX_MAX - AIR_VPH_HEADER - AIR_VPH_TAIL;
    guint64 vbv;

    if (bitrate <= 0) {
        return 10;
    }

    vbv = limit * 8000u * 3u / 4u / (guint64)bitrate;
    if (vbv < 10) {
        vbv = 10;
    } else if (vbv > 3000) {
        vbv = 3000;
    }

    return (int)vbv;
}

/** Set one integer encoder control, reporting but not failing on a control the driver lacks. */
static void air_enc_ctrl(struct air_tile *t, guint32 id, gint32 val, const char *name)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;

    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.value = val;
    cs.count = 1;
    cs.controls = &c;
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;

    if (ioctl(t->enc_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        g_printerr("[ml-air-video] tile %d: %s: %s\n", t->chn, name, strerror(errno));
    }
}

/** Set one integer encoder control on a live instance, reporting the failure to the caller. */
static int air_enc_set_int(struct air_tile *t, guint32 id, gint32 val, const char *name)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;

    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.value = val;
    cs.count = 1;
    cs.controls = &c;
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;

    if (ioctl(t->enc_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        g_printerr("[ml-air-video] tile %d: live %s %d: %s\n",
                   t->chn, name, val, strerror(errno));
        return -1;
    }

    return 0;
}

static int air_enc_set_bitrate(struct air_tile *t, int bitrate)
{
    if (!t->active || t->enc_fd < 0 || t->enc_bitrate == bitrate) {
        return 0;
    }

    if (air_enc_set_int(t, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate") != 0) {
        return -1;
    }

    t->enc_bitrate = bitrate;
    return 0;
}

static int air_enc_set_vbv(struct air_tile *t, int vbv)
{
    if (!t->active || t->enc_fd < 0 || t->enc_vbv == vbv) {
        return 0;
    }

    if (air_enc_set_int(t, V4L2_CID_MPEG_VIDEO_VBV_SIZE, vbv, "vbv") != 0) {
        return -1;
    }

    t->enc_vbv = vbv;
    return 0;
}

static int air_enc_set_fps(struct air_tile *t, int fps)
{
    struct v4l2_streamparm sp;

    if (!t->active || t->enc_fd < 0) {
        return 0;
    }

    if (t->enc_fps == fps) {
        return 0;
    }

    memset(&sp, 0, sizeof sp);
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = (guint32)fps;
    if (ioctl(t->enc_fd, VIDIOC_S_PARM, &sp) != 0) {
        g_printerr("[ml-air-video] tile %d: live fps %d: %s\n",
                   t->chn, fps, strerror(errno));
        return -1;
    }

    t->fps_pending = fps;
    t->enc_fps = fps;
    return 0;
}

static int air_active_encoder_count(void)
{
    int count = 0;

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0) {
            count++;
        }
    }

    return count;
}

static int air_set_bitrate_all(int bitrate, int vbv)
{
    int ret = 0;
    int active = air_active_encoder_count();
    int changed = 0;

    if (bitrate < 1 || bitrate > 700000000 || vbv < 10 || vbv > 3000) {
        return -1;
    }

    if (active == 0) {
        g_printerr("[ml-air-video] live bitrate needs the direct V4L2 encoder path\n");
        return -1;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0 &&
            (g_tile[i].enc_bitrate != bitrate || g_tile[i].enc_vbv != vbv)) {
            changed++;
        }
        /* Bitrate before the window: going down, the pair is briefly the new low rate against
         * the old short window, which is the constrained direction. The reverse order would
         * leave the old high rate against the new long window and permit a burst. */
        if (air_enc_set_bitrate(&g_tile[i], bitrate) != 0) {
            ret = -1;
        }
        if (air_enc_set_vbv(&g_tile[i], vbv) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] live bitrate %d bps/tile, vbv %d ms%s\n",
                   bitrate, vbv, changed == 0 ? " (unchanged)" : "");
    }

    return ret;
}

/* Force an IDR on every active tile.
 *
 * This stream carries exactly one IDR, at FrameId 0, and P-frames for the rest of the session, so a
 * receiver that was not listening at session start cannot decode and no amount of motion repairs it:
 * the encoder picks intra-vs-inter against its OWN reference, which is correct, so it never notices
 * that the receiver's is not. The vendor covers this with an on-demand keyframe
 * (AR_LOWDELAY_MESSAGE_MEDIA_IDR_REQUEST -> AR_LDRT_TX_PIPELINE_IdrEnable); this is our end of it.
 * V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME is a button control, so the value is ignored.
 */
static int air_force_keyframe_all(void)
{
    int ret = 0;
    int forced = 0;

    if (air_active_encoder_count() == 0) {
        g_printerr("[ml-air-video] keyframe needs the direct V4L2 encoder path\n");
        return -1;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (!g_tile[i].active || g_tile[i].enc_fd < 0) {
            continue;
        }

        if (air_enc_set_int(&g_tile[i], V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "keyframe") != 0) {
            ret = -1;
        } else {
            forced++;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] forced a keyframe on %d tile(s)\n", forced);
    }

    return ret;
}

static int air_set_fps_all(int fps)
{
    int ret = 0;
    int active = air_active_encoder_count();
    int changed = 0;

    if (fps < 1 || fps > 240) {
        return -1;
    }

    if (active == 0) {
        g_printerr("[ml-air-video] live fps needs the direct V4L2 encoder path\n");
        return -1;
    }

    /* Slow the feeder before telling rate control about the new rate. The encoder budgets
     * bitrate/fps per picture, so the window where the two disagree either under-spends (feeder
     * already slow, rate control still on the old high rate) or over-spends. Under-spending is
     * the safe side of an RF link that is sized for the requested rate. */
    g_atomic_int_set(&g_cap_fps, fps);

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0 && g_tile[i].enc_fps != fps) {
            changed++;
        }
        if (air_enc_set_fps(&g_tile[i], fps) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] live fps %d%s\n", fps, changed == 0 ? " (unchanged)" : "");
    }

    return ret;
}

static int air_set_rate_all(int bitrate, int fps, int vbv)
{
    int ret;

    /* Range-check both halves before applying either, so a rejected fps cannot leave the
     * encoders running at the new bitrate while the caller is told the command failed. */
    if (fps < 1 || fps > 240) {
        return -1;
    }

    ret = air_set_bitrate_all(bitrate, vbv);
    if (ret != 0) {
        return ret;
    }

    return air_set_fps_all(fps);
}

static gboolean air_on_ctrl(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition cond,
                            G_GNUC_UNUSED gpointer user)
{
    char buf[256];
    char reply[128];
    int cfd;
    struct pollfd pfd;
    ssize_t n;
    int bitrate;
    int fps;
    int vbv = -1;
    int ret;

    cfd = accept4(g_ctrl_fd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) {
        return G_SOURCE_CONTINUE;
    }

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = cfd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 100) <= 0) {
        close(cfd);
        return G_SOURCE_CONTINUE;
    }

    n = read(cfd, buf, sizeof buf - 1);
    if (n <= 0) {
        close(cfd);
        return G_SOURCE_CONTINUE;
    }
    buf[n] = '\0';

    ret = sscanf(buf, "rate %d %d %d", &bitrate, &fps, &vbv);
    if (ret >= 2) {
        if (vbv < 0) {
            vbv = air_vbv_for_bitrate(bitrate);
        }
        ret = air_set_rate_all(bitrate, fps, vbv);
        snprintf(reply, sizeof reply, "%s bitrate=%d fps=%d vbv=%d\n",
                 ret == 0 ? "ok" : "err", bitrate, fps, vbv);
    } else if (sscanf(buf, "bitrate %d %d", &bitrate, &vbv) >= 1) {
        if (vbv < 0) {
            vbv = air_vbv_for_bitrate(bitrate);
        }
        ret = air_set_bitrate_all(bitrate, vbv);
        snprintf(reply, sizeof reply, "%s bitrate=%d vbv=%d\n", ret == 0 ? "ok" : "err",
                 bitrate, vbv);
    } else if (sscanf(buf, "fps %d", &fps) == 1) {
        ret = air_set_fps_all(fps);
        snprintf(reply, sizeof reply, "%s fps=%d\n", ret == 0 ? "ok" : "err", fps);
    } else if (strncmp(buf, "keyframe", 8) == 0) {
        ret = air_force_keyframe_all();
        snprintf(reply, sizeof reply, "%s keyframe\n", ret == 0 ? "ok" : "err");
    } else {
        ret = -1;
        snprintf(reply, sizeof reply,
                 "err expected: bitrate <bps> [vbv] | fps <fps> | rate <bps> <fps> [vbv]"
                 " | keyframe\n");
    }

    (void)write(cfd, reply, strlen(reply));
    close(cfd);
    return G_SOURCE_CONTINUE;
}

static int air_ctrl_open(const char *path)
{
    struct sockaddr_un addr;
    char *dir;

    if (strlen(path) >= sizeof addr.sun_path) {
        g_printerr("[ml-air-video] control socket path too long: %s\n", path);
        return -1;
    }

    dir = g_path_get_dirname(path);
    if (dir != NULL && g_mkdir_with_parents(dir, 0755) != 0) {
        g_printerr("[ml-air-video] mkdir %s: %s\n", dir, strerror(errno));
        g_free(dir);
        return -1;
    }
    g_free(dir);

    g_ctrl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_ctrl_fd < 0) {
        g_printerr("[ml-air-video] control socket: %s\n", strerror(errno));
        return -1;
    }

    unlink(path);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    if (bind(g_ctrl_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(g_ctrl_fd, 4) != 0) {
        g_printerr("[ml-air-video] bind %s: %s\n", path, strerror(errno));
        close(g_ctrl_fd);
        g_ctrl_fd = -1;
        return -1;
    }

    g_strlcpy(g_ctrl_path, path, sizeof g_ctrl_path);
    g_unix_fd_add(g_ctrl_fd, G_IO_IN, air_on_ctrl, NULL);
    g_printerr("[ml-air-video] control socket %s\n", path);
    return 0;
}

/** Find the node that encodes to HEVC, by capability rather than by index.
 *
 * The camera node is skipped by path: it is already open, and a node that offers HEVC on its
 * capture queue is what makes an encoder, so probing is the only thing that stays true if the
 * enumeration order moves. ML_AIR_ENC_NODE overrides the search. */
static int air_enc_find_node(const char *camera, char *out, size_t len)
{
    const char *forced = getenv("ML_AIR_ENC_NODE");

    if (forced != NULL) {
        g_strlcpy(out, forced, len);
        return 0;
    }

    for (int n = 0; n < 16; n++) {
        struct v4l2_fmtdesc d;
        char path[32];
        int fd;
        int hit = 0;

        g_snprintf(path, sizeof path, "/dev/video%d", n);
        if (camera != NULL && strcmp(path, camera) == 0) {
            continue;
        }

        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        memset(&d, 0, sizeof d);
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        for (d.index = 0; hit == 0 && ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++) {
            if (d.pixelformat == V4L2_PIX_FMT_HEVC) {
                hit = 1;
            }
        }

        close(fd);

        if (hit) {
            g_strlcpy(out, path, len);
            return 0;
        }
    }

    g_printerr("[ml-air-video] no node offers HEVC on its capture queue\n");
    return -1;
}

/** Open and configure one encoder instance for this tile.
 *
 * The whole reason this path exists is the OUTPUT S_FMT below: bytesperline comes from the
 * capture node, not from the picture width, and the driver's answer is checked. A stride the
 * encoder silently rewrote produced two sessions of sheared video that every counter reported
 * as healthy, so a mismatch fails the run here rather than becoming a picture nobody decodes. */
static int air_enc_open(struct air_tile *t, const char *dev, int fps)
{
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    struct v4l2_streamparm sp;
    /* Vendor AR_8030_TX_GetBitRate() derives the encoder target from live RF throughput
     * (throughput * Ar803xThroutputRate, capped at ArMaxBitRate) and returns 8000 kbps when
     * throughput reads zero. 8000 kbps is the total across both tiles, so half it here. Link
     * adaptation should drive the live control path rather than move this default. */
    int bitrate = t->enc_bitrate > 0 ? t->enc_bitrate : atoi(env_or("ML_AIR_BITRATE", "4000000"));
    int vbv = t->enc_vbv > 0 ? t->enc_vbv : atoi(env_or("ML_AIR_VBV", "0"));
    enum v4l2_buf_type otype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int p;
    int i;

    /* O_NONBLOCK is load bearing: air_enc_drain empties both queues by dequeuing until DQBUF
     * fails, which on a blocking fd never happens. */
    t->enc_fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (t->enc_fd < 0) {
        g_printerr("[ml-air-video] tile %d: open %s: %s\n", t->chn, dev, strerror(errno));
        return -1;
    }

    memset(&f, 0, sizeof f);
    f.type = otype;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    f.fmt.pix_mp.width = AIR_COMP_W;
    f.fmt.pix_mp.height = (guint32)t->height;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = AIR_CAP_PLANES;
    for (p = 0; p < AIR_CAP_PLANES; p++) {
        f.fmt.pix_mp.plane_fmt[p].bytesperline = g_cap_stride[p];
        f.fmt.pix_mp.plane_fmt[p].sizeimage = (guint32)air_cap_len(t, p);
    }

    if (ioctl(t->enc_fd, VIDIOC_S_FMT, &f) != 0) {
        g_printerr("[ml-air-video] tile %d: S_FMT OUTPUT: %s\n", t->chn, strerror(errno));
        return -1;
    }

    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M ||
        f.fmt.pix_mp.num_planes != AIR_CAP_PLANES) {
        g_printerr("[ml-air-video] tile %d: encoder substituted the source format\n", t->chn);
        return -1;
    }

    for (p = 0; p < AIR_CAP_PLANES; p++) {
        guint32 bpl = f.fmt.pix_mp.plane_fmt[p].bytesperline;
        guint32 want = g_cap_stride[p];

        g_printerr("[ml-air-video] tile %d: enc plane %d bytesperline %u (asked %u), "
                   "sizeimage %u (buffer has %zu)\n",
                   t->chn, p, bpl, want, f.fmt.pix_mp.plane_fmt[p].sizeimage,
                   air_cap_len(t, p));

        if (bpl != want) {
            g_printerr("[ml-air-video] tile %d: encoder rewrote the source stride; "
                       "refusing to encode a sheared picture\n", t->chn);
            return -1;
        }
        if (f.fmt.pix_mp.plane_fmt[p].sizeimage > air_cap_len(t, p)) {
            g_printerr("[ml-air-video] tile %d: encoder demands more than the capture "
                       "buffer holds on plane %d\n", t->chn, p);
            return -1;
        }
    }

    if (fps > 0) {
        memset(&sp, 0, sizeof sp);
        sp.type = otype;
        sp.parm.output.timeperframe.numerator = 1;
        sp.parm.output.timeperframe.denominator = (guint32)fps;
        if (ioctl(t->enc_fd, VIDIOC_S_PARM, &sp) != 0) {
            g_printerr("[ml-air-video] tile %d: S_PARM %d fps: %s\n",
                       t->chn, fps, strerror(errno));
            return -1;
        }
    }

    memset(&f, 0, sizeof f);
    f.type = ctype;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    f.fmt.pix_mp.width = AIR_COMP_W;
    f.fmt.pix_mp.height = (guint32)t->height;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(t->enc_fd, VIDIOC_S_FMT, &f) != 0) {
        g_printerr("[ml-air-video] tile %d: S_FMT CAPTURE HEVC: %s\n", t->chn, strerror(errno));
        return -1;
    }

    if (vbv < 10) {
        vbv = air_vbv_for_bitrate(bitrate);
    }

    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_VBV_SIZE, vbv, "vbv size");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                 V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                 atoi(env_or("ML_AIR_GOP", "0")), "gop size");
    /* Repeat VPS/SPS/PPS on every IDR. The stream carries one IDR at session start and parameter
     * sets are sent once, so a receiver that joins later has neither - a forced keyframe on its own
     * gives it a picture it cannot configure a decoder for. Seq-init parameter, so it has to be set
     * before streaming starts, not alongside the keyframe request. */
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "prepend sps/pps to idr");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "frame rc");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE,
                 atoi(env_or("ML_AIR_MBRC", "1")), "mb rc");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
                 atoi(env_or("ML_AIR_MINQP", "0")), "min qp");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
                 atoi(env_or("ML_AIR_MAXQP", "51")), "max qp");
    air_enc_ctrl(t, V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP,
                 atoi(env_or("ML_AIR_IQP", "30")), "i frame qp");
    memset(&rb, 0, sizeof rb);
    rb.type = otype;
    rb.memory = V4L2_MEMORY_DMABUF;
    rb.count = AIR_ENC_OUT_BUFS;
    if (ioctl(t->enc_fd, VIDIOC_REQBUFS, &rb) != 0) {
        g_printerr("[ml-air-video] tile %d: REQBUFS OUTPUT: %s\n", t->chn, strerror(errno));
        return -1;
    }
    t->enc_out_n = (int)rb.count;

    memset(&rb, 0, sizeof rb);
    rb.type = ctype;
    rb.memory = V4L2_MEMORY_MMAP;
    rb.count = AIR_ENC_CAP_BUFS;
    if (ioctl(t->enc_fd, VIDIOC_REQBUFS, &rb) != 0) {
        g_printerr("[ml-air-video] tile %d: REQBUFS CAPTURE: %s\n", t->chn, strerror(errno));
        return -1;
    }
    t->enc_cap_n = (int)rb.count;

    for (i = 0; i < t->enc_cap_n; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane pl;

        memset(&b, 0, sizeof b);
        memset(&pl, 0, sizeof pl);
        b.type = ctype;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned int)i;
        b.length = 1;
        b.m.planes = &pl;
        if (ioctl(t->enc_fd, VIDIOC_QUERYBUF, &b) != 0) {
            g_printerr("[ml-air-video] tile %d: QUERYBUF %d: %s\n", t->chn, i, strerror(errno));
            return -1;
        }

        t->enc_cap_len[i] = pl.length;
        t->enc_cap_map[i] = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, t->enc_fd,
                                 (off_t)pl.m.mem_offset);
        if (t->enc_cap_map[i] == MAP_FAILED) {
            t->enc_cap_map[i] = NULL;
            g_printerr("[ml-air-video] tile %d: mmap capture %d: %s\n",
                       t->chn, i, strerror(errno));
            return -1;
        }

        if (ioctl(t->enc_fd, VIDIOC_QBUF, &b) != 0) {
            g_printerr("[ml-air-video] tile %d: QBUF capture %d: %s\n",
                       t->chn, i, strerror(errno));
            return -1;
        }
    }

    if (ioctl(t->enc_fd, VIDIOC_STREAMON, &otype) != 0 ||
        ioctl(t->enc_fd, VIDIOC_STREAMON, &ctype) != 0) {
        g_printerr("[ml-air-video] tile %d: STREAMON: %s\n", t->chn, strerror(errno));
        return -1;
    }

    /* Both are what recovery needs to rebuild this instance, and the stamp is what keeps the
     * stall watchdog from reading an unstarted encoder as one that has been silent since boot. */
    if (dev != t->enc_node) {
        g_strlcpy(t->enc_node, dev, sizeof t->enc_node);
    }
    t->enc_fps = fps;
    t->enc_bitrate = bitrate;
    t->enc_vbv = vbv;
    t->enc_progress_us = g_get_monotonic_time();

    return 0;
}

/** Queue one capture buffer into this tile's encoder at the tile's row offset. Takes a
 * reference on the buffer, released when the OUTPUT buffer comes back. */
static int air_enc_queue(struct air_tile *t, struct air_cap_buf *cb)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[AIR_CAP_PLANES];
    int idx = -1;
    int p;
    int i;

    for (i = 0; i < t->enc_out_n; i++) {
        if (t->enc_out_cb[i] == NULL) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return -1;
    }

    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = (unsigned int)idx;
    b.length = AIR_CAP_PLANES;
    b.m.planes = pl;
    for (p = 0; p < AIR_CAP_PLANES; p++) {
        pl[p].m.fd = cb->fd[p];
        pl[p].length = (unsigned int)cb->len[p];
        pl[p].data_offset = (unsigned int)air_cap_off(t, p);
        pl[p].bytesused = pl[p].data_offset + (unsigned int)air_cap_len(t, p);
    }

    if (ioctl(t->enc_fd, VIDIOC_QBUF, &b) != 0) {
        t->lost++;
        return -1;
    }

    t->enc_out_cb[idx] = cb;
    t->pushed++;
    return 0;
}

/** Drain whatever this tile's encoder has ready: coded access units out, spent source buffers
 * back. Non-blocking; the caller polls. */
static void air_enc_drain(struct air_tile *t)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[AIR_CAP_PLANES];

    for (;;) {
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_DMABUF;
        b.length = AIR_CAP_PLANES;
        b.m.planes = pl;
        if (ioctl(t->enc_fd, VIDIOC_DQBUF, &b) != 0) {
            break;
        }
        t->enc_progress_us = g_get_monotonic_time();

        if (b.index < (unsigned int)t->enc_out_n && t->enc_out_cb[b.index] != NULL) {
            air_cap_release(t->enc_out_cb[b.index]);
            t->enc_out_cb[b.index] = NULL;
        }
    }

    for (;;) {
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = 1;
        b.m.planes = pl;
        if (ioctl(t->enc_fd, VIDIOC_DQBUF, &b) != 0) {
            break;
        }

        t->enc_progress_us = g_get_monotonic_time();

        if (b.index < (unsigned int)t->enc_cap_n && t->enc_cap_map[b.index] != NULL &&
            pl[0].bytesused > 0) {
            air_emit_au(t, t->enc_cap_map[b.index] + pl[0].data_offset,
                        pl[0].bytesused - pl[0].data_offset, t->enc_seq++,
                        (b.flags & V4L2_BUF_FLAG_KEYFRAME) ? 1 : 0);
        }

        if (ioctl(t->enc_fd, VIDIOC_QBUF, &b) != 0) {
            g_printerr("[ml-air-video] tile %d: requeue capture %u: %s\n",
                       t->chn, b.index, strerror(errno));
            break;
        }
    }
}

/** Capture frames this encoder has taken and not yet given back. */
static int air_enc_held(const struct air_tile *t)
{
    int held = 0;
    int i;

    for (i = 0; i < t->enc_out_n; i++) {
        if (t->enc_out_cb[i] != NULL) {
            held++;
        }
    }

    return held;
}

/** Recover a stalled encoder by rebuilding the instance, and keep the stream alive either way.
 *
 * Cycling only the OUTPUT queue would be cheaper and was tried first; it does not work. wave5's
 * stop_streaming calls switch_state(inst, VPU_INST_STATE_STOP) whenever both queues are
 * streaming, and start_streaming's re-initialisation is guarded by inst->state ==
 * VPU_INST_STATE_OPEN, so a re-STREAMON on a STOP instance skips initialize_sequence and
 * prepare_fb and never reaches PIC_RUN again. The driver has no in-place restart: state leaves
 * NONE once and only a fresh open returns there.
 *
 * So this closes and re-opens. That does build a new encoder instance, which is the operation
 * this part has historically watchdogged on, but the alternative is an encoder that is already
 * dead. Attempts are capped: past AIR_ENC_MAX_RESTARTS the tile is retired rather than thrashed,
 * and the other tile and the capture loop carry on without it.
 *
 * Either way the held capture frames are released. They are the reason this cannot be ignored:
 * until they come back g_cap_inflight never falls, and a stall on one tile would starve the
 * whole pipeline instead of just that tile. Frames in flight are lost; the stream continues. */
static void air_enc_restart(struct air_tile *t)
{
    if (t->enc_restarts >= AIR_ENC_MAX_RESTARTS) {
        g_printerr("[ml-air-video] tile %d: %u recoveries, retiring the tile\n",
                   t->chn, t->enc_restarts);
        air_enc_close(t);
        t->active = 0;
        return;
    }

    t->enc_restarts++;
    air_enc_close(t);

    if (air_enc_open(t, t->enc_node, t->enc_fps) != 0) {
        g_printerr("[ml-air-video] tile %d: recovery re-open failed, retiring the tile\n", t->chn);
        air_enc_close(t);
        t->active = 0;
        return;
    }

    t->enc_progress_us = g_get_monotonic_time();
}

/** STREAMOFF both queues and close. STREAMOFF returns every queued buffer, so the capture
 * references this tile still holds are dropped here rather than leaked; wave5 needs the
 * instance torn down cleanly or its next open finds the firmware busy. */
static void air_enc_close(struct air_tile *t)
{
    enum v4l2_buf_type otype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int i;

    if (t->enc_fd < 0) {
        return;
    }

    ioctl(t->enc_fd, VIDIOC_STREAMOFF, &otype);
    ioctl(t->enc_fd, VIDIOC_STREAMOFF, &ctype);

    for (i = 0; i < t->enc_out_n; i++) {
        if (t->enc_out_cb[i] != NULL) {
            air_cap_release(t->enc_out_cb[i]);
            t->enc_out_cb[i] = NULL;
        }
    }

    for (i = 0; i < t->enc_cap_n; i++) {
        if (t->enc_cap_map[i] != NULL) {
            munmap(t->enc_cap_map[i], t->enc_cap_len[i]);
            t->enc_cap_map[i] = NULL;
        }
    }

    close(t->enc_fd);
    t->enc_fd = -1;
}

/** Source callback: split one 1920x1080 frame into the two aligned tiles and push each into its
 * encoder. Both tiles carry the same PTS, so their encoded FrameIds match. Skips a frame whole
 * if either tile has no free pool buffer, keeping the pair aligned.
 *
 * This is the pattern path only. It copies, which is what caps it near 17 fps at 1080p; the
 * camera path above shares instead. */
static GstFlowReturn air_on_src(GstAppSink *sink, gpointer user)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *sbuf;
    GstVideoFrame frame;
    GstClockTime pts;
    int idx[AIR_NCHN];

    (void)user;
    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    sbuf = gst_sample_get_buffer(sample);
    if (sbuf == NULL || !gst_video_frame_map(&frame, &g_src_info, sbuf, GST_MAP_READ)) {
        g_src_lost++;
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    pts = GST_BUFFER_PTS(sbuf);

    if (!air_reserve_pair(idx)) {
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);

        return GST_FLOW_OK;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            air_fill_tile(&g_tile[i], idx[i], &frame);
        }
    }
    gst_video_frame_unmap(&frame);

    air_push_pool_frame(idx, pts);

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/** Encoder-output callback: frame one tile access unit and send it to the goggle. */
/** Dump, count and transmit one access unit. Shared by the GStreamer and direct V4L2 backends. */
static void air_emit_au(struct air_tile *t, const guint8 *data, size_t size,
                        guint32 frame_id, guint32 is_idr)
{
    guint32 ts_ms;
    size_t len;

    /* A live rate change is adopted here rather than in the setter so the timestamp base is
     * frozen against a frame_id this tile has actually emitted. TimeStap counts elapsed
     * milliseconds, so the span already sent keeps the rate it was sent at and only the span
     * from here on uses the new one; recomputing the whole span at the new rate would step the
     * timestamp by the length of the stream so far. */
    if (t->fps_pending > 0) {
        t->ts_base_ms += (guint32)((guint64)(frame_id - t->ts_base_id) * 1000u /
                                   (guint32)t->fps);
        t->ts_base_id = frame_id;
        t->fps = t->fps_pending;
        t->fps_pending = 0;
    }

    ts_ms = t->ts_base_ms + (guint32)((guint64)(frame_id - t->ts_base_id) * 1000u /
                                      (guint32)t->fps);

    if (t->dumpfd >= 0) {
        if (write(t->dumpfd, data, size) != (ssize_t)size) {
            /* best-effort capture */
        }
    }

    if (size > t->tx_maxlen) {
        t->tx_maxlen = (guint32)size;
    }

    if (!g_notx) {
        len = vph_build(t->txbuf, AIR_TX_MAX, (guint32)t->chn, is_idr, frame_id, ts_ms,
                        t->resolution, data, (guint32)size);
        if (len == 0) {
            t->tx_oversize++;
        } else if (sendto(t->sock, t->txbuf, len, MSG_DONTWAIT,
                          (struct sockaddr *)&t->dst, sizeof t->dst) != (ssize_t)len) {
            t->tx_error++;
            t->tx_errno = errno;
        } else {
            t->sent++;
        }
    } else {
        t->sent++;
    }

    t->done++;
    t->bytes += size;
}

static GstFlowReturn air_on_enc(GstAppSink *sink, gpointer user)
{
    struct air_tile *t = user;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *buf;
    GstMapInfo map;
    guint32 frame_id;

    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    buf = gst_sample_get_buffer(sample);
    if (buf == NULL || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
        t->lost++;
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    if (GST_BUFFER_PTS_IS_VALID(buf)) {
        frame_id = (guint32)gst_util_uint64_scale(GST_BUFFER_PTS(buf), t->fps, GST_SECOND);
    } else {
        frame_id = 0;
    }

    air_emit_au(t, map.data, map.size, frame_id,
                GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT) ? 0 : 1);

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

/** Once a second: cumulative counts when verbose, and per-second rates when benchmarking, which
 * is the benchmark's entire output. Rates come from deltas rather than an average since start,
 * so a ramp or a stall is visible instead of being smoothed away. */
static gboolean air_on_tick(gpointer user)
{
    static guint64 last_pushed[AIR_NCHN];
    static guint64 last_done[AIR_NCHN];
    static guint64 last_sent[AIR_NCHN];
    static guint64 last_bytes[AIR_NCHN];
    static guint64 last_dropped[AIR_NCHN];
    const char *bench = g_bench_stage;

    (void)user;

    if (bench != NULL) {
        for (int i = 0; i < AIR_NCHN; i++) {
            guint64 dp = g_tile[i].pushed - last_pushed[i];
            guint64 dd = g_tile[i].done - last_done[i];
            guint64 ds = g_tile[i].sent - last_sent[i];
            guint64 db = g_tile[i].bytes - last_bytes[i];
            guint64 dx = g_tile[i].dropped - last_dropped[i];

            if (!g_tile[i].active) {
                continue;
            }

            /* tx is reported only when transmitting: with no peer it reads zero while the
             * encoder is healthy, and printing it beside done invites reading it as a fault. */
            if (g_notx) {
                g_printerr("[ml-air-video] bench %s tile %d: pushed %" G_GUINT64_FORMAT "/s"
                           " done %" G_GUINT64_FORMAT "/s %.2f Mbit/s"
                           " dropped %" G_GUINT64_FORMAT "/s\n",
                           bench, i, dp, dd, (double)db * 8.0 / 1e6, dx);
            } else {
                g_printerr("[ml-air-video] bench %s tile %d: pushed %" G_GUINT64_FORMAT "/s"
                           " done %" G_GUINT64_FORMAT "/s tx %" G_GUINT64_FORMAT "/s"
                           " %.2f Mbit/s dropped %" G_GUINT64_FORMAT "/s"
                           " oversize %" G_GUINT64_FORMAT " txerr %" G_GUINT64_FORMAT
                           " (%s) maxau %u\n",
                           bench, i, dp, dd, ds, (double)db * 8.0 / 1e6, dx,
                           g_tile[i].tx_oversize, g_tile[i].tx_error,
                           g_tile[i].tx_errno ? g_strerror(g_tile[i].tx_errno) : "-",
                           g_tile[i].tx_maxlen);
            }

            last_pushed[i] = g_tile[i].pushed;
            last_done[i] = g_tile[i].done;
            last_sent[i] = g_tile[i].sent;
            last_bytes[i] = g_tile[i].bytes;
            last_dropped[i] = g_tile[i].dropped;
        }
    } else if (g_verbose) {
        g_printerr("[ml-air-video] tx chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT
                   " dropped=%" G_GUINT64_FORMAT "\n",
                   g_tile[0].sent, g_tile[1].sent, g_tile[0].dropped);

        /* Camera rates as per-second deltas. The whole point of the zero-copy path is that
         * `cap` and `enc` both sit at the sensor rate with `skip` at zero; a cumulative count
         * cannot show that, and neither can a count taken once. */
        if (g_cap_fd >= 0) {
            static guint64 last_cap;
            static guint64 last_skip;

            g_printerr("[ml-air-video] cam cap=%" G_GUINT64_FORMAT "/s skip=%" G_GUINT64_FORMAT
                       "/s inflight=%d enc=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "/s\n",
                       g_cap_frames - last_cap, g_cap_skipped - last_skip,
                       g_atomic_int_get(&g_cap_inflight),
                       g_tile[0].done - last_done[0], g_tile[1].done - last_done[1]);
            last_cap = g_cap_frames;
            last_skip = g_cap_skipped;
        }

        for (int i = 0; i < AIR_NCHN; i++) {
            last_done[i] = g_tile[i].done;
        }
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
    const char *bench = getenv("ML_AIR_BENCH");
    const char *ring_env = getenv("ML_AIR_RING");
    const char *pool_env = getenv("ML_AIR_POOL");
    int pool_want;
    int cap_want = atoi(env_or("ML_AIR_BUFS", G_STRINGIFY(AIR_CAP_BUFS_DEF)));
    const char *enc = env_or("ML_AIR_ENC",
                             "v4l2h265enc output-io-mode=dmabuf-import "
                             "extra-controls=\"controls,video_gop_size=65535\"");
    const char *dump = env_or("ML_AIR_DUMP", NULL);
    const char *ctrl = env_or("ML_AIR_CTRL", "/run/missinglynk/air-video.sock");
    char desc[512];
    GError *err = NULL;
    GstElement *src_pipe;
    GstElement *enc_pipe[AIR_NCHN];
    GstBus *bus;
    GstAppSinkCallbacks cbs;
    GstElement *el;
    struct sockaddr_in dstaddr;
    GThread *feeder = NULL;
    int sock;

    g_verbose = (getenv("ML_AIR_VERBOSE") != NULL);
    g_notx = (getenv("ML_AIR_NOTX") != NULL);
    g_bench_free = (getenv("ML_AIR_BENCH_FREE") != NULL);
    g_bench_fps = fps > 0 ? fps : 60;
    g_cap_fps = fps;
    g_cap_inflight_max = atoi(env_or("ML_AIR_INFLIGHT", G_STRINGIFY(AIR_CAP_INFLIGHT_DEF)));
    if (g_cap_inflight_max < 1) {
        g_cap_inflight_max = 1;
    }

    if (bench != NULL && *bench == '\0') {
        bench = NULL;
    }

    if (bench != NULL) {
        g_bench_secs = atoi(env_or("ML_AIR_BENCH_SECS", "10"));
        if (g_bench_secs < 1) {
            g_bench_secs = 1;
        }
        g_bench_stages = g_strsplit(bench, ",", 0);
    }

    /* The live path wants a pipelining depth; the benchmark wants its whole ring resident, and
     * is the only caller that asks for more than a handful. */
    if (bench != NULL) {
        pool_want = (ring_env != NULL && *ring_env != '\0') ? atoi(ring_env) : AIR_RING_DEF;
    } else {
        pool_want = (pool_env != NULL && *pool_env != '\0') ? atoi(pool_env) : AIR_POOL_DEF;
    }
    /* Staggered encoder bring-up is the default: concurrent instance creation while the other
     * encoder has frames in flight races the wave5 firmware (corrupt output or a VCPU watchdog,
     * HW-confirmed). ML_AIR_NO_STAGGER restores the concurrent bring-up for diagnostics. */
    g_stagger = (getenv("ML_AIR_NO_STAGGER") == NULL);
    /* Camera mode drives the encoders directly; ML_AIR_GST=1 falls back to GStreamer,
     * which cannot state the capture stride and therefore shears the picture. */
    g_enc_direct = (camera != NULL && getenv("ML_AIR_GST") == NULL);
    for (int i = 0; i < AIR_NCHN; i++) {
        g_tile[i].enc_fd = -1;
    }
    /* A control client that gives up before reading its reply (ml-air-ctl under `timeout`) leaves
     * the reply write to take EPIPE, whose default action would kill the whole video daemon. */
    signal(SIGPIPE, SIG_IGN);
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

    /* ML_AIR_COPY names the tile fed by copy instead of by sharing the capture buffer, so the two
     * encoder instances no longer read overlapping ranges of one allocation. That is the isolation
     * test for the watchdog the second instance hits under full zero-copy, and the fallback if the
     * firmware turns out not to take aliased source windows: it keeps half the copy saving. */
    {
        const char *copy = getenv("ML_AIR_COPY");

        if (copy != NULL && (copy[0] == '0' || copy[0] == '1')) {
            g_tile[copy[0] - '0'].copy = 1;
        }
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

        /* The camera path shares capture buffers, so it allocates no tile buffers at all and
         * the whole mmz pool stays with the two encoder instances. A tile named by ML_AIR_COPY is
         * the exception: it is fed by copy and needs its pool back. */
        if ((camera == NULL || t->copy) && air_pool_init(t, pool_want) < AIR_POOL_MIN) {
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

        /* Render the ring once. Everything after this point in benchmark mode is pointer
         * shuffling: no source element, no copy, no CPU pass over a pixel. */
        if (bench != NULL) {
            air_render_ring(t, g_bench_stages[0]);
            g_printerr("[ml-air-video] bench: tile %d ring of %d rendered as %s\n",
                       i, t->pool_n, g_bench_stages[0]);
        }
    }

    /* Per-tile encode pipelines (one v4l2h265enc instance each). */
    for (int i = 0; i < AIR_NCHN; i++) {
        GstBus *ebus = NULL;

        if (!g_tile[i].active || g_enc_direct) {
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
    src_pipe = NULL;
    if (bench == NULL && camera == NULL) {
        snprintf(desc, sizeof desc,
                 "videotestsrc is-live=true pattern=%s ! "
                 "video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1 ! "
                 "appsink name=src sync=false max-buffers=4 drop=true",
                 pattern, AIR_COMP_W, AIR_COMP_H, fps);

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
    }

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, air_on_signal, NULL);
    g_unix_signal_add(SIGTERM, air_on_signal, NULL);
    g_timeout_add_seconds(1, air_on_tick, NULL);
    air_ctrl_open(ctrl);

    if (bench != NULL) {
        g_printerr("[ml-air-video] bench %s: %dx%d two H.265 tiles, %s, %s, %d s per tier\n",
                   bench, AIR_COMP_W, AIR_COMP_H,
                   g_bench_free ? "unpaced" : "paced to ML_AIR_FPS",
                   g_notx ? "encode-only (no transmit)" : "transmitting", g_bench_secs);
    } else if (g_notx) {
        g_printerr("[ml-air-video] %dx%d @ %d fps from %s, two H.265 tiles, "
                   "encode-only (no transmit)\n",
                   AIR_COMP_W, AIR_COMP_H, fps, camera != NULL ? camera : "videotestsrc");
    } else {
        g_printerr("[ml-air-video] %dx%d @ %d fps from %s, two H.265 tiles -> %s:%d\n",
                   AIR_COMP_W, AIR_COMP_H, fps, camera != NULL ? camera : "videotestsrc",
                   dst, port);
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_element_set_state(enc_pipe[i], GST_STATE_PLAYING);
        }
    }

    if (bench != NULL) {
        feeder = g_thread_new("air-bench", air_bench_feed, NULL);
    } else if (camera != NULL) {
        /* Opened here rather than with the rest of the setup so the node streams for as short a
         * time as possible before anything dequeues: buffers completed while nothing is reading
         * are stale by the time the first DQBUF returns them. */
        if (air_cap_open(camera, cap_want) != 0) {
            close(sock);
            return 1;
        }

        /* After air_cap_open: the encoders are configured from the capture node's strides,
         * which is the entire point of this path, so they cannot be opened before it. */
        if (g_enc_direct) {
            char enc_node[32];

            if (air_enc_find_node(camera, enc_node, sizeof enc_node) != 0) {
                air_cap_close();
                close(sock);
                return 1;
            }

            g_print("[ml-air-video] encoder %s\n", enc_node);

            for (int i = 0; i < AIR_NCHN; i++) {
                if (!g_tile[i].active) {
                    continue;
                }
                if (air_enc_open(&g_tile[i], enc_node, fps) != 0) {
                    for (int j = 0; j < AIR_NCHN; j++) {
                        air_enc_close(&g_tile[j]);
                    }
                    air_cap_close();
                    close(sock);
                    return 1;
                }
            }
        }

        feeder = g_thread_new("air-cap", air_cap_feed, NULL);
    } else {
        gst_element_set_state(src_pipe, GST_STATE_PLAYING);
    }

    g_main_loop_run(g_loop);

    if (feeder != NULL) {
        g_bench_stop = 1;
        g_cap_stop = 1;
        g_thread_join(feeder);
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        air_enc_close(&g_tile[i]);
    }

    air_cap_close();

    if (src_pipe != NULL) {
        gst_element_set_state(src_pipe, GST_STATE_NULL);
    }
    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_app_src_end_of_stream(g_tile[i].src);
            gst_element_set_state(enc_pipe[i], GST_STATE_NULL);
        }
    }

    g_printerr("[ml-air-video] stopped (chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT
               ", oversize %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
               ", lost %" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT
               ", src lost %" G_GUINT64_FORMAT
               ", enc recoveries %u/%u)\n",
               g_tile[0].sent, g_tile[1].sent,
               g_tile[0].tx_oversize, g_tile[1].tx_oversize,
               g_tile[0].lost, g_tile[1].lost, g_src_lost,
               g_tile[0].enc_restarts, g_tile[1].enc_restarts);

    if (src_pipe != NULL) {
        gst_object_unref(src_pipe);
    }
    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_object_unref(enc_pipe[i]);
        }
    }
    g_main_loop_unref(g_loop);
    if (g_ctrl_fd >= 0) {
        close(g_ctrl_fd);
        if (g_ctrl_path[0] != '\0') {
            unlink(g_ctrl_path);
        }
    }
    close(sock);
    return 0;
}
