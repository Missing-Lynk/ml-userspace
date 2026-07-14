/* ml-pipeline record: DVR branch (zero-copy composite -> H.264 MP4) + HUD ctrl seam. */
#include "ml-pipeline.h"

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
    const char *res = getenv("ML_DVR_RES");   /* "720" -> downscale to 1280x720 (CPU videoscale) */
    const char *fps = getenv("ML_DVR_FPS");   /* "30"  -> record at 30 fps */
    char rate[96] = "", scale[96] = "";

    if (c->rec_bin) {                   /* already recording */
        return 0;
    }

    /* Optional levers. Framerate first (drop before scaling = less work). videorate on dmabuf
     * passes kept frames through zero-copy; videoscale copies to system memory (no HW scaler
     * wired), so 720p recording is CPU-bound - the base 1080p60 path stays zero-copy.
     */
    if (fps && !strcmp(fps, "30")) {
        snprintf(rate, sizeof rate, "! videorate ! video/x-raw,framerate=30/1 ");
    }

    if (res && !strcmp(res, "720")) {
        snprintf(scale, sizeof scale, "! videoscale ! video/x-raw,width=1280,height=720 ");
    }

    /* Feed the encoder system-memory I420. The composite dmabufs are CPU-mappable (CMA), so
     * pushing them under plain video/x-raw caps makes the encoder's mmap input pool map+copy each
     * frame (one 3 MiB memcpy at the encoder's own rate; the composite ring absorbs any lag). The
     * zero-copy dmabuf-import path (DMA_DRM/YU12) is a follow-up optimization.
     */
    desc = g_strdup_printf(
        "appsrc name=recsrc is-live=true format=time do-timestamp=false "
        "max-buffers=6 leaky-type=downstream block=false "
        "! video/x-raw,format=I420,width=%d,height=%d,framerate=60/1 "
        "%s%s"
        "! v4l2h264enc "

        /* Fragmented MP4 (moof/mdat every 1 s): the file on disk is playable up to the last
         * complete fragment even if the process is SIGKILLed (OOM) - which no signal handler can
         * catch. A clean stop (rec_stop EOS) still finalizes normally. Costs at most ~1 s of tail.
         */
        "! h264parse ! mp4mux fragment-duration=1000 ! filesink location=%s",
        COMP_W, COMP_H, rate, scale, path);

    c->rec_bin = gst_parse_launch(desc, &err);
    g_free(desc);

    if (!c->rec_bin) {
        fprintf(stderr, "ml-pipeline: DVR record bin: %s\n", err ? err->message : "?");
        if (err) {
            g_error_free(err);
        }

        return 1;
    }

    src = gst_bin_get_by_name(GST_BIN(c->rec_bin), "recsrc");
    c->rec_src = GST_APP_SRC(src);      /* keep this ref; released in rec_stop */
    caps = gst_caps_new_simple("video/x-raw",
                               "format", G_TYPE_STRING, "I420",
                               "width", G_TYPE_INT, COMP_W,
                               "height", G_TYPE_INT, COMP_H,
                               "framerate", GST_TYPE_FRACTION, 60, 1, NULL);
    gst_app_src_set_caps(c->rec_src, caps);
    gst_caps_unref(caps);
    snprintf(c->rec_path, sizeof c->rec_path, "%s", path);

    c->rec_epoch_set = FALSE;
    c->rec_pushed = c->rec_dropped = 0;

    if (gst_element_set_state(c->rec_bin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "ml-pipeline: DVR record bin failed to start (encoder CMA? see plans/wave5-encoder-fit.md)\n");
        rec_stop(c);

        return 1;
    }

    c->rec_on = 1;
    printf("ml-pipeline: DVR recording -> %s\n", path);

    return 0;
}

/* Finalize the MP4 (EOS -> flush encoder -> write moov) and tear the bin down. */
void rec_stop(struct ctx *c)
{
    if (!c->rec_bin) {
        return;
    }

    c->rec_on = 0;
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
    printf("ml-pipeline: DVR stopped -> %s (pushed=%llu dropped=%llu)\n",
           c->rec_path, (unsigned long long)c->rec_pushed, (unsigned long long)c->rec_dropped);
}

/* Feed one completed composite to the encoder, zero-copy. Shares the pool GstBuffer (a ref holds
 * the pool slot until the encoder releases it), so a slow encoder is bounded: if the queue is
 * backed up we DROP this frame rather than pin pool buffers and starve the display.
 */
void rec_push(struct ctx *c, GstBuffer *buf, GstClockTime pts)
{
    GstBuffer *rb;

    if (!c->rec_on || !c->rec_src) {
        return;
    }

    if (gst_app_src_get_current_level_buffers(c->rec_src) >= 6) {
        c->rec_dropped++;
        return;
    }

    if (!c->rec_epoch_set) {
        c->rec_pts0 = pts;
        c->rec_epoch_set = TRUE;
    }

    rb = gst_buffer_ref(buf);
    GST_BUFFER_PTS(rb) = (pts != GST_CLOCK_TIME_NONE && pts >= c->rec_pts0) ? pts - c->rec_pts0 : 0;
    GST_BUFFER_DTS(rb) = GST_CLOCK_TIME_NONE;
    if (gst_app_src_push_buffer(c->rec_src, rb) != GST_FLOW_OK) {   /* consumes the ref */
        c->rec_dropped++;
    } else {
        c->rec_pushed++;
    }
}

/* Broadcast the current mode to the HUD on telemetry.sock (MLM_T_STATE). The pipeline is the source
 * of truth; the HUD reflects what we report. Sent on every change and re-asserted by state_tick, so
 * a HUD that starts late or drops a datagram reconverges.
 */
static void send_state(struct ctx *c)
{
    struct {
        struct mlm_hdr h;
        struct mlm_state s;
    } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_STATE, .flags = 0 },
        .s = { .state = c->rec_on ? MLM_STATE_RECORDING : MLM_STATE_IDLE, .reserved = 0 },
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
    struct {
        struct mlm_hdr h;
        struct mlm_cmd cmd;
    } __attribute__((packed)) rec;

    (void)cond;
    ssize_t n = recv(fd, &rec, sizeof rec, MSG_DONTWAIT);
    if (n < (ssize_t)sizeof rec || rec.h.magic != MLM_MAGIC || rec.h.type != MLM_T_CMD) {
        return G_SOURCE_CONTINUE;
    }

    if (rec.cmd.cmd == MLM_CMD_REC_TOGGLE) {
        rec_toggle(c);
    }

    return G_SOURCE_CONTINUE;
}
