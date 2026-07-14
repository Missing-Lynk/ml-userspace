/*
 * ml-pipeline.h - shared decls for the ml-pipeline translation units
 * (split out of the former monolith: util / compose / display / record / rf /
 * main). Every .c here includes ONLY this header. It carries the full include
 * set, the locally-defined dma-heap + ml_dmablit UAPI, the RF/composite
 * constants, struct tileview, the shared struct ctx, and the cross-file
 * function prototypes. File-local helpers stay `static` in their own .c.
 */
#ifndef ML_PIPELINE_H
#define ML_PIPELINE_H

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "../../../ml-shared/mlm.h"

/* dma-heap alloc + CPU-access sync UAPI (defined locally). */
struct dma_heap_allocation_data { guint64 len; guint32 fd; guint32 fd_flags; guint64 heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
struct dma_buf_sync { guint64 flags; };
#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

/* ml_dmablit UAPI (kernel/modules/ml_dmablit.h) - inlined like the dma-heap ABI above so the
 * gst build needs no kernel headers. Submit a batch of contiguous dmabuf->dmabuf copies to the
 * AXI DMA engine (dw-axi-dmac); blocks until every copy has completed on the hardware.
 */
#define ML_DMABLIT_MAX_COPIES 8
struct ml_dmablit_copy { gint32 src_fd; guint32 src_off; guint32 dst_off; guint32 len; };
struct ml_dmablit_req { gint32 dst_fd; guint32 n; struct ml_dmablit_copy copy[ML_DMABLIT_MAX_COPIES]; };
#define ML_DMABLIT_SUBMIT _IOW('D', 0x01, struct ml_dmablit_req)
#define ML_DMABLIT_FLUSH  _IOW('D', 0x02, gint32)   /* explicit CPU-cache clean of a dmabuf */

#define RF_VIDEO_PORT   10001
#define VPH_LEN         36              /* video_packet_header size */
#define VPH_MAGIC       0x12345678u
#define VPH_TAIL_MAGIC  0x87654321u
#define RF_FPS          60              /* SPS says 60/1; TimeStap deltas confirm ~16.7 ms */
#define RF_NCHN         2               /* two vertical tiles */

#define COMP_W          1920
#define COMP_H          1080
#define TILE1_Y         528             /* tile 1 top row in the composite (even; absorbs the 32-row overlap) */

/* The composite is a CMA dma-heap buffer scanned out zero-copy by kmssink. It uses PACKED
 * I420 strides matching the wave5 decoder's own output: luma 1920 (= COMP_W), chroma 960
 * (W/2); the DC scans out a 1920-strided composite directly, no luma padding to 2048. This
 * makes each tile plane a single contiguous src->dst block (matching strides), so the blit
 * is one ml_dmablit copy per plane. The blit + the GstVideoMeta both use these.
 */
#define COMP_LSTRIDE    1920
#define COMP_CSTRIDE    960
#define COMP_YSIZE      (COMP_LSTRIDE * COMP_H)
#define COMP_UOFF       COMP_YSIZE
#define COMP_USIZE      (COMP_CSTRIDE * (COMP_H / 2))
#define COMP_VOFF       (COMP_UOFF + COMP_USIZE)
#define COMP_SIZE       (COMP_VOFF + COMP_USIZE)
#define COMP_TILE_SIZE  (COMP_W * COMP_H * 3 / 2)   /* max packed I420 tile for the staging buffer */
#define COMP_POOL       24              /* cap; comp_pool_init allocates as many as the heap yields.
                                         * Sizing: the display side retires one flip late (prev +
                                         * front + pending + next = 4 held), and pairing must back a
                                         * slot per frame of inter-decoder skew (SKEW_MAX 6 + in-flight
                                         * 2), so fewer than ~12 usable buffers turns the slot table
                                         * into an eviction storm (composed ~1 fps, evict ~60/s).
                                         */

struct ctx {
    int tsock;
    struct sockaddr_un taddr;
    guint32 frame_id;
    guint64 dropped;
    gint64 t_first, t_last;
    GMainLoop *loop;

    /* rf mode */
    gboolean rf;
    GstElement *pipe;                   /* decode pipeline (torn down on session restart) */
    GstAppSrc *src[RF_NCHN];            /* input: one HEVC AU feed per tile */
    GstAppSink *asink[RF_NCHN];         /* decoded raw frames out of each decoder */
    gboolean started[RF_NCHN];          /* have we seen this tile's session-start IDR yet */
    gint64 last_fid;                    /* last FrameId pushed (for session-restart detect) */
    GstClockTime pts_epoch;             /* accumulated PTS base across sessions: a FrameId wrap
                                         * bumps the epoch so downstream PTS stays MONOTONIC and
                                         * the decoders just see one endless stream - no teardown,
                                         * no rebuild transient, no re-staggered skew
                                         */

    pthread_mutex_t comp_lock;             /* guards the composite-assembly state below */
    /* Composite slot table: the two decoders deliver with a constant few-frame skew (tile 1
     * lags tile 0), so pairing needs to hold several frames' composites in flight, keyed by
     * PTS - the vendor equivalent is FusionThread pulling from per-channel queues. A slot is
     * pushed the moment both halves are blitted (every pushed frame is therefore fully
     * covered - no clearing needed); when the table is full the oldest incomplete slot is
     * evicted (counted in pair_evict).
     */
#define NSLOT 32
    struct comp_slot {
        gboolean used;                  /* FALSE = slot free */
        GstBuffer *buf;                 /* composite mode: claimed pool buffer */
        int cbi;                        /* composite mode: its pool index (blit target + flush fd) */
        GstSample *smp[RF_NCHN];        /* plane mode: the tile samples held until both land */
        guint32 sfb[RF_NCHN];           /* plane mode: their cached DRM FBs */
        GstClockTime pts;
        gboolean have[RF_NCHN];
    } slot[NSLOT];
    GstClockTime cur_pts;               /* PTS of the last pushed composite (telemetry) */
    guint64 pair_evict;

    /* DVR record branch (plans/gst-dvr.md, plans/wave5-encoder-fit.md): the completed composite
     * dma-buf is imported ZERO-COPY into the wave5 H.264 encoder -> MP4. dmabuf-import allocates
     * no CMA for the encoder input and does no CPU copy (the ~60 MiB / 42 fps blockers both trace
     * to the copy path). Bounded + drop-on-overflow so a slow encoder never stalls the display.
     * Composite mode only: plane scanout has no composite buffer to encode.
     */
    GstElement *rec_bin;                /* appsrc -> v4l2h264enc(dmabuf-import) -> mp4mux -> filesink */
    GstAppSrc *rec_src;
    volatile int rec_on;
    char rec_path[256];
    GstClockTime rec_pts0;              /* first recorded PTS, rebased to 0 for a clean MP4 */
    gboolean rec_epoch_set;
    guint64 rec_pushed, rec_dropped;

    /* Composite dma-heap pool: a fixed set of contiguous CMA buffers, each scanned out
     * zero-copy by kmssink (COMP_* layout). Claimed per composite, returned to comp_free
     * when the pushed GstBuffer is finalized (kmssink done). See gst-dma-compositor.md.
     */
    GstAllocator *comp_alloc;
    struct compbuf { int fd; guint8 *map; } comp_pool[COMP_POOL];
    int comp_n;                         /* buffers actually allocated (CMA-limited, adaptive) */
    GAsyncQueue *comp_free;             /* free compbuf indices (thread-safe free-list) */
    guint64 comp_starve;                /* composites dropped because the pool was empty */
    int dmablit_fd;                     /* /dev/ml-dmablit (AXI DMA blit), or -1 = CPU blit */
    gboolean dmablit_warned;            /* one-shot: logged a DMA submit failure already */
    guint64 blit_dma, blit_cpu;         /* per-path tile-blit counts (diagnostics) */
    /* Staging dmabuf for the non-dmabuf tile: CPU-pack it here (its OWN buffer), flush, then DMA
     * it into the composite - so the composite only ever receives DMA writes (no CPU/DMA cache
     * mix on the shared buffer, which no flush could make coherent). Serialized by c->comp_lock.
     */
    int stage_fd;
    guint8 *stage_map;

    /* Custom DRM/KMS display sink (replaces appsrc ! kmssink). Drives the artosyn_vo driver
     * directly and retires a composite buffer to the pool ONLY when its successor's page-flip
     * event fires - the vendor's retire-after-scanout guarantee. kmssink under skip-vsync freed
     * the buffer at the next commit (before the flip latched), letting the pool overwrite a
     * buffer the DC was still scanning: the reuse-during-scanout race. Newest-wins mailbox, so a
     * completed frame is never silently dropped (the leaky appsrc could).
     */
    int drm_fd;                         /* shared DRM master fd from ml-drmfd */
    guint32 fb_id[COMP_POOL];           /* one drmModeAddFB2 per pool buffer, cached at init */
    guint32 fb_handle[COMP_POOL];       /* PRIME-imported GEM handle behind each fb: must be closed
                                         * on shutdown or it pins the dmabuf on the BROKER's drm fd
                                         * for the broker's lifetime (leaks the pool across runs)
                                         */
    guint32 crtc_id, conn_id;           /* discovered output (DSI-1 on crtc-0) */
    drmModeModeInfo mode;
    pthread_t disp_thread;
    volatile int disp_run;
    pthread_mutex_t disp_lock;          /* guards the newest-wins mailbox */

    /* One displayable frame. Composite mode: a pool buffer (cbi/buf). Plane-scanout mode
     * (planes_on): the PAIR of decoder samples scanned out directly on video0/video1 -
     * the samples pin the decoder dmabufs until retirement.
     */
    struct ditem {
        int cbi;                        /* composite mode: pool index, else -1 */
        GstBuffer *buf;                 /* composite mode: pool buffer ref */
        GstSample *smp[RF_NCHN];        /* plane mode: the two tile samples */
        guint32 fb[RF_NCHN];            /* plane mode: their cached DRM FBs */
    } next_it,                          /* mailbox: newest frame awaiting flip */
      front_it, pending_it,             /* display-thread-only: on-screen / flip-in-flight */
      prev_it;                          /* retired one flip late, see drm_flip_handler */
    GstClockTime next_pts;
    int wake_r, wake_w;                 /* self-pipe to kick the display thread */

    /* Plane-scanout mode (plans/gst-plane-scanout.md): tiles scan out directly on the DC's
     * video0/video1 overlay planes; both banks latch on ONE vsync (shared 0x1518 bit3 shadow
     * bracket), so the pair is tear-free by hardware. No composite pool, no blits. The HUD
     * shares bank 1 with video1 and cannot run in this mode. ML_COMPOSE=1 forces the old
     * composite path.
     */
    gboolean planes_on;
    gboolean single;                    /* single-stream file playback: scan out one decoder frame's
                                         * own FB on the primary CRTC (no pool, no tiling, no kmssink).
                                         * Reuses map_tile + tile_fb_get + the drm_disp mailbox.
                                         */
    guint32 vid_plane[2];               /* video0/video1 DRM plane ids */
    guint32 pp_fb[2], pp_crtcid[2];     /* per-plane property ids for the atomic commit */
    guint32 pp_srcw[2], pp_srch[2], pp_cx[2], pp_cy[2], pp_cw[2], pp_ch[2];
    guint32 pp_srcx[2], pp_srcy[2];
    guint32 prim_fb, prim_dumb;         /* static black primary FB (CRTC keystone) + its dumb handle */
    int tile_w[2], tile_h[2];           /* per-tile geometry, latched from the first sample */
    struct tfb { int fd; guint32 fb, handle; } tfb[2][32];  /* per-decoder dmabuf-fd -> FB cache */
    int ntfb[2];
    volatile int retire_arm;            /* SIGUSR1 -> arm 6 more retire dumps at any moment */
    int retire_dumps, retire_seq;       /* ML_DUMP_RETIRE=N: dump the first N retired composites -
                                         * uncached memory, so this is byte-exact what the DC scanned
                                         * while the frame was on screen (safe replacement for poking
                                         * DC registers via /dev/mem, which hangs the device)
                                         */
    int modeset_done;

    /* Stagger: hold a FEW tile-1 AUs so the two wave5 instances do not start allocating at
     * the identical instant. Deliberately small: the hold becomes a PERMANENT decode lag
     * between the channels (the VPU serves both at arrival pace and never lets tile 1 catch
     * up while tile 0 streams - a large hold costs ~18-40 frames of skew), and the pairing
     * slot table must span that skew. 4 AUs ~= 70-130 ms of allocation stagger.
     */
    GstBuffer *t1_hold[4];
    int t1_nhold;
    gboolean t1_released;

    /* Lockstep flow control: tile-0 AUs held here while decoder 0 runs ahead (see rf_rx) */
    GstBuffer *t0_hold[256];
    int t0_nhold;

    /* Session restart = full decode rebuild (see rf_do_rebuild). While TRUE the rx thread
     * swallows datagrams without pushing.
     */
    volatile gboolean restart_req;

    pthread_t rx_thread;
    volatile int rx_run;
    int lsock;                          /* link.sock (READY heartbeat to ml-linkd) */
    struct sockaddr_un laddr;
    int ctrl_sock;                      /* ctrl.sock (HUD -> pipeline commands); pipeline binds */
    guint64 rx_pkts, rx_bad_hdr, rx_bad_crc, rx_pushed, rx_dropped, composed;
    guint64 ns_map[2], ns_blit[2], n_prof[2];   /* per-tile map/blit latency accumulators (us) */
    int skew_max, inflight_max;         /* input-gate bounds, derived from the ACTUAL pool yield in
                                         * comp_pool_init: gate overshoot (skew + in-flight) must stay
                                         * under the backable slot window (comp_n minus the 4 display-
                                         * held buffers), or pairs evict before their partner lands.
                                         * Hardcoded values silently broke every time the pool moved.
                                         */
    guint64 n_fd[2];                            /* samples that arrived as a single dmabuf (t.fd>=0) */
    guint64 samples[RF_NCHN], map_fail;   /* diagnostics: appsink hits per channel + blit map fails */
    GstClockTime out_pts[RF_NCHN];        /* last decoder-output PTS per channel (diagnostics) */
    guint64 sent[RF_NCHN];                /* AUs pushed into each decoder (session accounting) */
    gboolean drop_sync[RF_NCHN];          /* input queue overran: drop AUs until the next IRAP */
    guint64 discard_before[RF_NCHN];      /* outputs numbered <= this are from a PREVIOUS session:
                                             their PTS values would collide with the new session's
                                             and pair stale halves with fresh ones - drop them
                                             */
    guint64 stale_drop;
};


/* A decoded tile's real layout, taken from the buffer's GstVideoMeta when present (the wave5
 * decoder's meta and caps can disagree, so trust the meta) or from the caps as a fallback.
 */
struct tileview {
    const guint8 *y, *u, *v;
    int ys, us, vs;
    int w, h;
    gboolean nv12;             /* TRUE = 2-plane NV12 (u = interleaved UV); FALSE = 3-plane I420 */
    int fd;                    /* source dmabuf fd for the DMA blit, or -1 if not dmabuf-backed */
    gsize yoff, uoff, voff;    /* plane byte offsets within fd (parallel the y/u/v CPU pointers) */
};

/* cross-file prototypes; file-local helpers stay static in their .c */
/* util */
void crc32_init(void);
guint32 crc32_buf(const guint8 *p, int n);
gboolean au_has_idr(const guint8 *es, int n);
gboolean map_tile(GstSample *s, GstBuffer *buf, GstMapInfo *m, struct tileview *t);
void emit_framestats(struct ctx *c, GstClockTime pts);

/* compose */
void ml_dmabuf_sync(int fd, int start);
gboolean comp_pool_init(struct ctx *c);
GstBuffer *comp_get(struct ctx *c, int *idx_out);
void blit_tile(guint8 *out, const struct tileview *t, int dst_row);
gboolean blit_tile_dma(struct ctx *c, int dst_fd, const struct tileview *t, int dst_row);
gboolean blit_tile_staged(struct ctx *c, int dst_fd, const struct tileview *t, int dst_row);

/* display */
void drm_disp_submit(struct ctx *c, const struct ditem *it, GstClockTime pts);
guint32 tile_fb_get(struct ctx *c, int ch, const struct tileview *t);
int drm_disp_init(struct ctx *c);
void drm_disp_shutdown(struct ctx *c);

/* record */
int rec_start(struct ctx *c, const char *path);
void rec_stop(struct ctx *c);
void rec_push(struct ctx *c, GstBuffer *buf, GstClockTime pts);
gboolean state_tick(gpointer u);
gboolean on_ctrl(gint fd, GIOCondition cond, gpointer u);

/* rf */
GstFlowReturn on_tile(GstAppSink *sink, gpointer u);
void clear_pending(struct ctx *c);
void *rf_rx(void *arg);
gboolean rf_ready_tick(gpointer u);

#endif /* ML_PIPELINE_H */
