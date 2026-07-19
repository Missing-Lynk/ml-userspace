/** @file pipecmd.c @brief See pipecmd.h. */
#include "pipecmd.h"

#include "../../../ml-shared/mlm.h"

/* Every command goes out over the shared ctrl.sock helper (mlm_ctrl_send): a connectionless DGRAM
 * addressed to ctrl.sock each time, so a command sent while the pipeline is down is simply dropped
 * and nothing needs reconnecting when it restarts. Only PLAY carries a path; the rest pass NULL.
 */
void pipecmd_record_toggle(void)
{
    mlm_ctrl_send(MLM_CMD_REC_TOGGLE, 0, NULL);
}

/* Play a file (its path travels with the command); the pipeline preempts the live stream. */
void pipecmd_playback_play(const char *path)
{
    mlm_ctrl_send(MLM_CMD_PLAY, 0, path);
}

void pipecmd_playback_pause(void)
{
    mlm_ctrl_send(MLM_CMD_PAUSE, 0, NULL);
}

void pipecmd_playback_resume(void)
{
    mlm_ctrl_send(MLM_CMD_RESUME, 0, NULL);
}

void pipecmd_playback_stop(void)
{
    mlm_ctrl_send(MLM_CMD_STOP, 0, NULL);
}

/* Set play speed; the signed multiplier is bit-cast into the unsigned arg. */
void pipecmd_playback_speed(int speed)
{
    mlm_ctrl_send(MLM_CMD_SPEED, (uint32_t) speed, NULL);
}

/* Park the display on the no-signal splash (the live link dropped); a returning frame clears it. */
void pipecmd_show_nosignal(void)
{
    mlm_ctrl_send(MLM_CMD_SHOW_IDLE, 0, NULL);
}

/* One telemetry subtitle line for the .srt sidecar (the text travels like PLAY's path). */
void pipecmd_srt_text(const char *line)
{
    mlm_ctrl_send(MLM_CMD_SRT_TEXT, 0, line);
}

/* DVR recording format, packed as the wire arg (height << 16 | fps). */
void pipecmd_set_dvr_res(int height, int fps)
{
    mlm_ctrl_send(MLM_CMD_DVR_RES, ((uint32_t) height << 16) | (uint32_t) fps, NULL);
}
