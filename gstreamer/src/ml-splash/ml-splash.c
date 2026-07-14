/*
 * ml-splash - one-shot boot splash on the video (I420) plane.
 *
 * Paints a raw I420 frame (the vendor's no-signal mountain, nosignal.yuv) on the
 * DRM primary via the ml-drmfd broker fd, then exits. The dumb buffer and fb live
 * on the broker's open file description, so the image stays on the panel after
 * exit - and because it is on the PRIMARY, later video (composite flips on the
 * primary, or the video0/video1 overlay banks) and the fb/OSD overlay simply
 * cover it; when the pipeline stops, the splash is what's underneath again.
 * The vendor does exactly this: /usr/usrdata/nosignal.yuv -> the VO primary.
 *
 *   ml-splash [image.yuv]   (default: <bindir>/../share/nosignal.yuv)
 *
 * The image must be WxH*3/2 bytes of I420 for the connector's native mode.
 * Requires a running ml-drmfd broker (the ml-display boot service / splash-up.sh
 * start one).
 *
 * Raw DRM ioctls only (no libdrm), so it links fully static and can live on the
 * bare rootfs and run at boot, before any SD/squashfs mount.
 *
 * Build: gstreamer/src/build.sh (static).
 */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../../ml-shared/mlm.h"

/* Find the single connected connector and its preferred (first) mode; return the
 * connector id, fill *mode, and set *crtc to the first CRTC. */
static int probe_outputs(int drm, uint32_t *conn_out, uint32_t *crtc_out,
                         struct drm_mode_modeinfo *mode)
{
    struct drm_mode_card_res res = { 0 };

    if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        return -1;
    }
    if (!res.count_crtcs || !res.count_connectors) {
        return -1;
    }

    uint32_t *crtcs = calloc(res.count_crtcs, sizeof *crtcs);
    uint32_t *conns = calloc(res.count_connectors, sizeof *conns);
    int ret = -1;

    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.connector_id_ptr = (uintptr_t)conns;
    res.count_fbs = res.count_encoders = 0;
    if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        goto out;
    }
    *crtc_out = crtcs[0];

    for (uint32_t i = 0; i < res.count_connectors && ret; i++) {
        struct drm_mode_get_connector gc = { .connector_id = conns[i] };

        if (ioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) {
            continue;
        }
        if (gc.connection != 1 /* connected */ || !gc.count_modes) {
            continue;
        }

        struct drm_mode_modeinfo *modes = calloc(gc.count_modes, sizeof *modes);
        gc.modes_ptr = (uintptr_t)modes;
        gc.count_props = gc.count_encoders = 0;
        if (!ioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) {
            *conn_out = conns[i];
            *mode = modes[0];
            ret = 0;
        }
        free(modes);
    }
out:
    free(crtcs);
    free(conns);
    return ret;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    char defpath[512];

    if (!path) {
        ssize_t n = readlink("/proc/self/exe", defpath, sizeof defpath - 32);
        if (n > 0) {
            defpath[n] = 0;
            char *slash = strrchr(defpath, '/');
            if (slash) {
                strcpy(slash, "/../share/nosignal.yuv");
            }
            path = defpath;
        } else {
            path = "/usr/local/share/nosignal.yuv";
        }
    }

    int drm = mlm_get_drm_fd();
    if (drm < 0) {
        fprintf(stderr, "ml-splash: no ml-drmfd broker (start it first)\n");
        return 1;
    }

    uint32_t conn_id = 0, crtc = 0;
    struct drm_mode_modeinfo mode;
    if (probe_outputs(drm, &conn_id, &crtc, &mode)) {
        fprintf(stderr, "ml-splash: no connected connector/CRTC\n");
        return 1;
    }

    uint32_t w = mode.hdisplay, h = mode.vdisplay;
    size_t want = (size_t)w * h * 3 / 2;

    int img = open(path, O_RDONLY);
    struct stat st;
    if (img < 0 || fstat(img, &st)) {
        fprintf(stderr, "ml-splash: cannot open %s\n", path);
        return 1;
    }
    if ((size_t)st.st_size != want) {
        fprintf(stderr, "ml-splash: %s is %lld B, need %zu (I420 %ux%u)\n",
                path, (long long)st.st_size, want, w, h);
        return 1;
    }

    /* one dumb allocation holding all three planes: h*3/2 rows of 8-bpp at the
     * driver's pitch (artosyn_vo aligns to 64 px; 1920 is already aligned) */
    struct drm_mode_create_dumb cd = { .width = w, .height = h * 3 / 2, .bpp = 8 };
    if (ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        perror("ml-splash: create_dumb");
        return 1;
    }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        perror("ml-splash: map_dumb");
        return 1;
    }
    uint8_t *dst = mmap(NULL, cd.size, PROT_WRITE, MAP_SHARED, drm, md.offset);
    if (dst == MAP_FAILED) {
        perror("ml-splash: mmap");
        return 1;
    }

    uint32_t p = cd.pitch, cp = p / 2;
    uint32_t uoff = p * h, voff = uoff + cp * (h / 2);

    /* row-copy each plane (file strides are w / w/2; pitch may be wider) */
    uint8_t *row = malloc(w);
    uint32_t plane_off[3] = { 0, uoff, voff };
    uint32_t plane_w[3] = { w, w / 2, w / 2 };
    uint32_t plane_h[3] = { h, h / 2, h / 2 };
    uint32_t plane_p[3] = { p, cp, cp };
    for (int pl = 0; pl < 3; pl++) {
        for (uint32_t y = 0; y < plane_h[pl]; y++) {
            if (read(img, row, plane_w[pl]) != (ssize_t)plane_w[pl]) {
                goto short_read;
            }
            memcpy(dst + plane_off[pl] + (size_t)y * plane_p[pl], row, plane_w[pl]);
        }
    }
    free(row);
    close(img);
    munmap(dst, cd.size);

    struct drm_mode_fb_cmd2 fb = {
        .width = w, .height = h, .pixel_format = DRM_FORMAT_YUV420,
        .handles = { cd.handle, cd.handle, cd.handle, 0 },
        .pitches = { p, cp, cp, 0 },
        .offsets = { 0, uoff, voff, 0 },
    };
    if (ioctl(drm, DRM_IOCTL_MODE_ADDFB2, &fb)) {
        perror("ml-splash: AddFB2(YUV420)");
        return 1;
    }

    struct drm_mode_crtc sc = {
        .set_connectors_ptr = (uintptr_t)&conn_id,
        .count_connectors = 1,
        .crtc_id = crtc, .fb_id = fb.fb_id,
        .mode_valid = 1, .mode = mode,
    };
    if (ioctl(drm, DRM_IOCTL_MODE_SETCRTC, &sc)) {
        perror("ml-splash: SetCrtc");
        return 1;
    }
    fprintf(stderr, "ml-splash: %ux%u splash up (fb %u on crtc %u), exiting - "
            "frame persists on the broker fd\n", w, h, fb.fb_id, crtc);
    return 0;

short_read:
    fprintf(stderr, "ml-splash: short read from %s\n", path);
    return 1;
}
