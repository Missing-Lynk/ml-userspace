#include "ml-pipeline.h"

/* What outlives one ml-pipeline generation on ml-drmfd's shared drm_file, and how the next one
 * copes with it: the adopted splash (idle) FB that is deliberately left behind, and the reclaim of
 * everything a crashed predecessor left behind by accident. Init-time only; nothing here runs on
 * the frame path (mlp-display.c).
 */

/* Close the per-plane GEM handles a drmModeGetFB2 created (distinct ones once). */
static void fb2_close_handles(struct ctx *c, const drmModeFB2 *info)
{
    for (int plane = 0; plane < 4; plane++) {
        int seen_before = 0;

        if (!info->handles[plane]) {
            continue;
        }

        for (int earlier = 0; earlier < plane; earlier++) {
            if (info->handles[earlier] == info->handles[plane]) {
                seen_before = 1;
            }
        }

        if (!seen_before) {
            drmCloseBufferHandle(c->drm_fd, info->handles[plane]);
        }
    }
}

/* Where the splash FB's id is published for the next generation of this process. */
#define IDLE_FB_STATE   MLM_RUN_DIR "/idle-fb"

/* Every FB lives on ml-drmfd's shared drm_file, and a client's exit does not run drm_release,
 * so an FB this process creates and does not remove stays allocated for as long as ml-drmfd
 * lives: one leaked COMP_SIZE buffer per ml-pipeline generation. Removing the splash at exit is
 * not available, because drm_disp_shutdown parks the CRTC on it precisely so the DC is never
 * left fetching freed memory, and drm_framebuffer_remove would then disable the plane still
 * scanning it. The id is published instead and the next generation adopts it, so the buffer is
 * allocated once per ml-drmfd lifetime rather than once per pipeline lifetime.
 *
 * ml-drmfd unlinks and re-binds drm.sock on every start, so that socket identifies the broker
 * instance, and with it the lifetime of every FB on the shared file. Inode and creation time
 * together, because a tmpfs reuses inode numbers.
 */
static int drm_broker_id(char *out, size_t len)
{
    struct stat st;

    if (stat(MLM_DRM_SOCK, &st)) {
        return -1;
    }

    return snprintf(out, len, "%llu.%lld.%ld", (unsigned long long)st.st_ino,
                    (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec) > 0 ? 0 : -1;
}

/* The splash FB a previous generation left behind, or 0 when there is nothing to adopt. */
static guint32 idle_fb_adopt(struct ctx *c)
{
    char now[64], was[64];
    unsigned fb = 0;
    drmModeFB2Ptr info;
    FILE *f;

    /* Declining to adopt is the pre-adoption behaviour exactly, so one binary measures both
     * sides of the leak across a restart series.
     */
    if (getenv("ML_NOFBADOPT")) {
        return 0;
    }

    if (drm_broker_id(now, sizeof now)) {
        return 0;
    }

    f = fopen(IDLE_FB_STATE, "r");
    if (!f) {
        return 0;
    }

    if (fscanf(f, "%63s %u", was, &fb) != 2) {
        fb = 0;
    }

    fclose(f);
    if (!fb || strcmp(was, now)) {
        /* a different broker: its FBs died with it */
        return 0;
    }

    /* The id alone is not proof: check the FB behind it is still the splash and not something
     * a later AddFB2 was handed the same id for.
     */
    info = drmModeGetFB2(c->drm_fd, fb);
    if (!info) {
        return 0;
    }

    if (info->width != COMP_W || info->height != COMP_H ||
        info->pixel_format != DRM_FORMAT_YUV420) {
        fb = 0;
    }

    /* GETFB2 creates a GEM handle per plane for the caller; libdrm's FreeFB2 does not close
     * them, and on the shared drm_file an unclosed handle outlives this process. */
    fb2_close_handles(c, info);
    drmModeFreeFB2(info);
    if (fb) {
        fprintf(stderr, "ml-pipeline: adopted splash fb %u from the previous generation\n", fb);
    }

    return fb;
}

static void idle_fb_publish(guint32 fb)
{
    char now[64];
    FILE *f;

    if (drm_broker_id(now, sizeof now)) {
        return;
    }

    f = fopen(IDLE_FB_STATE, "w");
    if (!f) {
        return;
    }

    fprintf(f, "%s %u\n", now, fb);
    fclose(f);
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

    c->idle_fb = idle_fb_adopt(c);
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

    if (!c->idle_fb) {
        return -1;
    }

    idle_fb_publish(c->idle_fb);

    return 0;
}

/* Orphan reclaim.
 *
 * Everything a generation creates on ml-drmfd's shared drm_file outlives it: the kernel releases a
 * drm_file's FBs and GEM handles only when the file's LAST reference closes, and ml-drmfd keeps
 * one. A graceful exit removes its own (drm_disp_shutdown); an ungraceful one (SIGKILL, a crash)
 * leaves the compose pool behind - most of the CMA the pipeline needs, pinned by the FBs, their
 * importing GEM handles and, through them, the display controller's dma_buf attachment. The next
 * generation's comp_pool_init then OOMs: OSD alive, video black, only a reboot recovered it.
 *
 * Those objects are reclaimable from here: RmFB drops the FB's references, closing every handle
 * that still references the imported object frees it, and freeing it detaches the controller and
 * returns the dma_buf to CMA, so the restart allocates a fresh pool and runs at full rate. The one
 * FB still on the CRTC (the frozen frame) is released by parking the CRTC on the idle FB first; one
 * bound to an overlay plane by taking the plane off.
 *
 * An orphan is a YUV420 FB backed by an imported dma_buf that is not the idle FB and not one of
 * ours: the compose pool, plane-mode tile FBs, playback per-frame FBs. ml-hud's ARGB dumb FB and
 * the plane-mode XRGB primary are native objects and never match.
 *
 * Handles are enumerated by probing MAP_DUMB over the handle space: the DMA GEM helpers refuse to
 * dumb-map an imported object (EINVAL), a native one gets its mmap offset (0), an unused handle is
 * ENOENT. An imported handle is tied to its dma_buf by exporting it (no new object) and fstat'ing
 * the fd, which is closed at once.
 */
#define RECLAIM_PROBE_HANDLES   4096
#define RECLAIM_MAX_HANDLES     256
#define RECLAIM_MAX_FBS         64

struct reclaim_handle {
    guint32 handle;
    ino_t ino;
};

struct reclaim_fb {
    guint32 fb;
    ino_t ino;
    guint64 size;
    guint32 on_plane;   /* overlay plane id it is bound to, 0 if none */
    int on_crtc;        /* bound to the CRTC primary */
};

/* Imported GEM handles on the shared file, each with its dma_buf inode. */
static int reclaim_probe_handles(struct ctx *c, struct reclaim_handle *out, int max)
{
    int n = 0;

    for (guint32 handle = 1; handle < RECLAIM_PROBE_HANDLES && n < max; handle++) {
        struct drm_mode_map_dumb map_dumb = { .handle = handle };
        struct stat st;
        int fd;

        if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) == 0 || errno != EINVAL) {
            continue;
        }

        if (drmPrimeHandleToFD(c->drm_fd, handle, DRM_CLOEXEC, &fd)) {
            continue;
        }

        if (fstat(fd, &st) == 0) {
            out[n].handle = handle;
            out[n].ino = st.st_ino;
            n++;
        }
        close(fd);
    }

    return n;
}

static int is_own_fb(const struct ctx *c, guint32 fb)
{
    if (fb == c->idle_fb || fb == c->prim_fb) {
        return 1;
    }

    for (int i = 0; i < COMP_POOL; i++) {
        if (c->fb_id[i] == fb) {
            return 1;
        }
    }

    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < c->ntfb[ch]; i++) {
            if (c->tfb[ch][i].fb == fb) {
                return 1;
            }
        }
    }

    return 0;
}

/* Orphan candidates among the FBs on the shared file, with their scanout binding. */
static int reclaim_scan_fbs(struct ctx *c, struct reclaim_fb *out, int max)
{
    drmModeRes *res = drmModeGetResources(c->drm_fd);
    drmModeCrtc *crtc;
    drmModePlaneRes *planes;
    guint32 crtc_fb = 0;
    int n = 0;

    if (!res) {
        return 0;
    }

    for (int i = 0; i < res->count_fbs && n < max; i++) {
        drmModeFB2 *info = drmModeGetFB2(c->drm_fd, res->fbs[i]);
        struct drm_mode_map_dumb map_dumb;
        struct stat st;
        int fd;

        if (!info) {
            continue;
        }

        if (info->pixel_format != DRM_FORMAT_YUV420 || !info->handles[0] || is_own_fb(c, info->fb_id)) {
            fb2_close_handles(c, info);
            drmModeFreeFB2(info);
            continue;
        }

        map_dumb.handle = info->handles[0];
        map_dumb.pad = 0;
        map_dumb.offset = 0;
        if (drmIoctl(c->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) == 0 || errno != EINVAL ||
            drmPrimeHandleToFD(c->drm_fd, info->handles[0], DRM_CLOEXEC, &fd)) {
            fb2_close_handles(c, info);   /* native, or not exportable: not ours to reclaim */
            drmModeFreeFB2(info);
            continue;
        }

        if (fstat(fd, &st) == 0) {
            out[n].fb = info->fb_id;
            out[n].ino = st.st_ino;
            out[n].size = st.st_size;
            out[n].on_plane = 0;
            out[n].on_crtc = 0;
            n++;
        }
        close(fd);
        fb2_close_handles(c, info);
        drmModeFreeFB2(info);
    }
    drmModeFreeResources(res);

    if (n == 0) {
        return 0;
    }

    crtc = drmModeGetCrtc(c->drm_fd, c->crtc_id);
    if (crtc) {
        crtc_fb = crtc->buffer_id;
        drmModeFreeCrtc(crtc);
    }

    /* With UNIVERSAL_PLANES set on the shared file the primary plane is listed too; it carries
     * the CRTC's FB and is parked, not switched off. */
    planes = drmModeGetPlaneResources(c->drm_fd);
    if (planes) {
        for (guint32 i = 0; i < planes->count_planes; i++) {
            drmModePlane *plane = drmModeGetPlane(c->drm_fd, planes->planes[i]);

            if (!plane) {
                continue;
            }

            for (int k = 0; k < n; k++) {
                if (plane->fb_id && plane->fb_id == out[k].fb && plane->fb_id != crtc_fb) {
                    out[k].on_plane = planes->planes[i];
                }
            }
            drmModeFreePlane(plane);
        }
        drmModeFreePlaneResources(planes);
    }

    for (int k = 0; k < n; k++) {
        if (out[k].fb == crtc_fb) {
            out[k].on_crtc = 1;
        }
    }

    return n;
}

static long cma_free_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = -1;

    if (!f) {
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "CmaFree: %ld kB", &kb) == 1) {
            break;
        }
    }

    fclose(f);
    return kb;
}

/* Remove one orphan FB and close every handle of its object. Returns handles closed. */
static int reclaim_one(struct ctx *c, const struct reclaim_fb *fb,
                       const struct reclaim_handle *handles, int nhandles)
{
    int closed = 0;

    drmModeRmFB(c->drm_fd, fb->fb);
    for (int i = 0; i < nhandles; i++) {
        if (handles[i].ino == fb->ino) {
            drmCloseBufferHandle(c->drm_fd, handles[i].handle);
            closed++;
        }
    }

    return closed;
}

/* Free what a crashed predecessor left on the shared drm_file. Runs before this generation
 * imports anything of its own (the idle FB is the exception and is excluded by id). Returns the
 * number of FBs reclaimed. ML_NORECLAIM=1 declines, which is the pre-fix behaviour exactly.
 */
int comp_pool_reclaim(struct ctx *c)
{
    static struct reclaim_handle handles[RECLAIM_MAX_HANDLES];
    static struct reclaim_fb fbs[RECLAIM_MAX_FBS];
    int nfbs, nhandles, reclaimed = 0, closed = 0, parked = 0;
    guint64 bytes = 0;
    long cma_before, cma_after;

    if (getenv("ML_NORECLAIM")) {
        return 0;
    }

    if (!c->crtc_id && drm_find_output(c)) {
        return 0;
    }

    nfbs = reclaim_scan_fbs(c, fbs, RECLAIM_MAX_FBS);
    if (nfbs == 0) {
        return 0;
    }

    cma_before = cma_free_kb();
    nhandles = reclaim_probe_handles(c, handles, RECLAIM_MAX_HANDLES);

    /* Unbound orphans first: nothing is fetching them. */
    for (int i = 0; i < nfbs; i++) {
        if (fbs[i].on_crtc || fbs[i].on_plane) {
            continue;
        }

        closed += reclaim_one(c, &fbs[i], handles, nhandles);
        bytes += fbs[i].size;
        reclaimed++;
    }

    /* Bound orphans: take them off the scanout and wait past the shadow latch before removing
     * them, so the DC never fetches freed memory (the documented hang). An overlay plane is
     * switched off; the CRTC is parked on the idle FB, and left alone when there is none (the
     * first frame's modeset displaces that FB and a later generation reclaims it).
     */
    for (int i = 0; i < nfbs; i++) {
        if (fbs[i].on_plane) {
            drmModeSetPlane(c->drm_fd, fbs[i].on_plane, c->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            usleep(2 * 1000000 / RF_FPS);
            closed += reclaim_one(c, &fbs[i], handles, nhandles);
            bytes += fbs[i].size;
            reclaimed++;
        } else if (fbs[i].on_crtc && c->idle_fb) {
            if (!parked) {
                if (drmModeSetCrtc(c->drm_fd, c->crtc_id, c->idle_fb, 0, 0, &c->conn_id, 1, &c->mode)) {
                    perror("ml-pipeline: drmModeSetCrtc(park for reclaim)");
                    continue;
                }

                usleep(4 * 1000000 / RF_FPS);
                parked = 1;
            }

            closed += reclaim_one(c, &fbs[i], handles, nhandles);
            bytes += fbs[i].size;
            reclaimed++;
        }
    }

    cma_after = cma_free_kb();
    fprintf(stderr, "ml-pipeline: reclaimed %d orphan FB(s) (%d handle(s), %llu KiB) left by a crashed "
                    "predecessor; CmaFree %ld -> %ld kB\n",
            reclaimed, closed, (unsigned long long)(bytes / 1024), cma_before, cma_after);

    return reclaimed;
}
