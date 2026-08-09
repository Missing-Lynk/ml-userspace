/*
 * ml-air-bb.h - the air role's handle on the AR8030 bb control socket.
 *
 * Two opens of /dev/artosyn_sdio in one process wedge the RF chip, so every air-role user of the
 * node shares one reference-counted fd and one request-id counter.
 */
#ifndef ML_AIR_BB_H
#define ML_AIR_BB_H

#include <stdint.h>

#define AIR_BB_NODE            "/dev/artosyn_sdio"
#define AIR_BB_OPEN_RETRY_MS   5000    /* open retry cadence */
#define AIR_BB_WRITE_WAIT_MS   100     /* bounded wait for the chip's cmd TX queue to drain */
#define AIR_BB_WRITE_POLL_MS   5       /* POLLOUT retry step inside that budget */
#define AIR_BB_DRAIN_MAX       32      /* reads serviced per tick, so a chatty socket cannot spin */

struct air_bb {
    int fd;                 /* non-blocking; -1 while no user holds it */
    uint32_t seq;
    int users;
    long last_open_ms;
    int warned_open;
};

/* Called for each reply frame the drain recognises: @p selector is the GET selector it came in on. */
typedef void (*air_bb_reply_fn)(uint8_t selector, const uint8_t *payload, int plen, void *ctx);

int air_bb_acquire(struct air_bb *bb, long now, const char *why);
void air_bb_release(struct air_bb *bb);
int air_bb_send(struct air_bb *bb, const uint8_t *frame, int len, const char *what);
void air_bb_drain(struct air_bb *bb, air_bb_reply_fn cb, void *ctx);

#endif /* ML_AIR_BB_H */
