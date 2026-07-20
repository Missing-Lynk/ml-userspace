#include "ml-pipeline.h"

/* Custom DRM/KMS display sink: drives artosyn_vo directly, retires after page-flip. */

/* Upper bound on waiting for a page-flip completion event (~6 frame times at 60 Hz; the
 * poll below wakes at most every 100 ms, so this is also the effective check granularity).
 */
#define FLIP_TIMEOUT_US (100 * 1000)

/* Discover the connected output: connector + its mode + the CRTC driving it. */
static int drm_find_output(struct ctx *c)
{
    drmModeRes *res = drmModeGetResources(c->drm_fd);
    int ok = -1;

    if (!res) {
        perror("ml-pipeline: drmModeGetResources");
        return -1;
    }

    for (int i = 0; i < res->count_connectors && ok < 0; i++) {
        drmModeConnector *conn = drmModeGetConnector(c->drm_fd, res->connectors[i]);
        if (!conn) {
            continue;
        }

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            drmModeEncoder *enc = conn->encoder_id ?
                drmModeGetEncoder(c->drm_fd, conn->encoder_id) : NULL;
            c->conn_id = conn->connector_id;
            c->mode = conn->modes[0];
            if (enc && enc->crtc_id) {
                c->crtc_id = enc->crtc_id;
                ok = 0;
            } else if (res->count_crtcs > 0) {
                c->crtc_id = res->crtcs[0];
                ok = 0;
            }

            if (enc) {
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    if (ok == 0) {
        fprintf(stderr, "ml-pipeline: DRM output connector %u crtc %u mode %s %ux%u\n",
                c->conn_id, c->crtc_id, c->mode.name, c->mode.hdisplay, c->mode.vdisplay);
    }

    return ok;
}

/* Import a composite dmabuf as a scanout FB (YUV420, three planes in one contiguous buffer). */
static guint32 drm_make_fb(struct ctx *c, int dmabuf_fd, guint32 *handle_out)
{
    guint32 handle = 0, fb = 0;
    guint32 handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

    if (drmPrimeFDToHandle(c->drm_fd, dmabuf_fd, &handle)) {
        perror("ml-pipeline: drmPrimeFDToHandle");
        return 0;
    }

    *handle_out = handle;
    handles[0] = handles[1] = handles[2] = handle;
    pitches[0] = COMP_LSTRIDE;
    pitches[1] = COMP_CSTRIDE;
    pitches[2] = COMP_CSTRIDE;
    offsets[0] = 0;
    offsets[1] = COMP_UOFF;
    offsets[2] = COMP_VOFF;

    if (drmModeAddFB2(c->drm_fd, COMP_W, COMP_H, DRM_FORMAT_YUV420,
                      handles, pitches, offsets, &fb, 0)) {
        perror("ml-pipeline: drmModeAddFB2");
        return 0;
    }

    return fb;
}

/* Page-flip completion. Runs on the display thread inside drmHandleEvent, so
 * prev/front/pending are single-threaded here (no lock). The unref returns the retired
 * compbuf to comp_free via comp_on_finalize.
 *
 * Retirement is ONE FLIP LATE (grandparent, not parent): artosyn_vo arms the flip event on
 * the software vblank counter, but the DC latches the shadowed scanout address at the
 * hardware frame-start edge. An address write landing between that edge and its ISR gets
 * its event completed one full frame EARLY, while the DC still scans the outgoing buffer.
 * Freeing on the event would let the compositor blit into a buffer mid-scanout (the moving-
 * frame bottom garbage). Holding each buffer one extra flip makes retirement immune to the
 * +-1-frame event uncertainty.
 */
/* Release everything a ditem holds: the composite pool ref, or the tile sample pair
 * (which returns the decoder capture buffers).
 */
static void ditem_release(struct ditem *it)
{
    if (it->buf) {
        gst_buffer_unref(it->buf);
    }

    if (it->smp[0]) {
        gst_sample_unref(it->smp[0]);
    }

    if (it->smp[1]) {
        gst_sample_unref(it->smp[1]);
    }

    memset(it, 0, sizeof *it);
    it->cbi = -1;
}

static gboolean ditem_is_empty(const struct ditem *it)
{
    return !it->buf && !it->smp[0];
}

static void disp_try_submit(struct ctx *c);
static int plane_commit(struct ctx *c, const struct ditem *it);

static void drm_flip_handler(int fd, unsigned int seq, unsigned int tv_s,
                             unsigned int tv_us, void *data)
{
    struct ctx *c = data;

    (void)fd;
    (void)seq;
    (void)tv_s;
    (void)tv_us;

    if (c->retire_arm) {                /* SIGUSR1: arm a burst of post-scanout dumps */
        c->retire_dumps += c->retire_arm;
        c->retire_arm = 0;
    }

    c->flip_last_us = g_get_monotonic_time();

    /* pending_it/front_it/prev_it rotate under disp_lock: disp_try_submit runs on the
     * COMPOSITOR thread too and reads pending_it as the flip-in-flight gate.
     */
    pthread_mutex_lock(&c->disp_lock);

    /* The event completes pending_it's flip: that frame is (about to be) latched. */
    if (!ditem_is_empty(&c->pending_it)) {
        lat_mark_flip(c, c->pending_it.pts);
        pace_flip(c, c->flip_last_us - c->pending_since);
    }

    if (!ditem_is_empty(&c->prev_it)) {
        if (c->retire_dumps > 0 && c->prev_it.cbi >= 0) {
            char path[64];
            snprintf(path, sizeof path, "/tmp/retire%d.raw", c->retire_seq++);

            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(c->comp_pool[c->prev_it.cbi].map, 1, COMP_SIZE, f);
                fclose(f);
                fprintf(stderr, "ml-pipeline: dumped %s (post-scanout)\n", path);
            }
            c->retire_dumps--;
        }
        ditem_release(&c->prev_it);
    }

    c->prev_it = c->front_it;
    c->front_it = c->pending_it;
    memset(&c->pending_it, 0, sizeof c->pending_it);
    c->pending_it.cbi = -1;
    pthread_mutex_unlock(&c->disp_lock);

    /* Re-arm at once: a frame parked during this flip goes out before the next latch edge
     * instead of waiting for the poll loop, so back-to-back flips can sustain source rate.
     */
    disp_try_submit(c);
}

/* Submit the newest parked frame if no flip is in flight. Runs on the display thread (poll
 * loop + flip-event handler tail) AND on the compositor thread at frame completion
 * (drm_disp_submit): the display rate ceiling is 1 / (submit-to-event round trip), so the
 * submit must happen at completion/event time, not on the next loop wake. The first frame
 * after a (re)init needs the blocking modeset and stays in the display loop (!modeset_done
 * bails here). Caller must not hold disp_lock.
 */
static void disp_try_submit(struct ctx *c)
{
    pthread_mutex_lock(&c->disp_lock);
    if (!c->modeset_done || !ditem_is_empty(&c->pending_it) || ditem_is_empty(&c->next_it)) {
        pthread_mutex_unlock(&c->disp_lock);
        return;
    }

    struct ditem it = c->next_it;
    memset(&c->next_it, 0, sizeof c->next_it);
    c->next_it.cbi = -1;

    /* A real frame is going out: video is back, drop the splash. */
    c->show_idle = c->idle_shown = 0;

    int rc;
    lat_mark_issue(c, it.pts);
    if (c->planes_on) {
        rc = plane_commit(c, &it);
    } else {
        guint32 fbid = c->single ? it.fb[0] : c->fb_id[it.cbi];
        rc = drmModePageFlip(c->drm_fd, c->crtc_id, fbid, DRM_MODE_PAGE_FLIP_EVENT, c);
    }

    if (rc) {
        pthread_mutex_unlock(&c->disp_lock);
        perror("ml-pipeline: flip submit");
        ditem_release(&it);

        return;
    }

    c->pending_it = it;
    c->pending_since = g_get_monotonic_time();
    pthread_mutex_unlock(&c->disp_lock);
    lat_mark_submit(c, it.pts);
}

/* Plane-scanout: commit both tiles' FBs to video0/video1 in ONE atomic request. Both
 * banks' addresses go through the shared 0x1518 bit3 shadow bracket, so the pair latches
 * on the same frame-start - the seam is atomically consistent by hardware.
 */
static int plane_commit(struct ctx *c, const struct ditem *it)
{
    static const int dsty[2] = { 0, TILE1_Y };
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    int ret;

    if (!r) {
        return -1;
    }

    for (int i = 0; i < 2; i++) {
        int w = c->tile_w[i], h = c->tile_h[i];

        if (dsty[i] + h > COMP_H) {
            h = COMP_H - dsty[i];      /* 1:1 clip (528 + 552 = 1080 exactly today) */
        }

        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_fb[i], it->fb[i]);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_crtcid[i], c->crtc_id);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_srcx[i], 0);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_srcy[i], 0);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_srcw[i], (guint64)w << 16);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_srch[i], (guint64)h << 16);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_cx[i], 0);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_cy[i], dsty[i]);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_cw[i], w);
        drmModeAtomicAddProperty(r, c->vid_plane[i], c->pp_ch[i], h);
    }

    ret = drmModeAtomicCommit(c->drm_fd, r,
                              DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, c);
    drmModeAtomicFree(r);
    return ret;
}

/* Stage overlay plane `i` in an atomic request: scan the `fb` region (src_w x src_h from the FB's
 * top-left) out onto the whole CRTC. fb == 0 takes the plane off the CRTC (the rest is ignored).
 * SRC_* are 16.16 fixed-point (hence << 16); CRTC_* are pixels.
 */
static void plane_stage_fullscreen(struct ctx *c, drmModeAtomicReq *req, int i,
                                   guint32 fb, int src_w, int src_h)
{
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_fb[i], fb);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_crtcid[i], fb ? c->crtc_id : 0);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_srcx[i], 0);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_srcy[i], 0);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_srcw[i], (guint64)src_w << 16);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_srch[i], (guint64)src_h << 16);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_cx[i], 0);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_cy[i], 0);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_cw[i], COMP_W);
    drmModeAtomicAddProperty(req, c->vid_plane[i], c->pp_ch[i], COMP_H);
}

/* Park the display on the no-signal splash (idle_fb) when the live link drops, so the panel stops
 * holding the last decoded frame. Plane mode: scan the full-screen splash out on video0 and take
 * video1 off the CRTC. Composite mode: modeset the CRTC to the splash. A blocking commit (no
 * page-flip event to track); the held tile samples are released only AFTER the shadow latch,
 * mirroring the shutdown teardown - freeing a buffer the DC is still scanning is the documented SoC
 * hang. A returning video frame re-commits the planes and clears idle_shown.
 */
static void drm_show_idle(struct ctx *c)
{
    if (!c->idle_fb) {
        /* No splash allocated (heap alloc failed): leave the last frame up. */
        return;
    }

    if (c->planes_on) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (req) {
            plane_stage_fullscreen(c, req, 0, c->idle_fb, COMP_W, COMP_H);   /* video0: the splash */
            plane_stage_fullscreen(c, req, 1, 0, 0, 0);                      /* video1: off */
            if (drmModeAtomicCommit(c->drm_fd, req, 0, c)) {
                perror("ml-pipeline: atomic commit(no-signal)");
            }
            drmModeAtomicFree(req);
        }
    } else if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->idle_fb, 0, 0, &c->conn_id, 1, &c->mode)) {
        perror("ml-pipeline: drmModeSetCrtc(no-signal)");
    }

    /* Wait past the shadow latch before releasing the old frame. Under disp_lock: the
     * compositor-thread disp_try_submit reads pending_it as its flip-in-flight gate.
     */
    usleep(2 * 1000000 / RF_FPS);
    pthread_mutex_lock(&c->disp_lock);
    ditem_release(&c->prev_it);
    ditem_release(&c->front_it);
    ditem_release(&c->pending_it);
    pthread_mutex_unlock(&c->disp_lock);
}

static void *drm_disp_run(void *arg)
{
    struct ctx *c = arg;
    drmEventContext ev = { .version = 2, .page_flip_handler = drm_flip_handler };
    struct pollfd pfd[2] = { { c->drm_fd, POLLIN, 0 }, { c->wake_r, POLLIN, 0 } };

    while (c->disp_run) {
        poll(pfd, 2, 100);
        if (pfd[0].revents & POLLIN) {
            drmHandleEvent(c->drm_fd, &ev);
        }

        if (pfd[1].revents & POLLIN) {
            char b[64];
            while (read(c->wake_r, b, sizeof b) > 0) { }
        }

        pfd[0].revents = pfd[1].revents = 0;
        if (!ditem_is_empty(&c->pending_it)) {
            /* A flip is in flight; wait for its completion event. Bounded: the completion
             * rides the VO vsync IRQ, and a lost edge would otherwise block this thread
             * forever. After FLIP_TIMEOUT_US force-retire as if the event had arrived. If
             * the real event still arrives later it rotates the queue one step early - a
             * one-frame glitch, not a freeze.
             */
            if (g_get_monotonic_time() - c->pending_since < FLIP_TIMEOUT_US) {
                continue;
            }

            fprintf(stderr, "ml-pipeline: flip completion lost (> %d ms), force-retiring "
                    "(VO vsync stall?)\n", (int)(FLIP_TIMEOUT_US / 1000));
            drm_flip_handler(c->drm_fd, 0, 0, 0, c);
        }

        if (c->show_idle && !c->idle_shown) {
            /* Live link dropped: park on the no-signal splash. */
            drm_show_idle(c);
            c->idle_shown = 1;
        }

        if (c->modeset_done) {
            /* Steady state: submissions happen at frame completion (drm_disp_submit) and at
             * flip-event time (drm_flip_handler tail); this is the fallback re-arm for wakes
             * that raced those paths (and after a force-retire).
             */
            disp_try_submit(c);
            continue;
        }

        /* First frame after a (re)init: the blocking modeset path (display-thread-only). */
        pthread_mutex_lock(&c->disp_lock);
        struct ditem it = c->next_it;
        memset(&c->next_it, 0, sizeof c->next_it);
        c->next_it.cbi = -1;
        pthread_mutex_unlock(&c->disp_lock);
        if (ditem_is_empty(&it)) {
            continue;
        }

        /* A real frame is here: video is back, drop the splash. */
        c->show_idle = c->idle_shown = 0;

        if (c->planes_on) {
            /* Light the CRTC with the static black primary once; the tiles then ride
             * the overlay planes only.
             */
            if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->prim_fb, 0, 0,
                               &c->conn_id, 1, &c->mode)) {
                perror("ml-pipeline: drmModeSetCrtc(primary)");
                ditem_release(&it);
                continue;
            }
            c->modeset_done = 1;

            if (plane_commit(c, &it)) {
                perror("ml-pipeline: drmModeAtomicCommit");
                ditem_release(&it);
            } else {
                pthread_mutex_lock(&c->disp_lock);
                c->pending_it = it;
                c->pending_since = g_get_monotonic_time();
                pthread_mutex_unlock(&c->disp_lock);
                lat_mark_submit(c, it.pts);
            }
        } else {
            /* composite: scan out the pool buffer's FB; single: the frame's own imported FB. */
            guint32 fbid = c->single ? it.fb[0] : c->fb_id[it.cbi];
            if (drmModeSetCrtc(c->drm_fd, c->crtc_id, fbid, 0, 0,
                               &c->conn_id, 1, &c->mode)) {
                perror("ml-pipeline: drmModeSetCrtc");
                ditem_release(&it);
                continue;
            }

            c->modeset_done = 1;
            c->front_it = it;   /* on screen now; SetCrtc gives no flip event */
        }
    }

    return NULL;
}

/* Hand a freshly composed frame to the display thread. Newest-wins: any still-queued older
 * frame is released (never blocking the compositor, never silently dropped by a leaky
 * queue). Caller holds c->comp_lock and transfers ownership of everything in `it`.
 */
void drm_disp_submit(struct ctx *c, const struct ditem *it, GstClockTime pts)
{
    struct ditem displaced;

    pthread_mutex_lock(&c->disp_lock);
    displaced = c->next_it;
    c->next_it = *it;
    c->next_it.pts = pts;
    c->next_pts = pts;
    pthread_mutex_unlock(&c->disp_lock);

    if (!ditem_is_empty(&displaced)) {
        ditem_release(&displaced);     /* never shown -> released (pool / decoder) */
    }

    /* Submit right here when no flip is in flight; the wake remains for the first-frame
     * modeset and the idle-splash logic, which stay on the display thread.
     */
    disp_try_submit(c);
    pipe_wake(c->wake_w);
}

/* Resolve the video0/video1 overlay planes (type OVERLAY, YUV420-capable, this CRTC) and
 * the property ids the atomic commit needs. Ascending plane id = registration order =
 * video0 then video1 (artosyn_vo).
 */
static int plane_scan(struct ctx *c)
{
    drmModePlaneRes *pr;
    int n = 0;

    drmSetClientCap(c->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(c->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        fprintf(stderr, "ml-pipeline: no atomic cap\n");
        return -1;
    }

    pr = drmModeGetPlaneResources(c->drm_fd);
    if (!pr) {
        return -1;
    }

    for (guint32 i = 0; i < pr->count_planes && n < 2; i++) {
        drmModePlane *p = drmModeGetPlane(c->drm_fd, pr->planes[i]);
        gboolean yuv = FALSE, overlay = FALSE;

        if (!p) {
            continue;
        }

        for (guint32 f = 0; f < p->count_formats; f++) {
            if (p->formats[f] == DRM_FORMAT_YUV420) {
                yuv = TRUE;
            }
        }

        drmModeObjectProperties *props =
            drmModeObjectGetProperties(c->drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (guint32 j = 0; j < props->count_props; j++) {
                drmModePropertyRes *q = drmModeGetProperty(c->drm_fd, props->props[j]);

                if (!q) {
                    continue;
                }

                if (!strcmp(q->name, "type") &&
                    props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY) {
                    overlay = TRUE;
                }

                if (yuv) {
                    guint32 *slot =
                        !strcmp(q->name, "FB_ID")   ? &c->pp_fb[n] :
                        !strcmp(q->name, "CRTC_ID") ? &c->pp_crtcid[n] :
                        !strcmp(q->name, "SRC_X")   ? &c->pp_srcx[n] :
                        !strcmp(q->name, "SRC_Y")   ? &c->pp_srcy[n] :
                        !strcmp(q->name, "SRC_W")   ? &c->pp_srcw[n] :
                        !strcmp(q->name, "SRC_H")   ? &c->pp_srch[n] :
                        !strcmp(q->name, "CRTC_X")  ? &c->pp_cx[n] :
                        !strcmp(q->name, "CRTC_Y")  ? &c->pp_cy[n] :
                        !strcmp(q->name, "CRTC_W")  ? &c->pp_cw[n] :
                        !strcmp(q->name, "CRTC_H")  ? &c->pp_ch[n] : NULL;
                    if (slot) {
                        *slot = q->prop_id;
                    }
                }
                drmModeFreeProperty(q);
            }
            drmModeFreeObjectProperties(props);
        }

        if (yuv && overlay) {
            c->vid_plane[n++] = p->plane_id;
        }

        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(pr);
    if (n < 2) {
        fprintf(stderr, "ml-pipeline: found %d YUV overlay planes, need 2\n", n);
        return -1;
    }

    fprintf(stderr, "ml-pipeline: video planes %u/%u on crtc %u\n",
            c->vid_plane[0], c->vid_plane[1], c->crtc_id);

    return 0;
}

/* Static black primary: the CRTC keystone the tiles composite over (and the letterbox
 * color if the tile geometry ever leaves gaps). A zeroed XRGB dumb buffer.
 */
static int plane_prim_init(struct ctx *c)
{
    struct drm_mode_create_dumb cd = {
        .width = c->mode.hdisplay, .height = c->mode.vdisplay, .bpp = 32,
    };

    if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        perror("ml-pipeline: create_dumb(primary)");
        return -1;
    }

    c->prim_dumb = cd.handle;
    if (drmModeAddFB(c->drm_fd, cd.width, cd.height, 24, 32, cd.pitch,
                     cd.handle, &c->prim_fb)) {
        perror("ml-pipeline: AddFB(primary)");
        return -1;
    }

    return 0;
}

/* Per-decoder dmabuf-fd -> DRM FB cache. The decoder capture pool is a fixed recycling set
 * (~9 buffers), so each dmabuf is PRIME-imported and AddFB2'd exactly once.
 */
guint32 tile_fb_get(struct ctx *c, int ch, const struct tileview *t)
{
    guint32 handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
    guint32 handle, fb;
    struct tfb *e;

    for (int i = 0; i < c->ntfb[ch]; i++) {
        if (c->tfb[ch][i].fd == t->fd) {
            return c->tfb[ch][i].fb;
        }
    }

    if (c->ntfb[ch] >= (int)(sizeof c->tfb[0] / sizeof c->tfb[0][0])) {
        return 0;
    }

    if (drmPrimeFDToHandle(c->drm_fd, t->fd, &handle)) {
        perror("ml-pipeline: PrimeFDToHandle(tile)");
        return 0;
    }

    if (t->nv12) {
        handles[0] = handles[1] = handle;
        pitches[0] = t->ys;
        pitches[1] = t->us;
        offsets[0] = t->yoff;
        offsets[1] = t->uoff;
    } else {
        handles[0] = handles[1] = handles[2] = handle;
        pitches[0] = t->ys;
        pitches[1] = t->us;
        pitches[2] = t->vs;
        offsets[0] = t->yoff;
        offsets[1] = t->uoff;
        offsets[2] = t->voff;
    }

    if (drmModeAddFB2(c->drm_fd, t->w, t->h,
                      t->nv12 ? DRM_FORMAT_NV12 : DRM_FORMAT_YUV420,
                      handles, pitches, offsets, &fb, 0)) {
        perror("ml-pipeline: AddFB2(tile)");
        drmCloseBufferHandle(c->drm_fd, handle);
        return 0;
    }

    e = &c->tfb[ch][c->ntfb[ch]++];
    e->fd = t->fd;
    e->fb = fb;
    e->handle = handle;

    return fb;
}

/* Allocate the persistent black primary FB the CRTC parks on while a decode graph is torn down,
 * so the DC never scans a freed FB (the fault that powered the panel off on a playback<->live
 * swap). artosyn_vo has no dumb-buffer support, so it is a zeroed I420 dma-heap buffer (same
 * layout/format as the video, so the park is a clean no-format-change modeset). Allocate this
 * BEFORE the composite pool grabs the CMA, and only once. Returns 0 on success. */
int drm_make_idle_fb(struct ctx *c)
{
    if (c->idle_fb) {
        return 0;
    }

    int fd = ml_heap_alloc(COMP_SIZE);
    if (fd < 0) {
        fprintf(stderr, "ml-pipeline: idle_fb heap alloc failed (no CMA?) - swaps may blank the panel\n");
        return -1;
    }

    guint8 *m = mmap(NULL, COMP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m != MAP_FAILED) {
        /* Fill with the no-signal splash (I420 1920x1080 == COMP_SIZE) so parking here on a swap -
         * and returning to live with no RF frame - shows the splash, not black. Falls back to black
         * if the image is missing.
         */
        const char *sp = getenv("ML_NOSIGNAL") ? getenv("ML_NOSIGNAL") : "/usr/local/share/nosignal.yuv";
        int imgfd = open(sp, O_RDONLY);
        size_t got = 0;
        if (imgfd >= 0) {
            ssize_t n;
            while (got < COMP_SIZE && (n = read(imgfd, m + got, COMP_SIZE - got)) > 0) {
                got += n;
            }
            close(imgfd);
        }

        if (got != COMP_SIZE) {
            memset(m, 0, COMP_YSIZE);                          /* black luma */
            memset(m + COMP_UOFF, 128, COMP_SIZE - COMP_UOFF); /* neutral chroma */
        }

        ml_dmabuf_sync(fd, 0);                                 /* flush to DDR for the DC */
        munmap(m, COMP_SIZE);
    }

    guint32 h = 0;
    c->idle_fb = drm_make_fb(c, fd, &h);   /* PRIME-import as a YUV420 scanout FB (keeps fd via GEM) */
    c->idle_dumb = h;

    return c->idle_fb ? 0 : -1;
}

int drm_disp_init(struct ctx *c)
{
    int p[2];

    memset(&c->next_it, 0, sizeof c->next_it);
    memset(&c->front_it, 0, sizeof c->front_it);
    memset(&c->pending_it, 0, sizeof c->pending_it);
    memset(&c->prev_it, 0, sizeof c->prev_it);
    c->next_it.cbi = c->front_it.cbi = c->pending_it.cbi = c->prev_it.cbi = -1;
    c->pending_since = 0;
    c->retire_dumps = getenv("ML_DUMP_RETIRE") ? atoi(getenv("ML_DUMP_RETIRE")) : 0;
    c->modeset_done = 0;   /* re-modeset the first frame of each (re)init - modes swap at runtime */

    /* A fresh graph starts live, not parked on the splash. */
    c->show_idle = c->idle_shown = 0;
    pthread_mutex_init(&c->disp_lock, NULL);

    if (pipe2(p, O_NONBLOCK | O_CLOEXEC)) {
        perror("ml-pipeline: pipe2");
        return -1;
    }

    c->wake_r = p[0];
    c->wake_w = p[1];
    if (drm_find_output(c)) {
        fprintf(stderr, "ml-pipeline: no connected DRM output\n");
        return -1;
    }

    if (c->single) {
        /* single-stream file playback: no composite pool and no overlay planes - each decoder
         * frame's own dmabuf is imported (tile_fb_get) and scanned out on the primary CRTC.
         */
    } else if (c->planes_on) {
        if (plane_scan(c) || plane_prim_init(c)) {
            return -1;
        }
    } else {
        for (int i = 0; i < c->comp_n; i++) {
            c->fb_id[i] = drm_make_fb(c, c->comp_pool[i].fd, &c->fb_handle[i]);
            if (!c->fb_id[i]) {
                return -1;
            }
        }
    }

    c->disp_run = 1;
    return pthread_create(&c->disp_thread, NULL, drm_disp_run, c) ? -1 : 0;
}

void drm_disp_shutdown(struct ctx *c)
{
    c->disp_run = 0;
    pipe_wake(c->wake_w);

    pthread_join(c->disp_thread, NULL);

    if (!c->planes_on && c->idle_fb) {
        /* Park the CRTC on the persistent black FB and wait past the shadow latch BEFORE freeing
         * the per-frame/composite FBs, so the DC is never left fetching a removed buffer (which
         * powers the panel off). The single/composite analogue of the plane-mode dance below.
         */
        drmModeSetCrtc(c->drm_fd, c->crtc_id, c->idle_fb, 0, 0, &c->conn_id, 1, &c->mode);
        usleep(4 * 1000000 / RF_FPS);
    }

    if (c->planes_on) {
        /* Take the video planes off the CRTC before their FBs (and the decoder buffers
         * behind them) go away, and WAIT past the next shadow latch: the EMIT clear only
         * takes effect at a frame edge, and freeing memory the DC is still fetching is a
         * suspected SoC hard-hang. Two frame times covers
         * the +-1-frame latch uncertainty.
         */
        drmModeSetPlane(c->drm_fd, c->vid_plane[0], c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        drmModeSetPlane(c->drm_fd, c->vid_plane[1], c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        usleep(2 * 1000000 / RF_FPS);
        for (int ch = 0; ch < 2; ch++) {
            for (int i = 0; i < c->ntfb[ch]; i++) {
                drmModeRmFB(c->drm_fd, c->tfb[ch][i].fb);
                drmCloseBufferHandle(c->drm_fd, c->tfb[ch][i].handle);
            }
            c->ntfb[ch] = 0;
        }

        if (c->prim_fb) {
            drmModeRmFB(c->drm_fd, c->prim_fb);
        }

        if (c->prim_dumb) {
            struct drm_mode_destroy_dumb dd = { .handle = c->prim_dumb };
            drmIoctl(c->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }

    if (c->single) {
        /* Retire the per-frame FBs imported on channel 0 (same as the composite fb_id cleanup
         * below: both scan out on the primary CRTC). The FBs pinned the decoder dmabufs on the
         * broker fd, so closing them here is what lets the buffers free at the gst NULL that
         * follows.
         */
        for (int i = 0; i < c->ntfb[0]; i++) {
            drmModeRmFB(c->drm_fd, c->tfb[0][i].fb);
            drmCloseBufferHandle(c->drm_fd, c->tfb[0][i].handle);
        }
        c->ntfb[0] = 0;
    }

    for (int i = 0; i < c->comp_n; i++) {
        if (c->fb_id[i]) {
            drmModeRmFB(c->drm_fd, c->fb_id[i]);
        }

        if (c->fb_handle[i]) {
            drmCloseBufferHandle(c->drm_fd, c->fb_handle[i]);
        }

        c->fb_id[i] = 0;        /* a later re-init re-imports these; don't double-remove */
        c->fb_handle[i] = 0;
    }

    ditem_release(&c->prev_it);
    ditem_release(&c->front_it);
    ditem_release(&c->pending_it);
    ditem_release(&c->next_it);

    /* Close the display thread's wake self-pipe so a re-init's pipe2 does not leak fds. */
    if (c->wake_r > 0) {
        close(c->wake_r);
        c->wake_r = -1;
    }

    if (c->wake_w > 0) {
        close(c->wake_w);
        c->wake_w = -1;
    }
}
