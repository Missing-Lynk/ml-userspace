/**
 * @file linkstate.h
 * @brief Air-unit link presence, read from ml-linkd's telemetry seam (ml-shared/mlm.h).
 *
 * ml-linkd owns the RF link and republishes the air unit's :10000 telemetry as MLM_T_STATUS on
 * telemetry.sock (unconditional, independent of video), plus MLM_T_LINK state changes. The HUD binds
 * that seam and treats recent traffic as a live air unit, so the menu can gate the Air Unit section
 * on the link. Air liveness = MLM_T_STATUS flowing; MLM_T_LINK TX_LOST marks it down at once.
 *
 * The HUD is the single telemetry.sock consumer (ml-pipeline and ml-linkd only send to it).
 */
#ifndef HUD_LINKSTATE_H
#define HUD_LINKSTATE_H

#include "../channel/osd_channel.h"

/** @brief Bind the telemetry seam. @return fd on success, -1 on error (the link then reads as down). */
int linkstate_open(void);

/** @brief Drain any pending link/telemetry datagrams and update the cached state. Non-blocking. */
void linkstate_poll(int fd);

/** @brief Register the sink for MLM_T_STATUS records: their payload (the raw 0x09/0x11 `:10000`
 *  status frame carrying voltage + link metrics) is fed to @p cb via osd_channel_dispatch. The
 *  pointers must stay valid for the process lifetime; NULL unregisters. */
void linkstate_set_osd_cb(const osd_channel_cb_t *cb, void *ctx);

/** @brief Whether the air unit is currently connected (its telemetry seen within the staleness window). */
int linkstate_airunit_connected(void);

/** @brief Local baseband link metrics from ml-linkd's MLM_T_LINKINFO. Each returns MLM_LINKINFO_NONE
 *  (-1) until a value has been received / while the link is down, so the System OSD shows a dim
 *  placeholder. Channel is the table index the RX is tuned to (the select value); SNR is in dB;
 *  distance is in metres. */
int linkstate_channel(void);
int linkstate_snr_db(void);
int linkstate_distance_m(void);

/** @brief Whether the air unit is currently in standby (quad disarmed + standby armed), from
 *  ml-linkd's SetStandyMode readback. 0 when active or no link. */
int linkstate_standby(void);

struct mlm_scan;   /* ml-shared/mlm.h */

/** @brief Copy the latest RF channel scan (ml-linkd's MLM_T_SCAN) into @p out. @return the scan
 *  generation, which increments on each scan received; 0 = none yet (and @p out is left untouched),
 *  so callers can re-render only when the generation changes. */
unsigned linkstate_scan(struct mlm_scan *out);

/** @brief ml-pipeline's last-reported mode (MLM_STATE_* from ml-shared/mlm.h). Defaults to
 *  MLM_STATE_IDLE until the pipeline broadcasts. The pipeline re-asserts every second, so this
 *  reconverges after a HUD or pipeline restart. */
int linkstate_pipeline_state(void);

/**
 * @brief Whether a real MLM_T_STATE from ml-pipeline has been received yet. Until it has,
 *  linkstate_pipeline_state() returns the default (IDLE), which callers must not act on - e.g.
 *  auto-record must wait for the true state so it does not toggle a recording it cannot yet see.
 */
int linkstate_pipeline_seen(void);

/**
 * @brief Playback progress from the pipeline's MLM_T_STATE.
 * @param paused  out: 1 if playback is paused (may be NULL).
 * @param pos_ms  out: current position in ms (may be NULL).
 * @param dur_ms  out: clip duration in ms (may be NULL).
 * @return 1 if a file is currently playing back, 0 otherwise.
 */
int linkstate_playback(int *paused, unsigned *pos_ms, unsigned *dur_ms);

/** @brief Whether the current clip has reached end-of-clip (last frame held, awaiting replay/exit). */
int linkstate_playback_ended(void);

/** @brief Whether the first decoded frame of the current clip is on the display (playback visible). */
int linkstate_playback_rendering(void);

/** @brief Close the seam. */
void linkstate_close(int fd);

#endif /* HUD_LINKSTATE_H */
