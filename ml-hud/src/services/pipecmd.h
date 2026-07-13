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

#endif /* HUD_PIPECMD_H */
