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
 *                     rate <bps> <fps> [vbv-ms], keyframe
 *   ML_AIR_ON_DEMAND  hold the encoders closed until the first keyframe request (camera path only)
 *   ML_AIR_HEAP       dma_heap name for tile bufs (default: first non-mmz heap, else any)
 *   ML_AIR_DUMP       prefix: also write <prefix>_tileN.h265
 *   ML_AIR_NOTX       encode (and dump) without transmitting
 *   ML_AIR_VERBOSE    log one line per second when set (same as -v / --verbose)
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
#include "ml-air-video.h"

/* Shared state declared in ml-air-video.h. Written during setup and then read concurrently; the
 * volatile counters inside g_tile[] are the only fields that move afterwards. */
volatile guint64 g_src_lost;
struct air_tile g_tile[AIR_NCHN];
GMainLoop *g_loop;
GstAllocator *g_dmabuf_alloc;
GstVideoInfo g_src_info;
GQuark g_recycle_quark;
static int g_verbose;
int g_notx;
int g_nosei;
int g_enc_direct;
int g_on_demand;
volatile gint g_enc_up;
volatile gint g_tx_armed;
int g_stagger;
int g_primed;
int g_bench_free;
volatile int g_bench_stop;
int g_bench_fps;
int g_bench_secs;
char **g_bench_stages;
const char *g_bench_stage;

gboolean air_on_signal(gpointer user)
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
gboolean air_on_tick(gpointer user)
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
                g_printerr(TAG " bench %s tile %d: pushed %" G_GUINT64_FORMAT "/s"
                           " done %" G_GUINT64_FORMAT "/s %.2f Mbit/s"
                           " dropped %" G_GUINT64_FORMAT "/s\n",
                           bench, i, dp, dd, (double)db * 8.0 / 1e6, dx);
            } else {
                g_printerr(TAG " bench %s tile %d: pushed %" G_GUINT64_FORMAT "/s"
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
        g_printerr(TAG " tx chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT
                   " dropped=%" G_GUINT64_FORMAT "\n",
                   g_tile[0].sent, g_tile[1].sent, g_tile[0].dropped);

        /* Camera rates as per-second deltas. The whole point of the zero-copy path is that
         * `cap` and `enc` both sit at the sensor rate with `skip` at zero; a cumulative count
         * cannot show that, and neither can a count taken once. */
        if (g_cap_fd >= 0) {
            static guint64 last_cap;
            static guint64 last_skip;

            g_printerr(TAG " cam cap=%" G_GUINT64_FORMAT "/s skip=%" G_GUINT64_FORMAT
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

const char *air_env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : dflt;
}

gboolean air_on_bus(GstBus *bus, GstMessage *msg, gpointer user)
{
    (void)bus;
    (void)user;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;

        gst_message_parse_error(msg, &err, &dbg);
        g_printerr(TAG " error from %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
        if (dbg != NULL) {
            g_printerr(TAG " debug: %s\n", dbg);
        }

        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(g_loop);
    } break;

    case GST_MESSAGE_EOS: {
        g_printerr(TAG " end of stream\n");
        g_main_loop_quit(g_loop);
    } break;

    default: {
    } break;
    }

    return TRUE;
}

/** Build one per-tile encode pipeline: appsrc -> encoder -> h265parse -> appsink. Returns the
 * pipeline and stores the tile's appsrc + appsink, or NULL on failure. */
GstElement *air_build_encoder(struct air_tile *tile, const char *enc, int fps, GstBus **bus_out)
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
             AIR_COMP_W, tile->height, fps, enc);

    pipe = gst_parse_launch(desc, &err);
    if (pipe == NULL) {
        g_printerr(TAG " encoder %d build failed: %s\n",
                   tile->chn, err ? err->message : "?");
        if (err != NULL) {
            g_error_free(err);
        }

        return NULL;
    }

    el = gst_bin_get_by_name(GST_BIN(pipe), "in");
    tile->src = GST_APP_SRC(el);

    el = gst_bin_get_by_name(GST_BIN(pipe), "out");
    memset(&cbs, 0, sizeof cbs);
    cbs.new_sample = air_on_enc;
    gst_app_sink_set_callbacks(GST_APP_SINK(el), &cbs, tile, NULL);
    gst_object_unref(el);

    *bus_out = gst_element_get_bus(pipe);
    return pipe;
}

int main(int argc, char **argv)
{
    const char *dst = air_env_or("ML_AIR_DST", "10.0.0.1");
    int port = atoi(air_env_or("ML_AIR_PORT", "10001"));
    int fps = atoi(air_env_or("ML_AIR_FPS", "15"));
    const char *pattern = air_env_or("ML_AIR_PATTERN", "ball");
    const char *camera = getenv("ML_AIR_CAMERA");
    const char *bench = getenv("ML_AIR_BENCH");
    const char *ring_env = getenv("ML_AIR_RING");
    const char *pool_env = getenv("ML_AIR_POOL");
    int pool_want;
    int cap_want = atoi(air_env_or("ML_AIR_BUFS", G_STRINGIFY(AIR_CAP_BUFS_DEF)));
    const char *enc = air_env_or("ML_AIR_ENC",
                             "v4l2h265enc output-io-mode=dmabuf-import "
                             "extra-controls=\"controls,video_gop_size=65535\"");
    const char *dump = air_env_or("ML_AIR_DUMP", NULL);
    const char *ctrl = air_env_or("ML_AIR_CTRL", "/run/missinglynk/air-video.sock");
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
    g_nosei = (getenv("ML_AIR_NOSEI") != NULL);
    g_bench_free = (getenv("ML_AIR_BENCH_FREE") != NULL);
    g_bench_fps = fps > 0 ? fps : 60;
    g_cap_fps = fps;

    g_cap_inflight_max = atoi(air_env_or("ML_AIR_INFLIGHT", G_STRINGIFY(AIR_CAP_INFLIGHT_DEF)));
    if (g_cap_inflight_max < 1) {
        g_cap_inflight_max = 1;
    }

    if (bench != NULL && *bench == '\0') {
        bench = NULL;
    }

    if (bench != NULL) {
        g_bench_secs = atoi(air_env_or("ML_AIR_BENCH_SECS", "10"));
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

    /* Only the direct path can defer: the GStreamer encoder is built into a pipeline at start-up,
     * and the bench feeder has no receiver to wait for. */
    g_on_demand = (g_enc_direct && getenv("ML_AIR_ON_DEMAND") != NULL);
    for (int i = 0; i < AIR_NCHN; i++) {
        g_tile[i].enc_fd = -1;
    }

    /* A control client that gives up before reading its reply (ml-air-ctl under `timeout`) leaves
     * the reply write to take EPIPE, whose default action would kill the whole video daemon. */
    signal(SIGPIPE, SIG_IGN);
    gst_init(&argc, &argv);

    /* Every other long-running daemon takes -v as well as its env knob; accept it here so the
     * convention holds. Scanned after gst_init so its own options are already consumed. Unknown
     * arguments stay ignored: this tool is configured through ML_AIR_* and has no other operands.
     */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            g_verbose = 1;
        }
    }

    g_recycle_quark = g_quark_from_static_string("air-recycle");
    g_dmabuf_alloc = gst_dmabuf_allocator_new();
    gst_video_info_set_format(&g_src_info, GST_VIDEO_FORMAT_I420, AIR_COMP_W, AIR_COMP_H);

    memset(&dstaddr, 0, sizeof dstaddr);
    dstaddr.sin_family = AF_INET;
    dstaddr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, dst, &dstaddr.sin_addr) != 1) {
        g_printerr(TAG " bad destination address: %s\n", dst);
        return 1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror(TAG " socket");
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
        struct air_tile *tile = &g_tile[i];

        if (!tile->active) {
            continue;
        }

        tile->alloc_h = (tile->height + 15) & ~15;
        tile->buf_size = (gsize)AIR_COMP_W * tile->alloc_h * 3 / 2;
        tile->sock = sock;
        tile->dst = dstaddr;
        tile->fps = fps;
        tile->resolution = vph_resolution(AIR_COMP_W, AIR_COMP_H);
        tile->txbuf = g_malloc(AIR_TX_MAX);
        tile->dumpfd = -1;
        tile->sent = 0;
        tile->dropped = 0;

        /* The camera path shares capture buffers, so it allocates no tile buffers at all and
         * the whole mmz pool stays with the two encoder instances. A tile named by ML_AIR_COPY is
         * the exception: it is fed by copy and needs its pool back. */
        if ((camera == NULL || tile->copy) && air_pool_init(tile, pool_want) < AIR_POOL_MIN) {
            g_printerr(TAG " tile %d: dma-heap pool alloc failed (need %d, got %d)\n",
                       i, AIR_POOL_MIN, tile->pool_n);
            return 1;
        }

        if (dump != NULL) {
            char path[300];

            snprintf(path, sizeof path, "%s_tile%d.h265", dump, i);
            tile->dumpfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (tile->dumpfd >= 0) {
                g_printerr(TAG " dumping tile %d -> %s\n", i, path);
            }
        }

        /* Render the ring once. Everything after this point in benchmark mode is pointer
         * shuffling: no source element, no copy, no CPU pass over a pixel. */
        if (bench != NULL) {
            air_render_ring(tile, g_bench_stages[0]);
            g_printerr(TAG " bench: tile %d ring of %d rendered as %s\n",
                       i, tile->pool_n, g_bench_stages[0]);
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
            g_printerr(TAG " source build failed: %s\n", err ? err->message : "?");
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
        g_printerr(TAG " bench %s: %dx%d two H.265 tiles, %s, %s, %d s per tier\n",
                   bench, AIR_COMP_W, AIR_COMP_H,
                   g_bench_free ? "unpaced" : "paced to ML_AIR_FPS",
                   g_notx ? "encode-only (no transmit)" : "transmitting", g_bench_secs);
    } else if (g_notx) {
        g_printerr(TAG " %dx%d @ %d fps from %s, two H.265 tiles, "
                   "encode-only (no transmit)\n",
                   AIR_COMP_W, AIR_COMP_H, fps, camera != NULL ? camera : "videotestsrc");
    } else {
        g_printerr(TAG " %dx%d @ %d fps from %s, two H.265 tiles -> %s:%d\n",
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

            g_print(TAG " encoder %s\n", enc_node);

            /* Recorded up front either way: the on-demand bring-up runs on the feeder thread and
             * takes its node and rate from here, the same fields air_enc_restart re-opens from. */
            for (int i = 0; i < AIR_NCHN; i++) {
                g_strlcpy(g_tile[i].enc_node, enc_node, sizeof g_tile[i].enc_node);
                g_tile[i].enc_fps = fps;
            }

            if (g_on_demand) {
                g_print(TAG " encoders held until a receiver asks for a keyframe\n");
            } else {
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

                g_atomic_int_set(&g_enc_up, 1);
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

    g_printerr(TAG " stopped (chn0=%" G_GUINT64_FORMAT " chn1=%" G_GUINT64_FORMAT
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
    air_ctrl_close();

    close(sock);
    return 0;
}
