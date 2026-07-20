/* ml-pipeline latstats: per-frame latency trace (ML_LATSTATS=1).
 *
 * Each frame's stage timestamps (rx, per-tile decode-out, pair-complete, flip-submit, flip-
 * event) are collected in a PTS-keyed ring and reduced to a 1 Hz summary line on stderr, so
 * ml-logd archives them with the run:
 *
 *   ml-pipeline: lat n=57 rx2flip p50=41.2 p99=63.8 | rx2dec 18.1/21.4 pair 3.2 sub2flip 9.6
 *                | fdt p50=16.7 p99=33.4 jud=3 rep=1 (ms)
 *
 * n        completed frames reduced this second
 * rx2flip  first datagram to flip event, p50/p99 (M1)
 * rx2dec   p50 per tile, pair = later-minus-earlier tile p50, sub2flip = submit to flip p50 (M2)
 * fdt      flip-to-flip interval p50/p99; jud = intervals outside +-20% of nominal, rep =
 *          intervals > 1.5x nominal (frame held across a refresh) (M3)
 *
 * Marks are called from the rx thread, both appsink streaming threads and the display thread;
 * one small mutex serialises them (a handful of ops per frame at 60 fps). With ML_LATSTATS
 * unset every mark returns on the first branch.
 */
#include "ml-pipeline.h"

#define LAT_N     (int)(sizeof ((struct ctx *)0)->lat_ent / sizeof ((struct ctx *)0)->lat_ent[0])
#define LAT_ACC_N (int)(sizeof ((struct ctx *)0)->lat_acc.rxflip / sizeof (gint32))

/* Frame duration in ns (PTS step) and the ring index for a PTS. FrameIds reset per air
 * session but PTS is epoch-continued and strictly monotonic, so pts/duration is a stable
 * frame ordinal.
 */
#define LAT_DUR_NS (GST_SECOND / RF_FPS)

static struct lat_ent *lat_ent_get(struct ctx *c, GstClockTime pts)
{
    struct lat_ent *e = &c->lat_ent[(pts / LAT_DUR_NS) % LAT_N];

    if (e->pts != pts) {
        memset(e, 0, sizeof *e);
        e->pts = pts;
    }

    return e;
}

void lat_mark_rx(struct ctx *c, GstClockTime pts)
{
    if (!c->lat_on) {
        return;
    }

    pthread_mutex_lock(&c->lat_lock);
    struct lat_ent *e = lat_ent_get(c, pts);
    if (!e->t_rx) {
        e->t_rx = g_get_monotonic_time();
    }

    pthread_mutex_unlock(&c->lat_lock);
}

/* t_us is passed in (appsink entry time) so the mark sits outside the caller's blit path. */
void lat_mark_dec(struct ctx *c, int ch, GstClockTime pts, gint64 t_us)
{
    if (!c->lat_on) {
        return;
    }

    pthread_mutex_lock(&c->lat_lock);
    struct lat_ent *e = lat_ent_get(c, pts);
    if (!e->t_dec[ch]) {
        e->t_dec[ch] = t_us;
    }

    pthread_mutex_unlock(&c->lat_lock);
}

void lat_mark_pair(struct ctx *c, GstClockTime pts)
{
    if (!c->lat_on) {
        return;
    }

    pthread_mutex_lock(&c->lat_lock);
    lat_ent_get(c, pts)->t_pair = g_get_monotonic_time();
    pthread_mutex_unlock(&c->lat_lock);
}

void lat_mark_issue(struct ctx *c, GstClockTime pts)
{
    if (!c->lat_on) {
        return;
    }

    pthread_mutex_lock(&c->lat_lock);
    struct lat_ent *e = lat_ent_get(c, pts);
    if (!e->t_issue) {
        e->t_issue = g_get_monotonic_time();
    }

    pthread_mutex_unlock(&c->lat_lock);
}

void lat_mark_submit(struct ctx *c, GstClockTime pts)
{
    if (!c->lat_on) {
        return;
    }

    pthread_mutex_lock(&c->lat_lock);
    struct lat_ent *e = lat_ent_get(c, pts);
    if (!e->t_submit) {
        e->t_submit = g_get_monotonic_time();
    }

    pthread_mutex_unlock(&c->lat_lock);
}

/* Flip event: the frame is latched for scanout. Completes the sample and records the flip
 * cadence (the cadence is recorded even for frames whose earlier stages were not all seen,
 * e.g. right after start).
 */
void lat_mark_flip(struct ctx *c, GstClockTime pts)
{
    if (!c->lat_on) {
        return;
    }

    gint64 now = g_get_monotonic_time();

    pthread_mutex_lock(&c->lat_lock);
    if (c->lat_last_flip && c->lat_acc.nfdt < LAT_ACC_N) {
        gint32 dt = (gint32)(now - c->lat_last_flip);
        gint32 nom = (gint32)(LAT_DUR_NS / 1000);

        c->lat_acc.fdt[c->lat_acc.nfdt++] = dt;
        if (dt > nom + nom / 2) {
            c->lat_acc.repeat++;
        }

        if (dt > nom + nom / 5 || dt < nom - nom / 5) {
            c->lat_acc.judder++;
        }
    }
    c->lat_last_flip = now;

    struct lat_ent *e = &c->lat_ent[(pts / LAT_DUR_NS) % LAT_N];
    if (c->lat_raw && e->pts == pts && !e->done) {
        /* one line per flip, absolute monotonic us: exact submit/latch interleave analysis */
        fprintf(stderr, "ml-pipeline: latraw pts=%llu pair=%lld issue=%lld sub=%lld evt=%lld\n",
                (unsigned long long)(pts / GST_MSECOND), (long long)e->t_pair,
                (long long)e->t_issue, (long long)e->t_submit, (long long)now);
    }

    /* rx2flip/dec0/sub2flip are recorded for every latched frame; dec1/pair only when both
     * were seen before the flip (a frame latched with just tile-0 marks at startup is skipped
     * for those), so they carry their own count.
     */
    if (e->pts == pts && !e->done && e->t_rx && e->t_dec[0]
        && e->t_submit && c->lat_acc.n < LAT_ACC_N) {
        struct lat_acc *a = &c->lat_acc;
        gint64 d0 = e->t_dec[0] - e->t_rx;

        a->rxflip[a->n] = (gint32)(now - e->t_rx);
        a->rxdec0[a->n] = (gint32)d0;
        a->subflip[a->n] = (gint32)(now - e->t_submit);
        a->n++;
        if (e->t_dec[1] && e->t_pair && a->n2 < LAT_ACC_N) {
            gint64 d1 = e->t_dec[1] - e->t_rx;

            a->rxdec1[a->n2] = (gint32)d1;
            a->pairw[a->n2] = (gint32)(d0 > d1 ? d0 - d1 : d1 - d0);
            a->n2++;
        }

        e->done = TRUE;
    }

    pthread_mutex_unlock(&c->lat_lock);
}

static int cmp_i32(const void *a, const void *b)
{
    return *(const gint32 *)a - *(const gint32 *)b;
}

/* Percentile of a sorted array, nearest-rank. */
static double pct(const gint32 *v, int n, int p)
{
    if (n == 0) {
        return 0.0;
    }

    int i = (n * p + 99) / 100 - 1;
    if (i < 0) {
        i = 0;
    }

    if (i >= n) {
        i = n - 1;
    }

    return v[i] / 1000.0;
}

static gboolean lat_flush_tick(gpointer u)
{
    struct ctx *c = u;
    struct lat_acc a;

    pthread_mutex_lock(&c->lat_lock);
    a = c->lat_acc;
    memset(&c->lat_acc, 0, sizeof c->lat_acc);
    pthread_mutex_unlock(&c->lat_lock);

    if (a.n == 0 && a.nfdt == 0) {
        return G_SOURCE_CONTINUE;
    }

    qsort(a.rxflip, a.n, sizeof a.rxflip[0], cmp_i32);
    qsort(a.rxdec0, a.n, sizeof a.rxdec0[0], cmp_i32);
    qsort(a.rxdec1, a.n2, sizeof a.rxdec1[0], cmp_i32);
    qsort(a.pairw, a.n2, sizeof a.pairw[0], cmp_i32);
    qsort(a.subflip, a.n, sizeof a.subflip[0], cmp_i32);
    qsort(a.fdt, a.nfdt, sizeof a.fdt[0], cmp_i32);

    fprintf(stderr, "ml-pipeline: lat n=%d rx2flip p50=%.1f p99=%.1f | rx2dec %.1f/%.1f "
            "pair %.1f sub2flip %.1f | fdt p50=%.1f p99=%.1f jud=%u rep=%u (ms)\n",
            a.n, pct(a.rxflip, a.n, 50), pct(a.rxflip, a.n, 99),
            pct(a.rxdec0, a.n, 50), pct(a.rxdec1, a.n2, 50),
            pct(a.pairw, a.n2, 50), pct(a.subflip, a.n, 50),
            pct(a.fdt, a.nfdt, 50), pct(a.fdt, a.nfdt, 99), a.judder, a.repeat);

    return G_SOURCE_CONTINUE;
}

void lat_init(struct ctx *c)
{
    c->lat_on = getenv("ML_LATSTATS") != NULL;
    if (!c->lat_on) {
        return;
    }

    c->lat_raw = getenv("ML_LATRAW") != NULL;

    pthread_mutex_init(&c->lat_lock, NULL);
    memset(c->lat_ent, 0, sizeof c->lat_ent);
    memset(&c->lat_acc, 0, sizeof c->lat_acc);
    c->lat_last_flip = 0;
    c->lat_timer = g_timeout_add(1000, lat_flush_tick, c);
    fprintf(stderr, "ml-pipeline: latstats on (1 Hz summary)\n");
}
