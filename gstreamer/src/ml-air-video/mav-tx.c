/**
 * @file mav-tx.c
 * @brief VPH framing and the :10001 datagrams.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/** Dump, count and transmit one access unit. Shared by the GStreamer and direct V4L2 backends. */
void air_emit_au(struct air_tile *tile, const guint8 *data, size_t size,
                 guint32 frame_id, guint32 is_idr)
{
    guint32 ts_ms;
    size_t len;

    /* A live rate change is adopted here rather than in the setter so the timestamp base is
     * frozen against a frame_id this tile has actually emitted. TimeStap counts elapsed
     * milliseconds, so the span already sent keeps the rate it was sent at and only the span
     * from here on uses the new one; recomputing the whole span at the new rate would step the
     * timestamp by the length of the stream so far. */
    if (tile->fps_pending > 0) {
        tile->ts_base_ms += (guint32)((guint64)(frame_id - tile->ts_base_id) * 1000u /
                                   (guint32)tile->fps);
        tile->ts_base_id = frame_id;
        tile->fps = tile->fps_pending;
        tile->fps_pending = 0;
    }

    ts_ms = tile->ts_base_ms + (guint32)((guint64)(frame_id - tile->ts_base_id) * 1000u /
                                      (guint32)tile->fps);

    if (tile->dumpfd >= 0) {
        if (write(tile->dumpfd, data, size) != (ssize_t)size) {
            /* best-effort capture */
        }
    }

    if (size > tile->tx_maxlen) {
        tile->tx_maxlen = (guint32)size;
    }

    if (!g_notx) {
        len = vph_build(tile->txbuf, AIR_TX_MAX, (guint32)tile->chn, is_idr, frame_id, ts_ms,
                        tile->resolution, data, (guint32)size);
        if (len == 0) {
            tile->tx_oversize++;
        } else if (sendto(tile->sock, tile->txbuf, len, MSG_DONTWAIT,
                          (struct sockaddr *)&tile->dst, sizeof tile->dst) != (ssize_t)len) {
            tile->tx_error++;
            tile->tx_errno = errno;
        } else {
            tile->sent++;
        }
    } else {
        tile->sent++;
    }

    tile->done++;
    tile->bytes += size;
}

/** Encoder-output callback: frame one tile access unit and send it to the goggle. */
GstFlowReturn air_on_enc(GstAppSink *sink, gpointer user)
{
    struct air_tile *tile = user;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *buf;
    GstMapInfo map;
    guint32 frame_id;

    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    buf = gst_sample_get_buffer(sample);
    if (buf == NULL || !gst_buffer_map(buf, &map, GST_MAP_READ)) {
        tile->lost++;
        gst_sample_unref(sample);

        return GST_FLOW_OK;
    }

    if (GST_BUFFER_PTS_IS_VALID(buf)) {
        frame_id = (guint32)gst_util_uint64_scale(GST_BUFFER_PTS(buf), tile->fps, GST_SECOND);
    } else {
        frame_id = 0;
    }

    air_emit_au(tile, map.data, map.size, frame_id,
                GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT) ? 0 : 1);

    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}
