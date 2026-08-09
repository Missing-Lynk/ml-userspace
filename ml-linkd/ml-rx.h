/*
 * ml-rx.h - state and helpers shared between the RX (goggle) role's modules.
 *
 * The role is split across ml-linkd.c (link FSM + steady bb-socket cadence, and the owner of the
 * state declared here), ml-rx-reader.c (bb-socket reply reader), ml-rx-udp.c (the :20001/:10000
 * protocol and the HUD command surface), ml-rx-chan.c (channel table, tuning, link metrics) and
 * ml-rx-bind.c (the pair sequence). Symbols shared with the air role live in ml-linkd.h instead.
 */
#ifndef ML_RX_H
#define ML_RX_H

#include <stddef.h>
#include <stdint.h>

#define AIR_LOSS_MS      5000             /* :10000 silence in STEADY => air lost */
#define OPEN_STEP_US     60000            /* spacing between OPEN-stage config frames */
#define OPEN_RETRY_EVERY 30               /* log every Nth failed open/bind retry */

extern int g_fd;                          /* /dev/artosyn_sdio */
extern int g_no_gate;                     /* --no-gate */
extern int g_scan_probe;                  /* --scan-probe */

/* Handshake/link state shared between the FSM tick (main), the UDP thread and the modules that
 * gate on it. All plain ints/timestamps, single writer per field, so volatile is enough. */
extern volatile int g_steady;             /* FSM reached STEADY */
extern volatile int g_hs_done;            /* :20001 3-way done */
extern volatile int g_params_acked;       /* the air answered our params poll this session */
extern volatile int g_air_lost;           /* >5 s :10000 silence flagged */
extern volatile int g_ready;              /* consumer READY (heartbeat fresh) */
extern volatile int g_video_confirmed;    /* consumer reported frames_seen after our ACK */
extern volatile int g_standby_state;      /* air's LIVE work-mode from SetStandyMode (0x12): 1 = standby */
extern volatile long g_last_telem_ms;     /* last :10000 RX */

/* MLM producer (telemetry.sock / osd.sock / led.sock; drop on error, never block). */
void mlm_pub(const char *path, uint16_t type, const void *payload, size_t n);

/* Print the transition, publish it as MLM_T_LINK and append it to the flight-session log. */
void link_event(uint32_t state, const char *what);
void link_event_aux(uint32_t state, uint32_t aux, const char *what);

/* Write one built frame to the bb socket. @return 0 on success. */
int send_frame(const uint8_t *frame, int n, const char *tag);

#endif /* ML_RX_H */
