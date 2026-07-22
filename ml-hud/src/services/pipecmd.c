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

/* RTSP restream on/off (idempotent set; the pipeline ignores an already-held value). */
void pipecmd_set_rtsp(int on)
{
    mlm_ctrl_send(MLM_CMD_RTSP, on ? 1u : 0u, NULL);
}

/* One rendered burn-in cell: the mlm_osd_cell header + RGBA pixels ride after the mlm_cmd. */
void pipecmd_osd_cell(int row, int col, int x, int y, int w, int h, const unsigned char *rgba)
{
    struct { struct mlm_osd_cell cell; unsigned char px[MLM_OSD_CELL_WMAX * MLM_OSD_CELL_HMAX * 4]; }
        __attribute__((packed)) frame;
    size_t len = sizeof frame.cell;

    if (w <= 0 || h <= 0 || w > MLM_OSD_CELL_WMAX || h > MLM_OSD_CELL_HMAX) {
        return;
    }

    frame.cell.row = (uint16_t) row;
    frame.cell.col = (uint16_t) col;
    frame.cell.x = (uint16_t) x;
    frame.cell.y = (uint16_t) y;
    frame.cell.w = (uint16_t) w;
    frame.cell.h = (uint16_t) h;
    if (rgba != NULL) {
        size_t n = (size_t) w * h * 4;
        memcpy(frame.px, rgba, n);
        len += n;
    }

    mlm_ctrl_send_blob(MLM_CMD_OSD_CELL, 0, &frame, len);
}

/* Clear-all: a header-only cell frame with the MLM_OSD_CLEAR_ALL sentinel coordinates. */
void pipecmd_osd_clear(void)
{
    struct mlm_osd_cell cell;

    memset(&cell, 0, sizeof cell);
    cell.row = MLM_OSD_CLEAR_ALL;
    cell.col = MLM_OSD_CLEAR_ALL;
    mlm_ctrl_send_blob(MLM_CMD_OSD_CELL, 0, &cell, sizeof cell);
}
