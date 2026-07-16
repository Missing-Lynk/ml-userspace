/**
 * @file linkcmd.h
 * @brief Send air-unit RF config commands to ml-linkd over its link.sock seam (ml-shared/mlm.h).
 *
 * ml-linkd owns /dev/artosyn_sdio and the :10000 message channel; the HUD never touches the air
 * directly. It sends intent, and ml-linkd applies it (and re-applies after a session restart). The
 * HUD re-asserts on every link-up edge so the air converges to the menu's state - a menu default
 * that was never toggled still needs pushing. Connectionless DGRAM: a command sent while ml-linkd
 * is down is simply dropped.
 */
#ifndef HUD_LINKCMD_H
#define HUD_LINKCMD_H

/** @brief Arm (1) or disarm (0) the air unit's standby mode (rides SetTranParm byte[8]). */
void linkcmd_set_standby(int arm);

/** @brief Set the air unit's TX power from a menu level label ("25 mW"/"100 mW"/"200 mW"); rides
 *  SetTranParm byte[0]. The label -> mW map is the single source of truth (linkcmd.c); an unknown
 *  label falls back to 100 mW. ml-linkd maps the mW to dBm and drops any value it does not know. */
void linkcmd_set_power(const char *level);

/** @brief Set the air unit's video bitrate from a menu level label ("8 Mbps"/"16 Mbps"/"24 Mbps");
 *  rides SetLdCfg bitrate_q at association, so it takes effect on the next session. The label ->
 *  Mbps map is the single source of truth (linkcmd.c); an unknown label falls back to 24 Mbps.
 *  ml-linkd drops any value it does not know. */
void linkcmd_set_bitrate(const char *level);

#endif /* HUD_LINKCMD_H */
