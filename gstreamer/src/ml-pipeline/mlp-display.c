#include "ml-pipeline.h"

/* Custom DRM/KMS display sink: drives artosyn_vo directly, retires after page-flip. */

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

static gboolean ditem_empty(const struct ditem *it)
{
    return !it->buf && !it->smp[0];
}

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

    if (!ditem_empty(&c->prev_it)) {
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
        if (!ditem_empty(&c->pending_it)) {
            continue;               /* a flip is in flight; wait for its completion event */
        }

        pthread_mutex_lock(&c->disp_lock);
        struct ditem it = c->next_it;
        memset(&c->next_it, 0, sizeof c->next_it);
        c->next_it.cbi = -1;
        pthread_mutex_unlock(&c->disp_lock);
        if (ditem_empty(&it)) {
            continue;
        }

        if (c->planes_on) {
            if (!c->modeset_done) {
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
            }

            if (plane_commit(c, &it)) {
                perror("ml-pipeline: drmModeAtomicCommit");
                ditem_release(&it);
            } else {
                c->pending_it = it;
            }
        } else {
            /* composite: scan out the pool buffer's FB; single: the frame's own imported FB. */
            guint32 fbid = c->single ? it.fb[0] : c->fb_id[it.cbi];
            if (!c->modeset_done) {
                if (drmModeSetCrtc(c->drm_fd, c->crtc_id, fbid, 0, 0,
                                   &c->conn_id, 1, &c->mode)) {
                    perror("ml-pipeline: drmModeSetCrtc");
                    ditem_release(&it);
                    continue;
                }
                c->modeset_done = 1;
                c->front_it = it;   /* on screen now; SetCrtc gives no flip event */
            } else if (drmModePageFlip(c->drm_fd, c->crtc_id, fbid,
                                       DRM_MODE_PAGE_FLIP_EVENT, c)) {
                perror("ml-pipeline: drmModePageFlip");
                ditem_release(&it);
            } else {
                c->pending_it = it;
            }
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
    c->next_pts = pts;
    pthread_mutex_unlock(&c->disp_lock);

    if (!ditem_empty(&displaced)) {
        ditem_release(&displaced);     /* never shown -> released (pool / decoder) */
    }

    char w = 1;
    if (write(c->wake_w, &w, 1) < 0) { }
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

int drm_disp_init(struct ctx *c)
{
    int p[2];

    memset(&c->next_it, 0, sizeof c->next_it);
    memset(&c->front_it, 0, sizeof c->front_it);
    memset(&c->pending_it, 0, sizeof c->pending_it);
    memset(&c->prev_it, 0, sizeof c->prev_it);
    c->next_it.cbi = c->front_it.cbi = c->pending_it.cbi = c->prev_it.cbi = -1;
    c->retire_dumps = getenv("ML_DUMP_RETIRE") ? atoi(getenv("ML_DUMP_RETIRE")) : 0;
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
    char w = 1;

    c->disp_run = 0;
    if (write(c->wake_w, &w, 1) < 0) { }

    pthread_join(c->disp_thread, NULL);
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
    }

    ditem_release(&c->prev_it);
    ditem_release(&c->front_it);
    ditem_release(&c->pending_it);
    ditem_release(&c->next_it);
}
