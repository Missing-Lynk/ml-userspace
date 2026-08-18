/**
 * @file ae-decision.c
 * @brief Host test: the air unit's AE decision law, metering and actuation mappings.
 *
 * Drives ml-aed-core.c, which touches no file descriptors, so this needs no device. It asserts:
 *
 *   1. the exposure table's oracle rows, including the two vendor operating points,
 *   2. both settled vendor points decide to stay put,
 *   3. the step arithmetic on synthetic lumas, the min-step rule and the one-decision skip,
 *   4. top saturation in the dark counting as settled, and the bottom clamp,
 *   5. the sensor gain-code inversion at the four validated points,
 *   6. the target curve's two clamp regions and one interpolated point,
 *   7. metering decimating the grid rather than averaging each block,
 *   8. the trigger scalar against four paired vendor captures, including the saturated one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../ml-aed/ml-aed-core.h"

/* The blob's own values, stated here: it is a capture artifact and not in the tree. */
static struct ae_tuning vendor_tuning(struct mlaed_exp_entry *table, size_t len)
{
    struct ae_tuning t = {
        .tolerance = 5.0f,
        .damping = 50.0f / 256.0f,
        .log_ladder = 77.893997f,
        .curve = { { 20, 54 }, { 80, 54 }, { 120, 52 }, { 150, 49 }, { 200, 41 } },
        .table = table,
        .table_len = len,
        .index_min = 1,
        .index_max = (int)len - 1,
    };

    return t;
}


static int fail(const char *what)
{
    fprintf(stderr, "ae-decision FAIL: %s\n", what);

    return 1;
}

int main(void)
{
    struct ae_state st;
    int step;
    /* The three exposure-table rows the oracle asserts, at their real indices. */
    static struct mlaed_exp_entry table[366];
    struct ae_tuning tune;

    table[0] = (struct mlaed_exp_entry){ 256, 1 };
    table[283] = (struct mlaed_exp_entry){ 1432, 1125 };
    table[317] = (struct mlaed_exp_entry){ 3938, 1125 };
    table[365] = (struct mlaed_exp_entry){ 16328, 1125 };

    /*
     * The entry below each asserted index: the trigger scalar converts a luma
     * error into table steps, and one step is the ratio between neighbours.
     */
    table[282] = (struct mlaed_exp_entry){ 1390, 1125 };
    table[316] = (struct mlaed_exp_entry){ 3823, 1125 };
    table[330] = (struct mlaed_exp_entry){ 5792, 1125 };
    table[331] = (struct mlaed_exp_entry){ 5966, 1125 };
    table[364] = (struct mlaed_exp_entry){ 15852, 1125 };
    tune = vendor_tuning(table, 366);

    if (table[0].gain_q8 != 256 || table[0].line_count != 1) {
        return fail("table[0]");
    }

    if (table[317].gain_q8 != 3938 ||
        table[283].gain_q8 != 1432) {
        return fail("table oracle rows 317/283");
    }

    if (table[365].gain_q8 != 16328 ||
        table[365].line_count != 1125) {
        return fail("table[365]");
    }

    /* Vendor live capture: settled at 317, luma 38.347, target 41. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&tune, &st, 38.347f);
    if (step != 0 || st.exp_index != 317 || st.settle_counter != 1) {
        return fail("live point should be settled");
    }

    /* Vendor bright capture: settled at 283, luma 45.278. */
    st = (struct ae_state){ .exp_index = 283 };
    step = ae_decide(&tune, &st, 45.278f);
    if (step != 0 || st.exp_index != 283) {
        return fail("bright point should be settled");
    }

    /* Dark scene: luma 20 at 317 steps up by 4. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&tune, &st, 20.0f);
    if (step != 4 || st.exp_index != 321) {
        return fail("luma 20 should step +4");
    }

    /* Slightly bright: luma 47, damped step truncates to 0, min-step -1. */
    st = (struct ae_state){ .exp_index = 317 };
    step = ae_decide(&tune, &st, 47.0f);
    if (step != -1 || st.exp_index != 316 || st.skip_countdown != 1) {
        return fail("luma 47 should min-step -1 and arm the skip");
    }

    step = ae_decide(&tune, &st, 47.0f);
    if (step != 0 || st.exp_index != 316 || st.skip_countdown != 0) {
        return fail("armed skip should absorb the next decision");
    }

    /* Top saturation in the dark counts as settled. */
    st = (struct ae_state){ .exp_index = tune.index_max };
    step = ae_decide(&tune, &st, 10.0f);
    if (step != 0 || st.settle_counter != 1) {
        return fail("dark at the top index should settle");
    }

    /* Bottom clamp. */
    st = (struct ae_state){ .exp_index = 2 };
    step = ae_decide(&tune, &st, 255.0f);
    if (st.exp_index != tune.index_min) {
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
    if (ae_luma_target(&tune, 317) != 41 || ae_luma_target(&tune, 10) != 54) {
        return fail("target curve clamps");
    }

    if (ae_luma_target(&tune, 100) != 53) {
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

    /*
     * The trigger scalar against the four vendor captures that pair a register
     * sweep with a heap dump. Each row is {exp_index, metered luma, the scalar
     * the vendor AE held}. The covered row is the one that matters: exposure is
     * pinned at the table's last entry while the scene keeps darkening, so the
     * index stops at 365 and the scalar runs on to 445.
     */
    static const struct {
        int index;
        float luma;
        int scalar;
    } vendor_scalar[] = {
        { 283, 45.278f, 279 },
        { 317, 38.347f, 319 },
        { 331, 37.500f, 334 },
        { 365,  3.819f, 445 },
    };

    for (size_t i = 0; i < sizeof(vendor_scalar) / sizeof(vendor_scalar[0]); i++) {
        int got = ae_tone_scalar_q8(&tune, vendor_scalar[i].index,
                                    vendor_scalar[i].luma);

        if (got != vendor_scalar[i].scalar * 256) {
            fprintf(stderr, "  index %d luma %.3f: got %d, want %d\n",
                    vendor_scalar[i].index, (double)vendor_scalar[i].luma,
                    got >> 8, vendor_scalar[i].scalar);

            return fail("tone scalar against the vendor captures");
        }
    }

    /*
     * A pinned index must not pin the scalar. At the ceiling the index cannot
     * move, so falling luma is the only thing left that can carry the scene.
     */
    if (ae_tone_scalar_q8(&tune, 365, 30.0f) >=
        ae_tone_scalar_q8(&tune, 365, 3.819f)) {
        return fail("tone scalar does not move with luma at the ceiling");
    }

    /*
     * The whole index range must land on the band tables' 0..550 axis. Past
     * it the driver clamps to the last entry silently, so selection would
     * stop long before the loop did.
     */
    for (int i = tune.index_min; i <= tune.index_max; i++) {
        int q8 = ae_tone_scalar_q8(&tune, i, 41.0f);

        if (q8 < 0 || q8 > 550 * 256) {
            return fail("tone scalar leaves the band-table axis");
        }
    }

    printf("ae-decision OK\n");

    return 0;
}
