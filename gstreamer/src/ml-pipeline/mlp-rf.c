#include "ml-pipeline.h"

/* Free one composite slot without displaying it. Caller holds c->comp_lock. Unref'ing the buffer
 * runs comp_on_finalize, returning the compbuf to the pool.
 */
static void slot_drop(struct ctx *c, struct comp_slot *sl)
{
    (void)c;
    if (sl->buf) {
        gst_buffer_unref(sl->buf);
        sl->buf = NULL;
        sl->cbi = -1;
    }

    for (int ch = 0; ch < RF_NCHN; ch++) {
        if (sl->smp[ch]) {
            gst_sample_unref(sl->smp[ch]);   /* returns the decoder capture buffer */
            sl->smp[ch] = NULL;
        }
        sl->sfb[ch] = 0;
    }

    sl->used = FALSE;
    sl->have[0] = sl->have[1] = FALSE;
}

/* Find the slot for this PTS, or claim one (free slot, else evict the OLDEST incomplete
 * slot - its frame lost a half for good), backed by a fresh compbuf from the pool. Caller
 * holds c->comp_lock. Returns NULL only if the compbuf pool is momentarily exhausted (the caller
 * then drops this half). No clearing: a slot is only ever displayed once BOTH tiles have
 * blitted, and together they cover all 1080 rows.
 */
static struct comp_slot *slot_get(struct ctx *c, GstClockTime pts)
{
    struct comp_slot *free_sl = NULL, *oldest = NULL;
    for (int i = 0; i < NSLOT; i++) {
        struct comp_slot *sl = &c->slot[i];
        if (sl->used && sl->pts == pts) {
            return sl;
        }

        if (!sl->used && !free_sl) {
            free_sl = sl;
        }

        if (sl->used && (!oldest || sl->pts < oldest->pts)) {
            oldest = sl;
        }
    }

    struct comp_slot *sl = free_sl;
    if (!sl) {
        slot_drop(c, oldest);
        c->pair_evict++;
        sl = oldest;
    }

    if (!c->planes_on) {
        sl->buf = comp_get(c, &sl->cbi);
        if (!sl->buf) {
            /* Pool drained even though a slot was free: the composite pool is smaller than
             * the slot table, so under inter-decoder skew every buffer ends up parked in a
             * half-full slot waiting on a lagging decoder, and the free slots can never be
             * backed. Reclaim the oldest incomplete slot's buffer so we degrade to dropping
             * the oldest unpairable frame instead of deadlocking the pool (freeze at N).
             */
            if (oldest && oldest != sl && oldest->buf) {
                slot_drop(c, oldest);
                c->pair_evict++;
                sl->buf = comp_get(c, &sl->cbi);
            }

            if (!sl->buf) {
                c->comp_starve++;
                return NULL;
            }
        }
    }

    sl->used = TRUE;
    sl->pts = pts;
    sl->have[0] = sl->have[1] = FALSE;

    return sl;
}

/* Clear a slot's per-frame fields after its buffers are handed to the display thread. */
static void slot_reset(struct comp_slot *sl)
{
    sl->used = FALSE;
    sl->buf = NULL;
    sl->cbi = -1;
    sl->sfb[0] = sl->sfb[1] = 0;
    sl->have[0] = sl->have[1] = FALSE;
}

/* Push a completed frame to the display thread. Caller holds c->comp_lock. Plane mode hands
 * over the sample pair; composite mode the pool buffer.
 */
static void slot_push(struct ctx *c, struct comp_slot *sl)
{
    lat_mark_pair(c, sl->pts);
    if (c->planes_on) {
        struct ditem it = { .cbi = -1 };

        it.smp[0] = sl->smp[0];
        it.smp[1] = sl->smp[1];
        it.fb[0] = sl->sfb[0];
        it.fb[1] = sl->sfb[1];
        sl->smp[0] = sl->smp[1] = NULL;    /* display thread owns them now */
        c->cur_pts = sl->pts;
        drm_disp_submit(c, &it, sl->pts);
        c->composed++;
        emit_framestats(c, c->cur_pts);
        slot_reset(sl);

        return;
    }

    /* end CPU write: flush to DDR for the DC */
    ml_dmabuf_sync(c->comp_pool[sl->cbi].fd, 0);
    if (c->rec_on) {
        /* DVR: import the flushed composite zero-copy into the encoder */
        rec_push(c, sl->buf, sl->pts);
    }

    c->cur_pts = sl->pts;

    {
        struct ditem it = { .cbi = sl->cbi, .buf = sl->buf };
        /* transfers sl->buf ownership */
        drm_disp_submit(c, &it, sl->pts);
    }

    c->composed++;
    emit_framestats(c, c->cur_pts);
    /* display thread owns the buffer now (compbuf returns post-flip) */
    slot_reset(sl);
}

/* appsink new-sample: blit the decoded tile straight into OUR composite buffer and release the
 * decoder sample immediately. CRITICAL: never hold a decoder capture buffer across frames -
 * doing so starves the wave5 capture pool and wedges the VPU.
 * The two tiles for one frame arrive interleaved 1:1; we push the composite once both halves
 * are in. If a half never gets its partner (a dropped tile), its slot is evicted when the
 * table wraps - the panel simply keeps the previous complete frame (vendor policy). Runs on
 * each decoder's streaming thread.
 */
GstFlowReturn on_tile(GstAppSink *sink, gpointer u)
{
    struct ctx *c = u;
    int ch = (sink == c->asink[0]) ? 0 : 1;
    GstSample *s = gst_app_sink_pull_sample(sink);
    if (!s) {
        return GST_FLOW_OK;
    }

    GstBuffer *b = gst_sample_get_buffer(s);
    GstMapInfo m;
    struct tileview t;
    gint64 t_in = g_get_monotonic_time();

    if (!map_tile(s, b, &m, &t)) {
        /* do NOT count unmappable samples: samples[] gates both the tile-1 stagger release
         * and the partial-composite flush, and must mean "this decoder produced usable
         * output", or the display fills with never-blitted halves
         */
        c->map_fail++;
        gst_sample_unref(s);

        return GST_FLOW_OK;
    }

    c->samples[ch]++;
    if (c->samples[ch] <= c->discard_before[ch]) {
        /* old-session output emerging after a session restart: discard (see discard_before) */
        c->stale_drop++;
        gst_buffer_unmap(b, &m);
        gst_sample_unref(s);

        return GST_FLOW_OK;
    }

    /* Vendor pairing policy: the panel only ever gets COMPLETE frames whose two halves carry
     * the same PTS (= FrameId). The decoders deliver with a constant few-frame skew, so
     * composites live in a PTS-keyed slot table until their partner half lands. Push-on-
     * completion also keeps the display idle until both decoders are fully up.
     */
    gint64 t_mapped = g_get_monotonic_time();
    lat_mark_dec(c, ch, GST_BUFFER_PTS(b), t_in);
    pthread_mutex_lock(&c->comp_lock);
    c->out_pts[ch] = GST_BUFFER_PTS(b);

    /* Pair by PTS = FrameId (the vendor's key). The wave5 path preserves input timestamps;
     * the rx-side flow control bounds the inter-decoder skew to the slot-table window.
     */
    struct comp_slot *sl = slot_get(c, GST_BUFFER_PTS(b));
    if (sl && c->planes_on) {
        /* Plane-scanout: no blit at all. Cache an FB for this decoder dmabuf, park the
         * SAMPLE in the slot (holding the capture buffer until the pair retires - the
         * decoder pool has ~9 buffers, display holds at most 4 pairs, watched via the
         * pool-starvation symptom: decode rate collapse).
         */
        guint32 fb = t.fd >= 0 ? tile_fb_get(c, ch, &t) : 0;

        if (!fb) {
            c->map_fail++;
        } else {
            if (c->tile_w[ch] == 0) {
                c->tile_w[ch] = t.w;
                c->tile_h[ch] = t.h;
            }

            if (sl->smp[ch]) {
                gst_sample_unref(sl->smp[ch]);   /* duplicate half; keep the newer */
            }

            sl->smp[ch] = gst_sample_ref(s);
            sl->sfb[ch] = fb;
            sl->have[ch] = TRUE;

            if (sl->have[0] && sl->have[1]) {
                slot_push(c, sl);
            }
        }
    } else if (sl) {
        int dst_row = (ch == 0) ? 0 : TILE1_Y;
        int cfd = c->comp_pool[sl->cbi].fd;

        /* Keep the composite DMA-only: a dmabuf tile DMAs straight in; a non-dmabuf tile is packed
         * into our staging dmabuf and DMA'd from there. Only if BOTH fail do we CPU-blit directly
         * into the composite (rare - NV12 / non-packed), which reintroduces the CPU/DMA mix, so
         * flush it the vendor way (ML_DMABLIT_FLUSH). The pooled CMA heap no-ops dma_buf_sync, so
         * a direct CPU blit into a DMA-touched composite could never be made coherent - hence the
         * staging path exists to avoid that mix entirely.
         */
        if (blit_tile_dma(c, cfd, &t, dst_row) || blit_tile_staged(c, cfd, &t, dst_row)) {
            c->blit_dma++;
        } else {
            blit_tile(c->comp_pool[sl->cbi].map, &t, dst_row);
            if (c->dmablit_fd >= 0) {
                ioctl(c->dmablit_fd, ML_DMABLIT_FLUSH, &cfd);
            } else {
                ml_dmabuf_sync(cfd, 0);
            }

            c->blit_cpu++;
        }

        sl->have[ch] = TRUE;
        if (sl->have[0] && sl->have[1]) {
            slot_push(c, sl);
        }
    }

    c->ns_map[ch] += t_mapped - t_in;
    c->ns_blit[ch] += g_get_monotonic_time() - t_mapped;
    c->n_prof[ch]++;

    if (t.fd >= 0) {
        c->n_fd[ch]++;
    }

    pthread_mutex_unlock(&c->comp_lock);
    gst_buffer_unmap(b, &m);

    /* decoder capture buffer freed right away */
    gst_sample_unref(s);
    return GST_FLOW_OK;
}

void clear_pending(struct ctx *c)
{
    pthread_mutex_lock(&c->comp_lock);
    for (int i = 0; i < NSLOT; i++) {
        slot_drop(c, &c->slot[i]);
    }
    pthread_mutex_unlock(&c->comp_lock);
}

/* FrameId regression = a new air session (stream restarts at FrameId 0 with a fresh IDR).
 * Two hard constraints:
 *  - gst flush events kill a live wave5 decoder ("result not ready: 0x800", permanent stall);
 *  - feeding THROUGH the decoders flushlessly desyncs them: without a flush, the codec's
 *    input-to-output frame accounting degrades across the boundary and every later output
 *    carries a PTS that no longer matches its content (bottom half visibly out of sync from
 *    the second session on, even with stale-output discard);
 * so a session restart tears the WHOLE pipeline down to NULL and rebuilds it - the one warm
 * wave5 operation proven safe (it is what a process restart does). ~0.5 s blackout, resync
 * on the new session's next IDR; real sessions restart only when the air unit power-cycles.
 * Forward FrameId gaps (lost packets) are still fed straight through: decoder smear beats a
 * frozen panel. Runs on the MAIN loop (g_idle); the rx thread swallows datagrams while
 * restart_req is up.
 */
/* Kept (unused) as the fallback should a real air unit change stream parameters across a
 * power-cycle in a way the epoch continuation cannot absorb (e.g. resolution change).
 */
G_GNUC_UNUSED static gboolean rf_do_rebuild(gpointer u)
{
    struct ctx *c = u;
    fprintf(stderr, "ml-pipeline: rf session restart (decode rebuild)\n");
    gst_element_set_state(c->pipe, GST_STATE_NULL);
    gst_element_get_state(c->pipe, NULL, NULL, GST_CLOCK_TIME_NONE);
    clear_pending(c);
    pthread_mutex_lock(&c->comp_lock);
    for (int i = 0; i < c->t1_nhold; i++) {
        gst_buffer_unref(c->t1_hold[i]);
    }

    c->t1_nhold = 0;
    /* re-arm the startup stagger for the fresh decoders */
    c->t1_released = FALSE;

    for (int i = 0; i < c->t0_nhold; i++) {
        gst_buffer_unref(c->t0_hold[i]);
    }

    c->t0_nhold = 0;
    c->started[0] = c->started[1] = FALSE;
    c->samples[0] = c->samples[1] = 0;
    c->sent[0] = c->sent[1] = 0;
    c->drop_sync[0] = c->drop_sync[1] = FALSE;
    c->discard_before[0] = c->discard_before[1] = 0;
    c->last_fid = -1;

    pthread_mutex_unlock(&c->comp_lock);
    gst_element_set_state(c->pipe, GST_STATE_PLAYING);
    c->restart_req = FALSE;

    return G_SOURCE_REMOVE;
}

/* TRUE if the AU contains an IRAP slice (H.265 nal_unit_type 16..21: BLA/IDR/CRA) - a point
 * the decoder can restart from cleanly. Walks the byte-stream start codes and decides on the
 * first VCL NAL (types < 32); leading VPS/SPS/PPS are skipped, so the scan stays cheap.
 */
static gboolean au_is_irap(GstBuffer *buf)
{
    GstMapInfo mi;
    gboolean irap = FALSE;

    if (!gst_buffer_map(buf, &mi, GST_MAP_READ)) {
        return FALSE;
    }

    for (gsize i = 0; i + 3 < mi.size; i++) {
        if (mi.data[i] == 0 && mi.data[i + 1] == 0 && mi.data[i + 2] == 1) {
            guint8 t = (mi.data[i + 3] >> 1) & 0x3f;
            if (t < 32) {                   /* first VCL NAL decides the AU */
                irap = (t >= 16 && t <= 21);
                break;
            }
            i += 3;
        }
    }

    gst_buffer_unmap(buf, &mi);
    return irap;
}

/* Hard cap on each input appsrc's queued bytes. appsrc with block=FALSE and leaky-type=0
 * NEVER fails a push - max-bytes only drives the need-data/enough-data signals - so any
 * stretch where decode runs below the input rate (recording: the encoder shares the VPU)
 * accumulates the surplus here without bound. That unbounded anon growth was the recording
 * OOM. 4 MiB ~= 2.5 s of stream, enough for IDR bursts.
 */
#define AU_QUEUE_MAX (4 << 20)

/* Push one AU into a decoder, keeping the per-channel session accounting exact (sent[]
 * is how on_tile tells old-session outputs from new-session ones). Enforces AU_QUEUE_MAX
 * ourselves (see above); once over it, AUs drop until the next IRAP so the reference
 * chain restarts clean instead of painting accumulating garbage.
 */
static void push_au(struct ctx *c, int ch, GstBuffer *buf)
{
    if (c->drop_sync[ch]) {
        if (!au_is_irap(buf)) {
            gst_buffer_unref(buf);
            c->rx_dropped++;

            return;
        }

        c->drop_sync[ch] = FALSE;
    } else if (gst_app_src_get_current_level_bytes(c->src[ch]) >= AU_QUEUE_MAX) {
        c->drop_sync[ch] = TRUE;
        gst_buffer_unref(buf);
        c->rx_dropped++;

        return;
    }

    if (gst_app_src_push_buffer(c->src[ch], buf) == GST_FLOW_OK) {
        c->rx_pushed++;
        c->sent[ch]++;
    } else {
        c->rx_dropped++;
    }
}

/* Release the held tile-1 AUs to its decoder, in order (tile-0's decoder is now up). */
static void t1_flush_hold(struct ctx *c)
{
    for (int i = 0; i < c->t1_nhold; i++) {
        push_au(c, 1, c->t1_hold[i]);
    }

    c->t1_nhold = 0;
    c->t1_released = TRUE;
}

/* UDP :10001 reader: deframe, validate, demux by ChnIndex, push AUs into the input appsrcs. */
void *rf_rx(void *arg)
{
    struct ctx *c = arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("ml-pipeline: rf socket");
        return NULL;
    }

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int rcvbuf = 4 << 20;
    if (setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf) < 0) {
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    }
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_ANY),
                             .sin_port = htons(RF_VIDEO_PORT) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("ml-pipeline: rf bind :10001");
        close(s);

        return NULL;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    guint8 pkt[65536];
    while (c->rx_run) {
        ssize_t n = recv(s, pkt, sizeof pkt, 0);
        if (n < 0) {
            /* timeout tick */
            continue;
        }

        c->rx_pkts++;
        if (n < VPH_LEN) {
            c->rx_bad_hdr++;
            continue;
        }

        guint32 magic, stream_len, chn, idr, frame_id, ts, res, tail, crc;
        memcpy(&magic, pkt + 0, 4);
        memcpy(&stream_len, pkt + 4, 4);
        memcpy(&chn, pkt + 8, 4);
        memcpy(&idr, pkt + 12, 4);
        memcpy(&frame_id, pkt + 16, 4);
        memcpy(&ts, pkt + 20, 4);
        memcpy(&res, pkt + 24, 4);
        memcpy(&tail, pkt + 28, 4);
        memcpy(&crc, pkt + 32, 4);

        (void)idr;
        (void)ts;
        (void)res;

        if (magic != VPH_MAGIC || tail != VPH_TAIL_MAGIC) {
            c->rx_bad_hdr++;
            continue;
        }

        if (crc32_buf(pkt, 32) != crc) {
            c->rx_bad_crc++;
            continue;
        }

        if (chn >= RF_NCHN || stream_len == 0 || (ssize_t)(VPH_LEN + stream_len) > n) {
            c->rx_bad_hdr++;
            continue;
        }

        const guint8 *es = pkt + VPH_LEN;

        /* Air encoder self-report (BR/QP) from this tile's PREFIX_SEI, for the HUD air group. Per
         * tile: the SEI precedes the VCL NAL so the scan is bounded and cheap. Values persist until
         * the next SEI, so a rare missing one holds the last reading rather than blanking. */
        {
            int br = 0, qp = 0;
            if (sei_parse_brqp(es, stream_len, &br, &qp)) {
                c->sei_br_kbps[chn] = br;
                c->sei_qp[chn] = qp;
            }
        }

        if (c->last_fid >= 0 && (gint64)frame_id < c->last_fid - 8) {
            /* FrameId regression = new air session. Historical policy was a full decode
             * rebuild (rf_do_rebuild) because feeding WRAPPED PTS through desynced the
             * decoders; instead keep PTS monotonic by bumping the epoch base - the decoders
             * see one endless stream (the new session opens on an IDR, which the started[]
             * gate below re-enforces per tile). No teardown, no 0.5 s blackout, no ~3 s
             * input-drop transient, no re-staggered inter-decoder skew.
             */
            c->pts_epoch += gst_util_uint64_scale(c->last_fid + 1, GST_SECOND, RF_FPS);
            c->started[0] = c->started[1] = FALSE;
            c->last_fid = -1;
            fprintf(stderr, "ml-pipeline: rf session wrap -> pts epoch %.3fs\n",
                    (double)c->pts_epoch / GST_SECOND);
        }

        if (!c->started[chn]) {
            if (!au_has_idr(es, stream_len)) {
                c->rx_dropped++;
                continue;
            }

            c->started[chn] = TRUE;
            fprintf(stderr, "ml-pipeline: rf tile %u started at FrameId %u\n", chn, frame_id);
        }
        c->last_fid = frame_id;

        GstBuffer *buf = gst_buffer_new_allocate(NULL, stream_len, NULL);
        gst_buffer_fill(buf, 0, es, stream_len);
        GstClockTime pts = c->pts_epoch + gst_util_uint64_scale(frame_id, GST_SECOND, RF_FPS);
        lat_mark_rx(c, pts);
        GST_BUFFER_PTS(buf) = pts;
        GST_BUFFER_DTS(buf) = pts;
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, RF_FPS);

        /* Stagger tile-1 startup: hold its first few AUs so the two decoders do not allocate
         * at the identical instant. Tile 0 always streams directly.
         */
        if (chn == 1 && !c->t1_released) {
            if (c->samples[0] > 0) {
                t1_flush_hold(c);                       /* dec0 up: drain the hold, then this */
                push_au(c, 1, buf);
            } else if (c->t1_nhold < (int)(sizeof c->t1_hold / sizeof c->t1_hold[0])) {
                c->t1_hold[c->t1_nhold++] = buf;        /* hold until dec0 is up */
            } else {
                t1_flush_hold(c);                       /* hold full: release early, don't stall */
                push_au(c, 1, buf);
            }

            continue;
        }

        /* Lockstep flow control (the vendor's implicit property, fusion §4: its decoders stay
         * within a frame of each other). Our wave5 port serves instance 0 preferentially
         * under load - up to ~18-40 frames of tile-1 lag with unconstrained input, far
         * beyond any affordable pairing window. Bound it at the source: tile-0 AUs queue here
         * whenever decoder 0 is more than SKEW_MAX decoded frames ahead of decoder 1, and
         * drain (in order) once decoder 1 catches back up. Tile-1 AUs always flow.
         */
        /* Gate bounds are DERIVED from the pool yield in comp_pool_init (skew_max /
         * inflight_max): overshoot = skew + in-flight must stay under the backable slot
         * window, or every pair evicts before its partner lands. Too tight oscillates tile 0;
         * too loose re-creates the eviction storm.
         */
        if (chn == 0 && c->t1_released) {
            /* gate on decoded skew AND on decoder-0 queue depth: when decoder 1 stalls
             * (IDR cost spike), decoder 0 keeps producing from its already-queued AUs long
             * after the skew gate closes, and that overrun blew through the slot table and
             * dropped frames in blocks. Keeping dec0's in-flight depth shallow caps the
             * overrun at inflight_max + skew_max < the backable window.
             *
             * The skew is measured on the decoders' OUTPUT PTS (frame position), not on
             * cumulative sample counts: counts drift from position when the tiles catch
             * their first IDR at different FrameIds or a decoder consumes an AU without
             * emitting output, and a count-based gate then holds tile 0 against a delta
             * that cannot close - the positions diverge past the slot window and every
             * pair evicts.
             */
            GstClockTime skew_ns = gst_util_uint64_scale(c->skew_max, GST_SECOND, RF_FPS);

            /* The queue-depth bound reads the appsrc level directly: a sent[]-samples[]
             * count delta ratchets whenever the decoder consumes an AU without emitting
             * output (session restart, corrupt AU) and then holds tile 0 against a debt
             * that can never clear - the same count-vs-position disease as the old skew
             * gate. The appsrc level is the actual backlog and cannot drift.
             */
            if (c->t0_nhold > 0
                || c->out_pts[0] > c->out_pts[1] + skew_ns
                || gst_app_src_get_current_level_buffers(c->src[0]) > (guint64)c->inflight_max) {
                if (c->t0_nhold < (int)(sizeof c->t0_hold / sizeof c->t0_hold[0])) {
                    c->t0_hold[c->t0_nhold++] = buf;
                    continue;
                }

                /* hold overflowed: fall through and push everything, skew be damned */
                for (int i = 0; i < c->t0_nhold; i++) {
                    push_au(c, 0, c->t0_hold[i]);
                }
                c->t0_nhold = 0;
            }
        }
        push_au(c, chn, buf);

        /* drain held tile-0 AUs as decoder 1 catches up - bounded per incoming datagram,
         * since out_pts[] only advances asynchronously and an unbounded drain would just
         * re-create the skew in one burst
         */
        GstClockTime drain_skew_ns = gst_util_uint64_scale(c->skew_max, GST_SECOND, RF_FPS);

        for (int k = 0; k < 8 && c->t0_nhold > 0
                        && c->out_pts[0] <= c->out_pts[1] + drain_skew_ns
                        && gst_app_src_get_current_level_buffers(c->src[0]) <= (guint64)c->inflight_max; k++) {
            push_au(c, 0, c->t0_hold[0]);
            c->t0_nhold--;
            memmove(&c->t0_hold[0], &c->t0_hold[1], c->t0_nhold * sizeof c->t0_hold[0]);
        }
    }

    close(s);
    return NULL;
}

gboolean rf_ready_tick(gpointer u)
{
    struct ctx *c = u;
    struct { struct mlm_hdr h; struct mlm_ready r; } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_READY, .flags = 0 },
        .r = { .frames_seen = (c->composed > 0) ? 1u : 0u,
               .rx_pkts = (uint32_t) c->rx_pkts },
    };
    sendto(c->lsock, &rec, sizeof rec, MSG_DONTWAIT,
           (struct sockaddr *)&c->laddr, sizeof c->laddr);

    /* The stats line is debug-grade and this tick runs every 2s, so throttle the log to a ~30s
     * heartbeat (the READY sendto above stays at full cadence). ML_STATS=1 restores every-tick.
     */
    static unsigned stats_tick;
    static int stats_every = -1;
    if (stats_every < 0) {
        stats_every = (getenv("ML_STATS") != NULL) ? 1 : 15;
    }

    if ((stats_tick++ % stats_every) != 0) {
        return G_SOURCE_CONTINUE;
    }

    fprintf(stderr, "ml-pipeline: rx=%llu pushed=%llu drop=%llu q=%u/%uKiB samples=%llu/%llu "
            "composed=%llu evict=%llu stale=%llu mapfail=%llu hold=%d blit=%llud/%lluc p0=%ums p1=%ums "
            "| sei br=%d/%d kbps qp=%d/%d\n",
            (unsigned long long)c->rx_pkts, (unsigned long long)c->rx_pushed,
            (unsigned long long)c->rx_dropped,
            c->src[0] ? (unsigned)(gst_app_src_get_current_level_bytes(c->src[0]) >> 10) : 0,
            c->src[1] ? (unsigned)(gst_app_src_get_current_level_bytes(c->src[1]) >> 10) : 0,
            (unsigned long long)c->samples[0], (unsigned long long)c->samples[1],
            (unsigned long long)c->composed, (unsigned long long)c->pair_evict,
            (unsigned long long)c->stale_drop, (unsigned long long)c->map_fail, c->t0_nhold,
            (unsigned long long)c->blit_dma, (unsigned long long)c->blit_cpu,
            (unsigned)(c->out_pts[0] / GST_MSECOND), (unsigned)(c->out_pts[1] / GST_MSECOND),
            c->sei_br_kbps[0], c->sei_br_kbps[1], c->sei_qp[0], c->sei_qp[1]);

    if (c->n_prof[0] || c->n_prof[1]) {
        fprintf(stderr, "ml-pipeline: prof t0 map=%lluus blit=%lluus (n=%llu fd=%llu) | t1 map=%lluus blit=%lluus (n=%llu fd=%llu)\n",
                (unsigned long long)(c->n_prof[0] ? c->ns_map[0] / c->n_prof[0] : 0),
                (unsigned long long)(c->n_prof[0] ? c->ns_blit[0] / c->n_prof[0] : 0),
                (unsigned long long)c->n_prof[0], (unsigned long long)c->n_fd[0],
                (unsigned long long)(c->n_prof[1] ? c->ns_map[1] / c->n_prof[1] : 0),
                (unsigned long long)(c->n_prof[1] ? c->ns_blit[1] / c->n_prof[1] : 0),
                (unsigned long long)c->n_prof[1], (unsigned long long)c->n_fd[1]);
        c->ns_map[0] = c->ns_map[1] = c->ns_blit[0] = c->ns_blit[1] = 0;
        c->n_prof[0] = c->n_prof[1] = 0;
        c->n_fd[0] = c->n_fd[1] = 0;
    }

    return G_SOURCE_CONTINUE;
}
