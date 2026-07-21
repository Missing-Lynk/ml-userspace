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

/** @brief Set one air-unit camera (ISP) field: @p sel is an mlm.h MLM_CAM_* selector, @p value its
 *  u16 payload (exposure: 0 = auto, else the manual exposure time in us). Rides SetCameraInfo
 *  (:10000 msg 0x0C), applied live by the air; rotation blips the feed, the rest are seamless.
 *  ml-linkd validates the selector and clamps the value. */
void linkcmd_set_camera(unsigned sel, unsigned value);

/** @brief Set the air unit's VIN scale: @p aspect_4_3 (0 = 16:9, 1 = 4:3) and @p zoom_pct (zoom
 *  factor in percent, 100 or 70 - the two stock values). Rides SetScaleMode (:10000 msg 0x15),
 *  applied live; a change blips the feed (geometry restart). Both fields ride one message, so both
 *  are always sent together. */
void linkcmd_set_scale(int aspect_4_3, unsigned zoom_pct);

/** @brief Request a one-shot RF channel scan; ml-linkd fires it and publishes the result as a scan
 *  telemetry record (read by linkstate_scan). Read-only - the sweep self-restores the active channel. */
void linkcmd_request_scan(void);

/** @brief Tune the local RX to channel table index @p idx (0..18, the value the scan reports and the
 *  tiles show - passed verbatim, no +1). Unlike the setters above this is LOCAL: nothing is sent to
 *  the air unit, which follows the retune over its own management link. ml-linkd queues it onto the
 *  bb-socket thread and rejects an index outside the table. The tune is async: read the applied
 *  channel back from linkstate_channel(), do not assume it took. */
void linkcmd_select_channel(unsigned idx);

/** @brief Start binding a new air unit: ml-linkd runs the pair sequence and persists the peer. The
 *  caller must gate this on no air unit being connected (ml-linkd refuses otherwise, so nothing
 *  happens mid-flight). Progress arrives back as MLM_T_LINK BINDING/BIND_OK/BIND_FAIL
 *  (linkstate_is_binding / linkstate_bind_result). */
void linkcmd_bind(void);

#endif /* HUD_LINKCMD_H */
