/*
 * ml-aed decision law: metering, the AE step, and the actuation mappings.
 *
 * Everything here is pure: no file descriptors, no clock, no globals. The
 * daemon half (ml-aed.c) does the sysfs and debugfs I/O and owns the loop, and
 * tests/ae-decision.c replays the vendor operating points against this half on
 * the host. Recovery notes: plans/au-ae-decision-law.md.
 *
 * The control variable is an index into the 366-entry exposure table
 * (ml-aed-exptable.h). One entry is {gain Q8, line_count}, and its gain is also
 * the ladder abscissa (gain_q8 / 256), so one index actuates sensor exposure,
 * sensor gain and the ISP gain ladders together.
 */
#ifndef ML_AED_CORE_H
#define ML_AED_CORE_H

#include <stdint.h>

#include "ml-aed-exptable.h"

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

/*
 * Every AE constant below is a field of nt99235_tuning_preview_fpv.bin, the
 * blob the ISP driver request_firmware()s. None is a literal in
 * libmpp_service.so. Blob offsets are the source of record; the heap offsets
 * are into the AEC state block at 0x9c47a0 in out/au-vendor-session/
 * heap-live.bin, a runtime copy, so a fresh dump can be checked against it.
 *
 *   blob 0xb6524  366 x {gain Q8, line_count} u32   exposure table
 *                                                   (heap state+0x68)
 *   blob 0xba560  5 x {index, target} u32           luma target curve
 *                                                   (heap state+0x4000+184)
 *   blob 0xba5ec  512, 1024, 512, then shift 11     zone-luma weights
 *                                                   (heap state+0x4000+120)
 *   blob 0xba5fc  50                                damping numerator
 *                                                   (heap state+40)
 *   blob 0xba600  5                                 settle tolerance
 *                                                   (heap state+44)
 *   blob 0xba620  77.893997f                        log ladder
 *                                                   (heap state+36)
 *
 * Blob and heap agree over all 366 table entries. Replacing these with a
 * runtime read of the blob is plans/isp-tuning-blob-wrapper.md phase 2.
 */
#define AE_TOLERANCE            5.0f
#define AE_DAMPING              (50.0f / 256.0f)
#define AE_LOG_LADDER           77.893997f
#define AE_MIN_STEP_SKIP        1
#define AE_INDEX_MIN            1
#define AE_INDEX_MAX            (MLAED_EXP_TABLE_LEN - 1)
#define AE_TARGET_KNOTS         5

struct ae_state {
    int exp_index;
    int skip_countdown;
    unsigned int settle_counter;
};

uint32_t mlaed_get_le32(const uint8_t *p);

/*
 * Metered luma over one rro_stats grid, and the vendor's clamp on it. `rro` is
 * the grid alone, so a caller holding a whole stats_raw snapshot passes
 * buf + 4.
 */
float ae_metered_luma(const uint8_t *rro);
float ae_current_luma(float metered);

/* The five-knot target curve over exp_index, interpolated and clamped. */
int ae_luma_target(int exp_index);

/*
 * One decision; returns the step taken and updates the state. The saturation
 * rule is asymmetric on purpose: wanting more exposure at the top index counts
 * as settled, so the settle counter climbs in the dark instead of resetting
 * every frame.
 */
int ae_decide(struct ae_state *st, float current_luma);

/*
 * Sensor analogue gain: the largest code whose gain does not exceed the table
 * gain. The residue stays in the ladder abscissa, matching the vendor's split.
 */
unsigned int ae_sensor_gain_code(uint32_t gain_q8);

/*
 * The AEC trigger scalar gamma, DRC, cm and cm2 key on, in the Q8 the driver
 * wants. Not the gain the five ladders take. Its producer is unproven: the
 * exposure index is the standing candidate, contradicted by none of the three
 * vendor captures (plans/isp-tone-selector.md), which is why --tone is opt-in.
 */
int ae_tone_scalar_q8(int exp_index);

#endif /* ML_AED_CORE_H */
