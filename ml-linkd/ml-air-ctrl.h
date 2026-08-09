/*
 * ml-air-ctrl.h - one-shot requests to ml-air-video's control socket.
 */
#ifndef ML_AIR_CTRL_H
#define ML_AIR_CTRL_H

#define AIR_CTRL_SOCK        "/run/missinglynk/air-video.sock"
#define AIR_CTRL_TIMEOUT_MS  200   /* reply budget */

/* Send one command line. @return 0 when ml-air-video answered "ok". */
int air_ctrl_send(const char *cmd);

#endif /* ML_AIR_CTRL_H */
