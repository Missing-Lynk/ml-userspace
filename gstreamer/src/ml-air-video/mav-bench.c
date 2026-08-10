/**
 * @file mav-bench.c
 * @brief Synthetic patterns and the throughput benchmark.
 *
 * Diagnostic only: nothing here runs on the live camera path.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/* BT.601 studio range. Patterns stay inside it so the encoder sees the value distribution a
 * sensor actually produces; a full 0..255 excursion is not something the camera path can emit. */
#define AIR_Y_MIN         16
#define AIR_Y_MAX        235
#define AIR_CHROMA_MID   128

/* Odd multiplier that decorrelates one seed from the next, so consecutive ring slots and the two
 * tiles do not render the same field. 0x9e3779b1: the largest prime below 2^32/phi, Knuth's
 * multiplicative hash constant. Any large odd value works, this one is just conventional. */
#define AIR_SEED_MIX     2654435761u

/* Per-pattern seed bases. Arbitrary, and only required to be non-zero and distinct from each
 * other: air_rand is an xorshift, whose all-zero state is a fixed point it can never leave.
 * AIR_SEED_NOISE is the other golden-ratio constant, round(2^32/phi), which is AIR_SEED_MIX + 8.
 * The two are interchangeable here, and being different is the point: it keeps the noise seed
 * from collapsing when frame is 1. */
#define AIR_SEED_BARS    0x1234567u
#define AIR_SEED_TEXTURE 0x2545f491u
#define AIR_SEED_NOISE   0x9e3779b9u

/* Dither applied to bar luma: a few LSB of per-pixel perturbation, which is what leaves a
 * residual behind after motion compensation predicts the scroll. Range is -3..+4, off-centre by
 * a half step because the mask is cheaper than a symmetric range. */
#define AIR_DITHER_MASK    7
#define AIR_DITHER_BIAS    3

/* Noise excursion: AIR_Y_MIN + (byte & AIR_NOISE_MASK) lands in 16..239, inside studio range for
 * both luma and chroma. Note this masks rather than takes a modulo, so bit 5 is always clear and
 * only 128 of the 256 levels occur, leaving gaps at 48..79, 112..143 and 176..207. Left as-is
 * deliberately: the noise tier overflows the bitstream buffer and reports no rate either way, so
 * the comb costs nothing. Use `detail` for a worst case that measures. */
#define AIR_NOISE_MASK   0xdfu

/* BT.601 colour bars, left to right: white, yellow, cyan, green, magenta, red, blue, black.
 * Nominal studio-range values, not a conformance-checked SMPTE field; the benchmark cares about
 * the spectral content, not the colorimetry. */
static const guint8 air_bar_y[8] = { 235, 210, 170, 145, 106,  81,  41,  16 };
static const guint8 air_bar_u[8] = { 128,  16, 166,  54, 202,  90, 240, 128 };
static const guint8 air_bar_v[8] = { 128, 146,  16,  34, 222, 240, 110, 128 };

/** xorshift32. Deterministic so two runs of the same tier encode identical content. */
guint32 air_rand(guint32 *s)
{
    guint32 x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;

    return x;
}

/** Render colour bars into a tile buffer, packed I420 at the coded height (chroma at
 * stride*height), the layout air_wrap_tile describes to the encoder.
 *
 * @p shift scrolls the bars horizontally. On its own that is close to best case, because a
 * global translation is what motion estimation is built to predict; @p dither perturbs luma by
 * a few LSB per pixel with a per-frame seed, which is what leaves a residual behind after the
 * motion vector and makes the tier represent something.
 */
void air_render_bars(struct air_tile *tile, guint8 *dst, int shift, int dither)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    guint8 *y = dst;
    guint8 *cb = dst + (gsize)ys * tile->height;
    guint8 *cr = cb + (gsize)cs * (tile->height / 2);
    guint32 s = AIR_SEED_BARS + (guint32)shift * AIR_SEED_MIX + (guint32)tile->chn;

    for (int row = 0; row < tile->height; row++) {
        for (int col = 0; col < ys; col++) {
            int bar = ((col + shift) % AIR_COMP_W) * 8 / AIR_COMP_W;
            int v = air_bar_y[bar];

            if (dither) {
                v += (int)(air_rand(&s) & AIR_DITHER_MASK) - AIR_DITHER_BIAS;
            }

            y[(gsize)row * ys + col] = (guint8)CLAMP(v, AIR_Y_MIN, AIR_Y_MAX);
        }
    }

    for (int row = 0; row < tile->height / 2; row++) {
        for (int col = 0; col < cs; col++) {
            int bar = ((col * 2 + shift) % AIR_COMP_W) * 8 / AIR_COMP_W;

            cb[(gsize)row * cs + col] = air_bar_u[bar];
            cr[(gsize)row * cs + col] = air_bar_v[bar];
        }
    }
}

/** Build the detail tier's source texture: white noise low-passed with a 3x3 box.
 *
 * The blur is the whole point. Full-entropy content expands under entropy coding, so a frame of
 * it cannot fit a bitstream buffer bounded by the raw frame, and the encoder returns
 * WAVE5_SYSERR_VLC_BUF_FULL before any rate can be measured. That makes `noise` a measurement of
 * the buffer rather than of the encoder. Low-passing keeps dense high-frequency structure, which
 * is what costs coefficients, while leaving the frame compressible enough to code.
 */
static void air_make_texture(struct air_tile *tile)
{
    const int w = AIR_COMP_W;
    const int h = tile->height;
    guint8 *raw = g_malloc((gsize)w * h);
    guint32 s = AIR_SEED_TEXTURE ^ (guint32)tile->chn;

    tile->tex = g_malloc((gsize)w * h);

    for (gsize i = 0; i < (gsize)w * h; i++) {
        raw[i] = (guint8)(16 + (air_rand(&s) % 220));
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int acc = 0;
            int taps = 0;

            for (int drow = -1; drow <= 1; drow++) {
                int srow = row + drow;

                if (srow < 0 || srow >= h) {
                    continue;
                }

                for (int dcol = -1; dcol <= 1; dcol++) {
                    int scol = col + dcol;

                    if (scol < 0 || scol >= w) {
                        continue;
                    }

                    acc += raw[(gsize)srow * w + scol];
                    taps++;
                }
            }

            tile->tex[(gsize)row * w + col] = (guint8)(acc / taps);
        }
    }

    g_free(raw);
}

/** Detail tier: the shared texture sampled with a different horizontal velocity per band.
 *
 * A single global scroll is close to best case, because one motion vector predicts the whole
 * frame. Eight bands moving at different speeds and in alternating directions leave no global
 * vector that works, so the encoder pays real residual across the picture while the content
 * itself stays codeable. This is the worst case that produces a number instead of a wedge.
 */
void air_render_detail(struct air_tile *tile, guint8 *dst, int frame)
{
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    const int bands = 8;
    guint8 *y = dst;
    guint8 *cb = dst + (gsize)ys * tile->height;
    guint8 *cr = cb + (gsize)cs * (tile->height / 2);
    int band_h = tile->height / bands;

    if (tile->tex == NULL) {
        air_make_texture(tile);
    }

    if (band_h < 1) {
        band_h = 1;
    }

    for (int row = 0; row < tile->height; row++) {
        int band = row / band_h;
        int vel;
        int off;
        const guint8 *src = tile->tex + (gsize)row * ys;

        if (band > bands - 1) {
            band = bands - 1;
        }

        vel = (band & 1) ? -(3 + band) : (3 + band);
        off = ((frame * vel) % AIR_COMP_W + AIR_COMP_W) % AIR_COMP_W;

        for (int col = 0; col < ys; col++) {
            y[(gsize)row * ys + col] = src[(col + off) % AIR_COMP_W];
        }
    }

    /* Chroma carries the same structure at a quarter of the excursion: a sensor's chroma is
     * smoother than its luma, and a full-amplitude chroma field would be harder than anything
     * the camera can produce. */
    for (int row = 0; row < tile->height / 2; row++) {
        const guint8 *src = tile->tex + (gsize)(row * 2) * ys;
        int off = (frame * 5) % AIR_COMP_W;

        for (int col = 0; col < cs; col++) {
            int v = (int)src[(col * 2 + off) % AIR_COMP_W] - AIR_CHROMA_MID;

            cb[(gsize)row * cs + col] = (guint8)(AIR_CHROMA_MID + (v / 4));
            cr[(gsize)row * cs + col] = (guint8)(AIR_CHROMA_MID - (v / 4));
        }
    }
}

/** Fill a tile buffer with pseudo-random bytes across the whole packed extent. Every block then
 * carries full spectral energy and no prediction mode is cheap.
 *
 * Kept as a diagnostic, not as the worst-case tier: it reliably overflows the bitstream buffer
 * and wedges the instance, so it yields no rate. Use `detail` for a worst case that measures.
 */
void air_render_noise(struct air_tile *tile, guint8 *dst, int frame)
{
    const gsize len = (gsize)AIR_COMP_W * tile->height
                      + (gsize)(AIR_COMP_W / 2) * (tile->height / 2) * 2;
    guint32 s = AIR_SEED_NOISE ^ ((guint32)frame * AIR_SEED_MIX) ^ (guint32)tile->chn;
    gsize i = 0;

    while (i + 4 <= len) {
        guint32 v = air_rand(&s);

        dst[i++] = (guint8)(AIR_Y_MIN + (v & AIR_NOISE_MASK));
        dst[i++] = (guint8)(AIR_Y_MIN + ((v >> 8) & AIR_NOISE_MASK));
        dst[i++] = (guint8)(AIR_Y_MIN + ((v >> 16) & AIR_NOISE_MASK));
        dst[i++] = (guint8)(AIR_Y_MIN + ((v >> 24) & AIR_NOISE_MASK));
    }

    while (i < len) {
        dst[i++] = (guint8)(AIR_Y_MIN + (air_rand(&s) & AIR_NOISE_MASK));
    }
}

/** Render ring slot @p idx. `static` writes the same field into every slot, so the ring still
 * supplies pipelining depth while the content never changes; anything unrecognised is bars. */
void air_render_one(struct air_tile *tile, int idx, const char *pattern)
{
    air_dmabuf_sync(tile->pool[idx].fd, 1);

    if (strcmp(pattern, "noise") == 0) {
        air_render_noise(tile, tile->pool[idx].map, idx);
    } else if (strcmp(pattern, "detail") == 0) {
        air_render_detail(tile, tile->pool[idx].map, idx);
    } else if (strcmp(pattern, "static") == 0) {
        air_render_bars(tile, tile->pool[idx].map, 0, 0);
    } else {
        air_render_bars(tile, tile->pool[idx].map, idx * 7, 1);
    }

    air_dmabuf_sync(tile->pool[idx].fd, 0);
}

/** Render the whole ring. */
void air_render_ring(struct air_tile *tile, const char *pattern)
{
    for (int i = 0; i < tile->pool_n; i++) {
        air_render_one(tile, i, pattern);
    }
}

/* Tiers differ only in buffer content, so running them as separate processes would open a fresh
 * pair of wave5 instances for each. Instances after the first pair in a boot watchdog or produce
 * nothing at all (HW-confirmed, and reconfirmed the hard way: a third instance sat in PIC_RUN
 * emitting zero frames), which would corrupt exactly the measurement being taken. So the ring is
 * rewritten in place and one instance pair lives for the whole run.
 *
 * The rewrite happens slot by slot as the feeder reserves each one, not in a barrier at the tier
 * boundary. Draining the whole free queue first looks tidier but cannot finish: the encoder keeps
 * the most recently pushed buffer referenced until another replaces it, so one slot per tile
 * never comes back and carries the previous tier's content for the whole of the next one.
 * Renewing on reserve covers every slot, because a reserved slot is held exclusively.
 */


gpointer air_bench_feed(gpointer user)
{
    const gint64 period = G_USEC_PER_SEC / g_bench_fps;
    guint64 n = 0;

    (void)user;

    for (int i = 0; g_bench_stages[i] != NULL && !g_bench_stop; i++) {
        gint64 next = g_get_monotonic_time();
        gint64 until = next + (gint64)g_bench_secs * G_USEC_PER_SEC;
        guint32 renewed[AIR_NCHN] = { 0, 0 };
        guint64 at_start[AIR_NCHN];
        guint64 seen[AIR_NCHN];
        gint64 progress = next;
        int need[AIR_NCHN];
        int scroll;

        for (int j = 0; j < AIR_NCHN; j++) {
            at_start[j] = g_tile[j].done;
            seen[j] = g_tile[j].done;
        }

        /* The first tier was rendered at startup; later tiers renew each slot on reserve. */
        for (int j = 0; j < AIR_NCHN; j++) {
            need[j] = (i > 0 && g_tile[j].active) ? g_tile[j].pool_n : 0;
        }

        g_bench_stage = g_bench_stages[i];
        scroll = strcmp(g_bench_stage, "scroll") == 0;

        if (scroll) {
            for (int j = 0; j < AIR_NCHN; j++) {
                need[j] = 0;
            }
        }

        g_printerr(TAG " bench: tier %s for %d s%s\n", g_bench_stage, g_bench_secs,
                   scroll ? " (visual tier: renders every frame, not a rate measurement)" : "");

        while (!g_bench_stop && g_get_monotonic_time() < until) {
            int idx[AIR_NCHN];

            if (g_bench_free) {
                /* Block on each active tile so the loop tracks buffer returns rather than
                 * spinning on try_pop; the pop is put straight back for air_reserve_pair. */
                gboolean starved = FALSE;

                for (int j = 0; j < AIR_NCHN; j++) {
                    if (g_tile[j].active) {
                        gpointer p = g_async_queue_timeout_pop(g_tile[j].freeq, 100000);

                        if (p == NULL) {
                            starved = TRUE;
                            break;
                        }

                        g_async_queue_push_front(g_tile[j].freeq, p);
                    }
                }

                if (starved) {
                    continue;
                }
            }

            if (air_reserve_pair(idx)) {
                if (scroll) {
                    /* Visual tier: phase comes from the shared frame counter, so both tiles
                     * render the same point in the scroll and the tile seam is continuous.
                     * Re-rendering every frame is the only way to get smooth motion; the ring
                     * tiers hold pool_n fixed phases and the feeder replays them in buffer
                     * completion order, which hops rather than scrolls and puts the two tiles
                     * on unrelated phases. That costs a CPU pass per frame per tile, so this
                     * tier measures nothing: use it to look at a picture, not to take a rate. */
                    for (int j = 0; j < AIR_NCHN; j++) {
                        if (g_tile[j].active) {
                            struct air_tile *tile = &g_tile[j];

                            air_dmabuf_sync(tile->pool[idx[j]].fd, 1);

                            /* No dither. It exists to defeat motion estimation so the ring tiers
                             * carry residual, which is the opposite of what a visual tier wants:
                             * perturbing every pixel every frame costs about 5x the bytes and
                             * pushes access units past the 65467 B datagram ceiling, where they
                             * are discarded and the picture breaks. A pure translation of flat
                             * bars is what keeps frames small enough to arrive. */
                            air_render_bars(tile, tile->pool[idx[j]].map,
                                            (int)((n * AIR_SCROLL_PX) % AIR_COMP_W), 0);
                            air_dmabuf_sync(tile->pool[idx[j]].fd, 0);
                        }
                    }
                } else {
                    for (int j = 0; j < AIR_NCHN; j++) {
                        if (need[j] > 0 && !(renewed[j] & (1u << idx[j]))) {
                            air_render_one(&g_tile[j], idx[j], g_bench_stage);
                            renewed[j] |= 1u << idx[j];
                            need[j]--;
                        }
                    }
                }

                air_push_pool_frame(idx,
                                    gst_util_uint64_scale(n, GST_SECOND, (guint64)g_bench_fps));
                n++;
            }

            for (int j = 0; j < AIR_NCHN; j++) {
                if (g_tile[j].active && g_tile[j].done != seen[j]) {
                    seen[j] = g_tile[j].done;
                    progress = g_get_monotonic_time();
                }
            }

            if (!g_bench_free) {
                gint64 now;

                next += period;
                now = g_get_monotonic_time();
                if (next > now) {
                    g_usleep((gulong)(next - now));
                } else {
                    next = now;
                }
            }
        }

        /* Per-tier verdict. Some tiers are rate measurements and some are survival tests, and
         * the counters alone do not say which failed: a wedged encoder and a slow one both show
         * a low rate. Stall time since the last output separates them. */
        {
            double idle = (double)(g_get_monotonic_time() - progress) / G_USEC_PER_SEC;

            for (int j = 0; j < AIR_NCHN; j++) {
                if (g_tile[j].active) {
                    g_printerr(TAG " bench: tier %s tile %d: %" G_GUINT64_FORMAT
                               " frames, %" G_GUINT64_FORMAT " oversize, %" G_GUINT64_FORMAT
                               " tx errors, %" G_GUINT64_FORMAT " lost, largest AU %u B%s\n",
                               g_bench_stage, j, g_tile[j].done - at_start[j],
                               g_tile[j].tx_oversize, g_tile[j].tx_error, g_tile[j].lost,
                               g_tile[j].tx_maxlen, idle > 2.0 ? ", STALLED" : "");
                }
            }

            if (idle > 2.0) {
                g_printerr(TAG " bench: tier %s produced no output for %.1f s: FAIL\n",
                           g_bench_stage, idle);
            }
        }
    }

    /* All tiers done: end the run rather than idling, so a scripted benchmark terminates. */
    if (g_loop != NULL) {
        g_main_loop_quit(g_loop);
    }

    return NULL;
}

/** Source callback: split one 1920x1080 frame into the two aligned tiles and push each into its
 * encoder. Both tiles carry the same PTS, so their encoded FrameIds match. Skips a frame whole
 * if either tile has no free pool buffer, keeping the pair aligned.
 *
 * This is the pattern path only. It copies, which is what caps it near 17 fps at 1080p; the
 * camera path above shares instead. */
GstFlowReturn air_on_src(GstAppSink *sink, gpointer user)
{
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *sbuf;
    GstVideoFrame frame;
    GstClockTime pts;
    int idx[AIR_NCHN];

    (void)user;
    if (sample == NULL) {
        return GST_FLOW_OK;
    }

    sbuf = gst_sample_get_buffer(sample);
    if (sbuf == NULL || !gst_video_frame_map(&frame, &g_src_info, sbuf, GST_MAP_READ)) {
        g_src_lost++;
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    pts = GST_BUFFER_PTS(sbuf);

    if (!air_reserve_pair(idx)) {
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);

        return GST_FLOW_OK;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active) {
            air_fill_tile(&g_tile[i], idx[i], &frame);
        }
    }
    gst_video_frame_unmap(&frame);

    air_push_pool_frame(idx, pts);

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
