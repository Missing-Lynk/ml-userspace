/*
 * ml-aed decision law: metering, the AE step, and the actuation mappings.
 *
 * Pure: no file descriptors, no clock, no globals. ml-aed.c does the I/O and owns the loop;
 * tests/ae-decision.c replays the vendor operating points on the host.
 *
 * The control variable is an index into the exposure table, read from the sensor tuning blob at
 * startup. One entry is {gain Q8, line_count}, and its gain is also the ladder abscissa
 * (gain_q8 / 256), so one index actuates sensor exposure, sensor gain and the ISP gain ladders.
 */
#ifndef ML_AED_CORE_H
#define ML_AED_CORE_H

#include <stdint.h>

#include "ml-aed-tuning.h"

/* Grid geometry, mirroring ar-isp-stats.h. */
#define RRO_COLS        36
#define RRO_ROWS        16
#define RRO_ZONES       (RRO_COLS * RRO_ROWS)
#define RRO_BLOCK       0x100
#define RRO_COL_STRIDE  0x200
#define RRO_SIZE        (RRO_COLS * RRO_COL_STRIDE)
#define HIST_SIZE       0x1000
#define STATS_RAW_SIZE  (4 + RRO_SIZE + HIST_SIZE + 4)

/* Vendor rebin: 36x16 decimated to 9 columns by 8 rows. */
#define BIN_COLS        9
#define BIN_ROWS        8

#define AE_MIN_STEP_SKIP        1

struct ae_state {
    int exp_index;
    int skip_countdown;
    unsigned int settle_counter;
};

uint32_t mlaed_get_le32(const uint8_t *p);

/*
 * Metered luma over one rro_stats grid, and the vendor's clamp on it. `rro` is the grid alone:
 * a caller holding a whole stats_raw snapshot passes buf + 4.
 */
float ae_metered_luma(const uint8_t *rro);
float ae_current_luma(float metered);

/* The five-knot target curve over exp_index, interpolated and clamped. */
int ae_luma_target(const struct ae_tuning *t, int exp_index);

/*
 * One decision; returns the step taken and updates the state. The saturation
 * rule is asymmetric on purpose: wanting more exposure at the top index counts
 * as settled, so the settle counter climbs in the dark instead of resetting
 * every frame.
 */
int ae_decide(const struct ae_tuning *t, struct ae_state *st, float current_luma);

/*
 * Sensor analogue gain: the largest code whose gain does not exceed the table
 * gain. The residue stays in the ladder abscissa, matching the vendor's split.
 */
unsigned int ae_sensor_gain_code(uint32_t gain_q8);

/*
 * The AEC trigger scalar gamma, DRC, cm and cm2 key on, in the Q8 the driver wants. Not the gain
 * the five ladders take.
 *
 * It is the exposure-table index the current luma would need to reach its target, which is
 * exp_index plus the luma error converted to table steps, truncated. The two differ only while AE
 * is off target, and they separate hard once the table saturates: with the lens covered the vendor
 * reads 445 where exp_index is pinned at its ceiling of 365. Because it is unclamped by the table,
 * it keeps describing the scene past the point exposure can follow, which is what the tone bands
 * need out to 550.
 */
#define AE_TONE_SCALAR_MAX      550

int ae_tone_scalar_q8(const struct ae_tuning *t, int exp_index, float current_luma);

#endif /* ML_AED_CORE_H */
