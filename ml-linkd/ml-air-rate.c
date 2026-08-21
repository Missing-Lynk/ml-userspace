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

/* The vendor's frame-rate derivation, AR_8030_TX_GetFrameRate. Reduces only while the modulation
 * index is at or below AIR_RATE_MIN_MCS and returns @p nominal_fps untouched otherwise, which on
 * the shipped config is every sample. Returns a frame rate. */
int air_rate_frame_rate(int mcs, int nominal_fps)
{
    int ratio = AIR_RATE_FPS_RATIO_PCT;

    if (mcs > AIR_RATE_MIN_MCS) {
        return nominal_fps;
    }

    if (ratio < AIR_RATE_FPS_QUARTER_PCT) {
        return nominal_fps / 4;
    }

    if (ratio < AIR_RATE_FPS_HALF_PCT) {
        return nominal_fps / 2;
    }

    return nominal_fps * 3 / 4;
}

/*
 * Derive the target for @p mcs / @p throughput_kbps and apply it, unless it is already applied or
 * we are only probing.
 *
 * The bitrate is the whole of what this commands. Frame rate on this stack belongs to the standby
 * path, which runs the capture feeder at 15 or 60, and nothing the link does may move it: a
 * picture that drops to 45 fps because the radio dipped is a worse failure than the bits it saves.
 * That also matches the vendor at the shipped configuration, whose own frame-rate ladder is gated
 * on an Ar803xMinMcs of -1 it never reaches. air_rate_frame_rate keeps that ladder as recovered
 * arithmetic, exercised by the test, and no caller feeds it the encoder.
 */
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

int air_rate_sim_parse(const char *line, int *mcs, int *throughput_kbps)
{
    char *end;
    long parsed_mcs, parsed_throughput;

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    if (*line == '\0' || *line == '#' || *line == '\n') {
        return 0;
    }

    parsed_mcs = strtol(line, &end, 10);
    if (end == line) {
        return 0;
    }

    line = end;
    parsed_throughput = strtol(line, &end, 10);
    if (end == line) {
        return 0;
    }

    *mcs = (int)parsed_mcs;
    *throughput_kbps = (int)parsed_throughput;

    return 1;
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

    /* The index is the reply's first byte less MCS_INDEX_BIAS, so a byte below the bias decodes
     * negative, which no modulation index is. A well-sized reply of zeros reaches here and would
     * otherwise be acted on as a valid sample at the bottom of the range. Dropped, warned once. */
    if (mcs < 0) {
        if (!rate->warned_index) {
            fprintf(stderr, TAG " rate: modulation index decoded as %d, dropping the sample\n",
                    mcs);
            rate->warned_index = 1;
        }

        return;
    }

    rate->warned_index = 0;

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

/*
 * One simulated sample, read fresh so the file can be rewritten under a running governor.
 *
 * Feeding it on every poll rather than on change is deliberate: it is what the baseband path does,
 * and the settle window that delays a rise is counted in samples arriving, so a file read once
 * would never let a rise mature.
 */
static void air_rate_sim_poll(struct air_rate *rate, long now)
{
    char line[AIR_RATE_SIM_MAX];
    int mcs, throughput_kbps;
    FILE *f = fopen(rate->sim_path, "r");

    if (f == NULL) {
        if (!rate->warned_sim) {
            fprintf(stderr, TAG " rate sim: %s unreadable, the governor has no input\n",
                    rate->sim_path);
            rate->warned_sim = 1;
        }

        return;
    }

    if (fgets(line, sizeof line, f) == NULL || !air_rate_sim_parse(line, &mcs, &throughput_kbps)) {
        fclose(f);
        if (!rate->warned_sim) {
            fprintf(stderr, TAG " rate sim: %s carries no `<mcs> <throughput_kbps>` line\n",
                    rate->sim_path);
            rate->warned_sim = 1;
        }

        return;
    }

    fclose(f);
    rate->warned_sim = 0;
    air_rate_sample(rate, mcs, throughput_kbps, now);
}

/* Service tick: hold the bb socket while enabled, poll GET_MCS on cadence. Replies arrive through
 * the shared drain. */
void air_rate_service(struct air_rate *rate, struct air_bb *bb, long now)
{
    uint8_t frame[32];

    if (rate->mode == ML_RATE_OFF) {
        return;
    }

    /* Simulated input replaces the baseband entirely: no acquire, no GET_MCS, nothing on
     * /dev/artosyn_sdio. Same cadence as the live poll, so the settle window behaves identically. */
    if (rate->sim_path != NULL) {
        if (now - rate->last_poll_ms >= AIR_MCS_POLL_MS) {
            rate->last_poll_ms = now;
            air_rate_sim_poll(rate, now);
        }

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
