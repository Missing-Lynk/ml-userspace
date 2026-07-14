/*
 * ml-drmfd - the DRM master-fd broker (plans/done/gst-two-process-hud.md, design decision 1).
 *
 * Opens /dev/dri/card0 ONCE and hands that same fd to every client via SCM_RIGHTS,
 * so all clients share one open file description = all are DRM master and can each
 * commit to their own plane (pipeline: primary 33, HUD: overlay 38) on the one CRTC.
 * DRM leases can't do this (a lease removes the CRTC from the lessor's namespace).
 *
 * The broker exists so either client can die and rejoin: master status lives with
 * the shared open file description for as long as anyone (at minimum: this broker)
 * holds it. Clients must use blocking legacy SetPlane only - DRM events on the
 * shared fd would land in one shared event queue and get stolen cross-process.
 *
 * Build: gstreamer/src/build.sh. Runs foregrounded; background it from the runtime script.
 */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "../../../ml-shared/mlm.h"

#define DRM_IOCTL_SET_MASTER 0x641e /* _IO('d', 0x1e) */

int main(int argc, char **argv)
{
    const char *card = (argc > 1) ? argv[1] : "/dev/dri/card0";

    int drm = open(card, O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        perror(card);
        return 1;
    }
    /* First opener is master implicitly; assert it anyway so a stale master
     * elsewhere is loud, not a mystery EACCES in the clients. */
    if (ioctl(drm, DRM_IOCTL_SET_MASTER, 0)) {
        fprintf(stderr, "ml-drmfd: WARNING: not DRM master (%s) - another process holds the card?\n",
                strerror(errno));
    }

    mkdir(MLM_RUN_DIR, 0755);
    unlink(MLM_DRM_SOCK);
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, MLM_DRM_SOCK, sizeof a.sun_path - 1);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 8) < 0) {
        perror("listen");
        return 1;
    }

    printf("ml-drmfd: serving %s fd on %s\n", card, MLM_DRM_SOCK);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            return 1;
        }
        /* re-assert master per handout: if this broker was restarted while old
         * clients still held the previous fd, master returns to us once they
         * are gone; a broker restart with live clients cannot reclaim it
         * (supervise the broker, restart it only when clients are down) */
        ioctl(drm, DRM_IOCTL_SET_MASTER, 0);
        if (mlm_send_fd(c, drm)) {
            perror("send_fd");
        }
        close(c);
    }
}
