/* ml-pipeline RTSP restream: serves the DVR encoder's elementary stream over gst-rtsp-server.
 *
 * The record bin's tee feeds every encoded AU here (rec_start's rtspsink branch -> rtsp_push);
 * while enabled, a shared media factory at rtsp://0.0.0.0:554/venc8/stream re-payloads them for
 * any number of clients. The mount path and default codec (H.265) match the vendor stack, so the
 * stock companion app's default URL plays unchanged. The module never produces media itself: with
 * no recording active, rec_start runs the encoder with no file branch (MLM_CMD_RTSP in on_ctrl).
 *
 * Feed discipline: parameter-set NALs (VPS/SPS/PPS) are cached from the stream even while
 * disabled - the encoder may emit them only once, in its first AU - and each (re)prepared media
 * is started with the cached parameter sets followed by the next IRAP, never mid-GOP. The
 * factory appsrc is bounded and leaky, so a stalled client can never backpressure the encoder.
 */
#include "ml-pipeline.h"
#include <gst/rtsp-server/rtsp-server.h>

/* Singleton module state (one pipeline process, one server); guarded by g_lock where the
 * record bin's streaming thread (rtsp_push) meets the server's main-context callbacks.
 */
static GstRTSPServer *g_srv;
static gint64 g_bind_retry_us;      /* monotonic deadline before the next bind attempt (0 = now) */
static guint g_srv_id;              /* server attach source id (0 = not attached) */
static guint g_pool_timer;          /* periodic session-pool cleanup source */
static GMutex g_lock;
static GstAppSrc *g_media_src;      /* the shared live media's appsrc, NULL = no prepared media */
static gboolean g_wait_key;         /* hold AUs until the next IRAP (fresh media / re-sync) */
static int g_h264 = -1;             /* codec latch: -1 unset, else ML_DVR_CODEC resolution */
static guint64 g_pushed, g_dropped;

/* Parameter-set cache. One slot per kind (H.265 VPS/SPS/PPS, H.264 SPS/PPS in slots 1 and 2),
 * because the encoder may deliver them AU-aligned in one buffer or one NAL per buffer: a single
 * cached prefix keeps only whichever kind arrived last, and a payloader fed one PPS never
 * negotiates caps, so the media never leaves PREPARING and every DESCRIBE times out.
 * g_params is their concatenation, rebuilt whenever a slot changes, and is what gets pushed.
 */
#define PS_SLOTS 3

struct ps_range {
    gsize off, len;                 /* within the scanned buffer; len 0 = this kind not present */
};

static guint8 *g_ps[PS_SLOTS];
static gsize g_ps_len[PS_SLOTS];
static GstBuffer *g_params;         /* cached parameter-set prefix, byte-stream */

/* Same codec resolution as rec_start: ML_DVR_CODEC=h264/avc selects H.264, else H.265. */
static gboolean codec_is_h264(void)
{
    const char *codec = getenv("ML_DVR_CODEC");

    return codec && (strcasecmp(codec, "h264") == 0 || strcasecmp(codec, "avc") == 0);
}

/* Scan a byte-stream buffer up to its first VCL NAL. Reports whether the picture is an IRAP
 * (H.265 BLA/IDR/CRA, H.264 IDR), the byte length of a leading parameter-set prefix (0 = the
 * buffer does not open with parameter sets), and the extent of each parameter-set kind found
 * (@p ps, indexed by PS slot; len 0 = absent). Parameter sets precede the picture, so the scan
 * stops at the first VCL NAL and that NAL decides the key property.
 */
static void au_scan(const guint8 *p, gsize n, gboolean h264, gboolean *key, gsize *param_len,
                    struct ps_range ps[PS_SLOTS])
{
    gsize i = 0, prev_start = 0;
    gboolean in_prefix = TRUE;
    int prev_slot = -1;

    *key = FALSE;
    *param_len = 0;
    for (int s = 0; s < PS_SLOTS; s++) {
        ps[s].len = 0;
    }

    while (i + 3 < n) {
        /* next 00 00 01 start code (a 4-byte 00 00 00 01 lands here via its inner 3 bytes) */
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1) {
            i++;
            continue;
        }

        /* start of this NAL including its (3- or 4-byte) start code */
        gsize nal_start = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
        guint8 nal_header = p[i + 3];
        gboolean vcl, irap;
        int slot;

        if (h264) {
            guint8 nal_type = nal_header & 0x1f;
            slot = nal_type == 7 ? 1 : (nal_type == 8 ? 2 : -1);
            vcl = nal_type >= 1 && nal_type <= 5;
            irap = nal_type == 5;
        } else {
            guint8 nal_type = (nal_header >> 1) & 0x3f;
            slot = nal_type >= 32 && nal_type <= 34 ? (int)(nal_type - 32) : -1;
            vcl = nal_type < 32;
            irap = nal_type >= 16 && nal_type <= 21;
        }

        /* the pending parameter-set NAL ends where this one begins */
        if (prev_slot >= 0) {
            ps[prev_slot].off = prev_start;
            ps[prev_slot].len = nal_start - prev_start;
            prev_slot = -1;
        }

        if (in_prefix && slot < 0) {
            in_prefix = FALSE;
            /* the prefix ends where the first non-parameter-set NAL starts */
            *param_len = nal_start;
        }

        if (vcl) {
            *key = irap;
            return;
        }

        prev_slot = slot;
        prev_start = nal_start;
        i += 4;
    }

    /* ran off the end with no VCL NAL: close the pending parameter set against the buffer end */
    if (prev_slot >= 0) {
        ps[prev_slot].off = prev_start;
        ps[prev_slot].len = n - prev_start;
    }

    if (in_prefix) {
        *param_len = n;
    }
}

/* Rebuild the pushed prefix from the per-kind cache, in VPS/SPS/PPS order. Call under g_lock. */
static void ps_rebuild(void)
{
    gsize total = 0, at = 0;
    guint8 *flat;

    for (int s = 0; s < PS_SLOTS; s++) {
        total += g_ps_len[s];
    }

    if (g_params) {
        gst_buffer_unref(g_params);
        g_params = NULL;
    }

    if (total == 0) {
        return;
    }

    flat = g_malloc(total);
    for (int s = 0; s < PS_SLOTS; s++) {
        if (g_ps_len[s] > 0) {
            memcpy(flat + at, g_ps[s], g_ps_len[s]);
            at += g_ps_len[s];
        }
    }

    g_params = gst_buffer_new_memdup(flat, total);
    g_free(flat);
}

/* Kick the encoder for an IRAP so a freshly prepared media (or a post-rebuild resume) does not
 * wait out the GOP. Sent into the record bin (travels upstream from its sinks to the encoder);
 * harmless if the driver ignores V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME.
 */
static void rtsp_request_key(struct ctx *c)
{
    if (c->rec_bin) {
        gst_element_send_event(c->rec_bin,
                               gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                                           TRUE, 0));
    }
}

/* The shared media went away (last client left / teardown): drop our feed handle. Only when
 * the handle still belongs to that media's appsrc (@p u): session-timeout cleanup unprepares
 * without the media lock, so a dying media's deferred signal can land after a new client's
 * media has already been configured, and must not clear the new feed. A replaced appsrc's
 * ref was already released at replacement time (on_media_configure), so a stale signal has
 * nothing to do.
 */
static void on_media_unprepared(GstRTSPMedia *media, gpointer u)
{
    (void)media;
    g_mutex_lock(&g_lock);
    if (g_media_src != NULL && g_media_src == GST_APP_SRC(u)) {
        gst_object_unref(g_media_src);
        g_media_src = NULL;
    }

    g_mutex_unlock(&g_lock);
}

/* A client prepared the shared media: grab its appsrc as the feed target and gate the feed on
 * the next IRAP (preceded by the cached parameter sets), then ask the encoder for one.
 */
static void on_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer u)
{
    struct ctx *c = u;
    GstElement *bin = gst_rtsp_media_get_element(media);
    GstElement *src = gst_bin_get_by_name(GST_BIN(bin), "rtspsrc");

    (void)factory;
    gst_object_unref(bin);
    if (!src) {
        return;
    }

    g_mutex_lock(&g_lock);
    if (g_media_src) {
        gst_object_unref(g_media_src);
    }

    /* takes the get_by_name ref; released on this media's unprepared or on replacement */
    g_media_src = GST_APP_SRC(src);
    g_wait_key = TRUE;
    g_mutex_unlock(&g_lock);

    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), src);
    rtsp_request_key(c);
}

/* Expire sessions whose clients vanished without TEARDOWN (battery pull, app kill). */
static gboolean pool_cleanup(gpointer u)
{
    GstRTSPSessionPool *pool = gst_rtsp_server_get_session_pool((GstRTSPServer *)u);

    gst_rtsp_session_pool_cleanup(pool);
    g_object_unref(pool);

    return G_SOURCE_CONTINUE;
}

static GstRTSPFilterResult client_kick(GstRTSPServer *srv, GstRTSPClient *client, gpointer u)
{
    (void)srv;
    (void)client;
    (void)u;

    return GST_RTSP_FILTER_REMOVE;
}

/* One-time server bring-up: mount the vendor-compatible path with a shared live factory whose
 * codec matches the DVR encoder. Port 554 needs root (ml-pipeline runs as root); ML_RTSP_PORT
 * overrides for bench runs.
 */
static gboolean rtsp_server_up(struct ctx *c)
{
    const char *port = getenv("ML_RTSP_PORT") ? getenv("ML_RTSP_PORT") : "554";
    const char *cname = codec_is_h264() ? "h264" : "h265";
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;
    gchar *launch;

    if (g_srv) {
        return TRUE;
    }

    /* A failed bind (port held by another stack) backs off: the HUD re-asserts the setting
     * every second while the reported state diverges, and re-attempting the whole server
     * construct/attach/destroy cycle at that rate is pure churn.
     */
    if (g_bind_retry_us != 0 && g_get_monotonic_time() < g_bind_retry_us) {
        return FALSE;
    }

    g_srv = gst_rtsp_server_new();
    gst_rtsp_server_set_service(g_srv, port);

    /* do-timestamp restamps AUs with the media's own running time: the encoder PTS rebases to 0
     * on every record-bin rebuild and would run backwards mid-session. Bounded + leaky so a
     * stalled internal branch drops (self-heals at the next IRAP) instead of growing.
     */
    launch = g_strdup_printf(
        "( appsrc name=rtspsrc is-live=true format=time do-timestamp=true "
        "max-buffers=16 leaky-type=downstream block=false "
        "caps=video/x-%s,stream-format=byte-stream,alignment=au "
        "! %sparse ! rtp%spay name=pay0 pt=96 config-interval=-1 )",
        cname, cname, cname);

    factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory, launch);
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_free(launch);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), c);

    mounts = gst_rtsp_server_get_mount_points(g_srv);
    gst_rtsp_mount_points_add_factory(mounts, "/venc8/stream", factory);   /* consumes factory */
    g_object_unref(mounts);

    g_srv_id = gst_rtsp_server_attach(g_srv, NULL);
    if (g_srv_id == 0) {
        fprintf(stderr, "ml-pipeline: RTSP server failed to bind :%s (next attempt in 30 s)\n", port);
        g_object_unref(g_srv);
        g_srv = NULL;
        g_bind_retry_us = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;

        return FALSE;
    }

    g_pool_timer = g_timeout_add_seconds(2, pool_cleanup, g_srv);
    printf("ml-pipeline: RTSP serving rtsp://0.0.0.0:%s/venc8/stream (%s)\n", port, cname);

    return TRUE;
}

/* MLM_CMD_RTSP: enable/disable the restream. The enable path is a reconcile, not a latch:
 * MLM_STATE_F_RTSP reports the ACTUAL state (enabled AND encoder running, see send_state), the
 * HUD re-asserts the setting whenever that diverges from it, and every assert re-attempts
 * whatever is missing (server bind, encoder start). A failed start or a bus-error teardown
 * therefore self-heals on the next assert instead of leaving a dead stream advertised as on.
 * Playback owns the wave5 budget, so the encoder is left down while a file is playing; the
 * playback exit path (resume_live) restores it. The server itself stays up once created: an
 * idle bound socket costs nothing.
 */
void rtsp_set(struct ctx *c, gboolean on)
{
    if (on) {
        if (!rtsp_server_up(c)) {
            return;
        }

        c->rtsp_on = 1;
        if (!c->enc_on && !c->pb_active) {
            /* stream-only: encoder up, no file branch */
            rec_start(c, NULL);
        }
    } else {
        if (!c->rtsp_on) {
            return;
        }

        c->rtsp_on = 0;
        g_mutex_lock(&g_lock);
        g_wait_key = TRUE;
        g_mutex_unlock(&g_lock);
        if (g_srv) {
            /* REMOVE-filtered clients are closed by the server; the returned list only
             * carries entries for GST_RTSP_FILTER_REF results, so it is empty here - free
             * defensively.
             */
            g_list_free_full(gst_rtsp_server_client_filter(g_srv, client_kick, NULL),
                             g_object_unref);
        }

        if (c->enc_on && !c->rec_on) {
            /* the encoder only ran for the stream */
            rec_stop(c);
        }

        printf("ml-pipeline: RTSP disabled (pushed=%llu dropped=%llu)\n",
               (unsigned long long)g_pushed, (unsigned long long)g_dropped);
    }
}

/* One encoded AU from the record bin's tee (rec_start's rtspsink branch; record-bin streaming
 * thread). Always scans the prefix - the parameter-set cache must be warm before the first
 * client, and the encoder may emit VPS/SPS/PPS only once - then, while enabled and a media is
 * prepared, feeds the AU (IRAP-gated, parameter sets first) into the media's appsrc.
 */
void rtsp_push(struct ctx *c, GstBuffer *au)
{
    GstMapInfo map;
    struct ps_range ps[PS_SLOTS];
    gboolean key = FALSE;
    gsize param_len = 0;

    if (g_h264 < 0) {
        g_h264 = codec_is_h264() ? 1 : 0;
    }

    if (!gst_buffer_map(au, &map, GST_MAP_READ)) {
        return;
    }

    au_scan(map.data, map.size, g_h264 == 1, &key, &param_len, ps);
    {
        gboolean changed = FALSE;

        g_mutex_lock(&g_lock);
        for (int s = 0; s < PS_SLOTS; s++) {
            if (ps[s].len == 0) {
                continue;
            }

            g_free(g_ps[s]);
            g_ps[s] = g_memdup2(map.data + ps[s].off, ps[s].len);
            g_ps_len[s] = ps[s].len;
            changed = TRUE;
        }

        if (changed) {
            ps_rebuild();
        }

        g_mutex_unlock(&g_lock);
    }

    gst_buffer_unmap(au, &map);
    if (!c->rtsp_on) {
        return;
    }

    g_mutex_lock(&g_lock);
    if (!g_media_src) {
        g_mutex_unlock(&g_lock);
        return;
    }

    if (g_wait_key) {
        if (!key) {
            g_dropped++;
            g_mutex_unlock(&g_lock);
            return;
        }

        g_wait_key = FALSE;
    }

    /* Lead every IRAP with the cached parameter sets, unless the AU carries its own. The
     * payloader has no caps, and so the media no SDP, until the parser has seen VPS/SPS/PPS;
     * the encoder emits them once, in its first AU; and the media's appsrc is bounded and
     * leaky, so a set pushed while the media is still being prepared can be dropped before
     * the parser consumes it. Repeating them on each IRAP bounds that loss to one GOP
     * instead of the life of the media.
     */
    if (key && param_len == 0 && g_params) {
        gst_app_src_push_buffer(g_media_src, gst_buffer_copy(g_params));
    }

    /* metadata-only copy (shares the AU's memory); PTS cleared so do-timestamp restamps */
    {
        GstBuffer *copy = gst_buffer_copy(au);

        GST_BUFFER_PTS(copy) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
        if (gst_app_src_push_buffer(g_media_src, copy) == GST_FLOW_OK) {
            g_pushed++;
        } else {
            g_dropped++;
        }
    }

    g_mutex_unlock(&g_lock);
}
