/*
 * ml-air-rate.c - MCS-driven encoder rate governor.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-air-bb.h"
#include "ml-air-ctrl.h"
#include "ml-air-rate.h"

/* The vendor derivation, kept in its original shape: the integer divide by 100 happens BEFORE the
 * multiply, so the throughput input is quantised to 100 kbps steps. Returns a total in kbps. */
static int air_rate_total_kbps(int mcs, int throughput_kbps)
{
    int kbps;

    if (throughput_kbps < 0) {
        throughput_kbps = 0;
    }

    if (AIR_RATE_MIN_MCS < mcs) {
        kbps = (int)((float)((throughput_kbps / 100) * AIR_RATE_RATIO_PCT)
                     * AIR_RATE_THROUGHPUT_RATE);
    } else {
        kbps = (int)((float)throughput_kbps * AIR_RATE_LOW_MCS_RATE);
    }

    if (kbps > AIR_RATE_MAX_KBPS) {
        kbps = AIR_RATE_MAX_KBPS;
    }

    return kbps != 0 ? kbps : AIR_RATE_FALLBACK_KBPS;
}

/* Derive the target for @p mcs / @p throughput_kbps and apply it, unless it is already applied or
 * we are only probing. */
static void air_rate_apply(struct air_rate *rate, int mcs, int throughput_kbps, const char *why)
{
    int total_kbps = air_rate_total_kbps(mcs, throughput_kbps);
    int bps = total_kbps * 1000 / AIR_RATE_TILES;
    char cmd[64];

    if (rate->mode == ML_RATE_PROBE) {
        printf(TAG " rate probe: %s mcs=%d throughput=%d kbps -> %d kbps total, %d bps/tile"
               " (not applied)\n", why, mcs, throughput_kbps, total_kbps, bps);
        fflush(stdout);

        return;
    }

    if (bps == rate->applied_bps) {
        return;
    }

    snprintf(cmd, sizeof cmd, "bitrate %d\n", bps);
    if (air_ctrl_send(cmd) != 0) {
        if (!rate->warned_ctrl) {
            fprintf(stderr, TAG " rate: ml-air-video control socket not answering, retrying\n");
            rate->warned_ctrl = 1;
        }

        return;
    }

    rate->warned_ctrl = 0;
    rate->applied_bps = bps;
    printf(TAG " rate: %s mcs=%d throughput=%d kbps -> %d bps/tile (%d kbps total)\n",
           why, mcs, throughput_kbps, bps, total_kbps);
    fflush(stdout);
}

/* One decoded GET_MCS sample. The first sample seeds the baseline; after that a DROP is acted on
 * immediately and a RISE only once the higher MCS has held for AIR_RATE_RISE_MS, which is what the
 * vendor's split between MCS_CHANGE and MCS_CHANGE_FINISHED amounts to. A change in throughput
 * alone does not move the rate - the vendor recomputes on MCS transitions only. */
static void air_rate_sample(struct air_rate *rate, int mcs, int throughput_kbps, long now)
{
    if (g_verbose) {
        fprintf(stderr, TAG " mcs sample: mcs=%d throughput=%d kbps\n", mcs, throughput_kbps);
    }

    if (rate->mcs < 0) {
        rate->mcs = mcs;
        air_rate_apply(rate, mcs, throughput_kbps, "baseline");

        return;
    }

    if (mcs < rate->mcs) {
        rate->mcs = mcs;
        rate->pending_mcs = -1;
        air_rate_apply(rate, mcs, throughput_kbps, "mcs drop");

        return;
    }

    if (mcs == rate->mcs) {
        rate->pending_mcs = -1;
        return;
    }

    if (rate->pending_mcs != mcs) {
        rate->pending_mcs = mcs;
        rate->pending_since_ms = now;

        return;
    }

    if (now - rate->pending_since_ms >= AIR_RATE_RISE_MS) {
        rate->mcs = mcs;
        rate->pending_mcs = -1;
        air_rate_apply(rate, mcs, throughput_kbps, "mcs rise");
    }
}

/* One GET_MCS reply. */
void air_rate_reply(struct air_rate *rate, const uint8_t *pay, int plen, long now)
{
    if (plen < MCS_OFF_THROUGHPUT + 4) {
        return;
    }

    if (rate->mode == ML_RATE_PROBE && rate->probe_dumped < AIR_PROBE_DUMPS) {
        rate->probe_dumped++;
        printf(TAG " mcs reply (%d B):", plen);
        for (int k = 0; k < plen; k++) {
            printf(" %02x", pay[k]);
        }
        printf("\n");
        fflush(stdout);
    }

    air_rate_sample(rate, (int)pay[MCS_OFF_INDEX] - MCS_INDEX_BIAS,
                    (int)((uint32_t)pay[MCS_OFF_THROUGHPUT]
                          | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 1] << 8)
                          | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 2] << 16)
                          | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 3] << 24)),
                    now);
}

/* Service tick: hold the bb socket while enabled, poll GET_MCS on cadence. Replies arrive through
 * the shared drain. */
void air_rate_service(struct air_rate *rate, struct air_bb *bb, long now)
{
    uint8_t frame[32];

    if (rate->mode == ML_RATE_OFF) {
        return;
    }

    if (!rate->holding) {
        if (air_bb_acquire(bb, now, rate->mode == ML_RATE_PROBE ? "rate probe, sends nothing"
                                                            : "rate governor") != 0) {
            return;
        }
        rate->holding = 1;
    }

    if (bb->fd < 0) {
        /* a write failure dropped the socket under us; take a fresh reference */
        rate->holding = 0;
        return;
    }

    if (now - rate->last_poll_ms >= AIR_MCS_POLL_MS) {
        rate->last_poll_ms = now;
        air_bb_send(bb, frame, bb_get(frame, GET_MCS, bb->seq++), "GET_MCS");
    }
}
