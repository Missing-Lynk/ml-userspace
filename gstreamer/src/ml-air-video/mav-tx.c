/**
 * @file mav-tx.c
 * @brief VPH framing and the :10001 datagrams.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/* Arming state that spans access units. File scope rather than function statics so a session reset
 * can clear it: a second receiver has to go through the same warm-up the first one did. */
static guint32 g_arm_dropped;   /* access units held while waiting for a substantive IDR */
static guint32 g_arm_retries;   /* keyframes asked for because the IDR would not fit */

void air_tx_session_reset(void)
{
    /* Unarmed means there is no session to end. A receiver repeats its params request while
     * establishing, so those repeats land here and do nothing: one reset per session. */
    if (!g_atomic_int_get(&g_tx_armed)) {
        return;
    }

    /* Request only. This runs on the control socket; air_emit_au performs the reset, so the tile
     * transmit state it touches has a single writing thread. Writing it from here lands between
     * the two tiles' emits and leaves them one FrameId apart, which a receiver that pairs tiles by
     * FrameId cannot assemble (measured: chn 0 Id 5542, chn 1 Id 5543, 27 keyframes demanded).
     *
     * Flags before arming, so a tile reaching air_emit_au between the two sees an unarmed transmit
     * with a reset pending, never an armed transmit whose reset was already consumed. */
    for (int i = 0; i < AIR_NCHN; i++) {
        g_atomic_int_set(&g_tile[i].reset_pending, 1);
    } /* for each tile */

    g_atomic_int_set(&g_tx_armed, 0);

    g_printerr(TAG " session reset requested: transmit held, timestamp rebases on the next frame\n");
}

/** Force a keyframe on every active tile, so all tiles restart on a matching FrameId. */
static void air_tx_force_keyframes(const char *why)
{
    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0) {
            air_enc_set_int(&g_tile[i], V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, why);
        }
    } /* for each tile */
}

/**
 * @brief Ask this tile's encoder for another keyframe, because the last IDR could not be sent.
 *
 * Bounded, and spaced by AIR_TX_FORCE_GAP frames: two forces inside the encoder's arm/restore
 * window latch its intra period at 1 and turn the whole instance all-intra. Forces this instance
 * only, so the other tile's encoder is untouched.
 */
static void air_request_smaller_idr(struct air_tile *tile, guint32 frame_id, size_t size,
                                    const char *why)
{
    if (tile->idr_retries > AIR_TX_ARM_MAX_RETRY) {
        return;
    }

    if (tile->idr_retries == AIR_TX_ARM_MAX_RETRY) {
        tile->idr_retries++;
        g_printerr(TAG " tile %d: no IDR has been sendable in %u attempts (last %zu B); this tile"
                   " will show nothing. Raise ML_AIR_MINQP (vendor uses 15) or lower"
                   " ML_AIR_BITRATE.\n", tile->chn, AIR_TX_ARM_MAX_RETRY, size);

        return;
    }

    if (frame_id - tile->idr_forced_at < AIR_TX_FORCE_GAP) {
        return;
    }

    tile->idr_retries++;
    tile->idr_forced_at = frame_id;
    g_printerr(TAG " tile %d: asking for another IDR (%u/%u)\n",
               tile->chn, tile->idr_retries, AIR_TX_ARM_MAX_RETRY);
    air_enc_set_int(tile, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, why);
}

/** Dump, count and transmit one access unit. Shared by the GStreamer and direct V4L2 backends. */
void air_emit_au(struct air_tile *tile, const guint8 *data, size_t size,
                 guint32 frame_id, guint32 is_idr)
{
    /* The transmit path is one access unit per datagram, as the vendor's is, so anything past this
     * cannot go out at all. vph_build returns 0 rather than truncating. */
    const size_t max_es = AIR_TX_MAX - AIR_VPH_HEADER - AIR_VPH_TAIL;
    guint32 ts_ms;
    size_t len;

    /* Adopt a pending session reset (air_tx_session_reset). Performed here so these fields have a
     * single writing thread and the timestamp base is a frame_id this tile has reached.
     *
     * A receiver that power-cycles restarts its clock at zero. Timestamps carried over from the
     * previous session arrive in its future - measured 31 s - and its constant-frame-rate stage
     * discards every frame while all of its decode counters stay healthy.
     *
     * enc_seq is untouched, so FrameId continues across sessions: the receiver logs
     * "Stream Not Continue" and accepts the IDR. Restarting FrameId needs a frame identity shared
     * by both tiles, which per-tile counters do not provide - plans/vendor-goggle-interop.md. */
    if (g_atomic_int_get(&tile->reset_pending)) {
        g_atomic_int_set(&tile->reset_pending, 0);

        tile->ts_base_id = frame_id;
        tile->ts_base_ms = 0;
        tile->idr_sent = 0;
        tile->idr_retries = 0;
        tile->idr_forced_at = 0;

        /* Arming is global and driven by tile 0 alone, so it is cleared by tile 0 alone: the other
         * tile clearing it would be a second thread writing counters this one owns. */
        if (tile->chn == 0) {
            g_arm_dropped = 0;
            g_arm_retries = 0;
        }

        g_printerr(TAG " tile %d: session reset at frame %u, timestamp rebased to 0\n",
                   tile->chn, frame_id);
    }

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

    /* Splice in the per-access-unit PREFIX_SEI. A vendor goggle calls AR_MPI_VDEC_GetUserData on
     * every decoded frame and resets its receive pipeline when the access unit carried none; the
     * panel then stays black across an air-unit reboot. Placed immediately before the first VCL
     * NAL, leaving the access-unit head the receiver byte-checks untouched, and ahead of every size
     * check so the SEI counts against the datagram limit. userspace/docs/rf-video-downlink.md.
     *
     * QP is the configured value: V4L2 reports no per-frame QP on this encoder. Whether the
     * receiver reads the field is not established, only that the call must succeed. */
    if (!g_nosei) {
        uint8_t sei[VPH_SEI_MAX];
        size_t sei_len = vph_sei_build(sei, sizeof sei, (guint32)tile->chn, frame_id,
                                       ts_ms * 1000u,
                                       (guint32)(tile->enc_bitrate > 0 ? tile->enc_bitrate / 1000
                                                                       : 0),
                                       (guint32)tile->enc_qp);
        size_t vcl = vph_first_vcl_offset(data, size);

        /* vcl == size means no slice NAL, so there is no picture to attach user data to and an
         * appended SEI would describe the next one. That and any build or capacity failure make
         * the access unit unsendable: transmitting one without an SEI stops a vendor receiver. */
        if (sei_len == 0 || vcl >= size || size + sei_len > sizeof tile->aubuf) {
            /* The common case is an access unit already past the datagram limit, where the SEI is
             * a casualty of the size rather than the cause. */
            const char *why = (size + sei_len > max_es) ? "over the datagram limit"
                            : (vcl >= size)             ? "no slice NAL to attach it to"
                            : (sei_len == 0)            ? "the SEI would not build"
                                                        : "it does not fit the assembly buffer";

            tile->sei_skipped++;
            g_printerr(TAG " tile %d: dropping a %zu B %s access unit, %s"
                       " (%" G_GUINT64_FORMAT " so far)\n", tile->chn, size,
                       is_idr ? "IDR" : "P", why, tile->sei_skipped);

            if (is_idr && !tile->idr_sent) {
                air_request_smaller_idr(tile, frame_id, size, "sei retry keyframe");
            }

            tile->done++;
            tile->bytes += size;

            return;
        }

        memcpy(tile->aubuf, data, vcl);
        memcpy(tile->aubuf + vcl, sei, sei_len);
        memcpy(tile->aubuf + vcl + sei_len, data + vcl, size - vcl);
        data = tile->aubuf;
        size += sei_len;
    }

    if (tile->dumpfd >= 0) {
        if (write(tile->dumpfd, data, size) != (ssize_t)size) {
            /* best-effort capture */
        }
    }

    if (size > tile->tx_maxlen) {
        tile->tx_maxlen = (guint32)size;
    }

    /* Hold the wire until a real IDR. See AIR_TX_ARM_BYTES in ml-air-video.h for why the encoder's
     * first pictures are black and what a vendor receiver does with them. Counting and forcing are
     * done on tile 0 only so the two tiles are armed by the same keyframe and therefore start on a
     * matching FrameId, which is how a receiver pairs them. */
    if (!g_atomic_int_get(&g_tx_armed)) {

        /* vph_build refuses an IDR bigger than one datagram, and gop 0 produces no further one, so
         * arming on it leaves the receiver without an entry point for the session. Require that it
         * fits and ask for another: by the retry, rate control has a frame of history and the QP
         * floor has taken hold, so it is far smaller than the first attempt. */
        if (is_idr && size > max_es) {
            tile->tx_oversize++;

            /* Force from tile 0 only: air_tx_force_keyframes covers both tiles, so forcing per
             * tile issues two forces on consecutive frames, and a force landing before the
             * previous one's restore latches the intra period at 1 (every frame an IDR). Guarded
             * in wave5-vpu-enc.c as well. */
            if (tile->chn != 0) {
                g_printerr(TAG " tile %d: IDR of %zu B also over the limit; tile 0 drives the"
                           " retry\n", tile->chn, size);
                tile->done++;
                tile->bytes += size;

                return;
            }

            if (g_arm_retries < AIR_TX_ARM_MAX_RETRY) {
                g_arm_retries++;
                g_printerr(TAG " tile %d: IDR of %zu B exceeds the %zu B datagram limit, not"
                           " arming; asking for another keyframe (%u/%u)\n",
                           tile->chn, size, max_es, g_arm_retries, AIR_TX_ARM_MAX_RETRY);
                air_tx_force_keyframes("oversize retry keyframe");
            } else if (g_arm_retries == AIR_TX_ARM_MAX_RETRY) {
                g_arm_retries++;
                g_printerr(TAG " tile %d: IDR still %zu B after %u retries - this scene does not"
                           " fit one datagram at the current bitrate and QP floor. Raise"
                           " ML_AIR_MINQP (vendor uses 15) or lower ML_AIR_BITRATE; no picture"
                           " will reach the receiver until an IDR fits.\n",
                           tile->chn, size, AIR_TX_ARM_MAX_RETRY);
            }

            tile->done++;
            tile->bytes += size;

            return;
        }

        if (is_idr && (size >= AIR_TX_ARM_BYTES || g_arm_dropped >= AIR_TX_ARM_MAX_DROP)) {
            g_atomic_int_set(&g_tx_armed, 1);
            g_printerr(TAG " transmit armed on a %zu B IDR after %u held access unit(s)\n",
                       size, g_arm_dropped);
        } else {
            if (tile->chn == 0 && ++g_arm_dropped == AIR_TX_WARMUP_AUS) {
                air_tx_force_keyframes("arming keyframe");
            }

            tile->done++;
            tile->bytes += size;

            return;
        }
    }

    if (!g_notx) {
        len = vph_build(tile->txbuf, AIR_TX_MAX, (guint32)tile->chn, is_idr, frame_id, ts_ms,
                        tile->resolution, data, (guint32)size);
        if (len == 0) {
            /* Reported per occurrence, not only counted: a dropped P-frame is a visible glitch, a
             * dropped IDR is a black receiver for the session. */
            tile->tx_oversize++;
            g_printerr(TAG " tile %d: dropping a %zu B %s access unit, over the %zu B datagram"
                       " limit (%" G_GUINT64_FORMAT " so far)\n", tile->chn, size,
                       is_idr ? "IDR" : "P", max_es, tile->tx_oversize);

            /* Arming is global, so the other tile may have armed it while this one still owes the
             * receiver an entry point. Forces this instance only, spaced by AIR_TX_FORCE_GAP. */
            if (is_idr && !tile->idr_sent) {
                air_request_smaller_idr(tile, frame_id, size, "oversize idr retry");
            }
        } else if (sendto(tile->sock, tile->txbuf, len, MSG_DONTWAIT,
                          (struct sockaddr *)&tile->dst, sizeof tile->dst) != (ssize_t)len) {
            tile->tx_error++;
            tile->tx_errno = errno;
        } else {
            tile->sent++;
            if (is_idr) {
                tile->idr_sent = 1;
            }
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
