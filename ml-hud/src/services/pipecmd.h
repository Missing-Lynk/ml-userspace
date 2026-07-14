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

#endif /* HUD_PIPECMD_H */
