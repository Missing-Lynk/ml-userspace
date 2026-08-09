/*
 * ml-air-rate.h - MCS-driven encoder rate governor for the air role.
 *
 * The vendor air recomputes its encoder target from the live RF link every time the MCS changes
 * (AR_FSM_TX_ProcessMcsChange @0x428b88 on a decrease, AR_FSM_TX_ProcessMcsChangeFinished @0x428fc8
 * on an increase), derives the number in AR_8030_TX_GetBitRate @0x4349b0 and applies it with
 * AR_LDRT_TX_VENC_SetRcParam. This reproduces the derivation and the drop-fast/rise-slow asymmetry,
 * polling GET_MCS instead of consuming an FSM event and applying through ml-air-video.
 */
#ifndef ML_AIR_RATE_H
#define ML_AIR_RATE_H

#include <stdint.h>

#include "ml-linkd.h"
#include "ml-air-bb.h"

/* Mirrors of the vendor's cfg_transmedium.json knobs, values as captured from a stock air unit
 * (archive/out/air-probe/cfg_transmedium.json). Ar803xMinMcs is -1 there and MCS is never negative,
 * so the low-MCS branch is dead on a stock unit; it is kept because the constant is the switch. */
#define AIR_RATE_THROUGHPUT_RATE  0.7f      /* Ar803xThroutputRate */
#define AIR_RATE_LOW_MCS_RATE     0.8f      /* f32Ar803xThroutputRateLowMcs */
#define AIR_RATE_MAX_KBPS         20000     /* ArMaxBitRate */
#define AIR_RATE_MIN_MCS          (-1)      /* Ar803xMinMcs */
#define AIR_RATE_FALLBACK_KBPS    8000      /* the vendor's "use default bitrate 8Mbps" */
#define AIR_RATE_RATIO_PCT        100       /* runtime ratio; SetThroutputRate's default, never set */

/* The derived rate is a TOTAL across the two tiles the 1080p frame is split into; the control
 * socket takes a per-tile value. */
#define AIR_RATE_TILES            2

#define AIR_MCS_POLL_MS       200           /* GET_MCS cadence */
#define AIR_RATE_RISE_MS      1000          /* a higher MCS must hold this long before it is acted on */
#define AIR_CTRL_TIMEOUT_MS   200           /* control-socket reply budget */
#define AIR_PROBE_DUMPS       20            /* raw GET_MCS replies to hexdump before going quiet */

struct air_rate {
    enum ml_rate_mode mode;
    int holding;            /* holds a reference on the bb socket */
    long last_poll_ms;
    int mcs;                /* last acted-on MCS, -1 = no sample yet */
    int pending_mcs;        /* higher MCS waiting out the settle window, -1 = none */
    long pending_since_ms;
    int applied_bps;        /* per-tile bps last pushed, 0 = none */
    int warned_ctrl;
    int probe_dumped;       /* raw reply hexdumps emitted so far (capped, see AIR_PROBE_DUMPS) */
};

void air_rate_service(struct air_rate *rate, struct air_bb *bb, long now);
void air_rate_reply(struct air_rate *rate, const uint8_t *pay, int plen, long now);

#endif /* ML_AIR_RATE_H */
