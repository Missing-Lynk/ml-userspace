/*
 * ml-air-bind.h - bind button and the DEV-role pair window.
 *
 * The button is a gpio-keys-polled device reporting KEY_CONNECT; the hold time decides the action,
 * measured from the evdev event timestamps rather than the service tick, so the 50 ms poll interval
 * of the input device is the only quantisation. A hold of at most AIR_BIND_HOLD_MAX_MS opens a pair
 * window, a longer one is reserved and does nothing.
 *
 * While the window is open the red indicator blinks at AIR_BIND_LED_MS via the kernel timer
 * trigger, the green power LED is held off, and the caller shortens its tick to AIR_BIND_TICK_US so
 * GET_PAIR is polled at the chip's cadence.
 *
 * The window runs the DEV-role pair sequence (AR_AR8030_TX_BbPair @0x4331f0), which is the AP-role
 * sequence with three deltas: a broadcast ap_mac before pair mode, a commit that writes ap_mac
 * rather than the pair lock, and a slot discovered from the candidate bitmask rather than fixed at
 * zero. It runs as a state machine across ticks rather than blocking, because ml_msp_service()
 * drains the FC UART every tick and a blocking window overruns that tty in well under a second.
 *
 * The vendor's timeout path leaves the broadcast in place, which unbinds an already-bound unit for
 * the rest of the session. The saved ap_mac is written back instead.
 */
#ifndef ML_AIR_BIND_H
#define ML_AIR_BIND_H

#include <stdint.h>

#include "ml-air-bb.h"

#define AIR_BIND_DEV_NAME       "ml-bind-button"
#define AIR_BIND_LED_RED        "/sys/class/leds/red:indicator"
#define AIR_BIND_LED_GREEN      "/sys/class/leds/green:power"
#define AIR_BIND_LED_MS         40      /* timer-trigger half period while the window is open */
#define AIR_BIND_HOLD_MAX_MS    2000    /* longer holds are reserved */
#define AIR_BIND_WINDOW_MS      30000   /* pair window length */
#define AIR_BIND_TICK_US        20000   /* service cadence while the window is open */
#define AIR_BIND_OPEN_RETRY_MS  5000    /* evdev resolve retry, covers deferred probe of the GPIO */
#define AIR_BIND_POLL_MS        20      /* GET_PAIR poll spacing */
#define AIR_BIND_HITS           6       /* cumulative candidate hits before the commit */
#define AIR_BIND_CFG            "/lib/firmware/bb_config_air.json"
#define AIR_BIND_CFG_USR        "/usrdata/missinglynk/bb_config_air.json"
#define AIR_BIND_PERSIST        "/usr/local/bin/ml-rf-persist"
#define AIR_BIND_RESTORE_TRIES  10      /* ticks the ap_mac restore is retried before giving up */
#define AIR_BIND_EV_BURST_MAX   32      /* evdev events drained per tick; a 50 ms polled button
                                         * cannot produce more, so the cap only bounds the loop */
#define AIR_BIND_GATE_WAIT_MS   300     /* budget for the pre-bind link-state query */
#define AIR_BIND_GATE_POLL_MS   10      /* read step inside that budget */

/* Phases of one pair window. The commit and the restore each wait a tick after pair mode is
 * exited, reproducing the spacing the AP role gets from its sleep between exit and lock. */
enum air_pair_phase {
    PAIR_IDLE = 0,
    PAIR_POLL,      /* pair mode on, polling GET_PAIR for a candidate */
    PAIR_COMMIT,    /* candidate found, pair mode exited, ap_mac write pending */
    PAIR_RESTORE,   /* window expired, pair mode exited, saved ap_mac write pending */
};

struct air_bind {
    int fd;                 /* evdev, non-blocking; -1 until the device resolves */
    long last_open_ms;
    long press_ms;          /* event timestamp of the current press, 0 = not held */
    long window_until_ms;   /* window deadline, 0 = closed */
    int green_saved;        /* green brightness sampled at window open, -1 = nothing to restore */
    int warned_open;

    enum air_pair_phase phase;
    int holding;            /* holds a reference on the bb socket */
    long last_poll_ms;
    int slot;               /* candidate slot; starts 0, overwritten by the bitmask scan */
    int hits;               /* cumulative candidate hits, never reset by a zero read */
    uint8_t peer[4];        /* candidate MAC, wire order */
    uint8_t saved[4];       /* ap_mac to restore if the window expires */
    int have_saved;
    int restore_tries;
    int polls;              /* GET_PAIR requests sent this window */
    int replies;            /* GET_PAIR replies seen, candidate or not */
};

void air_bind_service(struct air_bind *button, struct air_bb *bb, long now);
void air_bind_pair_service(struct air_bind *button, struct air_bb *bb, long now);
void air_bind_pair_reply(struct air_bind *button, const uint8_t *pay, int plen);
void air_bind_abort(struct air_bind *button, struct air_bb *bb, const char *why);
int air_bind_active(const struct air_bind *button);

#endif /* ML_AIR_BIND_H */
