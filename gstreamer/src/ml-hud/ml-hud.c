/*
 * ml-hud - first-cut HUD: live framecount on the DRM overlay plane
 * (plans/done/gst-two-process-hud.md, Phase B; the seed of the libre menu's DRM
 * overlay display-HAL backend).
 *
 * Gets the shared DRM master fd from ml-drmfd, puts a 1920x1080 ARGB4444 dumb
 * framebuffer on the overlay plane (38), binds the telemetry socket, and
 * renders "F <frame_id>" (stb_truetype, same rasterizer as native/fbtext.c)
 * for every MLM_T_FRAMESTATS record the pipeline emits. The plane enable
 * retries until the CRTC is active (the pipeline's kmssink does the modeset),
 * and the plane is disabled again on SIGINT/SIGTERM.
 *
 * Raw DRM ioctls, no libdrm; blocking legacy SetPlane only (shared-fd rule).
 *
 * Usage: ml-hud [font.ttf] [plane-id]
 *   default font: /mnt/gst/usr/share/fonts/dejavu/DejaVuSans.ttf
 * Build: gstreamer/src/build.sh (static, -lm for stb_truetype).
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../../../ml-shared/mlm.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#define W 1920
#define H 1080
#define DEFAULT_FONT "/mnt/gst/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define TEXT_PX 56          /* glyph pixel height */
#define BOX_X 1310          /* text box, top-right (testsrc2 draws its own counter top-left) */
#define BOX_Y 40
#define BOX_W 560
#define BOX_H 80

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* --- tiny glyph cache -------------------------------------------------- */
struct glyph {
    unsigned char *cov;
    int w, h, xoff, yoff, advance;
};

static stbtt_fontinfo g_font;
static float g_scale;
static int g_ascent;
static struct glyph g_glyphs[128];

static const struct glyph *get_glyph(int ch)
{
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    struct glyph *g = &g_glyphs[ch];
    if (!g->cov && !g->advance) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, ch, &adv, &lsb);
        g->advance = (int)(adv * g_scale + 0.5f);
        g->cov = stbtt_GetCodepointBitmap(&g_font, g_scale, g_scale, ch,
                                          &g->w, &g->h, &g->xoff, &g->yoff);
    }
    return g;
}

/* draw string into the ARGB4444 buffer; straight (non-premultiplied) alpha,
 * which the DC composites correctly (validated in Phase 0) */
static void draw_text(uint16_t *px, int pitch_px, const char *s)
{
    /* clear the box to semi-transparent black for readability */
    for (int y = BOX_Y; y < BOX_Y + BOX_H; y++) {
        for (int x = BOX_X; x < BOX_X + BOX_W; x++) {
            px[y * pitch_px + x] = 0x8000;
        }
    }

    int pen_x = BOX_X + 16, base_y = BOX_Y + 12 + g_ascent;
    for (; *s; s++) {
        const struct glyph *g = get_glyph(*s);
        if (!g->cov) {
            pen_x += g->advance;
            continue;
        }
        for (int gy = 0; gy < g->h; gy++) {
            int y = base_y + g->yoff + gy;
            if (y < BOX_Y || y >= BOX_Y + BOX_H) {
                continue;
            }
            for (int gx = 0; gx < g->w; gx++) {
                int cov = g->cov[gy * g->w + gx] >> 4;
                if (!cov) {
                    continue;
                }
                int x = pen_x + g->xoff + gx;
                if (x < BOX_X || x >= BOX_X + BOX_W) {
                    continue;
                }
                /* white text; keep the box's floor alpha under faint pixels */
                int a = cov > 8 ? cov : 8;
                px[y * pitch_px + x] = (a << 12) | (cov << 8) | (cov << 4) | cov;
            }
        }
        pen_x += g->advance;
    }
}

int main(int argc, char **argv)
{
    const char *font_path = (argc > 1) ? argv[1] : DEFAULT_FONT;
    uint32_t plane_id = (argc > 2) ? atoi(argv[2]) : 38;

    /* font */
    FILE *f = fopen(font_path, "rb");
    if (!f) {
        perror(font_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *fbuf = malloc(fsz);
    if (fread(fbuf, 1, fsz, f) != (size_t)fsz) {
        perror("fread");
        return 1;
    }
    fclose(f);
    if (!stbtt_InitFont(&g_font, fbuf, stbtt_GetFontOffsetForIndex(fbuf, 0))) {
        fprintf(stderr, "ml-hud: bad font %s\n", font_path);
        return 1;
    }
    g_scale = stbtt_ScaleForPixelHeight(&g_font, TEXT_PX);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    g_ascent = (int)(ascent * g_scale + 0.5f);

    /* DRM: shared master fd, CRTC, ARGB4444 dumb fb */
    int drm = mlm_get_drm_fd();
    if (drm < 0) {
        fprintf(stderr, "ml-hud: cannot get DRM fd from %s (is ml-drmfd running?)\n", MLM_DRM_SOCK);
        return 1;
    }
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        perror("GETRESOURCES");
        return 1;
    }
    uint32_t crtcs[8] = { 0 };
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.count_fbs = res.count_connectors = res.count_encoders = 0;
    if (ioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        perror("GETRESOURCES2");
        return 1;
    }
    uint32_t crtc_id = crtcs[0];

    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    cd.width = W; cd.height = H; cd.bpp = 16;
    if (ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        perror("CREATE_DUMB");
        return 1;
    }
    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        perror("MAP_DUMB");
        return 1;
    }
    uint16_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset);
    if (px == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(px, 0, cd.size); /* fully transparent */
    int pitch_px = cd.pitch / 2;

    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof fb);
    fb.width = W; fb.height = H; fb.pixel_format = DRM_FORMAT_ARGB4444;
    fb.handles[0] = cd.handle; fb.pitches[0] = cd.pitch;
    if (ioctl(drm, DRM_IOCTL_MODE_ADDFB2, &fb)) {
        perror("ADDFB2");
        return 1;
    }

    /* telemetry: consumer binds */
    mkdir(MLM_RUN_DIR, 0755);
    unlink(MLM_TELEMETRY_SOCK);
    int ts = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un ta = { .sun_family = AF_UNIX };
    strncpy(ta.sun_path, MLM_TELEMETRY_SOCK, sizeof ta.sun_path - 1);
    if (bind(ts, (struct sockaddr *)&ta, sizeof ta) < 0) {
        perror("bind telemetry");
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    printf("ml-hud: plane %u crtc %u fb %u font %s; waiting for framestats ...\n",
           plane_id, crtc_id, fb.fb_id, font_path);

    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof sp);
    sp.plane_id = plane_id; sp.crtc_id = crtc_id; sp.fb_id = fb.fb_id;
    sp.crtc_w = W; sp.crtc_h = H;
    sp.src_w = W << 16; sp.src_h = H << 16;

    int plane_on = 0;
    unsigned long records = 0;
    while (!g_stop) {
        struct pollfd pfd = { .fd = ts, .events = POLLIN };
        if (poll(&pfd, 1, 250) <= 0) {
            continue;
        }
        unsigned char buf[512];
        ssize_t len = recv(ts, buf, sizeof buf, 0);
        struct mlm_hdr h;
        if (len < (ssize_t)(sizeof h + sizeof(struct mlm_framestats))) {
            continue;
        }
        memcpy(&h, buf, sizeof h);
        if (h.magic != MLM_MAGIC || h.type != MLM_T_FRAMESTATS) {
            continue;
        }
        struct mlm_framestats fs;
        memcpy(&fs, buf + sizeof h, sizeof fs);
        records++;

        char text[64];
        snprintf(text, sizeof text, "F %u", fs.frame_id);
        draw_text(px, pitch_px, text);

        if (!plane_on) {
            /* CRTC becomes active once the pipeline's kmssink modesets; retry */
            if (ioctl(drm, DRM_IOCTL_MODE_SETPLANE, &sp) == 0) {
                plane_on = 1;
                printf("ml-hud: overlay enabled at frame %u\n", fs.frame_id);
            }
        }
    }

    sp.fb_id = 0;
    ioctl(drm, DRM_IOCTL_MODE_SETPLANE, &sp); /* leave the plane clean */
    printf("ml-hud: %lu records, exiting\n", records);
    return 0;
}
