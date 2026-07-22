/**
 * @file pipecmd.h
 * @brief Send control commands to ml-pipeline over its ctrl.sock seam (ml-shared/mlm.h).
 *
 * The pipeline is the source of truth for what it is doing; the HUD sends intent (a toggle), and the
 * pipeline reports the resulting mode back as MLM_T_STATE (see linkstate.c). The socket is a
 * connectionless DGRAM, so a command sent while the pipeline is down is simply dropped.
 */
#ifndef HUD_PIPECMD_H
#define HUD_PIPECMD_H

/** @brief Ask ml-pipeline to toggle recording (start if idle, stop if recording). */
void pipecmd_record_toggle(void);

/** @brief Play a file (preempting the live stream). @p path is the absolute clip path. */
void pipecmd_playback_play(const char *path);

/** @brief Pause the current playback (hold the frame). */
void pipecmd_playback_pause(void);

/** @brief Resume paused playback. */
void pipecmd_playback_resume(void);

/** @brief Stop playback and return to the live stream. */
void pipecmd_playback_stop(void);

/** @brief Set play speed: 1 = normal, 2/4/8 fast-forward, -2/-4/-8 rewind. */
void pipecmd_playback_speed(int speed);

/**
 * @brief Ask ml-pipeline to show the no-signal splash instead of the last decoded frame.
 *  Sent when the live RF link drops; the pipeline resumes video on its own when frames return.
 */
void pipecmd_show_nosignal(void);

/**
 * @brief Send one telemetry subtitle line for the recording's .srt sidecar. Only meaningful while
 *  the pipeline is recording (it stamps the line with the recording-relative video time and
 *  appends it as an SRT cue); ignored otherwise. The caller gates on the dvr.save_srt setting.
 */
void pipecmd_srt_text(const char *line);

/**
 * @brief Latch the DVR recording format in ml-pipeline (dvr.resolution). Applied at the NEXT
 *  recording start; a running recording keeps its format. @p height 720 or 1080, @p fps 30 or 60.
 */
void pipecmd_set_dvr_res(int height, int fps);

/**
 * @brief Enable/disable the RTSP restream (dvr.rtsp_stream). Idempotent set: the pipeline ignores
 *  a value it already has, so the HUD re-asserts freely (on change and via the reconcile tick,
 *  which catches a restarted pipeline). While on with no recording active the pipeline runs the
 *  DVR encoder file-less; the actual up-state (enabled AND encoder running) reports back as
 *  MLM_STATE_F_RTSP, so re-asserting on divergence also retries pipeline-side failures.
 */
void pipecmd_set_rtsp(int on);

/**
 * @brief Send one rendered BTFL OSD cell for the DVR burn-in. @p rgba is the cell's w*h RGBA
 *  patch (opaque glyph pixels, transparent background), or NULL to clear the cell pipeline-side.
 *  The rect is the cell's luma-pixel rectangle in the 1080p composite. The caller (btfl_burn)
 *  gates on the dvr.record_osd setting + recording state.
 */
void pipecmd_osd_cell(int row, int col, int x, int y, int w, int h, const unsigned char *rgba);

/** @brief Clear every cached burn-in cell in ml-pipeline (burn gate opened or closed). */
void pipecmd_osd_clear(void);

#endif /* HUD_PIPECMD_H */
