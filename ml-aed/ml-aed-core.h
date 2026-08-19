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

/*
 * Mains anti-flicker, the vendor's aec_process_apply_flicker_50_60hz.
 *
 * Artificial light rippples at twice the mains frequency, so an exposure that is not a whole
 * number of half-periods integrates a different amount of light on each row and the frame carries
 * bands. This snaps the exposure down to a whole number of half-periods and multiplies gain by the
 * inverse ratio, holding the gain-times-time product, then converts back to whole lines.
 *
 * `mains_hz` is 0 (leave alone), 50 or 60, matching the blob's ae_antibanding selector and the
 * SetCameraInfo banding field. `line_ns` is the sensor line time. `lines` and `gain_q8` are the
 * exposure-table entry; both are updated in place.
 *
 * The exposure-table entry itself is NOT rewritten by the vendor, only the sensor-bound copy, so
 * the gain-keyed ladder abscissa keeps the table's gain and is not compensated. Callers must pass
 * copies for that reason.
 *
 * Returns 1 when it changed something, 0 when it left the pair alone. It never lengthens an
 * exposure: an exposure already shorter than one half-period is returned untouched, which is the
 * vendor's own early-out and is why bright scenes are not correctable this way.
 */
int ae_flicker_snap(int mains_hz, unsigned int line_ns, uint32_t *lines, uint32_t *gain_q8);

/*
 * The banding-file contents, parsed by the rule the ml-air-ae init script
 * established: an empty file (or a bare newline) means 50, otherwise the
 * number, with anything but 50 and 60 forced to off. An absent file means off
 * and is the caller's case, since this takes text and not a path.
 */
int ae_banding_parse(const char *text);

/*
 * Loop health over a window of decisions: the two numbers glue/boot/au-health.sh
 * derives from the outside (index moves against the stats_flips delta, and the
 * mean luma error against target), accumulated by the loop itself so a recorded
 * run carries them without a harness. A zeroed struct is an empty window.
 *
 * The hold rate is only meaningful while decisions are being made at all: a
 * dead loop makes no moves and would read as a perfect hold, which is the
 * caller's gate (au-health checks for a running daemon; inside ml-aed the
 * window only accumulates on real decisions, so the number cannot be produced
 * by a stalled loop).
 */
struct ae_health {
    unsigned int decisions;
    unsigned int moves;
    float err_sum;              /* sum of |current_luma - target| */
};

void ae_health_update(struct ae_health *h, int step, float current_luma, int target);

/* Percent of the window's decisions that held, 100 for an empty window. */
unsigned int ae_health_hold_pct(const struct ae_health *h);

/* Mean |luma error| over the window, 0 for an empty window. */
float ae_health_mean_err(const struct ae_health *h);

#endif /* ML_AED_CORE_H */
