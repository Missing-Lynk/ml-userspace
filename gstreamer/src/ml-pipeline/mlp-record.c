/* ml-pipeline record: DVR branch (zero-copy composite -> H.264 MP4) + HUD ctrl seam. */
#include "ml-pipeline.h"

/* Record-bin bus watch: errors on this bus previously vanished (nothing read it), so a failed
 * encoder recorded a 0-byte file with no trace. Log the error and tear the recording down from
 * the main loop (rec_stop from inside the watch would re-enter the bus).
 */
static gboolean rec_stop_idle(gpointer u)
{
    struct ctx *c = u;

    rec_stop(c);
    send_state(c);

    return G_SOURCE_REMOVE;
}

static gboolean rec_bus_cb(GstBus *bus, GstMessage *msg, gpointer u)
{
    struct ctx *c = u;

    (void)bus;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gchar *dbg = NULL;

        gst_message_parse_error(msg, &e, &dbg);
        fprintf(stderr, "ml-pipeline: DVR record ERROR: %s (%s)\n",
                e ? e->message : "?", dbg ? dbg : "-");
        if (e) {
            g_error_free(e);
        }

        g_free(dbg);
        g_idle_add(rec_stop_idle, c);
    }

    return TRUE;
}

/* Write one complete SRT cue: index, "HH:MM:SS,mmm --> HH:MM:SS,mmm", text, blank line. */
static void srt_write_cue(struct ctx *c, guint64 start_ms, guint64 end_ms, const char *text)
{
    unsigned s_h = (unsigned)(start_ms / 3600000);
    unsigned s_m = (unsigned)(start_ms / 60000 % 60);
    unsigned s_s = (unsigned)(start_ms / 1000 % 60);
    unsigned e_h = (unsigned)(end_ms / 3600000);
    unsigned e_m = (unsigned)(end_ms / 60000 % 60);
    unsigned e_s = (unsigned)(end_ms / 1000 % 60);

    fprintf(c->srt_fp, "%u\n%02u:%02u:%02u,%03u --> %02u:%02u:%02u,%03u\n%s\n\n",
            ++c->srt_n,
            s_h, s_m, s_s, (unsigned)(start_ms % 1000),
            e_h, e_m, e_s, (unsigned)(end_ms % 1000), text);
}

/* Flush the pending cue with @p end_ms as its end. Cues are continuous: each line's arrival
 * time is the previous cue's end and the new cue's start, so the sidecar has no gaps.
 */
static void srt_flush_pending(struct ctx *c, guint64 end_ms)
{
    if (!c->srt_pend_set || !c->srt_fp) {
        c->srt_pend_set = FALSE;
        return;
    }

    if (end_ms <= c->srt_pend_ms) {
        end_ms = c->srt_pend_ms + 1000;   /* degenerate cue (no video advanced): give it 1 s */
    }

    srt_write_cue(c, c->srt_pend_ms, end_ms, c->srt_pend);
    c->srt_pend_set = FALSE;
}

/* MLM_CMD_SRT_TEXT while recording: buffer @p line as the pending cue, flushing the previous
 * one with this line's timestamp as its end. Timestamps come from the encoder-side rebased
 * PTS (rec_last_ms), so the cues track the MP4 timeline. The file is created lazily here:
 * with the save_srt setting off the HUD sends nothing and no sidecar ever exists.
 */
void rec_srt_text(struct ctx *c, const char *line)
{
    if (!c->rec_on || !line || !*line) {
        return;
    }

    if (!c->srt_fp) {
        c->srt_fp = fopen(c->srt_path, "w");
        if (!c->srt_fp) {
            fprintf(stderr, "ml-pipeline: DVR subtitle open %s: %s\n",
                    c->srt_path, strerror(errno));
            return;
        }
        printf("ml-pipeline: DVR subtitles -> %s\n", c->srt_path);
    }

    guint64 now_ms = c->rec_epoch_set ? c->rec_last_ms : 0;
    srt_flush_pending(c, now_ms);
    snprintf(c->srt_pend, sizeof c->srt_pend, "%s", line);
    c->srt_pend_ms = now_ms;
    c->srt_pend_set = TRUE;
}

/* Finalize the sidecar at recording stop: flush the last cue and close the file. */
static void srt_stop(struct ctx *c)
{
    if (c->srt_fp) {
        srt_flush_pending(c, c->rec_epoch_set ? c->rec_last_ms : 0);
        fclose(c->srt_fp);
        c->srt_fp = NULL;
        printf("ml-pipeline: DVR subtitles stopped -> %s (%u cues)\n", c->srt_path, c->srt_n);
    }

    c->srt_pend_set = FALSE;
}

/* DVR recording formats, keyed by MLM_CMD_DVR_RES height. Entry 0 is the native default: the
 * composite itself, imported with no scale stage. Every other entry records through the
 * ar_scaler; adding a resolution is adding a row (rec_hw_init sizes the dst pool for the
 * largest scaled entry). Geometry constraints are in the struct rec_fmt comment (ml-pipeline.h).
 */
#define REC_HALIGN(h)   ((((h) + 15) / 16) * 16)
#define REC_FMT(w, h)   { (w), (h), (w), (w) / 2, \
                          (gsize)(w) * (h), (gsize)((w) / 2) * ((h) / 2), \
                          (gsize)(w) * (h) + (gsize)((w) / 2) * ((h) / 2), \
                          (gsize)(w) * (h) * 3 / 2, \
                          (gsize)(w) * REC_HALIGN(h) * 3 / 2 }
static const struct rec_fmt g_rec_fmts[] = {
    REC_FMT(1920, 1080),
    REC_FMT(1280, 720),
};
#define REC_NFMT        ((int) (sizeof(g_rec_fmts) / sizeof(g_rec_fmts[0])))
#define REC_NATIVE      (&g_rec_fmts[0])

/* The format row for @p height, or NULL if the table has no such format. */
static const struct rec_fmt *rec_fmt_by_height(int height)
{
    for (int i = 0; i < REC_NFMT; i++) {
        if (g_rec_fmts[i].h == height) {
            return &g_rec_fmts[i];
        }
    }

    return NULL;
}

/* HW downscale (scaled recording): one-time setup of the scaler fd + the scaled-dst dma-buf pool.
 * Called from comp_pool_init at PIPELINE STARTUP, not at the first recording: the CMA heap runs
 * steady-state ~0.3 MiB free once the composite pool / HUD overlay / DRM framebuffers are up, so
 * a record-time allocation is guaranteed to fail (HW-observed: pool = 1 of 4, 0-byte fallback
 * recording). At startup the rec pool claims its ~5.4 MiB first and the composite pool's adaptive
 * grab sizes itself around it. Both persist for the process lifetime: the kernel caches dmabuf
 * mappings per open scaler fd, and re-allocating per recording churns the heap into fragmentation.
 * Returns FALSE (720p unavailable) when the ar_scaler module is not loaded (no CMA is spent then)
 * or the heap yields fewer than 2 buffers.
 */
gboolean rec_hw_init(struct ctx *c)
{
    if (c->scaler_fd < 0) {
        c->scaler_fd = open("/dev/arscaler", O_RDWR | O_CLOEXEC);
        if (c->scaler_fd < 0) {
            return FALSE;
        }
    }

    if (!c->rec_free) {
        /* One buffer size serves every scaled row: the largest scaled format's sizeimage. */
        gsize bufsize = 0;
        for (int i = 1; i < REC_NFMT; i++) {
            if (g_rec_fmts[i].alloc > bufsize) {
                bufsize = g_rec_fmts[i].alloc;
            }
        }

        pthread_mutex_init(&c->rec_scale_lock, NULL);
        pthread_cond_init(&c->rec_scale_cond, NULL);
        c->rec_free = g_async_queue_new();
        for (int i = 0; i < REC_POOL; i++) {
            int fd = ml_heap_alloc(bufsize);

            if (fd < 0) {
                break;                  /* heap exhausted - use what we have */
            }

            c->rec_pool_fd[c->rec_pool_n] = fd;
            g_async_queue_push(c->rec_free, GINT_TO_POINTER(c->rec_pool_n + 1));
            c->rec_pool_n++;
        }

        fprintf(stderr, "ml-pipeline: DVR scale pool = %d x %d KiB\n",
                c->rec_pool_n, (int) (bufsize / 1024));
    }

    return c->rec_pool_n >= 2;          /* one for the encoder + one to scale into, minimum */
}

/* A scaled dst buffer's finalize hook: return its pool slot once the encoder is done with it. */
struct rec_ret { struct ctx *c; int idx; };
static void rec_on_finalize(gpointer user, GstMiniObject *obj)
{
    struct rec_ret *h = user;

    (void)obj;
    g_async_queue_push(h->c->rec_free, GINT_TO_POINTER(h->idx + 1));
    g_free(h);
}

/* Scale one composite into a pool dst (3-plane ar_scaler batch, COMP_* geometry in, the
 * recording's rec_fmt geometry out), wrap the dst for the encoder's dmabuf import, and push it.
 * Runs on the scale worker thread. Consumes the composite ref either way; the composite is
 * pinned only for the ~6 ms scale, not the whole encode. No cache sync anywhere: the composite
 * was flushed at slot_push and the dst is DMA-to-DMA (scaler write -> encoder read), never
 * CPU-touched.
 */
static void rec_scale_one(struct ctx *c, GstBuffer *buf, GstClockTime pts)
{
    const struct rec_fmt *f = c->rec_fmt;
    const guint32 soffs[3] = { 0, COMP_UOFF, COMP_VOFF };
    const guint32 doffs[3] = { 0, (guint32) f->uoff, (guint32) f->voff };
    const gsize dsizes[3] = { (gsize) f->lstride * f->h, f->usize, f->usize };
    gsize voffs[GST_VIDEO_MAX_PLANES] = { 0, f->uoff, f->voff };
    gint strd[GST_VIDEO_MAX_PLANES] = { f->lstride, f->cstride, f->cstride };
    struct ar_scaler_dmabuf_batch batch;
    struct rec_ret *h;
    GstBuffer *rb;
    int src_fd = gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(buf, 0));
    gpointer v = g_async_queue_try_pop(c->rec_free);

    if (!v) {                           /* every dst pinned by the encoder: drop this frame */
        c->rec_dropped++;
        gst_buffer_unref(buf);
        return;
    }

    int idx = GPOINTER_TO_INT(v) - 1;
    int dst_fd = c->rec_pool_fd[idx];

    memset(&batch, 0, sizeof batch);
    batch.count = 3;
    for (int p = 0; p < 3; p++) {
        struct ar_scaler_dmabuf_op *op = &batch.ops[p];
        guint32 sw = p ? COMP_W / 2 : COMP_W;
        guint32 sh = p ? COMP_H / 2 : COMP_H;

        op->src_fd = src_fd;
        op->dst_fd = dst_fd;
        op->src_off = soffs[p];
        op->dst_off = doffs[p];
        op->op.srcw = sw;
        op->op.srch = sh;
        op->op.srcstride = p ? COMP_CSTRIDE : COMP_LSTRIDE;
        op->op.cropw = sw;
        op->op.croph = sh;
        op->op.dstw = (guint32) (p ? f->w / 2 : f->w);
        op->op.dsth = (guint32) (p ? f->h / 2 : f->h);
        op->op.dststride = (guint32) (p ? f->cstride : f->lstride);
        op->op.channels = 1;
    }

    int rc = ioctl(c->scaler_fd, SCALER_IOC_BATCH_DMABUF, &batch);

    gst_buffer_unref(buf);              /* composite slot returns to the pool now */
    if (rc) {
        if (!c->rec_scale_fail++) {
            fprintf(stderr, "ml-pipeline: DVR scaler batch: %s (dropping frames)\n",
                    strerror(errno));
        }

        g_async_queue_push(c->rec_free, GINT_TO_POINTER(idx + 1));
        return;
    }

    rb = gst_buffer_new();
    for (int p = 0; p < 3; p++) {
        GstMemory *mem = gst_dmabuf_allocator_alloc(c->comp_alloc, dup(dst_fd), f->alloc);

        gst_memory_resize(mem, doffs[p], dsizes[p]);
        gst_buffer_append_memory(rb, mem);
    }

    gst_buffer_add_video_meta_full(rb, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_I420, (guint) f->w, (guint) f->h, 3, voffs, strd);
    h = g_malloc(sizeof *h);
    h->c = c;
    h->idx = idx;
    gst_mini_object_weak_ref(GST_MINI_OBJECT(rb), rec_on_finalize, h);

    GST_BUFFER_PTS(rb) = (pts != GST_CLOCK_TIME_NONE && pts >= c->rec_pts0) ? pts - c->rec_pts0 : 0;
    c->rec_last_ms = GST_BUFFER_PTS(rb) / GST_MSECOND;
    GST_BUFFER_DTS(rb) = GST_CLOCK_TIME_NONE;
    if (gst_app_src_push_buffer(c->rec_src, rb) != GST_FLOW_OK) {   /* consumes the ref */
        c->rec_dropped++;
    } else {
        c->rec_pushed++;
    }
}

/* Scale worker: drains the 1-deep mailbox rec_push fills. Exists because rec_push runs under
 * comp_lock on a decoder streaming thread, where a ~6 ms synchronous scale would stall the
 * compositor; here the scale overlaps the next frame's compose (6 ms < the 16.7 ms frame period,
 * so the mailbox never overflows in steady state).
 */
static void *rec_scale_worker(void *arg)
{
    struct ctx *c = arg;

    for (;;) {
        GstBuffer *buf;
        GstClockTime pts;

        pthread_mutex_lock(&c->rec_scale_lock);
        while (c->rec_scale_run && !c->rec_scale_buf) {
            pthread_cond_wait(&c->rec_scale_cond, &c->rec_scale_lock);
        }

        if (!c->rec_scale_run) {
            pthread_mutex_unlock(&c->rec_scale_lock);
            break;
        }

        buf = c->rec_scale_buf;
        pts = c->rec_scale_pts;
        c->rec_scale_buf = NULL;
        pthread_mutex_unlock(&c->rec_scale_lock);

        rec_scale_one(c, buf, pts);
    }

    return NULL;
}

/* Build and start the record bin. The encoder imports the composite dma-buf (output-io-mode=
 * dmabuf-import), so its input pool allocates NO CMA and there is no per-frame copy. H.264 to
 * match the vendor DVR and the scripts/dvr-frame.sh tooling.
 */
int rec_start(struct ctx *c, const char *path)
{
    GError *err = NULL;
    gchar *desc;
    GstElement *src;
    GstCaps *caps;
    const char *res = getenv("ML_DVR_RES");   /* env overrides the HUD-latched format (bench) */
    const char *fps = getenv("ML_DVR_FPS");
    char rate[96] = "", scale[96] = "";

    if (c->rec_bin) {                   /* already recording */
        return 0;
    }

    /* Recording format: the env levers win (bench), else the HUD's MLM_CMD_DVR_RES latch
     * (dvr.resolution), else native. A height with no g_rec_fmts row snaps to native.
     */
    const struct rec_fmt *fmt = rec_fmt_by_height(res ? atoi(res) : c->dvr_h);
    if (fmt == NULL) {
        fmt = REC_NATIVE;
    }

    c->rec_fps = fps ? atoi(fps) : (c->dvr_fps ? c->dvr_fps : RF_FPS);
    if (c->rec_fps != 30) {
        c->rec_fps = RF_FPS;
    }

    /* A scaled format prefers the ar_scaler (rec_scale_one: HW batch scale on a worker thread,
     * encoder dmabuf-import preserved). ML_DVR_NO_HWSCALE=1 forces the CPU videoscale path
     * (A/B bench).
     */
    c->rec_hwscale = fmt != REC_NATIVE && getenv("ML_DVR_NO_HWSCALE") == NULL && rec_hw_init(c);

    /* A menu-requested scaled format without a working scaler records NATIVE instead of taking
     * the CPU videoscale path: that path silently wedges the encoder on HW (bin starts, PIC_RUN
     * entered, zero bytes ever written, no bus error - a 0-byte file with REC lit the whole
     * flight). A native recording beats a broken scaled one. The videoscale path stays reachable
     * only through the explicit env levers (ML_DVR_RES / ML_DVR_NO_HWSCALE) for bench work.
     */
    if (fmt != REC_NATIVE && !c->rec_hwscale && res == NULL && getenv("ML_DVR_NO_HWSCALE") == NULL) {
        fprintf(stderr, "ml-pipeline: DVR %dp HW scale unavailable (scaler/pool); recording native %dp\n",
                fmt->h, COMP_H);
        fmt = REC_NATIVE;
    }

    c->rec_fmt = fmt;

    /* CPU-path levers (env-only). Framerate first (drop before scaling = less work). videorate
     * on dmabuf passes kept frames through zero-copy; videoscale copies to system memory, so CPU
     * scaled recording is CPU-bound. The HW-scale path needs neither element: rec_push halves
     * the rate pre-scale and the scaler emits the target size directly.
     */
    if (c->rec_fps == 30 && !c->rec_hwscale) {
        snprintf(rate, sizeof rate, "! videorate ! video/x-raw,framerate=30/1 ");
    }

    if (fmt != REC_NATIVE && !c->rec_hwscale) {
        snprintf(scale, sizeof scale, "! videoscale ! video/x-raw,width=%d,height=%d ", fmt->w, fmt->h);
    }

    /* Zero-copy dmabuf-import is the default: the encoder imports the composite buffers
     * directly (no MMAP input pool from the wave5 device pool, no 3 MiB/frame copy). Needs
     * COMP_ALLOC buffer sizing, the 3-plane wrap in rec_push, wave5 plane data_offset
     * support (kernel patch 0013), and the gst qbuf bytesused-offset fix
     * (gstreamer/patches/0001). ML_DVR_NO_IMPORT=1 falls back to copy mode (the encoder's
     * own MMAP pool + per-frame memcpy); the CPU videoscale path always copies (scale
     * output is system memory). The HW-scale path imports too - the scaled dst dma-bufs,
     * wrapped in rec_scale_one - but rec_import stays FALSE (it gates rec_push's
     * composite wrap).
     *
     * Import mode: a queued buffer pins a composite pool slot (display holds 4 of the ~10),
     * so the appsrc queue stays shallow and a lagging encoder drops frames instead of
     * starving the display. Copy mode pins nothing; keep the deeper queue. HW-scale mode
     * pins REC pool slots: leave 2 outside the queue (one being scaled, one being pushed).
     */
    c->rec_import = !c->rec_hwscale && scale[0] == '\0' && getenv("ML_DVR_NO_IMPORT") == NULL;
    if (c->rec_hwscale) {
        c->rec_qmax = c->rec_pool_n - 2 < 3 ? c->rec_pool_n - 2 : 3;
        if (c->rec_qmax < 1) {
            c->rec_qmax = 1;
        }
    } else {
        c->rec_qmax = c->rec_import ? 3 : 6;
    }

    int cap_w = c->rec_hwscale ? fmt->w : COMP_W;
    int cap_h = c->rec_hwscale ? fmt->h : COMP_H;
    int cap_fps = c->rec_hwscale ? c->rec_fps : RF_FPS;

    /* Feed the encoder the composite dmabufs themselves (output-io-mode=dmabuf-import): the
     * encoder QBUFs each buffer's fd instead of allocating an MMAP input pool and copying. That
     * saves a ~3 MiB memcpy per frame AND the ~19 MiB input pool, which comes from the wave5
     * device's dedicated coherent pool - the allocation that silently failed (0-byte file, wedged
     * VPU command queue) once the composite pool shared that same pool. The buffers already carry
     * dmabuf memory + I420 VideoMeta in the exact single-fd planar layout wave5 derives.
     * The videoscale (720p) path copies to system memory, so import cannot be forced there; it
     * keeps the encoder's own pool.
     */
    desc = g_strdup_printf(
        "appsrc name=recsrc is-live=true format=time do-timestamp=false "
        "max-buffers=%d leaky-type=downstream block=false "
        "! video/x-raw,format=I420,width=%d,height=%d,framerate=%d/1 "
        "%s%s"
        "! v4l2h264enc %s "

        /* Fragmented MP4 (moof/mdat every 1 s): the file on disk is playable up to the last
         * complete fragment even if the process is SIGKILLed (OOM) - which no signal handler can
         * catch. A clean stop (rec_stop EOS) still finalizes normally. Costs at most ~1 s of tail.
         */
        "! h264parse ! mp4mux fragment-duration=1000 ! filesink location=%s",
        c->rec_qmax, cap_w, cap_h, cap_fps, rate, scale,
        c->rec_import || c->rec_hwscale ? "output-io-mode=dmabuf-import" : "", path);

    c->rec_bin = gst_parse_launch(desc, &err);
    g_free(desc);

    if (!c->rec_bin) {
        fprintf(stderr, "ml-pipeline: DVR record bin: %s\n", err ? err->message : "?");
        if (err) {
            g_error_free(err);
        }

        return 1;
    }

    {
        GstBus *b = gst_element_get_bus(c->rec_bin);
        gst_bus_add_watch(b, rec_bus_cb, c);
        gst_object_unref(b);
    }

    src = gst_bin_get_by_name(GST_BIN(c->rec_bin), "recsrc");
    c->rec_src = GST_APP_SRC(src);      /* keep this ref; released in rec_stop */
    caps = gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, "I420",
                               "width", G_TYPE_INT, cap_w,
                               "height", G_TYPE_INT, cap_h,
                               "framerate", GST_TYPE_FRACTION, cap_fps, 1, NULL);
    gst_app_src_set_caps(c->rec_src, caps);
    gst_caps_unref(caps);
    snprintf(c->rec_path, sizeof c->rec_path, "%s", path);

    /* SRT sidecar path: the recording path with its extension swapped to .srt (the vendor's
     * VideoNNN.mp4 -> VideoNNN.srt convention). Any stale sidecar at that path is removed so a
     * re-recorded pinned path (ML_DVR) never keeps an old file's cues; the new file itself is
     * only created if subtitle lines actually arrive (rec_srt_text).
     */
    {
        const char *dot = strrchr(path, '.');
        int stem = dot ? (int)(dot - path) : (int)strlen(path);
        snprintf(c->srt_path, sizeof c->srt_path, "%.*s.srt", stem, path);
        unlink(c->srt_path);
    }
    c->srt_fp = NULL;
    c->srt_n = 0;
    c->srt_pend_set = FALSE;
    c->rec_last_ms = 0;

    c->rec_epoch_set = FALSE;
    c->rec_pushed = c->rec_dropped = 0;

    if (gst_element_set_state(c->rec_bin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ml-pipeline: DVR record bin failed to start (encoder CMA? see plans/wave5-encoder-fit.md)\n");
        rec_stop(c);

        return 1;
    }

    if (c->rec_hwscale) {
        c->rec_scale_buf = NULL;
        c->rec_next_pts = 0;
        c->rec_scale_run = 1;
        if (pthread_create(&c->rec_scale_thr, NULL, rec_scale_worker, c)) {
            fprintf(stderr, "ml-pipeline: DVR scale worker: %s\n", strerror(errno));
            c->rec_scale_run = 0;
            rec_stop(c);

            return 1;
        }
    }

    c->rec_on = 1;
    printf("ml-pipeline: DVR recording %dx%d@%d (%s) -> %s\n",
           fmt->w, fmt->h, c->rec_fps,
           c->rec_hwscale ? "HW scale" : c->rec_import ? "import" : "copy", path);

    return 0;
}

/* Finalize the MP4 (EOS -> flush encoder -> write moov) and tear the bin down. */
void rec_stop(struct ctx *c)
{
    if (!c->rec_bin) {
        return;
    }

    c->rec_on = 0;

    /* Stop the scale worker BEFORE touching rec_src (the worker pushes into it). A frame mid-
     * scale finishes (join waits the ~6 ms); an undelivered mailbox frame is dropped.
     */
    if (c->rec_scale_run) {
        pthread_mutex_lock(&c->rec_scale_lock);
        c->rec_scale_run = 0;
        pthread_cond_signal(&c->rec_scale_cond);
        pthread_mutex_unlock(&c->rec_scale_lock);
        pthread_join(c->rec_scale_thr, NULL);
        if (c->rec_scale_buf) {
            gst_buffer_unref(c->rec_scale_buf);
            c->rec_scale_buf = NULL;
        }
    }

    srt_stop(c);
    {
        /* The watch comes off first: a bus refuses timed_pop while a watch is installed,
         * and the error-start path (rec_src NULL) must not leak it either.
         */
        GstBus *b = gst_element_get_bus(c->rec_bin);
        gst_bus_remove_watch(b);
        gst_object_unref(b);
    }

    if (c->rec_src) {
        gst_app_src_end_of_stream(c->rec_src);
        {
            GstBus *b = gst_element_get_bus(c->rec_bin);
            GstMessage *m = gst_bus_timed_pop_filtered(b, 3 * GST_SECOND,
                                                       GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
            if (m) {
                gst_message_unref(m);
            }
            gst_object_unref(b);
        }
    }

    gst_element_set_state(c->rec_bin, GST_STATE_NULL);
    if (c->rec_src) {
        gst_object_unref(c->rec_src);
        c->rec_src = NULL;
    }

    gst_object_unref(c->rec_bin);
    c->rec_bin = NULL;
    osd_burn_clear(c);   /* the burn cells were this recording's OSD state; the HUD re-sends */
    printf("ml-pipeline: DVR stopped -> %s (pushed=%llu dropped=%llu)\n",
           c->rec_path, (unsigned long long)c->rec_pushed, (unsigned long long)c->rec_dropped);
}

/* Feed one completed composite to the encoder, zero-copy. Shares the pool GstBuffer (a ref holds
 * the pool slot until the encoder releases it); the encoder negotiates single-plane I420 and
 * imports the buffer's one dmabuf directly - possible because the pool allocates COMP_ALLOC (the
 * encoder's 16-row-aligned sizeimage) while addressing planes at the plain COMP_* offsets.
 * A slow encoder is bounded: if the queue is backed up we DROP this frame rather than pin pool
 * buffers and starve the display.
 */
void rec_push(struct ctx *c, GstBuffer *buf, GstClockTime pts)
{
    GstBuffer *rb;

    if (!c->rec_on || !c->rec_src) {
        return;
    }

    if (gst_app_src_get_current_level_buffers(c->rec_src) >= (guint)c->rec_qmax) {
        c->rec_dropped++;
        return;
    }

    if (!c->rec_epoch_set) {
        c->rec_pts0 = pts;
        c->rec_epoch_set = TRUE;
    }

    if (c->rec_hwscale) {
        /* 30 fps: gate on PTS BEFORE the scale (halves the scaler work; no videorate needed).
         * The 25 ms threshold (3/4 of the 33.3 ms period) accepts every other 16.7 ms frame
         * and self-heals across upstream drops.
         */
        if (c->rec_fps == 30 && pts != GST_CLOCK_TIME_NONE) {
            if (pts < c->rec_next_pts) {
                return;
            }

            c->rec_next_pts = pts + (GST_SECOND * 3) / (4 * 30);
        }

        /* Hand the composite to the scale worker, newest-wins: this thread holds comp_lock and
         * must not eat the ~6 ms scale. A still-occupied mailbox (worker behind) drops the
         * older frame.
         */
        pthread_mutex_lock(&c->rec_scale_lock);
        if (c->rec_scale_buf) {
            gst_buffer_unref(c->rec_scale_buf);
            c->rec_dropped++;
        }

        c->rec_scale_buf = gst_buffer_ref(buf);
        c->rec_scale_pts = pts;
        pthread_cond_signal(&c->rec_scale_cond);
        pthread_mutex_unlock(&c->rec_scale_lock);

        return;
    }

    if (c->rec_import) {
        /* The driver negotiates 3 V4L2 planes (gst prefers the non-contiguous variant), and
         * gst's dmabuf import demands one memory per plane. All three share the pool buffer's
         * fd: each memory's offset becomes the plane's data_offset (wave5 applies it, kernel
         * patch 0013), and its maxsize (COMP_ALLOC) covers every plane's 16-row-aligned
         * minimum length. The pool buffer rides as qdata so the slot returns only when the
         * encoder is done with the frame.
         */
        static const gsize offs[3] = { 0, COMP_UOFF, COMP_VOFF };
        static const gsize sizes[3] = { COMP_YSIZE, COMP_USIZE, COMP_USIZE };
        gsize voffs[GST_VIDEO_MAX_PLANES] = { 0, COMP_UOFF, COMP_VOFF };
        gint strd[GST_VIDEO_MAX_PLANES] = { COMP_LSTRIDE, COMP_CSTRIDE, COMP_CSTRIDE };
        int fd = gst_dmabuf_memory_get_fd(gst_buffer_peek_memory(buf, 0));

        rb = gst_buffer_new();
        for (int p = 0; p < 3; p++) {
            GstMemory *mem = gst_dmabuf_allocator_alloc(c->comp_alloc, dup(fd), COMP_ALLOC);
            gst_memory_resize(mem, offs[p], sizes[p]);
            gst_buffer_append_memory(rb, mem);
        }

        gst_buffer_add_video_meta_full(rb, GST_VIDEO_FRAME_FLAG_NONE,
                                       GST_VIDEO_FORMAT_I420, COMP_W, COMP_H, 3, voffs, strd);
        gst_mini_object_set_qdata(GST_MINI_OBJECT(rb),
                                  g_quark_from_static_string("ml-comp-pin"),
                                  gst_buffer_ref(buf), (GDestroyNotify)gst_buffer_unref);
    } else {
        rb = gst_buffer_ref(buf);
    }

    GST_BUFFER_PTS(rb) = (pts != GST_CLOCK_TIME_NONE && pts >= c->rec_pts0) ? pts - c->rec_pts0 : 0;
    c->rec_last_ms = GST_BUFFER_PTS(rb) / GST_MSECOND;
    GST_BUFFER_DTS(rb) = GST_CLOCK_TIME_NONE;
    if (gst_app_src_push_buffer(c->rec_src, rb) != GST_FLOW_OK) {   /* consumes the ref */
        c->rec_dropped++;
    } else {
        c->rec_pushed++;
    }
}

/* Broadcast the current mode to the HUD on telemetry.sock (MLM_T_STATE). The pipeline is the source
 * of truth; the HUD reflects what we report. Playback takes precedence over recording in the mode
 * field and carries position/duration for the scrubber. Sent on every change and re-asserted by
 * state_tick, so a HUD that starts late or drops a datagram reconverges.
 */
void send_state(struct ctx *c)
{
    uint32_t mode = c->pb_active ? MLM_STATE_PLAYBACK
                  : c->rec_on    ? MLM_STATE_RECORDING
                                 : MLM_STATE_IDLE;
    struct {
        struct mlm_hdr h;
        struct mlm_state s;
    } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_STATE, .flags = 0 },
        .s = { .state = mode,
               .flags = (c->pb_active && c->pb_paused ? MLM_STATE_F_PAUSED : 0)
                      | (c->pb_active && c->pb_ended ? MLM_STATE_F_ENDED : 0)
                      | (c->pb_active && c->pb_rendering ? MLM_STATE_F_RENDERING : 0)
                      | (c->flip_last_us != 0
                         && g_get_monotonic_time() - c->flip_last_us < 500000
                             ? MLM_STATE_F_VIDEO_LIVE : 0),
               .pos_ms = c->pb_active ? c->pb_pos_ms : 0,
               .dur_ms = c->pb_active ? c->pb_dur_ms : 0 },
    };
    sendto(c->tsock, &rec, sizeof rec, MSG_DONTWAIT,
           (struct sockaddr *)&c->taddr, sizeof c->taddr);
}

/* Toggle recording (the HUD record button's REC_TOGGLE, or the state_tick fallback). */
/* Next free recording path: <dir>/Video<NNN>.mp4 with vendor-style 3-digit auto-increment. Scans
 * the SD recordings dir for the highest existing VideoNNN.mp4 and returns NNN+1. dir defaults to
 * /mnt/sdcard (the mounted SD, where the HUD Playback list reads); override with ML_REC_DIR.
 */
static void rec_next_path(char *out, size_t outsz)
{
    const char *dir = getenv("ML_REC_DIR") ? getenv("ML_REC_DIR") : "/mnt/sdcard";
    int max = 0;
    DIR *d = opendir(dir);

    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = 0;
            char tail[8] = "";

            /* strict match: Video<digits>.mp4 (reject Video5.txt, Video5.mp4x, etc.) */
            if (sscanf(e->d_name, "Video%d%7s", &n, tail) == 2
                && strcmp(tail, ".mp4") == 0 && n > max) {
                max = n;
            }
        }
        closedir(d);
    }
    snprintf(out, outsz, "%s/Video%03d.mp4", dir, max + 1);
}

static void rec_toggle(struct ctx *c)
{
    if (c->rec_on) {
        rec_stop(c);
    } else {
        char path[300];
        /* ML_DVR pins an explicit path (test/bench); otherwise auto-number onto the SD. */
        if (getenv("ML_DVR")) {
            rec_start(c, getenv("ML_DVR"));
        } else {
            rec_next_path(path, sizeof path);
            rec_start(c, path);
        }
    }

    send_state(c);   /* report the new mode immediately, not just on the next tick */
}

/* Re-assert the current mode once a second so a restarted HUD reconverges without a state change. */
gboolean state_tick(gpointer u)
{
    send_state((struct ctx *)u);
    return G_SOURCE_CONTINUE;
}

/* ctrl.sock: a HUD command arrived (MLM_T_CMD). Connectionless DGRAM, so a down HUD just means no
 * datagrams; nothing to reconnect. Unknown commands are ignored (forward-compatible).
 */
gboolean on_ctrl(gint fd, GIOCondition cond, gpointer u)
{
    struct ctx *c = u;
    /* sized for the largest payload: an OSD burn-in cell frame (> MLM_PATH_MAX) */
    char buf[sizeof(struct mlm_hdr) + sizeof(struct mlm_cmd) + MLM_OSD_CELL_MAX];
    const size_t head = sizeof(struct mlm_hdr) + sizeof(struct mlm_cmd);

    (void)cond;
    ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
    if (n < (ssize_t)head) {
        return G_SOURCE_CONTINUE;
    }

    struct mlm_hdr h;
    struct mlm_cmd cmd;
    memcpy(&h, buf, sizeof h);
    memcpy(&cmd, buf + sizeof h, sizeof cmd);
    if (h.magic != MLM_MAGIC || h.type != MLM_T_CMD) {
        return G_SOURCE_CONTINUE;
    }

    /* MLM_CMD_PLAY carries a NUL-terminated path after the mlm_cmd; force-terminate defensively. */
    buf[n] = '\0';
    const char *path = (n > (ssize_t)head) ? buf + head : NULL;

    switch (cmd.cmd) {
        case MLM_CMD_REC_TOGGLE: {
            rec_toggle(c);
        } break;

        case MLM_CMD_PLAY: {
            if (path && *path) {
                playback_play(c, path);
            }
        } break;

        case MLM_CMD_PAUSE: {
            playback_pause(c);
        } break;

        case MLM_CMD_RESUME: {
            playback_resume(c);
        } break;

        case MLM_CMD_SEEK: {
            playback_seek(c, cmd.arg);
        } break;

        case MLM_CMD_STOP: {
            playback_stop(c, TRUE);
        } break;

        case MLM_CMD_SPEED: {
            playback_set_speed(c, (gint32)cmd.arg);   /* arg is the signed multiplier bit-cast */
        } break;

        case MLM_CMD_SRT_TEXT: {
            /* one telemetry subtitle line from the HUD; text rides after the cmd like PLAY's path */
            if (path && *path) {
                rec_srt_text(c, path);
            }
        } break;

        case MLM_CMD_OSD_CELL: {
            /* one rendered BTFL OSD cell (or a clear) for the DVR burn-in; the cell frame rides
             * after the cmd like PLAY's path */
            osd_burn_cell(c, (const guint8 *)buf + head, n - (gssize)head);
        } break;

        case MLM_CMD_DVR_RES: {
            /* Latch the recording format (arg = height << 16 | fps); applied at the next
             * rec_start. Out-of-range values are ignored (the latch keeps its last good state).
             */
            unsigned hgt = cmd.arg >> 16;
            unsigned rfps = cmd.arg & 0xffff;
            if (rec_fmt_by_height((int) hgt) != NULL && (rfps == 30 || rfps == 60)) {
                c->dvr_h = (int)hgt;
                c->dvr_fps = (int)rfps;
            }
        } break;

        case MLM_CMD_SHOW_IDLE: {
            /* Live link dropped: park on the no-signal splash instead of the last decoded frame.
             * Never during playback - a file owns the display. The display thread does the actual
             * flip (it owns every DRM commit); we just raise the flag and kick it awake.
             */
            if (!c->pb_active) {
                c->show_idle = 1;
                pipe_wake(c->wake_w);
            }
        } break;

        default: {
            /* unknown command: ignore (forward-compatible) */
        } break;
    }

    return G_SOURCE_CONTINUE;
}
