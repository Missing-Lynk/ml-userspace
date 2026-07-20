/*
 * ml-rec.c - send a control command to ml-pipeline's ctrl.sock seam (ml-shared/mlm.h).
 *
 * This is exactly what the HUD record button does (pipecmd_record_toggle), usable from a shell when
 * the HUD is not running. ml-pipeline is the source of truth for recording state and reports it back
 * as MLM_T_STATE, so this just sends intent.
 *
 *   ml-rec                toggle recording (start if idle, stop if recording)
 *   ml-rec toggle         same
 *   ml-rec res <H> <FPS>  latch the recording format (MLM_CMD_DVR_RES, e.g. res 1080 60);
 *                         applied at the NEXT recording start, exactly like the HUD's
 *                         dvr.resolution push
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../../ml-shared/mlm.h"

int main(int argc, char **argv)
{
    uint32_t cmd = MLM_CMD_REC_TOGGLE;
    uint32_t arg = 0;

    if (argc > 1 && strcmp(argv[1], "res") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s res <height> <fps>\n", argv[0]);
            return 2;
        }

        cmd = MLM_CMD_DVR_RES;
        arg = ((uint32_t) atoi(argv[2]) << 16) | (uint32_t) atoi(argv[3]);
    } else if (argc > 1 && strcmp(argv[1], "toggle") != 0) {
        fprintf(stderr, "usage: %s [toggle | res <height> <fps>]\n", argv[0]);
        return 2;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct {
        struct mlm_hdr h;
        struct mlm_cmd cmd;
    } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_CMD, .flags = 0 },
        .cmd = { .cmd = cmd, .arg = arg },
    };

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, MLM_CTRL_SOCK, sizeof a.sun_path - 1);
    if (sendto(fd, &rec, sizeof rec, 0, (struct sockaddr *)&a, sizeof a) != (ssize_t)sizeof rec) {
        perror("sendto (is ml-pipeline running?)");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
