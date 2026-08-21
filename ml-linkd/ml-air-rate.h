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

/*
 * Frame rate. AR_8030_TX_GetFrameRate @0x434dc0 reduces the frame rate on the same condition the
 * bitrate's low-MCS branch uses, `mcs <= Ar803xMinMcs`, and leaves it untouched otherwise. The
 * reduction is a three-step ladder on its own runtime ratio, which defaults to 100 exactly as the
 * bitrate ratio does:
 *
 *   ratio <  50   fps / 4
 *   ratio <  75   fps / 2
 *   otherwise     fps * 3 / 4
 *
 * With the shipped Ar803xMinMcs of -1 and a modulation index that is never negative, the condition
 * is false on every sample and the vendor's frame rate never moves in flight. The ladder is
 * implemented because it is the vendor's, and because AIR_RATE_MIN_MCS is the switch that arms it.
 */
#define AIR_RATE_FPS_RATIO_PCT    100       /* runtime ratio; the default the vendor falls back to */
#define AIR_RATE_FPS_QUARTER_PCT  50        /* below this the ladder quarters the rate */
#define AIR_RATE_FPS_HALF_PCT     75        /* below this it halves; at or above it takes 3/4 */

/* The rate a stream is opened at, the nominal air_rate_frame_rate derives from. Frame rate on this
 * stack is owned by the standby path, so this is the ladder's input in the test and nowhere else. */
#define AIR_RATE_NOMINAL_FPS      60

/*
 * Bench simulation. The link quality a real sample carries is not reproducible on a desk: the chip
 * picks the modulation index from conditions, and a bench link sits at one operating point or at
 * none. So the sample can be read from a file instead of the baseband, one line of
 * `<mcs> <throughput_kbps>`, re-read every poll. Everything downstream is the live path: the same
 * derivation, the same drop-fast/rise-slow asymmetry, the same control-socket write.
 *
 * While it is set the baseband socket is never opened, so this runs with no RF hardware at all and
 * cannot become a second opener on /dev/artosyn_sdio.
 */
#define AIR_RATE_SIM_PATH     "/run/missinglynk/rate-sim"
#define AIR_RATE_SIM_MAX      64            /* longest line the file may carry */

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
    int warned_index;       /* a sample with an impossible modulation index has been reported */
    int probe_dumped;       /* raw reply hexdumps emitted so far (capped, see AIR_PROBE_DUMPS) */
    const char *sim_path;   /* read samples from this file instead of the baseband, NULL = live */
    int warned_sim;         /* the sim file has been reported unreadable */
};

/* The vendor's frame-rate ladder. Exposed because the branch that reaches it needs a
 * modulation index no chip reports, so a unit test is the only way it is covered. */
int air_rate_frame_rate(int mcs, int nominal_fps);

/*
 * Parse one simulation line into @p mcs and @p throughput_kbps.
 *
 * Accepts `<mcs> <throughput_kbps>` with any run of spaces or tabs between them, ignores anything
 * after a '#', and ignores a blank or comment-only line. @return 1 on a complete pair, 0
 * otherwise, leaving both outputs untouched. Pure, so the host test drives it directly.
 */
int air_rate_sim_parse(const char *line, int *mcs, int *throughput_kbps);

void air_rate_service(struct air_rate *rate, struct air_bb *bb, long now);
void air_rate_reply(struct air_rate *rate, const uint8_t *pay, int plen, long now);

#endif /* ML_AIR_RATE_H */
