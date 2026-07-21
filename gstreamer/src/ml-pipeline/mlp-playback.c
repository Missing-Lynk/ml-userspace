/*
 * mlp-playback - file playback that preempts the live RF stream. Driven by ctrl.sock commands
 * (MLM_CMD_PLAY/PAUSE/RESUME/SEEK/STOP). PLAY tears the RF decode graph down to NULL and
 * re-inits the display sink for single-stream scanout, then decodes the file; STOP or end-of-
 * stream rebuilds the RF graph and returns to live. The swap uses the proven full-teardown
 * pattern (rf_decode_stop + drm_disp_shutdown, then rebuild) so only one wave5 decode graph is
 * ever live. Progress is published to the HUD as MLM_T_STATE at ~5 Hz for the menu scrubber.
 */
#include "ml-pipeline.h"

/* Query position + duration and re-publish the playback state to the HUD. */
static gboolean pb_pos_tick(gpointer u)
{
    struct ctx *c = u;
    gint64 pos = 0, dur = 0;

    if (!c->pb_active || !c->pb_pipe) {
        return G_SOURCE_REMOVE;
    }

    if (gst_element_query_position(c->pb_pipe, GST_FORMAT_TIME, &pos) && pos >= 0) {
        c->pb_pos_ms = pos / GST_MSECOND;
    }

    /* Keep the largest duration seen: some MP4s (and any fragmented recording) only report the
     * length progressively, so a raw query can track the current position - latch the max instead
     * so the total never appears to grow with playback.
     */
    if (gst_element_query_duration(c->pb_pipe, GST_FORMAT_TIME, &dur) && dur > 0) {
        guint32 d = dur / GST_MSECOND;
        if (d > c->pb_dur_ms) {
            c->pb_dur_ms = d;
        }
    }

    send_state(c);
    return G_SOURCE_CONTINUE;
}

/* pb_pipe bus: end-of-stream or a fatal error returns to the live stream. Runs on the main loop. */
static gboolean on_pb_bus(GstBus *bus, GstMessage *msg, gpointer u)
{
    struct ctx *c = u;

    (void)bus;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS: {
            printf("ml-pipeline: playback EOS -> hold last frame\n");
            playback_end(c);
        } break;

        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            fprintf(stderr, "ml-pipeline: playback ERROR %s (%s) -> live\n",
                    err->message, dbg ? dbg : "");
            g_clear_error(&err);
            g_free(dbg);
            playback_stop(c, TRUE);
        } break;

        default: {}
    }

    return TRUE;
}

/* Compose the decode graph for a clip: MP4 (DVR H.264) is demuxed by qtdemux; a raw Annex-B
 * elementary stream is parsed directly. The wave5 decoder exports dmabuf so on_file_sample can
 * scan each frame out zero-copy on the primary CRTC.
 */
static gchar *pb_desc(const char *path)
{
    const char *dot = strrchr(path, '.');
    gboolean mp4 = dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".mov"));
    gboolean h265 = dot && (!strcasecmp(dot, ".h265") || !strcasecmp(dot, ".hevc"));

    if (mp4) {
        /* DVR recordings are H.264 in MP4; qtdemux's dynamic pad links to h264parse via parse. */
        return g_strdup_printf(
            "filesrc location=\"%s\" ! qtdemux ! h264parse ! v4l2h264dec capture-io-mode=dmabuf "
            "name=dec ! appsink name=sink", path);
    }

    return g_strdup_printf(
        "filesrc location=\"%s\" ! %sparse ! v4l2%sdec capture-io-mode=dmabuf name=dec "
        "! appsink name=sink", path, h265 ? "h265" : "h264", h265 ? "h265" : "h264");
}

/* Whether a clip decodes at the panel's native geometry, read from the MP4's avc1 sample entry. A
 * native clip scans out zero-copy (single mode); only a sub-panel clip (e.g. a 720p DVR recording)
 * needs the ar_scaler. Walks the top-level atoms to moov (front or after the mdat), reads the moov
 * body, and finds the avc1 box's width/height (the 2-byte fields 28 bytes past the 'avc1' fourcc).
 * Defaults to FALSE (scale) when the size cannot be determined: the scaler handles any size, so an
 * unparsed clip still plays (a native one just takes the scaler's unity path instead of zero-copy).
 */
static gboolean clip_is_native_res(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return FALSE;
    }

    gboolean native = FALSE;
    for (;;) {
        unsigned char hdr[16];
        if (fread(hdr, 1, 8, f) != 8) {
            break;
        }

        guint64 size = ((guint64) hdr[0] << 24) | ((guint64) hdr[1] << 16) |
                       ((guint64) hdr[2] << 8) | hdr[3];
        int header_len = 8;
        if (size == 1) {
            if (fread(hdr + 8, 1, 8, f) != 8) {
                break;
            }

            size = 0;
            for (int i = 0; i < 8; i++) {
                size = (size << 8) | hdr[8 + i];
            }

            header_len = 16;
        }

        if (size < (guint64) header_len) {
            break;
        }

        if (memcmp(hdr + 4, "moov", 4) == 0) {
            guint64 body = size - header_len;
            if (body > 8 * 1024 * 1024) {
                body = 8 * 1024 * 1024;   /* a moov is small; cap the read regardless */
            }

            unsigned char *m = g_malloc(body);
            if (fread(m, 1, body, f) == body) {
                for (guint64 i = 0; i + 32 <= body; i++) {
                    if (memcmp(m + i, "avc1", 4) == 0) {
                        int w = (m[i + 28] << 8) | m[i + 29];
                        int h = (m[i + 30] << 8) | m[i + 31];
                        native = (w == COMP_W && h == COMP_H);
                        break;
                    }
                }
            }

            g_free(m);
            break;
        }

        if (fseeko(f, (off_t) (size - header_len), SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(f);
    return native;
}

/* Return to the live RF stream: restore the RF display mode and rebuild the decode graph. */
static void resume_live(struct ctx *c)
{
    c->pb_scale = FALSE;   /* live uses the RF display path, never the playback scaler */
    c->single = FALSE;
    c->planes_on = c->rf_planes;
    if (drm_disp_init(c)) {
        fprintf(stderr, "ml-pipeline: display re-init failed returning to live\n");
    }

    rf_decode_start(c);
}

void playback_play(struct ctx *c, const char *path)
{
    GError *err = NULL;

    if (c->pb_active) {
        playback_stop(c, FALSE);   /* switching clips: drop the current one, stay off-live */
    } else {
        drm_disp_shutdown(c);      /* leaving live: tear the RF graph + its display mode down */
        rf_decode_stop(c);
    }

    gchar *desc = pb_desc(path);
    c->pb_pipe = gst_parse_launch(desc, &err);
    g_free(desc);
    if (!c->pb_pipe) {
        fprintf(stderr, "ml-pipeline: playback parse (%s): %s\n", path, err ? err->message : "?");
        if (err) {
            g_error_free(err);
        }

        /* never sit dark: fall back to the live stream */
        resume_live(c);
        send_state(c);

        return;
    }

    GstElement *dec = gst_bin_get_by_name(GST_BIN(c->pb_pipe), "dec");
    GstPad *pad = gst_element_get_static_pad(dec, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_frame, c, NULL);
    gst_object_unref(pad);
    gst_object_unref(dec);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(c->pb_pipe), "sink");
    g_object_set(sink, "emit-signals", FALSE, "sync", TRUE, "max-buffers", 2, "drop", FALSE, NULL);
    GstAppSinkCallbacks cb = { .new_sample = on_file_sample };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, c, NULL);

    /* Advertise GstVideoMeta on the allocation query, else v4l2videodec normalize-COPIES the
     * decoded frame into system memory (fd = -1) and the zero-copy scanout drops every frame.
     */
    GstPad *sp = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, asink_alloc_probe, NULL, NULL);
    gst_object_unref(sp);
    gst_object_unref(sink);

    GstBus *bus = gst_element_get_bus(c->pb_pipe);
    c->pb_bus_watch = gst_bus_add_watch(bus, on_pb_bus, c);
    gst_object_unref(bus);

    /* Display mode. A native-resolution clip scans out zero-copy (single mode), exactly as before -
     * that path is proven and cheapest. A sub-panel clip (e.g. a 720p DVR recording) cannot scan its
     * own FB out on the primary CRTC (the VO overlay planes are 1:1, DRM_PLANE_NO_SCALING), so with
     * the ar_scaler present it is upscaled per frame into a composite pool buffer and rides the
     * composite page-flip path (pb_scale). Without the scaler a sub-panel clip cannot display.
     */
    gboolean scale = !clip_is_native_res(path);
    if (scale && c->scaler_fd < 0) {
        c->scaler_fd = open("/dev/arscaler", O_RDWR | O_CLOEXEC);
    }

    c->planes_on = FALSE;
    if (scale && c->scaler_fd >= 0 && c->comp_n > 0) {
        c->single = FALSE;   /* composite scanout: the scaled frame rides a comp pool buffer + FB */
        c->pb_scale = TRUE;
    } else {
        c->single = TRUE;    /* native clip (or no scaler/pool): zero-copy single scanout */
        c->pb_scale = FALSE;
    }

    if (drm_disp_init(c)) {
        fprintf(stderr, "ml-pipeline: playback display init failed\n");
    }

    gst_element_set_state(c->pb_pipe, GST_STATE_PLAYING);
    c->pb_active = TRUE;
    c->pb_paused = FALSE;
    c->pb_ended = FALSE;
    c->pb_rendering = FALSE;
    c->pb_pos_ms = 0;
    c->pb_dur_ms = 0;
    c->pb_timer = g_timeout_add(200, pb_pos_tick, c);

    printf("ml-pipeline: playback -> %s (%s)\n", path, c->pb_scale ? "scaled" : "native");
    send_state(c);
}

/* Set the play speed as a single rate seek (no periodic seeking): rate > 1 fast-forwards, rate < 0
 * rewinds. Trickmode key-units is requested for any non-1x speed so the wave5 decoder only has to
 * emit keyframes and never falls behind at 4x/8x. A forward seek runs from the current position to
 * end; a reverse seek runs from the start up to the current position (the GStreamer reverse idiom).
 */
void playback_set_speed(struct ctx *c, gint32 speed)
{
    gint64 pos = 0;
    GstSeekFlags flags = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT;

    if (!c->pb_active || !c->pb_pipe) {
        return;
    }

    if (speed == 0) {
        speed = 1;
    }

    gst_element_query_position(c->pb_pipe, GST_FORMAT_TIME, &pos);
    if (speed != 1) {
        flags |= GST_SEEK_FLAG_TRICKMODE | GST_SEEK_FLAG_TRICKMODE_KEY_UNITS;
    }

    if (speed > 0) {
        gst_element_seek(c->pb_pipe, (gdouble)speed, GST_FORMAT_TIME, flags,
                         GST_SEEK_TYPE_SET, pos, GST_SEEK_TYPE_SET, -1);
    } else {
        gst_element_seek(c->pb_pipe, (gdouble)speed, GST_FORMAT_TIME, flags,
                         GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, pos);
    }
    /* speed selection always plays: leaving a paused hold resumes into the scrub */
    gst_element_set_state(c->pb_pipe, GST_STATE_PLAYING);
    c->pb_paused = FALSE;
    send_state(c);
}

/* End of clip: hold the last decoded frame on screen and wait for the user (replay or exit). The
 * decode graph and display sink stay up - the last sample is still pinned by the display mailbox, so
 * pausing (not tearing down) keeps that frame lit. Position is pinned to the duration so the HUD
 * scrubber sits at the end, and the ENDED flag tells it to show the stop icon.
 */
void playback_end(struct ctx *c)
{
    if (!c->pb_active || c->pb_ended) {
        return;
    }

    if (c->pb_timer) {
        g_source_remove(c->pb_timer);   /* position is fixed at the end; stop polling it */
        c->pb_timer = 0;
    }

    gst_element_set_state(c->pb_pipe, GST_STATE_PAUSED);
    c->pb_ended = TRUE;
    c->pb_paused = TRUE;
    if (c->pb_dur_ms > 0) {
        c->pb_pos_ms = c->pb_dur_ms;
    }

    send_state(c);
}

void playback_stop(struct ctx *c, gboolean want_live)
{
    if (!c->pb_active) {
        return;
    }

    if (c->pb_timer) {
        g_source_remove(c->pb_timer);
        c->pb_timer = 0;
    }

    if (c->pb_bus_watch) {
        g_source_remove(c->pb_bus_watch);
        c->pb_bus_watch = 0;
    }

    /* scanout down before the decoder frees its buffers (single mode releases the tfb FBs) */
    drm_disp_shutdown(c);
    gst_element_set_state(c->pb_pipe, GST_STATE_NULL);
    gst_element_get_state(c->pb_pipe, NULL, NULL, GST_CLOCK_TIME_NONE);
    gst_object_unref(c->pb_pipe);
    c->pb_pipe = NULL;
    c->pb_active = FALSE;
    c->pb_paused = FALSE;
    c->pb_ended = FALSE;
    c->pb_pos_ms = 0;
    c->pb_dur_ms = 0;

    if (want_live) {
        resume_live(c);
        send_state(c);
    }
}

void playback_pause(struct ctx *c)
{
    if (!c->pb_active || c->pb_paused) {
        return;
    }

    gst_element_set_state(c->pb_pipe, GST_STATE_PAUSED);
    c->pb_paused = TRUE;
    send_state(c);
}

void playback_resume(struct ctx *c)
{
    if (!c->pb_active || !c->pb_paused) {
        return;
    }

    gst_element_set_state(c->pb_pipe, GST_STATE_PLAYING);
    c->pb_paused = FALSE;
    send_state(c);
}

void playback_seek(struct ctx *c, guint32 permille)
{
    gint64 dur = 0;

    if (!c->pb_active || !c->pb_pipe) {
        return;
    }

    if (!gst_element_query_duration(c->pb_pipe, GST_FORMAT_TIME, &dur) || dur <= 0) {
        return;
    }

    if (permille > 1000) {
        permille = 1000;
    }

    /* NB: a seek issues a FLUSH to the wave5 decoder. Flushing a live wave5 decoder is a known
     * stall risk (the RF path never seeks by design), so this file-playback seek is the one
     * control that still needs on-hardware validation; if it wedges, switch to a rebuild-at-
     * position seek (pb_pipe -> NULL, rebuild, seek while PAUSED, then PLAYING).
     */
    gst_element_seek_simple(c->pb_pipe, GST_FORMAT_TIME,
                            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                            gst_util_uint64_scale(dur, permille, 1000));
    send_state(c);
}
