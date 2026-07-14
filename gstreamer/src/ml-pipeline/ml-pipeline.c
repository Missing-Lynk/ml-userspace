/*
 * ml-pipeline - the data-plane GStreamer process (plans/done/gst-two-process-hud.md,
 * plans/rf-gstreamer-pipeline.md).
 *
 * Two input modes:
 *
 *   file:  ml-pipeline <file.h264|file.h265>
 *     Plays a file through the wave5 V4L2 decoder, scanning each decoded frame out on the
 *     primary CRTC via the custom DRM sink (single-stream mode). The original decode->display
 *     validation path. NOT kmssink: artosyn_vo has no dumb-buffer support, so kmssink aborts.
 *
 *   rf:    ml-pipeline rf [plane-id]
 *     Binds UDP :10001 (the air unit's H.265 downlink over sdio0), strips the 36-byte
 *     video_packet_header, demuxes the two encode channels (vertical tiles), decodes each
 *     with its own wave5 instance, then COMPOSES the two tiles into one 1920x1080 I420
 *     frame and displays it on the primary plane via the custom DRM sink (method 2, decided in
 *     plans/rf-gstreamer-pipeline.md - two separate scanouts on one DRM fd in one process wedges
 *     the device, so we compose to one plane instead). Tile 0 (1920x560) lands at y=0, tile 1
 *     (1920x552) at y=528; the 32-row overlap self-resolves (the overlapping rows are
 *     pixel-identical). Tiles are paired by PTS (= FrameId / fps). CPU blit for now; the
 *     dw-axi-dmac DMA blit is the later perf optimization. Testable with no air unit via
 *     glue/capture/rf-replay.py. The freed second plane is where the HUD goes.
 *
 * Both modes emit one MLM_T_FRAMESTATS record per composed/decoded frame, and rf mode sends
 * a periodic MLM_T_READY heartbeat to link.sock so ml-linkd can gate the air's
 * video-start on a listening consumer. The producer never blocks: telemetry is SOCK_DGRAM +
 * MSG_DONTWAIT.
 *
 * Build: gstreamer/src/build.sh; needs the /mnt/gst prefix at runtime (gst-env.sh).
 */
#include "ml-pipeline.h"


/* file mode's per-decoded-frame FRAMESTATS probe (unchanged). */
static GstPadProbeReturn on_frame(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    struct ctx *c = u;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);

    emit_framestats(c, buf ? GST_BUFFER_PTS(buf) : GST_CLOCK_TIME_NONE);
    return GST_PAD_PROBE_OK;
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer u)
{
    struct ctx *c = u;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        g_main_loop_quit(c->loop);
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "ml-pipeline: ERROR %s (%s)\n", err->message, dbg ? dbg : "");
        g_clear_error(&err);
        g_free(dbg);
        g_main_loop_quit(c->loop);
        break;
    }
    default:
        break;
    }
    return TRUE;
}

static gboolean on_signal(gpointer u)
{
    g_main_loop_quit(((struct ctx *)u)->loop);
    return G_SOURCE_REMOVE;
}

/* SIGUSR1: arm 6 post-scanout retire dumps at an operator-chosen moment (e.g. when the
 * on-panel frame counter nears a suspect region). Runs on the main loop, not in signal
 * context; the display thread picks retire_arm up at the next flip.
 */
static gboolean on_sigusr1(gpointer u)
{
    struct ctx *c = u;

    c->retire_seq = 0;
    c->retire_arm = 6;
    fprintf(stderr, "ml-pipeline: SIGUSR1 - arming 6 retire dumps\n");
    return G_SOURCE_CONTINUE;
}

static void setup_appsrc(struct ctx *c, int ch)
{
    char name[8];
    snprintf(name, sizeof name, "src%d", ch);
    GstElement *e = gst_bin_get_by_name(GST_BIN(c->pipe), name);
    GstCaps *caps = gst_caps_new_simple("video/x-h265",
                                        "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au", NULL);
    /* leaky-type MUST be 0: leaking (dropping) queued COMPRESSED AUs mid-GOP silently breaks
     * the H.265 reference chain and paints accumulating garbage until the next IDR. But with
     * block=FALSE and leaky-type=0, appsrc NEVER fails a push either - max-bytes only drives
     * the need-data/enough-data signals - so the real bound is enforced in push_au
     * (AU_QUEUE_MAX + drop-until-IRAP resync). max-bytes here is kept in step for the
     * signalling.
     */
    g_object_set(e, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME,
                 "do-timestamp", FALSE, "max-bytes", (guint64)(4 << 20),
                 "block", FALSE, "leaky-type", 0, NULL);
    gst_caps_unref(caps);
    c->src[ch] = GST_APP_SRC(e);
}

/* Advertise GstVideoMeta support on the appsink's allocation query. Without it the decoder
 * can only push buffers whose layout needs no meta: tile 0 (560 rows = 16-aligned) sails
 * through zero-copy, but tile 1 (552 rows, padded allocation) NEEDS meta to describe its
 * planes, so gstv4l2videodec silently normalize-COPIES every tile-1 frame into system
 * memory (~the entire historical tile-1 slowness/allocator=sysmem asymmetry).
 */
static GstPadProbeReturn asink_alloc_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);

    (void)pad;
    (void)u;
    if (GST_QUERY_TYPE(q) == GST_QUERY_ALLOCATION) {
        gst_query_add_allocation_meta(q, GST_VIDEO_META_API_TYPE, NULL);
    }
    return GST_PAD_PROBE_OK;
}

static void setup_appsink(struct ctx *c, int ch)
{
    char name[8];
    snprintf(name, sizeof name, "asink%d", ch);
    GstElement *e = gst_bin_get_by_name(GST_BIN(c->pipe), name);
    g_object_set(e, "emit-signals", FALSE, "sync", FALSE, "async", FALSE,
                 "max-buffers", 2, "drop", TRUE, "caps", NULL, NULL);
    GstAppSinkCallbacks cb = { .new_sample = on_tile };
    gst_app_sink_set_callbacks(GST_APP_SINK(e), &cb, c, NULL);
    GstPad *sp = gst_element_get_static_pad(e, "sink");
    gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, asink_alloc_probe, NULL, NULL);
    gst_object_unref(sp);
    c->asink[ch] = GST_APP_SINK(e);
}

static int run_rf(struct ctx *c, int plane, int drm_fd)
{
    /* Two decoders -> two appsinks (raw I420 tiles); a compositor thread pairs them by PTS,
     * DMA-blits both into one 1920x1080 I420 composite, and hands it to the custom DRM/KMS
     * display sink (drm_disp_*), which flips it onto the primary plane and retires the buffer
     * only when the NEXT frame's page-flip completes - the vendor's retire-after-scanout model,
     * replacing kmssink (whose skip-vsync release freed the buffer before the flip latched, so
     * the pool overwrote a buffer the DC was still scanning). The decode pipeline is rebuilt on
     * each session restart; the display sink persists (holds the DRM fd, keeps the last frame on
     * the panel across a restart, the vendor behavior).
     */
    GError *err = NULL;
    (void)plane;   /* the composite owns the CRTC primary plane via page-flip, no plane-id arg */
    c->drm_fd = drm_fd;
    /* NOTE: dec0 exports its decoded buffers as dmabuf (so tile 0 DMA-blits), but dec1 gives
     * non-dmabuf memory (tile 1 CPU-falls-back) - a wave5 asymmetry tied to the 552-row / meta-less
     * tile, NOT fixed by capture-io-mode=dmabuf (tried, no effect). Full-DMA needs a wave5/v4l2
     * export dive; the CPU fallback keeps it correct meanwhile. See plans/gst-dma-compositor.md.
     */
    c->pipe = gst_parse_launch(
        "appsrc name=src0 ! h265parse ! v4l2h265dec capture-io-mode=dmabuf name=dec0 ! appsink name=asink0 "
        "appsrc name=src1 ! h265parse ! v4l2h265dec capture-io-mode=dmabuf name=dec1 ! appsink name=asink1 ", &err);
    if (!c->pipe) {
        fprintf(stderr, "ml-pipeline: rf parse: %s\n", err->message);
        return 1;
    }
    setup_appsrc(c, 0);
    setup_appsrc(c, 1);
    setup_appsink(c, 0);
    setup_appsink(c, 1);
    pthread_mutex_init(&c->comp_lock, NULL);
    c->last_fid = -1;

    c->lsock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    c->laddr.sun_family = AF_UNIX;
    strncpy(c->laddr.sun_path, MLM_LINK_SOCK, sizeof c->laddr.sun_path - 1);

    /* ctrl.sock: the HUD record button (and the ml-rec test tool) send MLM_T_CMD here. Bind is
     * best-effort - if it fails the pipeline still runs, just without external record control.
     */
    c->ctrl_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (c->ctrl_sock >= 0) {
        struct sockaddr_un ca = { .sun_family = AF_UNIX };
        strncpy(ca.sun_path, MLM_CTRL_SOCK, sizeof ca.sun_path - 1);
        unlink(MLM_CTRL_SOCK);
        if (bind(c->ctrl_sock, (struct sockaddr *)&ca, sizeof ca) < 0) {
            perror("ml-pipeline: bind ctrl.sock");
            close(c->ctrl_sock);
            c->ctrl_sock = -1;
        }
    }

    c->loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(c->pipe);
    gst_bus_add_watch(bus, on_bus, c);
    g_unix_signal_add(SIGINT, on_signal, c);
    g_unix_signal_add(SIGTERM, on_signal, c);
    g_unix_signal_add(SIGUSR1, on_sigusr1, c);
    if (c->ctrl_sock >= 0) {
        g_unix_fd_add(c->ctrl_sock, G_IO_IN, on_ctrl, c);
    }
    guint ready_id = g_timeout_add_seconds(2, rf_ready_tick, c);
    guint state_id = g_timeout_add_seconds(1, state_tick, c);   /* re-assert MLM_T_STATE to the HUD */

    /* Plane-scanout by default (plans/gst-plane-scanout.md): tiles ride video0/video1
     * directly, zero blits, no composite pool. ML_COMPOSE=1 forces the old composite
     * path (e.g. when the HUD needs bank 1). Falls back automatically if the plane
     * setup fails.
     */
    c->planes_on = !getenv("ML_COMPOSE");
    if (getenv("ML_DVR") && c->planes_on) {
        /* DVR encodes the composite; plane scanout builds no composite. Force composite mode. */
        fprintf(stderr, "ml-pipeline: ML_DVR set -> forcing composite mode (plane scanout has no composite to encode)\n");
        c->planes_on = FALSE;
    }
    c->dmablit_fd = -1;
    if (c->planes_on && drm_disp_init(c) == 0) {
        /* Gate bounds: display holds at most 4 pairs (next/pending/front/prev) of the
         * ~9-buffer decoder capture pools; keep unpaired skew small so parked halves
         * cannot starve a decoder.
         */
        c->skew_max = 3;
        c->inflight_max = 4;
        printf("ml-pipeline: rf -> PLANE SCANOUT (video0/video1 atomic pairs), drm fd %d, :%d\n",
               drm_fd, RF_VIDEO_PORT);
    } else {
        if (c->planes_on) {
            fprintf(stderr, "ml-pipeline: plane scanout unavailable, falling back to composite\n");
            c->planes_on = FALSE;
        }
        printf("ml-pipeline: rf -> composite 1920x1080 via custom DRM sink, drm fd %d, :%d\n",
               drm_fd, RF_VIDEO_PORT);
        if (!comp_pool_init(c)) {
            fprintf(stderr, "ml-pipeline: composite dma-heap pool init failed "
                            "(CONFIG_DMABUF_HEAPS_CMA? enough CMA?)\n");
            return 1;
        }
        if (drm_disp_init(c)) {
            fprintf(stderr, "ml-pipeline: DRM display sink init failed\n");
            return 1;
        }
        /* Increment 2: off-CPU tile blit via the AXI DMA engine. Optional - if /dev/ml-dmablit
         * is absent (ml_dmablit.ko not loaded) or a tile is not a packed dmabuf, on_tile
         * CPU-blits.
         */
        c->dmablit_fd = open("/dev/ml-dmablit", O_RDWR | O_CLOEXEC);
        fprintf(stderr, "ml-pipeline: tile blit = %s\n",
                c->dmablit_fd >= 0 ? "DMA (ml_dmablit) with CPU fallback" :
                "CPU only (/dev/ml-dmablit not available - load ml_dmablit.ko for DMA)");
    }

    gst_element_set_state(c->pipe, GST_STATE_PLAYING);
    if (getenv("ML_DVR")) {
        rec_start(c, getenv("ML_DVR"));   /* auto-start recording for the session */
    }

    c->rx_run = 1;
    pthread_create(&c->rx_thread, NULL, rf_rx, c);
    g_main_loop_run(c->loop);

    c->rx_run = 0;
    pthread_join(c->rx_thread, NULL);
    rec_stop(c);   /* finalize the DVR MP4 before tearing down decoders (composites have stopped) */
    g_source_remove(ready_id);
    g_source_remove(state_id);
    if (c->ctrl_sock >= 0) {
        close(c->ctrl_sock);
        unlink(MLM_CTRL_SOCK);
    }
    /* Scanout FIRST, decoders second: in plane mode the DC is fetching the decoders' own
     * dmabufs, so the planes must be off (and latched off) before the gst teardown starts
     * releasing capture pools. The reverse order left a window where the DC fetched
     * buffers whose owner was mid-teardown (a mode-switch hard hang).
     */
    drm_disp_shutdown(c);
    gst_element_set_state(c->pipe, GST_STATE_NULL);
    clear_pending(c);

    printf("ml-pipeline: rf done. rx=%llu bad_hdr=%llu bad_crc=%llu pushed=%llu composed=%llu\n",
           (unsigned long long)c->rx_pkts, (unsigned long long)c->rx_bad_hdr,
           (unsigned long long)c->rx_bad_crc, (unsigned long long)c->rx_pushed,
           (unsigned long long)c->composed);

    gst_object_unref(bus);
    gst_object_unref(c->pipe);
    if (c->dmablit_fd >= 0) {
        close(c->dmablit_fd);
    }
    return 0;
}

/* Single-stream file playback: scan out each decoded frame's own dmabuf on the primary CRTC via the
 * drm_disp mailbox (no composite pool, no kmssink). The sample is held in the ditem until the flip
 * retires it, keeping the decoder buffer alive while it is on screen. Zero-copy.
 */
static GstFlowReturn on_file_sample(GstAppSink *sink, gpointer u)
{
    struct ctx *c = u;
    GstSample *s = gst_app_sink_pull_sample(sink);
    if (!s) {
        return GST_FLOW_OK;
    }
    GstBuffer *buf = gst_sample_get_buffer(s);
    struct tileview t;
    GstMapInfo m;

    if (!map_tile(s, buf, &m, &t)) {
        gst_sample_unref(s);
        return GST_FLOW_OK;
    }
    int fd = t.fd;
    gst_buffer_unmap(buf, &m);   /* only the fd + geometry are needed to build the scanout FB */
    if (fd < 0) {                /* not a single-dmabuf buffer -> cannot scan out zero-copy */
        gst_sample_unref(s);
        return GST_FLOW_OK;
    }
    guint32 fb = tile_fb_get(c, 0, &t);
    if (!fb) {
        gst_sample_unref(s);
        return GST_FLOW_OK;
    }
    struct ditem it = { .cbi = -1 };
    it.smp[0] = s;               /* ownership -> display thread; released on retirement */
    it.fb[0] = fb;
    drm_disp_submit(c, &it, GST_BUFFER_PTS(buf));
    return GST_FLOW_OK;
}

static int run_file(struct ctx *c, const char *file, int plane, int drm_fd)
{
    (void)plane;   /* single-stream scans out on the primary CRTC, not a fixed plane id */
    const char *codec = (strstr(file, "265") || strstr(file, "hevc")) ? "h265" : "h264";
    gchar *desc = g_strdup_printf(
        "filesrc location=%s ! %sparse ! v4l2%sdec capture-io-mode=dmabuf name=dec "
        "! appsink name=sink",
        file, codec, codec);
    GError *err = NULL;
    c->pipe = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!c->pipe) {
        fprintf(stderr, "ml-pipeline: parse: %s\n", err->message);
        return 1;
    }
    GstElement *dec = gst_bin_get_by_name(GST_BIN(c->pipe), "dec");
    GstPad *pad = gst_element_get_static_pad(dec, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_frame, c, NULL);
    gst_object_unref(pad);
    gst_object_unref(dec);

    /* Display through the custom DRM sink (appsink -> drm_disp), NOT kmssink: artosyn_vo has no
     * dumb-buffer support, so kmssink's allocator aborts on this platform (which is why the RF path
     * uses this same sink). sync=TRUE paces playback to the clip's PTS.
     */
    GstElement *sink = gst_bin_get_by_name(GST_BIN(c->pipe), "sink");
    g_object_set(sink, "emit-signals", FALSE, "sync", TRUE, "max-buffers", 2, "drop", FALSE, NULL);
    GstAppSinkCallbacks cb = { .new_sample = on_file_sample };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, c, NULL);
    gst_object_unref(sink);

    c->drm_fd = drm_fd;
    c->single = TRUE;
    c->planes_on = FALSE;
    if (drm_disp_init(c)) {
        fprintf(stderr, "ml-pipeline: DRM display init failed\n");
        return 1;
    }

    c->loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(c->pipe);
    gst_bus_add_watch(bus, on_bus, c);
    g_unix_signal_add(SIGINT, on_signal, c);
    g_unix_signal_add(SIGTERM, on_signal, c);

    printf("ml-pipeline: %s (%s) -> single-stream scanout, drm fd %d\n", file, codec, drm_fd);
    gst_element_set_state(c->pipe, GST_STATE_PLAYING);
    g_main_loop_run(c->loop);

    /* Scanout off before the decoders free their buffers (same order as the RF teardown). */
    drm_disp_shutdown(c);
    gst_element_set_state(c->pipe, GST_STATE_NULL);

    double secs = (c->t_last - c->t_first) / 1e6;
    printf("ml-pipeline: %u frames in %.2fs = %.2f fps; %llu telemetry records dropped\n",
           c->frame_id, secs, secs > 0 ? (c->frame_id - 1) / secs : 0,
           (unsigned long long)c->dropped);

    gst_object_unref(bus);
    gst_object_unref(c->pipe);
    return 0;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    crc32_init();
    if (argc < 2) {
        fprintf(stderr, "usage: ml-pipeline <file.h264|file.h265> [plane-id]\n"
                        "       ml-pipeline rf [plane-id]\n");
        return 2;
    }

    int drm_fd = mlm_get_drm_fd();
    if (drm_fd < 0) {
        fprintf(stderr, "ml-pipeline: cannot get DRM fd from %s (is ml-drmfd running?)\n",
                MLM_DRM_SOCK);
        return 1;
    }

    struct ctx c = { .taddr = { .sun_family = AF_UNIX }, .dmablit_fd = -1, .stage_fd = -1 };
    strncpy(c.taddr.sun_path, MLM_TELEMETRY_SOCK, sizeof c.taddr.sun_path - 1);
    c.tsock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (c.tsock < 0) {
        perror("socket");
        return 1;
    }

    int rc;
    if (!strcmp(argv[1], "rf")) {
        int plane = (argc > 2) ? atoi(argv[2]) : 33;   /* composite on the full-screen primary */
        c.rf = TRUE;
        rc = run_rf(&c, plane, drm_fd);
    } else {
        int plane = (argc > 2) ? atoi(argv[2]) : 33;
        rc = run_file(&c, argv[1], plane, drm_fd);
    }
    return rc;
}
