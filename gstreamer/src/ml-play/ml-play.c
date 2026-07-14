/*
 * ml-play - send a playback command to ml-pipeline's ctrl.sock (ml-shared/mlm.h). The HUD's
 * Playback tab sends the same commands; this is the CLI for bring-up/testing without the menu.
 *
 *   ml-play <file>            play a file (preempts the live stream)
 *   ml-play pause|resume|stop
 *   ml-play seek <permille>   seek to 0..1000 of the duration
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../ml-shared/mlm.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file> | pause | resume | stop | seek <permille>\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "pause")) {
        return mlm_ctrl_send(MLM_CMD_PAUSE, 0, NULL) ? 1 : 0;
    }

    if (!strcmp(argv[1], "resume")) {
        return mlm_ctrl_send(MLM_CMD_RESUME, 0, NULL) ? 1 : 0;
    }

    if (!strcmp(argv[1], "stop")) {
        return mlm_ctrl_send(MLM_CMD_STOP, 0, NULL) ? 1 : 0;
    }

    if (!strcmp(argv[1], "seek")) {
        return mlm_ctrl_send(MLM_CMD_SEEK, argc > 2 ? (unsigned) atoi(argv[2]) : 0, NULL) ? 1 : 0;
    }

    return mlm_ctrl_send(MLM_CMD_PLAY, 0, argv[1]) ? 1 : 0;
}
