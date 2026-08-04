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
/* Pixels the `scroll` tier advances per frame. 8 divides 1920, so the pattern wraps exactly and
 * the loop has no discontinuity. */
#define AIR_SCROLL_PX 8

/** Number of tiles / encode channels. */
#define AIR_NCHN     2

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
static int g_stagger;              /* ML_AIR_STAGGER: serialize the two encoder bring-ups */
static int g_primed;               /* set once the staggered bring-up completed */
static int g_bench_free;           /* ML_AIR_BENCH_FREE: resubmit unpaced */
static volatile int g_bench_stop;  /* feeder thread exit flag */
static int g_bench_fps;            /* pacing rate and PTS base for the feeder */
static int g_bench_secs;           /* ML_AIR_BENCH_SECS: seconds per tier */
static char **g_bench_stages;      /* ML_AIR_BENCH split on commas */
static const char *g_bench_stage;  /* tier currently running, for the rate line */

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

/** Push one already-filled buffer per active tile into its encoder, both carrying @p pts so the
 * encoded FrameIds match. @p idx is read only for active tiles.
 *
 * Holds the staggered, ordered bring-up: on the first frame, feed tile 1 (552 lines) and wait
 * for its first encoded output, then tile 0 (560 lines). The order is what matters: the
 * firmware hangs bringing the 552-height instance up after the 560-height instance exists,
 * while 560-after-552 is clean.
 */
static void air_push_frame(const int *idx, GstClockTime pts)
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

            if (gst_app_src_push_buffer(t->src, air_wrap_tile(t, idx[order[o]], pts))
                != GST_FLOW_OK) {
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
            if (gst_app_src_push_buffer(g_tile[i].src, air_wrap_tile(&g_tile[i], idx[i], pts))
                != GST_FLOW_OK) {
                g_tile[i].lost++;
            } else {
                g_tile[i].pushed++;
            }
        }
    }
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

                air_push_frame(idx, gst_util_uint64_scale(n, GST_SECOND, (guint64)g_bench_fps));
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

/** Source callback: split one 1920x1080 frame into the two aligned tiles and push each into its
 * encoder. Both tiles carry the same PTS, so their encoded FrameIds match. Skips a frame whole
 * if either tile has no free pool buffer, keeping the pair aligned. */
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

    air_push_frame(idx, pts);

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
        t->lost++;
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

    if (map.size > t->tx_maxlen) {
        t->tx_maxlen = (guint32)map.size;
    }

    if (!g_notx) {
        len = vph_build(t->txbuf, AIR_TX_MAX, (guint32)t->chn, is_idr, frame_id, ts_ms,
                        t->resolution, map.data, (guint32)map.size);
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
    t->bytes += map.size;

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
    GThread *feeder = NULL;
    int sock;

    g_verbose = (getenv("ML_AIR_VERBOSE") != NULL);
    g_notx = (getenv("ML_AIR_NOTX") != NULL);
    g_bench_free = (getenv("ML_AIR_BENCH_FREE") != NULL);
    g_bench_fps = fps > 0 ? fps : 60;

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

        if (air_pool_init(t, pool_want) < AIR_POOL_MIN) {
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
    src_pipe = NULL;
    if (bench == NULL) {
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
    }

    g_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, air_on_signal, NULL);
    g_unix_signal_add(SIGTERM, air_on_signal, NULL);
    g_timeout_add_seconds(1, air_on_tick, NULL);

    if (bench != NULL) {
        g_printerr("[ml-air-video] bench %s: %dx%d two H.265 tiles, %s, %s, %d s per tier\n",
                   bench, AIR_COMP_W, AIR_COMP_H,
                   g_bench_free ? "unpaced" : "paced to ML_AIR_FPS",
                   g_notx ? "encode-only (no transmit)" : "transmitting", g_bench_secs);
    } else if (g_notx) {
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

    if (bench != NULL) {
        feeder = g_thread_new("air-bench", air_bench_feed, NULL);
    } else {
        gst_element_set_state(src_pipe, GST_STATE_PLAYING);
    }

    g_main_loop_run(g_loop);

    if (feeder != NULL) {
        g_bench_stop = 1;
        g_thread_join(feeder);
    }

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
               ", src lost %" G_GUINT64_FORMAT ")\n",
               g_tile[0].sent, g_tile[1].sent,
               g_tile[0].tx_oversize, g_tile[1].tx_oversize,
               g_tile[0].lost, g_tile[1].lost, g_src_lost);

    if (src_pipe != NULL) {
        gst_object_unref(src_pipe);
    }
    for (int i = 0; i < AIR_NCHN; i++) {
        if (enc_pipe[i] != NULL) {
            gst_object_unref(enc_pipe[i]);
        }
    }
    g_main_loop_unref(g_loop);
    close(sock);
    return 0;
}
