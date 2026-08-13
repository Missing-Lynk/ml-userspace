/*
 * ml-air-power.c - TX power and standby on the air role.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bb-cmd.h"
#include "ml-linkd.h"
#include "mp-cmd.h"
#include "ml-air-bb.h"
#include "ml-air-ctrl.h"
#include "ml-air-power.h"

/* Latch one commanded (power, standby) pair. Both 0x0a and 0x0d carry the same two fields and are
 * treated identically; only they are read out of either message. An out-of-range dBm leaves the
 * previous commanded value in place and does not block the standby bit, so a bad power byte cannot
 * also cost the work-mode command. */
static void air_power_command(struct air_power *pw, unsigned dbm, unsigned standby,
                              const char *source)
{
    if (dbm < AIR_POWER_MIN_DBM || dbm > AIR_POWER_MAX_DBM) {
        if (!pw->warned_range) {
            fprintf(stderr, TAG " power: %s commanded %u dBm, outside [%d, %d], ignored\n",
                    source, dbm, AIR_POWER_MIN_DBM, AIR_POWER_MAX_DBM);
            pw->warned_range = 1;
        }
    } else {
        if ((int)dbm != pw->commanded_dbm) {
            printf(TAG " power: %s commanded %u dBm\n", source, dbm);
            fflush(stdout);
        }

        pw->commanded_dbm = (int)dbm;
        pw->warned_range = 0;
    }

    if ((int)(standby ? 1 : 0) != pw->standby_cmd) {
        printf(TAG " power: %s commanded standby=%u\n", source, standby ? 1u : 0u);
        fflush(stdout);
    }

    pw->standby_cmd = standby ? 1 : 0;
}

/* A message this module owns, arriving while it is disabled: drained and discarded. */
static int air_power_disabled(const struct air_power *pw, const char *what)
{
    if (pw->mode != ML_POWER_OFF) {
        return 0;
    }

    if (g_verbose) {
        fprintf(stderr, TAG " rx %s, power control off\n", what);
    }

    return 1;
}

/* SetTranParm (0x0d): the live lever. */
void air_power_tran_parm(struct air_power *pw, const uint8_t *dgram, ssize_t n)
{
    if (air_power_disabled(pw, "SetTranParm") || n < MP_STP_LEN) {
        return;
    }

    air_power_command(pw, dgram[MP_STP_OFF_DBM], dgram[MP_STP_OFF_STANDBY], "SetTranParm");
}

/* Report the SetLdCfg body on first arrival and on every change. ml-linkd consumes two bytes of
 * 192; the rest is the sender's session state and the only statement a goggle makes about the
 * geometry, tiling and aspect it expects. A vendor sender configures its own receiver from this
 * block, so a mismatch against what the air transmits is not visible anywhere else. */
static void air_power_ld_cfg_report(const uint8_t *body)
{
    static uint8_t last[sizeof(struct mp_ldcfg)];
    static int have_last;
    struct mp_ldcfg cfg;

    if (have_last && memcmp(last, body, sizeof last) == 0) {
        return;
    }

    memcpy(last, body, sizeof last);
    have_last = 1;
    memcpy(&cfg, body, sizeof cfg);

    printf(TAG " ldcfg: rec %ux%u, aspect %u, scale %u/%u, rotation %u, zoom %.3f, roi %u,"
           " bitrate %u kbps, power %u dBm, standby %u\n",
           cfg.rec_width, cfg.rec_height, cfg.aspect_ratio, cfg.scale_mode, cfg.scale_unk25,
           cfg.rotation, (double)cfg.zoom_factor, cfg.roi_enable, cfg.bitrate_q * 250u,
           cfg.tx_power_dbm, cfg.standby_mode_en);
    printf(TAG " ldcfg: unk44 %u, unk45 %u, unk4a %u, unk4e %u, tran_blk %u/%u/%u/%u,"
           " caps %02x/%02x\n",
           cfg.unk44, cfg.unk45, cfg.unk4a, cfg.unk4e, cfg.tran_bw_mcs, cfg.tran_blk2,
           cfg.tran_blk4, cfg.tran_blk6, cfg.caps_flags1, cfg.caps_flags2);

    /* The undecoded remainder, so a field nobody has named yet is still recoverable from the log.
     * Offsets are body-relative, matching the struct comments in mp-cmd.h. */
    for (size_t off = 0; off < sizeof cfg; off += 16) {
        size_t run = sizeof cfg - off < 16 ? sizeof cfg - off : 16;

        printf(TAG " ldcfg: %02zx:", off);
        for (size_t i = 0; i < run; i++) {
            printf(" %02x", body[off + i]);
        }
        printf("\n");
    } /* for each 16-byte row */

    fflush(stdout);
}

/* SetLdCfg (0x0a): the durable lever, re-sent on every association. The other 190 bytes are
 * undecoded vendor state and are not read, but they are reported (above) because they are the only
 * record of what the sender expects the air to transmit. */
void air_power_ld_cfg(struct air_power *pw, const uint8_t *dgram, ssize_t n)
{
    struct mp_ldcfg cfg;

    if (n < MP_LDCFG_LEN) {
        return;
    }

    /* Ahead of the power gate: the report describes the sender, not our policy, so it must survive
     * a build or a boot that runs without --power-adapt. */
    air_power_ld_cfg_report(dgram + MP_LDCFG_BODY_OFF);

    if (air_power_disabled(pw, "SetLdCfg")) {
        return;
    }

    memcpy(&cfg, dgram + MP_LDCFG_BODY_OFF, sizeof cfg);
    air_power_command(pw, cfg.tx_power_dbm, cfg.standby_mode_en, "SetLdCfg");
}

/* STB_EVENT_ACK (0x1b): the goggle has seen the standby report, so the drop may go ahead. An ack
 * outside a standby entry is stray and dropped. */
void air_power_stb_ack(struct air_power *pw)
{
    if (air_power_disabled(pw, "StbAck") || pw->work_mode != MP_STANDBY_ON || pw->acked) {
        return;
    }

    pw->acked = 1;

    if (g_verbose) {
        fprintf(stderr, TAG " power: StbAck received, standby drop released\n");
    }
}

/* Any :10000 datagram, whatever its type: the goggle is reachable. */
void air_power_rx(struct air_power *pw, long now)
{
    pw->last_rx_ms = now;
}

/* The FC's arm state, and whether there is an FC to have one. Both are sampled every tick. */
void air_power_fc(struct air_power *pw, int fc_present, int armed)
{
    int state = fc_present ? (armed ? 2 : 1) : 0;

    if (state != pw->logged_arm) {
        pw->logged_arm = state;

        if (pw->mode != ML_POWER_OFF) {
            printf(TAG " power: %s\n",
                   fc_present ? (armed ? "FC armed, power follows the goggle"
                                       : "FC disarmed, power held at the minimum")
                              : "no FC, arm unknown, power follows the goggle");
            fflush(stdout);
        }
    }

    pw->fc_present = fc_present;
    pw->armed = armed;
}

/* One GET_POWER reply. */
void air_power_reply(struct air_power *pw, const uint8_t *pay, int plen)
{
    int dbm;

    if (plen < POWER_OFF_DBM + 1) {
        return;
    }

    if (pw->mode == ML_POWER_PROBE && pw->probe_dumped < AIR_POWER_PROBE_DUMPS) {
        pw->probe_dumped++;
        printf(TAG " power reply (%d B):", plen);
        for (int k = 0; k < plen; k++) {
            printf(" %02x", pay[k]);
        }
        printf("\n");
        fflush(stdout);
    }

    dbm = pay[POWER_OFF_DBM];
    if (dbm != pw->read_dbm) {
        printf(TAG " power: chip reports %d dBm\n", dbm);
        fflush(stdout);
    }

    pw->read_dbm = dbm;
}

/* Write one power target. The chip's closed loop is turned off first and stays off: honouring a
 * commanded power and letting the chip adjust it are mutually exclusive. */
static void air_power_apply(struct air_power *pw, struct air_bb *bb, int dbm, const char *why)
{
    uint8_t frame[32];

    if (pw->mode == ML_POWER_PROBE) {
        if (dbm != pw->probe_dbm) {
            pw->probe_dbm = dbm;
            printf(TAG " power probe: %s -> %d dBm (not applied)\n", why, dbm);
            fflush(stdout);
        }

        return;
    }

    if (dbm == pw->applied_dbm) {
        return;
    }

    if (!pw->auto_disabled) {
        if (air_bb_send(bb, frame, bb_set_power_auto(frame, 0, bb->seq++),
                        "SET_POWER_AUTO(0)") != 0) {
            return;
        }

        pw->auto_disabled = 1;
    }

    if (air_bb_send(bb, frame, bb_set_power(frame, RF_TX, (uint8_t)dbm, bb->seq++),
                    "SET_POWER") != 0) {
        return;
    }

    pw->applied_dbm = dbm;
    printf(TAG " power: %s, %d dBm applied\n", why, dbm);
    fflush(stdout);
}

/* Move the capture feeder to match the work mode, with `capfps` rather than `fps`: the encoders
 * keep the frame rate they were given, and their rate control budgets bits per picture from it, so
 * feeding fewer pictures at an unchanged declared rate lowers the emitted bitrate in proportion to
 * the frame rate. Standby is therefore a bitrate drop as well as a frame-rate drop.
 *
 * The feeder already runs at AIR_POWER_FPS_NORMAL, so nothing is pushed on a unit that never
 * enters standby. */
static void air_power_frame_rate(struct air_power *pw, int standby_active)
{
    int fps = standby_active ? AIR_POWER_FPS_STANDBY : AIR_POWER_FPS_NORMAL;
    char cmd[32];

    if (pw->applied_fps == fps || (pw->applied_fps == 0 && !standby_active)) {
        return;
    }

    snprintf(cmd, sizeof cmd, "capfps %d\n", fps);
    if (air_ctrl_send(cmd) != 0) {
        if (!pw->warned_ctrl) {
            fprintf(stderr, TAG " power: ml-air-video control socket not answering, retrying\n");
            pw->warned_ctrl = 1;
        }

        return;
    }

    pw->warned_ctrl = 0;
    pw->applied_fps = fps;
    printf(TAG " power: capture feeder %d fps (%s)\n", fps, standby_active ? "standby" : "normal");
    fflush(stdout);
}

/* Report the work mode on a change, and keep re-sending an unacked standby entry: the drop is
 * gated on the ack, so a lost report would otherwise hold the air at full power indefinitely. */
static void air_power_report(struct air_power *pw, long now, air_power_report_fn report, void *ctx)
{
    int due = (pw->work_mode != pw->reported_mode)
              || (pw->work_mode == MP_STANDBY_ON && !pw->acked
                  && now - pw->report_ms >= AIR_POWER_REPORT_MS);

    if (!due || report == NULL) {
        return;
    }

    if (report((uint32_t)pw->work_mode, ctx) != 0) {
        return;
    }

    if (pw->work_mode != pw->reported_mode) {
        printf(TAG " power: work mode %d reported\n", pw->work_mode);
        fflush(stdout);
    }

    pw->reported_mode = pw->work_mode;
    pw->report_ms = now;
}

/* Hand the radio back to the chip once the goggle has gone quiet, so a unit left in standby by a
 * receiver that disappeared does not stay at the minimum. The commanded power is forgotten with
 * it: the next association re-seeds it from SetLdCfg. */
static void air_power_goggle_lost(struct air_power *pw, struct air_bb *bb, long now)
{
    uint8_t frame[32];

    if (pw->last_rx_ms == 0 || now - pw->last_rx_ms < AIR_POWER_LOST_MS) {
        return;
    }

    pw->last_rx_ms = 0;
    pw->standby_cmd = MP_STANDBY_NORMAL;
    pw->work_mode = MP_STANDBY_NORMAL;
    pw->reported_mode = MP_STANDBY_NORMAL;
    pw->acked = 0;
    pw->commanded_dbm = -1;
    pw->applied_dbm = -1;
    pw->probe_dbm = -1;

    if (pw->auto_disabled
        && air_bb_send(bb, frame, bb_set_power_auto(frame, 1, bb->seq++),
                       "SET_POWER_AUTO(1)") == 0) {
        pw->auto_disabled = 0;
        printf(TAG " power: goggle silent for %d ms, back to the chip's closed loop\n",
               AIR_POWER_LOST_MS);
    } else {
        printf(TAG " power: goggle silent for %d ms, commanded state cleared\n",
               AIR_POWER_LOST_MS);
    }

    /* Not the place to reset the video session: this window fires on a healthy link. A vendor
     * goggle stops sending :10000 once video is up (measured: RX frozen for minutes while the
     * picture ran), so a reset here ends working video ~5 s in. It hangs off the params request
     * instead, which a receiver sends while establishing. */

    fflush(stdout);
}

/* Service tick: hold the bb socket while enabled, track the work mode, apply the target and poll
 * the read-back. Replies arrive through the shared drain. */
void air_power_service(struct air_power *pw, struct air_bb *bb, long now,
                       air_power_report_fn report, void *ctx)
{
    uint8_t frame[32];
    int standby_active;
    int target_dbm;

    if (pw->mode == ML_POWER_OFF) {
        return;
    }

    if (!pw->holding) {
        if (air_bb_acquire(bb, now, pw->mode == ML_POWER_PROBE ? "power probe, sends nothing"
                                                              : "tx power control") != 0) {
            return;
        }

        pw->holding = 1;
    }

    if (bb->fd < 0) {
        /* a write failure dropped the socket under us; take a fresh reference */
        pw->holding = 0;
        return;
    }

    air_power_goggle_lost(pw, bb, now);

    /* The reported work mode is the one the air is actually in, so arming cancels a commanded
     * standby rather than leaving the goggle showing a standby the air is not honouring. */
    int want_mode = (pw->standby_cmd && !(pw->fc_present && pw->armed))
                    ? MP_STANDBY_ON : MP_STANDBY_NORMAL;

    if (want_mode != pw->work_mode) {
        pw->work_mode = want_mode;
        pw->acked = 0;
    }

    /* The probe sends nothing at all, on the bb socket or on the wire. */
    if (pw->mode == ML_POWER_ON) {
        air_power_report(pw, now, report, ctx);
    }

    /* Entering standby waits for the ack before the drop; leaving it does not wait for anything. */
    standby_active = (pw->work_mode == MP_STANDBY_ON && pw->acked);

    /* Power follows the arm state, not standby. A disarmed aircraft holds the minimum; an absent
     * FC is arm-unknown and leaves the goggle in charge. Nothing is written before the goggle has
     * commanded a value, so the goggle-silence release above is not immediately undone. */
    if (pw->commanded_dbm >= 0) {
        int idle = pw->fc_present && !pw->armed;

        target_dbm = idle ? AIR_POWER_IDLE_DBM : pw->commanded_dbm;
        air_power_apply(pw, bb, target_dbm, idle ? "disarmed" : "goggle commanded");
    }

    if (pw->mode == ML_POWER_ON) {
        air_power_frame_rate(pw, standby_active);
    }

    if (now - pw->last_poll_ms >= AIR_POWER_POLL_MS) {
        pw->last_poll_ms = now;
        air_bb_send(bb, frame, bb_get_power(frame, RF_TX, bb->seq++), "GET_POWER");
    }
}
