/*
 * ml-mdump - reference consumer / debug dump for the metadata seam
 * (plans/done/gst-two-process-hud.md). Binds one of the datagram endpoints and
 * prints every record. Anything that speaks this is a stand-in for the HUD.
 *
 * Usage: ml-mdump [telemetry|osd]   (default telemetry)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include "../../../ml-shared/mlm.h"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: survive being killed mid-capture */
    const char *path = MLM_TELEMETRY_SOCK;
    if (argc > 1 && !strcmp(argv[1], "osd")) {
        path = MLM_OSD_SOCK;
    }

    mkdir(MLM_RUN_DIR, 0755);
    unlink(path);
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        return 1;
    }
    printf("ml-mdump: bound %s\n", path);

    unsigned char buf[65536];
    unsigned long n = 0;
    for (;;) {
        ssize_t len = recv(s, buf, sizeof buf, 0);
        if (len < (ssize_t)sizeof(struct mlm_hdr)) {
            continue;
        }
        struct mlm_hdr h;
        memcpy(&h, buf, sizeof h);
        if (h.magic != MLM_MAGIC) {
            printf("[%lu] BAD MAGIC %08x len=%zd\n", n++, h.magic, len);
            continue;
        }
        if (h.type == MLM_T_FRAMESTATS && len >= (ssize_t)(sizeof h + sizeof(struct mlm_framestats))) {
            struct mlm_framestats fs;
            memcpy(&fs, buf + sizeof h, sizeof fs);
            /* print 1/60th to keep the dump readable at 60 fps */
            if (fs.frame_id % 60 == 1 || fs.frame_id < 5) {
                printf("[%lu] framestats id=%u pts=%.3fs\n",
                       n, fs.frame_id, fs.pts_ns / 1e9);
            }
        } else {
            printf("[%lu] type=%04x flags=%04x len=%zd\n", n, h.type, h.flags, len);
        }
        n++;
    }
}
