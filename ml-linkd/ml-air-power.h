/*
 * ml-air-power.h - TX power and standby on the air role.
 *
 * Two independent gates, and which lever each one drives is the whole policy:
 *
 *   radiated power  <- the FC arm state
 *   frame rate      <- the goggle's standby arm
 *
 * | FC        | armed | standby | frame rate | power           |
 * |-----------|-------|---------|------------|-----------------|
 * | absent    | -     | no      | 60         | goggle commands |
 * | absent    | -     | yes     | 15         | goggle commands |
 * | present   | no    | no      | 60         | minimum         |
 * | present   | no    | yes     | 15         | minimum         |
 * | present   | yes   | either  | 60         | goggle commands |
 *
 * Arming means flying, so it takes both levers to full regardless of what the goggle asked for.
 * A disarmed aircraft holds the minimum whether or not standby was ever commanded.
 *
 * **An absent FC is "arm unknown", not "disarmed".** `arm_flag` reads 0 with no FC attached, which
 * is indistinguishable from a real disarm, so the arm gate applies only while `ml_msp_fc_present()`
 * holds. Without it a bench unit with no FC would sit at the minimum forever and look like a fault.
 *
 * The goggle's commanded power arrives in SetTranParm (0x0d, live, ~2 s cadence) and SetLdCfg
 * (0x0a, once per association). The air reports its work mode in a 0x12 and drops the frame rate
 * only once the goggle's 0x1b ack comes back, so the receiver always knows before the stream
 * changes under it. Transmit duty (the baseband power_save period) is driven by the AP and is not
 * touched here.
 *
 * The frame-rate half moves the capture feeder alone (`capfps`) and leaves the encoders' declared
 * rate where it is, so the emitted bitrate falls with the frame rate rather than holding at the
 * configured bits per second.
 */
#ifndef ML_AIR_POWER_H
#define ML_AIR_POWER_H

#include <stdint.h>
#include <sys/types.h>

#include "ml-linkd.h"
#include "ml-air-bb.h"

/* pwr_range from the chip's insmod config bounds every commanded value. A byte outside it is
 * rejected rather than clamped, so a fabricated dBm never reaches the radio. */
#define AIR_POWER_MIN_DBM      5
#define AIR_POWER_MAX_DBM      23

#define AIR_POWER_IDLE_DBM     5      /* radiated power while the aircraft is disarmed */
#define AIR_POWER_FPS_NORMAL   60     /* the feeder rate, restored on leaving standby */
#define AIR_POWER_FPS_STANDBY  15

#define AIR_POWER_POLL_MS      1000   /* GET_POWER read-back cadence */
#define AIR_POWER_REPORT_MS    500    /* 0x12 re-send while a standby entry is unacked */
#define AIR_POWER_REPORT_MAX   10     /* re-sends before giving up: no vendor goggle ever acks */

/* Commanded state is only meaningful while the goggle is there to command it. After this much
 * :10000 silence the air returns to the chip's closed loop and full frame rate, so an unreachable
 * goggle cannot leave it pinned at the standby minimum. Matches the goggle's own TX_LOST window. */
#define AIR_POWER_LOST_MS      120000 /* :10000 silence before the radio goes back to the chip.
                                      * Not 5000: a healthy vendor link is silent for minutes
                                      * (measured 3 datagrams in 90 s), so a short window cancels a
                                      * commanded standby on a working link. See ml-air-power.c. */

#define AIR_POWER_PROBE_DUMPS  20     /* raw GET_POWER replies hexdumped before going quiet */

/* Send one 0x12 work-mode report to the goggle. Returns 0 when the datagram went out. */
typedef int (*air_power_report_fn)(uint32_t work_mode, void *ctx);

struct air_power {
    enum ml_power_mode mode;
    int holding;             /* holds a reference on the bb socket */
    long last_poll_ms;
    long last_rx_ms;         /* last :10000 datagram from the goggle, 0 = none outstanding */

    int commanded_dbm;       /* the goggle's last commanded power, -1 = never commanded */
    int standby_cmd;         /* the goggle's u8StandbyModeEn, 0/1 */

    int fc_present;          /* FC MSP telemetry is fresh, so arm_flag means something */
    int armed;               /* the FC's arm flag; only read while fc_present */
    int logged_arm;          /* last (fc_present, armed) pair logged, -1 = none */

    int work_mode;           /* live work mode: 0 normal, 1 standby */
    int reported_mode;       /* work mode the goggle has been told about */
    int acked;               /* the goggle's 0x1b for the current standby entry */
    long report_ms;          /* when the current work mode was last reported */
    int reports;             /* 0x12 re-sends issued for the current standby entry */

    int applied_dbm;         /* dBm last written to the chip, -1 = none */
    int applied_fps;         /* feeder fps last pushed to ml-air-video, 0 = never touched */
    int auto_disabled;       /* the chip's power self-adjust has been turned off */

    int read_dbm;            /* last GET_POWER read-back, -1 = none */
    int probe_dbm;           /* target last reported by the probe, -1 = none */
    int probe_dumped;
    int warned_ctrl;
    int warned_range;
};

void air_power_service(struct air_power *pw, struct air_bb *bb, long now,
                       air_power_report_fn report, void *ctx);
void air_power_tran_parm(struct air_power *pw, const uint8_t *dgram, ssize_t n);
void air_power_ld_cfg(struct air_power *pw, const uint8_t *dgram, ssize_t n);
void air_power_stb_ack(struct air_power *pw);
void air_power_rx(struct air_power *pw, long now);
void air_power_fc(struct air_power *pw, int fc_present, int armed);
void air_power_reply(struct air_power *pw, const uint8_t *pay, int plen);

#endif /* ML_AIR_POWER_H */
