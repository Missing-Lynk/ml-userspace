/** @file drmoverlay.c @brief See drmoverlay.h. */
#include "drmoverlay.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#include "../../../ml-shared/mlm.h"

#define OV_W 1920
#define OV_H 1080

int drm_overlay_open(drm_overlay_t *d, uint32_t plane_id)
{
    memset(d, 0, sizeof(*d));
    d->drm = mlm_get_drm_fd();
    if (d->drm < 0) {
        fprintf(stderr, "drm-overlay: no DRM fd from %s (is ml-drmfd running?)\n", MLM_DRM_SOCK);
        return -1;
    }

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (ioctl(d->drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        perror("drm-overlay GETRESOURCES");
        return -1;
    }

    uint32_t crtcs[8] = { 0 };
    res.crtc_id_ptr = (uintptr_t) crtcs;
    res.count_fbs = res.count_connectors = res.count_encoders = 0;
    if (ioctl(d->drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        perror("drm-overlay GETRESOURCES2");
        return -1;
    }

    d->crtc_id = crtcs[0];
    d->plane_id = plane_id;
    d->w = OV_W;
    d->h = OV_H;

    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = OV_W; cd.height = OV_H; cd.bpp = 16;
    if (ioctl(d->drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        perror("drm-overlay CREATE_DUMB");
        return -1;
    }

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (ioctl(d->drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        perror("drm-overlay MAP_DUMB");
        return -1;
    }

    d->px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->drm, md.offset);
    if (d->px == MAP_FAILED) {
        perror("drm-overlay mmap");
        d->px = NULL;
        return -1;
    }

    memset(d->px, 0, cd.size);   /* fully transparent */
    d->size = cd.size;
    d->pitch_px = cd.pitch / 2;
    d->handle = cd.handle;

    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof fb);
    fb.width = OV_W; fb.height = OV_H; fb.pixel_format = DRM_FORMAT_ARGB4444;
    fb.handles[0] = cd.handle; fb.pitches[0] = cd.pitch;
    if (ioctl(d->drm, DRM_IOCTL_MODE_ADDFB2, &fb)) {
        perror("drm-overlay ADDFB2");
        return -1;
    }

    d->fb_id = fb.fb_id;
    fprintf(stderr, "drm-overlay: plane %u crtc %u fb %u %dx%d ARGB4444\n",
            d->plane_id, d->crtc_id, d->fb_id, OV_W, OV_H);

    return 0;
}

/* Pack one clamped rectangle of the RGBA surface into the ARGB4444 buffer (A[15:12] R[11:8] G[7:4]
 * B[3:0]). Only these pixels are written - the plane is shared, so we never touch more than changed.
 */
static void pack_rect(drm_overlay_t *d, const surface_t *s, int rx, int ry, int rw, int rh)
{
    int x1 = rx + rw, y1 = ry + rh;
    if (rx < 0) {
        rx = 0;
    }

    if (ry < 0) {
        ry = 0;
    }

    if (x1 > d->w) {
        x1 = d->w;
    }

    if (y1 > d->h) {
        y1 = d->h;
    }

    if (x1 > s->w) {
        x1 = s->w;
    }

    if (y1 > s->h) {
        y1 = s->h;
    }

    for (int y = ry; y < y1; y++) {
        const unsigned char *sp = s->px + ((size_t) y * s->w + rx) * 4;
        uint16_t *dp = d->px + (size_t) y * d->pitch_px + rx;
        for (int x = rx; x < x1; x++) {
            *dp++ = (uint16_t) (((sp[3] >> 4) << 12) | ((sp[0] >> 4) << 8)
                              | ((sp[1] >> 4) << 4) | (sp[2] >> 4));
            sp += 4;
        }
    }
}

/* Re-assert the plane on every present, not just the first. This VO latches plane state and only
 * re-fetches the overlay buffer on a commit, so with no video pipeline flipping the CRTC the HUD
 * must drive its own refresh or its updates never reach the panel. The enable is logged once.
 */
void drm_overlay_enable(drm_overlay_t *d)
{
    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof sp);
    sp.plane_id = d->plane_id;
    sp.crtc_id = d->crtc_id;
    sp.fb_id = d->fb_id;
    sp.crtc_w = d->w; sp.crtc_h = d->h;
    sp.src_w = d->w << 16; sp.src_h = d->h << 16;
    if (ioctl(d->drm, DRM_IOCTL_MODE_SETPLANE, &sp) == 0 && !d->plane_on) {
        d->plane_on = 1;
        fprintf(stderr, "drm-overlay: overlay plane %u enabled\n", d->plane_id);
    }
}

void drm_overlay_clear(drm_overlay_t *d)
{
    if (d->px != NULL) {
        memset(d->px, 0, d->size);
    }
}

void drm_overlay_present(drm_overlay_t *d, const surface_t *s, const rect_t *rects, int nrects)
{
    if (d->px == NULL || s->px == NULL) {
        return;
    }

    if (nrects < 0) {
        pack_rect(d, s, 0, 0, d->w, d->h);
    } else {
        for (int i = 0; i < nrects; i++) {
            pack_rect(d, s, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
        }
    }

    drm_overlay_enable(d);
}

void drm_overlay_close(drm_overlay_t *d)
{
    if (d->drm >= 0 && d->fb_id != 0) {
        struct drm_mode_set_plane sp;
        memset(&sp, 0, sizeof sp);
        sp.plane_id = d->plane_id;
        sp.crtc_id = d->crtc_id;
        sp.fb_id = 0;   /* leave the plane clean */
        ioctl(d->drm, DRM_IOCTL_MODE_SETPLANE, &sp);
    }

    if (d->px != NULL) {
        munmap(d->px, d->size);
    }

    /* The DRM fd is shared (SCM_RIGHTS from ml-drmfd), so closing it here does NOT run drm_release:
     * the FB and dumb buffer live in ml-drmfd's file and would leak. Free them explicitly - our fd
     * indexes the same drm_file, so the destroy reaches them.
     */
    if (d->drm >= 0 && d->fb_id != 0) {
        ioctl(d->drm, DRM_IOCTL_MODE_RMFB, &d->fb_id);
    }

    if (d->drm >= 0 && d->handle != 0) {
        struct drm_mode_destroy_dumb dd;
        memset(&dd, 0, sizeof dd);
        dd.handle = d->handle;
        ioctl(d->drm, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }

    if (d->drm >= 0) {
        close(d->drm);
    }

    d->px = NULL;
    d->fb_id = 0;
    d->handle = 0;
    d->drm = -1;
}
