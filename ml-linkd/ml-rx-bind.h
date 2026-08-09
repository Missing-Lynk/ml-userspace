/*
 * ml-rx-bind.h - the RX role's pair sequence: bind a new air unit on a HUD request.
 */
#ifndef ML_RX_BIND_H
#define ML_RX_BIND_H

#include <stdint.h>

/* An air unit is currently alive (fresh :10000 telemetry). Gates binding: pair-locking a new peer
 * while a bound air unit is up could re-pair away a flying quad. */
int rx_bind_air_alive(long now);

/* GET_PAIR reply handler, called from the reader thread. */
void rx_bind_on_pair(const uint8_t *payload, int plen);

/* Queue a bind for the bb-socket TX thread; refused here (with the HUD's failure cue) while an air
 * unit is alive. @p persist writes the locked peer into the config as well as the chip. */
void rx_bind_request(int persist, long now);

/* Run a queued bind. bb-socket TX thread (main) only; blocks its cadence for up to the pair
 * window, which is safe because the liveness gate means there is no session to disturb. */
void rx_bind_service(uint8_t *frame, uint32_t *seq_link);

#endif /* ML_RX_BIND_H */
