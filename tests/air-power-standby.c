/**
 * @file air-power-standby.c
 * @brief Host test: the air's TX power and standby state machine.
 *
 * The lever this exercises is the one that shrinks the link's margin, so the ordering constraints
 * matter more than the values. It drives the real ml-air-power.c against stubbed bb and control
 * sockets, recording every frame that would have reached the radio and every line that would have
 * reached ml-air-video, and asserts:
 *
 *   1. a commanded power reaches the radio exactly once, with the chip's self-adjust turned off
 *      first, and a repeat of the same value writes nothing,
 *   2. a dBm outside the chip's pwr_range is rejected and leaves the applied value alone,
 *   3. entering standby reports the work mode and drops NOTHING until the goggle's ack arrives,
 *   4. the frame-rate drop moves the capture feeder alone, so the encoders keep budgeting bits for
 *      their declared rate and the emitted bitrate falls with the frame rate,
 *   5. the whole policy truth table: power follows the FC arm state, the frame rate follows the
 *      goggle's standby arm, arming overrides both, and an absent FC is arm-UNKNOWN rather than
 *      disarmed so the goggle's value stands,
 *   6. a goggle that goes silent releases the radio back to its closed loop.
 *
 * Two are worth the test on their own. Dropping power before the receiver has acknowledged is how a
 * standby entry turns into a lost link. And treating an absent FC as disarmed would pin a bench
 * unit at the minimum forever, which is a failure this project has already misdiagnosed once.
 */
#include "../ml-linkd/bb-cmd.h"
#include "../ml-linkd/mp-cmd.h"
#include "../ml-linkd/ml-air-power.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The stubs stand in for ml-air-bb.c, ml-air-ctrl.c and ml-linkd.c. */
int g_verbose;

static int stub_power_auto = -1;    /* last SET_POWER_AUTO argument, -1 = never sent */
static int stub_power_dbm = -1;     /* last SET_POWER dBm, -1 = never sent */
static int stub_power_writes;       /* SET_POWER frames accepted */
static int stub_auto_writes;        /* SET_POWER_AUTO frames accepted */
static int stub_fps = -1;           /* last feeder fps pushed, -1 = never */
static int stub_fps_writes;
static char stub_cmd[64];           /* last control line, verbatim */
static uint32_t stub_report_mode = 0xffffffff;
static int stub_reports;

int air_bb_acquire(struct air_bb *bb, long now, const char *why)
{
    (void)now;
    (void)why;
    bb->fd = 3;
    bb->users++;

    return 0;
}

/* Decode the frames this test cares about out of the built wire bytes, so the payload packing is
 * covered rather than trusted: class at [5], selector at [8], payload from [18]. */
int air_bb_send(struct air_bb *bb, const uint8_t *frame, int len, const char *what)
{
    (void)bb;
    (void)what;

    if (len < 20 || frame[0] != 0xaa) {
        return -1;
    }

    if (frame[5] == BB_SET && frame[8] == SET_POWER) {
        stub_power_dbm = frame[19];
        stub_power_writes++;
    } else if (frame[5] == BB_SET && frame[8] == SET_POWER_AUTO) {
        stub_power_auto = frame[18];
        stub_auto_writes++;
    }

    return 0;
}

/* The verb matters as much as the number: `capfps` moves the feeder alone and leaves the encoders
 * budgeting for their declared rate, which is what makes the bitrate fall with the frame rate.
 * `fps` would hold the two in step and the bitrate flat, so this records the line verbatim. */
int air_ctrl_send(const char *cmd)
{
    snprintf(stub_cmd, sizeof stub_cmd, "%s", cmd);

    if (sscanf(cmd, "capfps %d", &stub_fps) == 1) {
        stub_fps_writes++;
    }

    return 0;
}

static int stub_report(uint32_t work_mode, void *ctx)
{
    (void)ctx;
    stub_report_mode = work_mode;
    stub_reports++;

    return 0;
}

static int g_failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failed++;
    }
}

/* One SetTranParm datagram carrying @p dbm and @p standby, fed through the real decoder. */
static void command(struct air_power *pw, uint8_t dbm, uint8_t standby)
{
    uint8_t frame[MP_STP_LEN];

    air_power_tran_parm(pw, frame, mp_set_tran_parm(frame, dbm, standby, 0));
}

/* The policy is a truth table over (FC present, armed, standby), so walk every row of it. Power
 * follows the arm state and the frame rate follows standby, with arming overriding both. */
static void row(struct air_power *pw, struct air_bb *bb, long now,
                int fc_present, int armed, int standby,
                int want_dbm, int want_fps, const char *what)
{
    air_power_fc(pw, fc_present, armed);
    command(pw, 20, (uint8_t)standby);

    /* A standby entry only takes effect once acked, so ack it the way the goggle would. */
    air_power_service(pw, bb, now, stub_report, NULL);
    if (standby) {
        air_power_stb_ack(pw);
    }
    air_power_service(pw, bb, now, stub_report, NULL);

    check(stub_power_dbm == want_dbm && stub_fps == want_fps, what);
    if (stub_power_dbm != want_dbm || stub_fps != want_fps) {
        printf("       wanted %d dBm / %d fps, got %d dBm / %d fps\n",
               want_dbm, want_fps, stub_power_dbm, stub_fps);
    }
}

int main(void)
{
    struct air_power pw = { .mode = ML_POWER_ON, .commanded_dbm = -1, .applied_dbm = -1,
                            .read_dbm = -1, .probe_dbm = -1, .logged_arm = -1 };
    struct air_bb bb = { .fd = -1 };
    long now = 1000;

    /* No FC anywhere in parts 1-4, which is the bench case: arm is unknown, so the goggle's
     * commanded power stands and only standby moves the frame rate. */

    /* 1: a commanded power reaches the radio once, self-adjust off first. */
    air_power_rx(&pw, now);
    command(&pw, 20, 0);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_auto_writes == 1 && stub_power_auto == 0, "self-adjust disabled before the first write");
    check(stub_power_writes == 1 && stub_power_dbm == 20, "commanded 20 dBm applied");
    check(stub_reports == 0, "no work-mode report while the mode has not changed");

    now += 2000;
    air_power_rx(&pw, now);
    command(&pw, 20, 0);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_power_writes == 1, "a repeat of the same power writes nothing");

    /* 2: an out-of-range dBm is rejected, and the applied value is untouched. */
    now += 2000;
    air_power_rx(&pw, now);
    command(&pw, 30, 0);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_power_writes == 1 && stub_power_dbm == 20, "30 dBm rejected, 20 dBm still applied");

    /* 3: standby is reported, and the frame rate does not drop until the goggle acks. */
    now += 2000;
    air_power_rx(&pw, now);
    command(&pw, 20, 1);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_reports == 1 && stub_report_mode == MP_STANDBY_ON, "standby entry reported");
    check(stub_fps_writes == 0, "frame rate held until the ack");

    now += AIR_POWER_REPORT_MS;
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_reports == 2, "an unacked standby entry is re-reported");

    now += 100;
    air_power_stb_ack(&pw);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_fps_writes == 1 && stub_fps == AIR_POWER_FPS_STANDBY, "standby drops the frame rate after the ack");
    check(strcmp(stub_cmd, "capfps 15\n") == 0, "the frame-rate drop moves the FEEDER only (capfps, not fps)");
    check(stub_power_writes == 1 && stub_power_dbm == 20,
          "standby does NOT move power with no FC: arm is unknown, so the goggle stands");

    /* 4: leaving standby restores the frame rate. */
    now += 100;
    air_power_rx(&pw, now);
    command(&pw, 20, 0);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_fps_writes == 2 && stub_fps == AIR_POWER_FPS_NORMAL, "leaving standby restores the frame rate");
    check(strcmp(stub_cmd, "capfps 60\n") == 0, "the restore also moves the feeder only");
    check(stub_reports == 3 && stub_report_mode == MP_STANDBY_NORMAL, "normal work mode reported on exit");

    /* 5: the policy truth table. Power follows the arm state, the frame rate follows standby, and
     * arming overrides both. An absent FC is arm-unknown, not disarmed. */
    printf("  -- policy truth table --\n");
    now += 100;
    air_power_rx(&pw, now);
    row(&pw, &bb, now, 0, 0, 0, 20, 60, "no FC,   disarmed, no standby -> goggle power, 60 fps");
    row(&pw, &bb, now, 0, 0, 1, 20, 15, "no FC,   disarmed, standby    -> goggle power, 15 fps");
    row(&pw, &bb, now, 1, 0, 0,  5, 60, "FC,      disarmed, no standby -> MINIMUM,      60 fps");
    row(&pw, &bb, now, 1, 0, 1,  5, 15, "FC,      disarmed, standby    -> MINIMUM,      15 fps");
    row(&pw, &bb, now, 1, 1, 0, 20, 60, "FC,      ARMED,    no standby -> goggle power, 60 fps");
    row(&pw, &bb, now, 1, 1, 1, 20, 60, "FC,      ARMED,    standby    -> goggle power, 60 fps (arm wins)");

    /* Arming cancels a commanded standby in the report too, so the goggle is never shown a standby
     * the air is not honouring. */
    check(stub_report_mode == MP_STANDBY_NORMAL, "an armed aircraft reports work mode 0 despite the standby command");

    /* 6: a silent goggle releases the radio back to the chip. */
    now += AIR_POWER_LOST_MS + 1;
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_power_auto == 1, "a silent goggle re-enables the chip's self-adjust");
    check(pw.commanded_dbm == -1, "the commanded power is forgotten with the goggle");

    int auto_writes_after_release = stub_auto_writes;
    now += 100;
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_auto_writes == auto_writes_after_release, "the release happens once");
    check(stub_power_dbm == 20, "a disarmed unit writes no power while no goggle is commanding");

    /* A returning goggle re-commands from scratch, self-adjust off again. */
    now += 100;
    air_power_fc(&pw, 1, 0);
    air_power_rx(&pw, now);
    command(&pw, 14, 0);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_power_auto == 0, "a returning goggle disables self-adjust again");
    check(stub_power_dbm == AIR_POWER_IDLE_DBM, "a returning goggle on a DISARMED unit lands at the minimum");

    now += 100;
    air_power_fc(&pw, 1, 1);
    air_power_service(&pw, &bb, now, stub_report, NULL);
    check(stub_power_dbm == 14, "arming then applies the goggle's commanded 14 dBm");

    printf("%s\n", g_failed == 0 ? "air-power-standby: all checks passed" : "air-power-standby: FAILED");

    return g_failed == 0 ? 0 : 1;
}
