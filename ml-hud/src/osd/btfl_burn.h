/**
 * @file btfl_burn.h
 * @brief Renders changed BTFL OSD cells for the DVR burn-in and sends them to ml-pipeline.
 *
 * The DVR burn-in (dvr.record_osd) records the BTFL OSD into the video itself. The pipeline
 * cannot render glyphs (no font, no canvas parser), so the HUD does: this module keeps its own
 * glyph grid, diffed per canvas exactly like btfl_osd, and for each changed cell renders the
 * glyph with the SAME loaded MSP font and the SAME cell rectangle (btfl_osd_cell_rect) into a
 * small RGBA patch sent over ctrl.sock (MLM_CMD_OSD_CELL, services/pipecmd). ml-pipeline caches
 * the patches and overwrites their opaque pixels into every recorded composite, so the recording
 * shows exactly the glyphs the panel shows. Only the BTFL OSD is sent - never the System OSD bar
 * or the menu, which live outside this module.
 *
 * The grid state is independent of btfl_osd's, so the burn keeps updating while the menu hides
 * the on-screen OSD (the recording keeps its OSD during menu use).
 */
#ifndef HUD_BTFL_BURN_H
#define HUD_BTFL_BURN_H

/**
 * @brief Reset the burn grid so the next update re-sends every occupied cell. Call on the
 *        burn-gate rising edge (recording started / dvr.record_osd switched on), after clearing
 *        the pipeline's cache (pipecmd_osd_clear), so HUD and pipeline restart in sync.
 */
void btfl_burn_invalidate(void);

/**
 * @brief Decode @p canvas, diff against the burn grid, and send each changed cell to ml-pipeline
 *        (a rendered RGBA patch, or a clear for a now-empty cell). Call per received canvas while
 *        the burn gate is open; a no-op without a loaded font.
 */
void btfl_burn_update(const unsigned char *canvas, int len);

#endif /* HUD_BTFL_BURN_H */
