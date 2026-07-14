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

#endif /* HUD_LINKCMD_H */
