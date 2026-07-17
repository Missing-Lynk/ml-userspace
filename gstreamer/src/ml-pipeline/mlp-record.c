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

    /* Zero-copy dmabuf-import is the default: the encoder imports the composite buffers
     * directly (no MMAP input pool from the wave5 device pool, no 3 MiB/frame copy). Needs
     * COMP_ALLOC buffer sizing, the 3-plane wrap in rec_push, wave5 plane data_offset
     * support (kernel patch 0013), and the gst qbuf bytesused-offset fix
     * (gstreamer/patches/0001). ML_DVR_NO_IMPORT=1 falls back to copy mode (the encoder's
     * own MMAP pool + per-frame memcpy); the 720p videoscale path always copies (scale
     * output is system memory).
     *
     * Import mode: a queued buffer pins a composite pool slot (display holds 4 of the ~10),
     * so the appsrc queue stays shallow and a lagging encoder drops frames instead of
     * starving the display. Copy mode pins nothing; keep the deeper queue.
     */
    c->rec_import = scale[0] == '\0' && getenv("ML_DVR_NO_IMPORT") == NULL;
    c->rec_qmax = c->rec_import ? 3 : 6;

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
        "! video/x-raw,format=I420,width=%d,height=%d,framerate=60/1 "
        "%s%s"
        "! v4l2h264enc %s "

        /* Fragmented MP4 (moof/mdat every 1 s): the file on disk is playable up to the last
         * complete fragment even if the process is SIGKILLed (OOM) - which no signal handler can
         * catch. A clean stop (rec_stop EOS) still finalizes normally. Costs at most ~1 s of tail.
         */
        "! h264parse ! mp4mux fragment-duration=1000 ! filesink location=%s",
        c->rec_qmax, COMP_W, COMP_H, rate, scale,
        c->rec_import ? "output-io-mode=dmabuf-import" : "", path);

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
                               "width", G_TYPE_INT, COMP_W,
                               "height", G_TYPE_INT, COMP_H,
                               "framerate", GST_TYPE_FRACTION, 60, 1, NULL);
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
                      | (c->pb_active && c->pb_rendering ? MLM_STATE_F_RENDERING : 0),
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
    char buf[sizeof(struct mlm_hdr) + sizeof(struct mlm_cmd) + MLM_PATH_MAX];
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
