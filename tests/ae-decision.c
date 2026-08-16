/**
 * @file ae-decision.c
 * @brief Host test: the air unit's AE decision law, metering and actuation mappings.
 *
 * This is the replay oracle that used to be `ml-aed --selftest` inside the daemon. It drives the
 * real ml-aed-core.c, which is the half that touches no file descriptors, so it needs no device,
 * no ISP and no capture session. It asserts:
 *
 *   1. the exposure table's oracle rows, including the two vendor operating points, so a
 *      regenerated ml-aed-exptable.h that moved cannot pass,
 *   2. both settled vendor points decide to stay put, which is the whole loop's resting behaviour,
 *   3. the recovered step arithmetic on synthetic lumas, including the min-step rule and the
 *      one-decision skip it arms,
 *   4. top saturation in the dark counting as settled, and the bottom clamp,
 *   5. the sensor gain-code inversion at the four validated points,
 *   6. the target curve's two clamp regions and one interpolated point,
 *   7. metering decimating the grid rather than averaging each block,
 *   8. the tone scalar staying on the band tables' axis across the whole index range.
 *
 * Two are worth the test on their own. The min-step skip is what stops the loop oscillating by one
 * index forever when the damped step truncates to zero, and asymmetric top saturation is what stops
 * the settle counter resetting every frame in the dark, which is the difference between a settled
 * reading and a permanently hunting one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../ml-aed/ml-aed-core.h"

static int fail(const char *what)
{
    fprintf(stderr, "ae-decision FAIL: %s\n", what);

    return 1;
}

int main(void)
{
    struct ae_state st;
    int step;

    if (mlaed_exp_table[0].gain_q8 != 256 || mlaed_exp_table[0].line_count != 1) {
        return fail("table[0]");
    }

    if (mlaed_exp_table[317].gain_q8 != 3938 ||
        mlaed_exp_table[283].gain_q8 != 1432) {
        return fail("table oracle rows 317/283");
    }

    if (mlaed_exp_table[365].gain_q8 != 16328 ||
        mlaed_exp_table[365].line_count != 1125) {
        return fail("table[365]");
    }

    /* Vendor live capture: settled at 317, luma 38.347, target 41. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&st, 38.347f);
    if (step != 0 || st.exp_index != 317 || st.settle_counter != 1) {
        return fail("live point should be settled");
    }

    /* Vendor bright capture: settled at 283, luma 45.278. */
    st = (struct ae_state){ .exp_index = 283 };
    step = ae_decide(&st, 45.278f);
    if (step != 0 || st.exp_index != 283) {
        return fail("bright point should be settled");
    }

    /* Dark scene: luma 20 at 317 steps up by 4. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&st, 20.0f);
    if (step != 4 || st.exp_index != 321) {
        return fail("luma 20 should step +4");
    }

    /* Slightly bright: luma 47, damped step truncates to 0, min-step -1. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&st, 47.0f);
    if (step != -1 || st.exp_index != 316 || st.skip_countdown != 1) {
        return fail("luma 47 should min-step -1 and arm the skip");
    }

    step = ae_decide(&st, 47.0f);
    if (step != 0 || st.exp_index != 316 || st.skip_countdown != 0) {
        return fail("armed skip should absorb the next decision");
    }

    /* Top saturation in the dark counts as settled. */
    st = (struct ae_state){ .exp_index = AE_INDEX_MAX };
    step = ae_decide(&st, 10.0f);
    if (step != 0 || st.settle_counter != 1) {
        return fail("dark at the top index should settle");
    }

    /* Bottom clamp. */
    st = (struct ae_state){ .exp_index = 2 };
    step = ae_decide(&st, 255.0f);
    if (st.exp_index != AE_INDEX_MIN) {
        return fail("bottom clamp");
    }

    /* Gain-code inversion at the validated points. */
    if (ae_sensor_gain_code(3938) != 0x3e) {
        return fail("gain 3938 should quantise to 0x3e");
    }

    if (ae_sensor_gain_code(1432) != 0x26) {
        return fail("gain 1432 should quantise to 0x26");
    }

    if (ae_sensor_gain_code(256) != 0x00) {
        return fail("gain 256 should quantise to 0");
    }

    if (ae_sensor_gain_code(16328) != 0x5f) {
        return fail("gain 16328 should quantise to 0x5f");
    }

    /* Target curve: clamp regions and one interpolated point. */
    if (ae_luma_target(317) != 41 || ae_luma_target(10) != 54) {
        return fail("target curve clamps");
    }

    if (ae_luma_target(100) != 53) {
        return fail("target at 100 should interpolate to 53");
    }

    /*
     * Synthetic grid. Count 99 with sum 4000 is a channel mean of 40 under
     * the count-plus-one divisor, so the frame meters 40. The unsampled rows
     * carry 24000, which must not move the result: that is what separates
     * decimation from an average over each block.
     */
    {
        uint8_t *rro = calloc(1, RRO_SIZE);
        unsigned int col, row, ch;
        float luma;

        if (!rro) {
            return fail("alloc");
        }

        for (col = 0; col < RRO_COLS; col++) {
            for (row = 0; row < RRO_ROWS; row++) {
                int sampled = (row % 2) == 0 && (col % 4) == 0;

                for (ch = 0; ch < 4; ch++) {
                    uint32_t off = col * RRO_COL_STRIDE + (row * 4 + ch) * 4;
                    uint32_t sum = sampled ? 4000 : 24000;

                    rro[off] = 99;
                    off += RRO_BLOCK;
                    rro[off] = sum & 0xff;
                    rro[off + 1] = (sum >> 8) & 0xff;
                }
            }
        }

        luma = ae_metered_luma(rro);
        free(rro);
        if (fabsf(luma - 40.0f) > 0.01f) {
            return fail("synthetic grid luma");
        }
    }

    /* The two measured operating points, and the ceiling. */
    if (ae_tone_scalar_q8(283) != 283 * 256 ||
        ae_tone_scalar_q8(317) != 317 * 256 ||
        ae_tone_scalar_q8(AE_INDEX_MAX) != AE_INDEX_MAX * 256) {
        return fail("tone scalar Q8 mapping");
    }

    /*
     * The whole index range must land on the band tables' 0..550 axis. Past
     * it the driver clamps to the last entry silently, so selection would
     * stop long before the loop did.
     */
    for (int i = AE_INDEX_MIN; i <= AE_INDEX_MAX; i++) {
        int q8 = ae_tone_scalar_q8(i);

        if (q8 <= 0 || q8 > 550 * 256) {
            return fail("tone scalar leaves the band-table axis");
        }
    }

    printf("ae-decision OK\n");

    return 0;
}
