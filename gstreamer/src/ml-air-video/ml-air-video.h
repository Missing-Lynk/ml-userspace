/**
 * @file ml-air-video.h
 * @brief Shared types, limits and cross-file declarations for the air-unit video transmitter.
 *
 * The translation units behind it, and what each owns:
 *   ml-air-video.c  process setup, environment, GStreamer glue, main
 *   mav-bufs.c      dma-heap tile pool, buffer wrapping and recycling
 *   mav-capture.c   the ar-cvisp capture node and the feeder thread
 *   mav-encoder.c   the direct V4L2 encoder instances
 *   mav-ctrl.c      the live control socket and its whole-pipeline commands
 *   mav-tx.c        VPH framing and the :10001 datagrams
 *   mav-bench.c     synthetic patterns and the throughput benchmark (diagnostic only)
 *
 * Ownership rules that are not visible from a single file:
 *   - The encoder fds and the feeder's poll set belong to the capture feeder thread. Anything on
 *     the main loop that wants an encoder opened asks through air_enc_start_request().
 *   - g_tile[] is written during setup and then read concurrently; the volatile counters in it are
 *     the only fields that move afterwards.
 */
#ifndef ML_AIR_VIDEO_H
#define ML_AIR_VIDEO_H

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

/** How long a requester waits for the feeder thread to finish an on-demand bring-up. */
#define AIR_ENC_START_MS 5000

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

/** Capture node geometry and credit. */
#define AIR_CAP_PLANES     3
#define AIR_CAP_BUFS_MAX   16
#define AIR_CAP_BUFS_DEF   8
/* Frames the encoders may hold at once. Each one is a buffer withheld from the rotation, and
 * also a frame of queueing ahead of the encoder, so this is a latency bound as much as a credit
 * bound. The rest of the pool stays with the driver. */
#define AIR_CAP_INFLIGHT_DEF 3

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

/* ---------------------------------------------------------------------------------------------
 * Shared state. Defined in ml-air-video.c unless noted.
 * ------------------------------------------------------------------------------------------- */

extern struct air_tile g_tile[AIR_NCHN];
extern GMainLoop *g_loop;
extern GstAllocator *g_dmabuf_alloc;
extern GstVideoInfo g_src_info;        /* source 1920x1080 I420 layout */
extern GQuark g_recycle_quark;
extern volatile guint64 g_src_lost;    /* frames dropped upstream of the tile split */
extern int g_notx;

/* Camera path drives the encoders through V4L2 directly instead of GStreamer, which is the only
 * way to state the capture stride. ML_AIR_GST=1 restores the GStreamer path for comparison. */
extern int g_enc_direct;

/* ML_AIR_ON_DEMAND: hold the encoders closed until a receiver asks for a keyframe, instead of
 * encoding and transmitting from start-up whether or not anyone is listening. The camera still
 * streams from start-up: ar-cvisp brings the sensor, VIF and ISP up from STREAMON, and deferring
 * that defers the fragile part of the bring-up. So this holds the VPU, and with no encoder there
 * is nothing to transmit either. */
extern int g_on_demand;
extern volatile gint g_enc_up;         /* encoders open and streaming; the "is it running" flag */

extern int g_stagger;                  /* serialize the two encoder bring-ups */
extern int g_primed;                   /* set once the staggered bring-up completed */
extern int g_bench_free;               /* ML_AIR_BENCH_FREE: resubmit unpaced */
extern volatile int g_bench_stop;      /* feeder thread exit flag */
extern int g_bench_fps;                /* pacing rate and PTS base for the feeder */
extern int g_bench_secs;               /* ML_AIR_BENCH_SECS: seconds per tier */
extern char **g_bench_stages;          /* ML_AIR_BENCH split on commas */
extern const char *g_bench_stage;      /* tier currently running, for the rate line */

/* Capture node state, defined in mav-capture.c. */
extern int g_cap_fd;
extern int g_cap_inflight_max;
extern int g_cap_fps;                  /* ML_AIR_FPS: 0 means take every frame the node gives */
extern gint g_cap_inflight;
extern guint32 g_cap_stride[AIR_CAP_PLANES];
extern volatile int g_cap_stop;
extern volatile guint64 g_cap_frames;  /* dequeued from the node */
extern volatile guint64 g_cap_skipped; /* returned unused: rate limit or no credit */

/* ---------------------------------------------------------------------------------------------
 * Cross-file entry points.
 * ------------------------------------------------------------------------------------------- */

/* ml-air-video.c */
const char *air_env_or(const char *name, const char *dflt);

/* mav-bufs.c */
void air_dmabuf_sync(int fd, int start);
int air_pool_init(struct air_tile *tile, int want);
void air_fill_tile(struct air_tile *tile, int idx, GstVideoFrame *src);
GstBuffer *air_wrap_tile(struct air_tile *tile, int idx, GstClockTime pts);
void air_push_frame(GstBuffer **buf);
void air_push_pool_frame(const int *idx, GstClockTime pts);
int air_reserve_pair(int *idx);

/* mav-capture.c */
int air_cap_open(const char *dev, int want);
void air_cap_close(void);
void air_cap_release(gpointer data);
gsize air_cap_off(const struct air_tile *tile, int plane);
gsize air_cap_len(const struct air_tile *tile, int plane);
gpointer air_cap_feed(gpointer user);

/* Ask the feeder thread to open the encoders and wait for the answer. Main-loop side of the
 * ML_AIR_ON_DEMAND bring-up; returns 0 once they are streaming. */
int air_enc_start_request(void);

/* mav-encoder.c */
int air_enc_find_node(const char *camera, char *out, size_t len);
int air_enc_open(struct air_tile *tile, const char *dev, int fps);
int air_enc_queue(struct air_tile *tile, struct air_cap_buf *capbuf);
void air_enc_drain(struct air_tile *tile);
void air_enc_restart(struct air_tile *tile);
void air_enc_close(struct air_tile *tile);
int air_enc_held(const struct air_tile *tile);
int air_enc_set_int(struct air_tile *tile, guint32 id, gint32 val, const char *name);
int air_enc_set_bitrate(struct air_tile *tile, int bitrate);
int air_enc_set_vbv(struct air_tile *tile, int vbv);
int air_enc_set_fps(struct air_tile *tile, int fps);
int air_vbv_for_bitrate(int bitrate);

/* mav-ctrl.c */
int air_ctrl_open(const char *path);
void air_ctrl_close(void);

/* mav-tx.c */
void air_emit_au(struct air_tile *tile, const guint8 *data, size_t size,
                 guint32 frame_id, guint32 is_idr);
GstFlowReturn air_on_src(GstAppSink *sink, gpointer user);
GstFlowReturn air_on_enc(GstAppSink *sink, gpointer user);

/* mav-bench.c */
void air_render_ring(struct air_tile *tile, const char *pattern);
gpointer air_bench_feed(gpointer user);

#endif /* ML_AIR_VIDEO_H */
