/*
 * ml-rx-udp.h - the RX role's UDP thread.
 */
#ifndef ML_RX_UDP_H
#define ML_RX_UDP_H

/* Runs the :20001 hello/ack and the :10000 handshake + telemetry drain, serves link.sock (the
 * consumer READY gate and the HUD's RF commands) and drives the status LED. Runs until g_run
 * clears. */
void *rx_udp_thread(void *arg);

#endif /* ML_RX_UDP_H */
