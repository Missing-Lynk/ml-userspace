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

/** @brief ml-pipeline's last-reported mode (MLM_STATE_* from ml-shared/mlm.h). Defaults to
 *  MLM_STATE_IDLE until the pipeline broadcasts. The pipeline re-asserts every second, so this
 *  reconverges after a HUD or pipeline restart. */
int linkstate_pipeline_state(void);

/** @brief Close the seam. */
void linkstate_close(int fd);

#endif /* HUD_LINKSTATE_H */
