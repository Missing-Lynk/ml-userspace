/*
 * ml-air-cam.h - SetCameraInfo (0x0C) on the air role.
 *
 * The goggle's camera page rides one selector-tagged union per datagram (mp-cmd.h struct
 * mp_camera); the vendor air applies exactly the selected field. This module is our air's handler.
 * Today it acts on one selector, banding (10, mains anti-flicker): the value is persisted to the
 * same /usrdata/missinglynk/banding file the ml-air-ae init script reads, and a running ml-aed is
 * told to re-read it with SIGHUP, so the correction applies without restarting the AE loop or
 * losing its operating point.
 *
 * Every other selector is drained and logged under verbose: our stack has no ISP call behind
 * them yet, and pretending to apply a value is worse than saying it was ignored.
 */
#ifndef ML_AIR_CAM_H
#define ML_AIR_CAM_H

#include <stdint.h>
#include <sys/types.h>

/*
 * The parse alone, no side effects, for the host test. @return 1 when the datagram is a
 * SetCameraInfo selecting banding, with *hz set to the value the air must run: 0, 50 or 60,
 * anything else forced to 0 exactly as the vendor's handler does. @return 0 for a short
 * datagram or any other selector, *hz untouched.
 */
int air_cam_parse_banding(const uint8_t *dgram, ssize_t n, unsigned int *hz);

/* Act on one 0x0C datagram: parse, persist on change, signal ml-aed. */
void air_cam_set(const uint8_t *dgram, ssize_t n);

#endif /* ML_AIR_CAM_H */
