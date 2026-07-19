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

/* ar_scaler dmabuf UAPI (kernel/modules/ar_scaler.h) - inlined like the two ABIs above. A batch of
 * fd-addressed crop/resize ops on /dev/arscaler: the kernel resolves fd + offset to phys (buffers
 * must be physically contiguous), pins them for the op, and caches the mapping per open device fd -
 * so the record path keeps ONE fd open across frames. DMA-to-DMA use (composite in, encoder import
 * out) needs no CPU cache sync. Strides and offsets are byte counts, multiples of 16.
 */
struct ar_scaler_op {
    guint32 srcphy;                     /* must be 0: kernel fills from src_fd + src_off */
    guint32 srcw, srch, srcstride;
    guint32 crop_x, crop_y, cropw, croph;
    guint32 dstphy;                     /* must be 0: kernel fills from dst_fd + dst_off */
    guint32 dstw, dsth, dststride;
    guint32 channels;                   /* planes per op; the record path scales plane-per-op (1) */
    guint32 control, interp, ctrl3c;
};
struct ar_scaler_dmabuf_op { gint32 src_fd, dst_fd; guint32 src_off, dst_off; struct ar_scaler_op op; };
#define SCALER_DMABUF_BATCH_MAX 8
struct ar_scaler_dmabuf_batch { guint32 count; guint32 reserved; struct ar_scaler_dmabuf_op ops[SCALER_DMABUF_BATCH_MAX]; };
#define SCALER_IOC_BATCH_DMABUF _IOWR('Z', 4, struct ar_scaler_dmabuf_batch)

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

/* Allocation size: the wave5 encoder's sizeimage rounds the height to a multiple of 16
 * (1080 -> 1088) and its dmabuf import rejects any buffer smaller than that, while its
 * plane ADDRESSING uses stride * height = the plain COMP_* layout. Allocate the padded
 * size, keep the 1080 content layout; the tail is dead padding.
 */
#define COMP_HALIGN     (((COMP_H + 15) / 16) * 16)
#define COMP_ALLOC      (COMP_LSTRIDE * COMP_HALIGN * 3 / 2)

/* One DVR recording format (dvr.resolution): the encoder-facing I420 geometry, derived entirely
 * from w/h with packed strides (lstride = w, cstride = w/2). alloc is the wave5 encoder's
 * sizeimage (height rounded up to a multiple of 16). The format table is g_rec_fmts in
 * mlp-record.c: entry 0 is the native composite geometry (imported zero-copy, no scale stage),
 * every other entry is produced by the ar_scaler. w must be a multiple of 32 so both strides
 * meet the scaler ABI (multiples of 16).
 */
struct rec_fmt {
    int   w, h;                         /* frame size; h is the MLM_CMD_DVR_RES selector */
    int   lstride, cstride;
    gsize uoff, usize, voff, size, alloc;
};
#define REC_POOL        4               /* scaled-dst dma-buf pool: the encoder pins rec_qmax (2)
                                         * + one being pushed + one being scaled. Allocated at
                                         * PIPELINE STARTUP (comp_pool_init calls rec_hw_init
                                         * before the composite grab): steady-state CmaFree is
                                         * ~0.3 MiB, so a record-time allocation always fails -
                                         * the pool must claim its slice while the composite pool
                                         * can still adapt around it. Buffers are sized for the
                                         * largest scaled format. */
#define COMP_TILE_SIZE  (COMP_W * COMP_H * 3 / 2)   /* max packed I420 tile for the staging buffer */
#define COMP_POOL       24              /* cap; comp_pool_init allocates as many as the heap yields.
                                         * Sizing: the display side retires one flip late (prev +
                                         * front + pending + next = 4 held), and pairing must back a
                                         * slot per frame of inter-decoder skew (SKEW_MAX 6 + in-flight
                                         * 2), so fewer than ~12 usable buffers turns the slot table
                                         * into an eviction storm (composed ~1 fps, evict ~60/s).
                                         */

/* One precomputed DVR OSD burn-in copy: @p len bytes from the cell's packed pixel store at
 * @p src land at absolute composite-buffer offset @p dst (the plane base is folded in). Spans
 * cover only the opaque glyph pixels, so the burn never reads the composite (it is WC memory)
 * and never writes video pixels outside the glyph.
 */
struct osd_span {
    guint32 dst, src, len;
};

/* One cached BTFL OSD cell (MLM_CMD_OSD_CELL): its packed Y/U/V bytes + the spans that place
 * them. px == NULL = cell empty.
 */
struct osd_cell {
    guint8 *px;
    struct osd_span *spans;
    int nspans;
};

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
    volatile int sei_br_kbps[RF_NCHN];  /* latest per-tile encoder bitrate from the PREFIX_SEI (kbps) */
    volatile int sei_qp[RF_NCHN];       /* latest per-tile encoder QP from the PREFIX_SEI */
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
    int rec_qmax;                       /* appsrc queue bound; low in import mode - every queued
                                         * buffer pins a composite pool slot */
    gboolean rec_import;                /* encoder in dmabuf-import mode (no videoscale) */

    /* Downscaled DVR (plans/done/hw-downscaled-dvr-scaler.md): the HUD latches the recording format via
     * MLM_CMD_DVR_RES; rec_start resolves it to a g_rec_fmts row. A scaled (non-native) format runs
     * each composite through the ar_scaler on a dedicated worker thread - rec_push runs under
     * comp_lock on a decoder streaming thread, where the ~6 ms synchronous scale ioctl would stall
     * the compositor - through a 1-deep newest-only mailbox (scale 6 ms < frame 16.7 ms, so steady
     * state never drops). The scaled dst is a pool dma-buf the encoder imports as usual; the
     * composite buffer is pinned only for the scale, not the whole encode. Without a working
     * scaler a menu-selected scaled format records native instead.
     */
    int dvr_h, dvr_fps;                 /* latched format; 0 = never set (defaults native/60) */
    const struct rec_fmt *rec_fmt;      /* this recording's format (a g_rec_fmts row) */
    gboolean rec_hwscale;               /* this recording scales via /dev/arscaler */
    int rec_fps;                        /* this recording's frame rate (30 or 60) */
    int scaler_fd;                      /* /dev/arscaler, opened once, kept (the kernel caches
                                         * dmabuf mappings per open fd) */
    int rec_pool_fd[REC_POOL];          /* scaled-dst dma-bufs (kept for process lifetime) */
    int rec_pool_n;
    GAsyncQueue *rec_free;              /* free rec_pool indices (+1, 0 == queue-empty) */
    pthread_t rec_scale_thr;
    volatile int rec_scale_run;
    pthread_mutex_t rec_scale_lock;     /* guards the mailbox pair below */
    pthread_cond_t rec_scale_cond;
    GstBuffer *rec_scale_buf;           /* mailbox: composite awaiting scale (ref held), or NULL */
    GstClockTime rec_scale_pts;
    GstClockTime rec_next_pts;          /* 30 fps PTS gate (hwscale path drops pre-scale) */
    guint64 rec_scale_fail;             /* scaler ioctl failures (frame dropped) */

    /* SRT telemetry sidecar (vendor DVR parity): the HUD sends one pre-formatted subtitle line
     * per second over ctrl.sock (MLM_CMD_SRT_TEXT) while recording; each line becomes one SRT
     * cue stamped with the recording-relative video time. The file is created lazily on the
     * first line (setting off = no datagrams = no file) and cues are written continuous: a cue
     * is flushed when the next line arrives (its start = the cue's end), the last one at stop.
     */
    FILE *srt_fp;                       /* open sidecar, or NULL (not recording / no line yet) */
    char srt_path[300];                 /* rec_path with the extension swapped to .srt */
    unsigned srt_n;                     /* cues written (SRT indices are 1-based) */
    char srt_pend[256];                 /* pending cue text, flushed on the next line / at stop */
    guint64 srt_pend_ms;                /* pending cue start (recording-relative ms) */
    gboolean srt_pend_set;
    guint64 rec_last_ms;                /* rebased PTS of the last frame pushed to the encoder,
                                         * in ms: the cue timestamps' source, so the sidecar
                                         * tracks the MP4 timeline, not the wall clock */

    /* DVR OSD burn-in (dvr.record_osd): the HUD renders each changed BTFL OSD cell with its own
     * MSP font and sends the RGBA patch over ctrl.sock (MLM_CMD_OSD_CELL); osd_burn_cell converts
     * it ONCE to YUV + opaque-pixel spans, and while recording osd_burn_apply overwrites the
     * spans into every completed composite (slot_push, before the flush + rec_push) - so the
     * recording carries exactly the glyphs the panel shows, and on screen they hide under the
     * overlay plane's identical ones. Pure malloc state: no CMA/MMZ is spent, and the composite
     * is only ever written (opaque overwrite, binary alpha), never read back.
     */
    pthread_mutex_t osd_lock;           /* guards the cell cache (ctrl thread vs decoder threads) */
    struct osd_cell osd_cells[MLM_OSD_ROWS][MLM_OSD_COLS];
    int osd_ncells;                     /* occupied cells (0 = burn is a no-op) */

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
        GstClockTime pts;               /* frame PTS, carried for the latency trace */
    } next_it,                          /* mailbox: newest frame awaiting flip */
      front_it, pending_it,             /* display-thread-only: on-screen / flip-in-flight */
      prev_it;                          /* retired one flip late, see drm_flip_handler */
    gint64 pending_since;               /* monotonic us when pending_it's flip was submitted */
    gint64 flip_last_us;                /* monotonic us of the last flip event; send_state derives
                                         * MLM_STATE_F_VIDEO_LIVE from its freshness */

    /* Display phase pacing (mlp-pace.c, ML_PACE=1): proportional servo pinning the
     * submit-to-latch wait at ~3 ms via artosyn_vo's pclk_hz. pace_n/pace_min_us
     * accumulate under disp_lock in the flip handler, drained by the 1 Hz tick.
     */
    int pace_on, pace_dbg;
    int pace_fd;                        /* open sysfs pclk_hz */
    long pace_base_hz, pace_cmd_hz;     /* boot rate; last commanded rate */
    long pace_hold_hz;                  /* integral term: the servo's adapting base rate */
    int pace_n;                         /* flips this interval */
    gint64 pace_w[64];                  /* the interval's waits (percentile sensor); [PACE_NWAIT] */
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
    guint32 idle_fb, idle_dumb;         /* persistent black primary FB the CRTC parks on across
                                         * decode-graph swaps, so tearing a graph down never leaves
                                         * the DC fetching a freed FB (which powers the panel off) */
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
    volatile int show_idle;             /* HUD asked (via MLM_CMD_SHOW_IDLE) to park on the no-signal
                                         * splash: the live link dropped. Set on the ctrl thread, read
                                         * by the display thread; a returning video frame clears it. */
    int idle_shown;                     /* display-thread-only: the no-signal splash is currently up */

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

    /* Playback (mlp-playback.c): a selected file preempts the live RF stream. The RF decode
     * pipeline is torn down to NULL and rebuilt around playback (the proven session-restart
     * teardown), so only one wave5 decode graph is ever live and the display sink is
     * re-initialised for the single-stream scanout. */
    gboolean pb_active;                 /* a file is currently playing (preempts RF) */
    gboolean pb_paused;
    gboolean pb_ended;                  /* reached EOS: last frame held, awaiting replay or exit */
    gboolean pb_rendering;              /* first decoded frame has been submitted to the display */
    GstElement *pb_pipe;                /* filesrc -> demux -> parse -> wave5 dec -> appsink */
    guint pb_timer;                     /* position-telemetry timeout id (0 = none) */
    guint pb_bus_watch;                 /* bus watch id on pb_pipe */
    guint32 pb_pos_ms, pb_dur_ms;       /* last queried playback position + duration */
    gboolean rf_planes;                 /* RF display mode (planes_on) to restore after playback */
    guint rf_bus_watch;                 /* bus watch id on the RF pipe (re-added on each rebuild) */

    /* Latency trace (ML_LATSTATS=1, mlp-latstats.c): per-frame stage timestamps keyed by PTS,
     * flushed to a 1 Hz summary line. All fields display-path-hot, so collection is fully
     * disabled unless lat_on.
     */
    gboolean lat_on;
    gboolean lat_raw;                   /* ML_LATRAW=1: one line per flip (short captures only) */
    pthread_mutex_t lat_lock;           /* guards lat_ent[] and the flush accumulators */
    struct lat_ent {
        GstClockTime pts;
        gint64 t_rx;                    /* first datagram of the FrameId seen in rf_rx (us) */
        gint64 t_dec[RF_NCHN];          /* tile decode-out (appsink new-sample entry) */
        gint64 t_pair;                  /* both halves complete (slot_push) */
        gint64 t_issue;                 /* flip ioctl entered (issue time) */
        gint64 t_submit;                /* flip ioctl returned (submitted) */
        gboolean done;                  /* flip event consumed; ignore late duplicate marks */
    } lat_ent[256];
    struct lat_acc {                    /* completed samples since the last 1 Hz flush (us) */
        gint32 rxflip[256], rxdec0[256], rxdec1[256], pairw[256], subflip[256];
        int n;                          /* samples in rxflip/rxdec0/subflip */
        int n2;                         /* samples in rxdec1/pairw (only frames whose tile 1 and
                                         * pair marks were both seen before the flip; a frame
                                         * latched with just tile-0 marks at startup lands in n) */
        gint32 fdt[256];                /* flip-to-flip intervals, completeness-independent */
        int nfdt;
        guint32 judder, repeat;         /* fdt outside +-20% nominal / > 1.5x nominal */
    } lat_acc;
    gint64 lat_last_flip;               /* previous flip-event time (us), 0 = none yet */
    guint lat_timer;                    /* 1 Hz flush g_timeout id */
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

/* Kick the display thread's poll loop via its self-pipe. The pipe is O_NONBLOCK, so a full
 * buffer (EAGAIN) means a wake token is already pending and the reader will drain it; any
 * error is therefore safe to ignore. Named helper so the ignored result is intentional, not
 * a bare empty-braced guard.
 */
static inline void pipe_wake(int fd)
{
    const char w = 1;
    ssize_t n;

    do {
        n = write(fd, &w, 1);
    } while (n < 0 && errno == EINTR);
}

/* cross-file prototypes; file-local helpers stay static in their .c */
/* util */
void crc32_init(void);
guint32 crc32_buf(const guint8 *p, int n);
gboolean au_has_idr(const guint8 *es, int n);
gboolean sei_parse_brqp(const guint8 *es, int n, int *br_kbps, int *qp);
gboolean map_tile(GstSample *s, GstBuffer *buf, GstMapInfo *m, struct tileview *t);
void emit_framestats(struct ctx *c, GstClockTime pts);

/* compose */
int ml_heap_alloc(gsize len);
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
int drm_make_idle_fb(struct ctx *c);

/* osdburn */
void osd_burn_cell(struct ctx *c, const guint8 *frame, gssize len);
void osd_burn_clear(struct ctx *c);
void osd_burn_apply(struct ctx *c, guint8 *map);

/* record */
gboolean rec_hw_init(struct ctx *c);
int rec_start(struct ctx *c, const char *path);
void rec_stop(struct ctx *c);
void rec_push(struct ctx *c, GstBuffer *buf, GstClockTime pts);
void rec_srt_text(struct ctx *c, const char *line);
void send_state(struct ctx *c);
gboolean state_tick(gpointer u);
gboolean on_ctrl(gint fd, GIOCondition cond, gpointer u);

/* rf */
GstFlowReturn on_tile(GstAppSink *sink, gpointer u);
void clear_pending(struct ctx *c);
void *rf_rx(void *arg);
gboolean rf_ready_tick(gpointer u);

/* mlp-latstats.c: per-frame latency trace (ML_LATSTATS=1). All marks are no-ops when off. */
/* mlp-pace.c */
void pace_init(struct ctx *c);
void pace_flip(struct ctx *c, gint64 wait_us);

void lat_init(struct ctx *c);
void lat_mark_rx(struct ctx *c, GstClockTime pts);
void lat_mark_dec(struct ctx *c, int ch, GstClockTime pts, gint64 t_us);
void lat_mark_pair(struct ctx *c, GstClockTime pts);
void lat_mark_issue(struct ctx *c, GstClockTime pts);
void lat_mark_submit(struct ctx *c, GstClockTime pts);
void lat_mark_flip(struct ctx *c, GstClockTime pts);

/* rf decode-graph lifecycle (ml-pipeline.c) - built/torn down around playback swaps */
int rf_decode_start(struct ctx *c);
void rf_decode_stop(struct ctx *c);

/* single-stream file scanout callbacks (ml-pipeline.c), shared by CLI file mode + playback */
GstFlowReturn on_file_sample(GstAppSink *sink, gpointer u);
GstPadProbeReturn on_frame(GstPad *pad, GstPadProbeInfo *info, gpointer u);
GstPadProbeReturn asink_alloc_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u);

/* playback (mlp-playback.c) - driven by ctrl.sock commands */
void playback_play(struct ctx *c, const char *path);
void playback_pause(struct ctx *c);
void playback_resume(struct ctx *c);
void playback_seek(struct ctx *c, guint32 permille);
void playback_set_speed(struct ctx *c, gint32 speed);
void playback_end(struct ctx *c);
void playback_stop(struct ctx *c, gboolean resume_live);

#endif /* ML_PIPELINE_H */
