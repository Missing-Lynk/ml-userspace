/*
 * ml-linkd.h - symbols shared between the two roles: the RX/goggle side (ml-linkd.c) and the
 * air-unit TX side (ml-linkd-air.c).
 */
#ifndef ML_LINKD_H
#define ML_LINKD_H

#define TAG              "[ml-linkd]"

#define LOCAL_ADDR       "10.0.0.1"        /* goggle (RX) sdio0 address */
#define AIR_ADDR         "10.0.0.100"      /* air unit (TX) sdio0 address */
#define HELLO_PORT       20001             /* :20001 3-way hello */
#define PARAMS_PORT      10000             /* :10000 params handshake + telemetry */

#define HELLO_LEN        520               /* :20001 hello datagram */
#define PKT_MAX          600               /* UDP receive buffer */
#define UDP_TICK_US      50000             /* 20 Hz service tick */

extern volatile int g_run;                 /* cleared by the signal handler to stop the loops */
extern int g_verbose;                      /* -v */

/* Monotonic milliseconds. */
long now_ms(void);

/* MCS-driven encoder rate adaptation on the air role. OFF is the default: the decode of the
 * GET_MCS reply is transcribed from the vendor and has not been confirmed on our stack, so it must
 * be observed with PROBE before it is allowed to move a flying encoder. */
enum ml_rate_mode {
    ML_RATE_OFF = 0,   /* do not open the bb socket at all */
    ML_RATE_PROBE,     /* poll and log MCS/throughput/derived rate, send nothing */
    ML_RATE_ON,        /* drive ml-air-video's control socket */
};

/* Air (TX) role entry point (--role air); hw_version is the board's hardware version string for the
 * status frame (NULL selects the default). Returns when g_run clears. */
void air_main(const char *hw_version, const char *fc_tty, enum ml_rate_mode rate_mode);

#endif /* ML_LINKD_H */
