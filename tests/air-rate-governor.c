/**
 * @file air-rate-governor.c
 * @brief Host test: the air's MCS-driven encoder rate governor.
 *
 * This is the lever that decides how many bits the encoder spends against a link whose capacity is
 * moving. Getting the asymmetry wrong is not a visible bug on a bench: a rate that rises as eagerly
 * as it falls only shows up as breakup at the edge of range, on a flying aircraft, where the
 * measurement costs a battery. So the policy is asserted here against the real ml-air-rate.c,
 * driven through synthesised GET_MCS replies with stubbed bb and control sockets.
 *
 * What it pins:
 *
 *   1. drop-fast/rise-slow: a lower MCS is acted on immediately, a higher one only after it has
 *      held for AIR_RATE_RISE_MS, which is what the vendor's split between MCS_CHANGE and
 *      MCS_CHANGE_FINISHED amounts to,
 *   2. a rise that does not hold is abandoned, and a different rise restarts the window,
 *   3. throughput moving on its own does not move the rate: the vendor recomputes on MCS
 *      transitions only, so a noisy throughput reading cannot thrash the encoder,
 *   4. the derivation itself, including the integer divide that happens BEFORE the multiply and
 *      the cliff that divide creates below 100 kbps,
 *   5. a control socket that does not answer leaves the applied value alone, so the next sample
 *      retries rather than believing a write that never landed.
 *
 * Point 4 is worth the test on its own. The quantisation and the fallback look like defects and are
 * not: both are transcribed from the vendor's AR_8030_TX_GetBitRate, and a well-meant "fix" to
 * either would put us off the rate a vendor goggle expects for the same link conditions.
 */
#include "../ml-linkd/bb-cmd.h"
#include "../ml-linkd/ml-air-rate.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The stubs stand in for ml-air-bb.c, ml-air-ctrl.c and ml-linkd.c. */
atomic_int g_run = 1;
int g_verbose;

static char stub_cmd[64];       /* last control line, verbatim */
static int stub_bps = -1;       /* last bitrate pushed, -1 = never */
static int stub_writes;         /* control lines accepted */
static int stub_ctrl_fail;      /* make the control socket refuse */
static int stub_bb_sends;

int air_bb_acquire(struct air_bb *bb, long now, const char *why)
{
    (void)now;
    (void)why;
    bb->fd = 3;
    bb->users++;

    return 0;
}

void air_bb_release(struct air_bb *bb)
{
    bb->users--;
}

int air_bb_send(struct air_bb *bb, const uint8_t *frame, int len, const char *what)
{
    (void)bb;
    (void)what;

    if (len < BB_FRAME_EXTRA || frame[BB_OFF_MAGIC] != 0xaa) {
        return -1;
    }

    stub_bb_sends++;

    return 0;
}

/* The verb is asserted as well as the number: `bitrate` is a per-tile encoder target, and the
 * governor derives a TOTAL that it has to halve before sending. Recording the line verbatim is
 * what catches a future change that sends the total.
 */
int air_ctrl_send(const char *cmd)
{
    if (stub_ctrl_fail) {
        return -1;
    }

    snprintf(stub_cmd, sizeof stub_cmd, "%s", cmd);
    if (sscanf(cmd, "bitrate %d", &stub_bps) == 1) {
        stub_writes++;
    }

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

/* One GET_MCS reply: the index biased by +2 at byte 0, the throughput as a little-endian u32 at 4.
 * Built here rather than hand-fed so the decode in air_rate_reply is covered too.
 */
static void sample(struct air_rate *rate, int mcs, uint32_t throughput_kbps, long now)
{
    uint8_t pay[8] = { 0 };

    pay[MCS_OFF_INDEX] = (uint8_t)(mcs + MCS_INDEX_BIAS);
    pay[MCS_OFF_THROUGHPUT + 0] = (uint8_t)(throughput_kbps);
    pay[MCS_OFF_THROUGHPUT + 1] = (uint8_t)(throughput_kbps >> 8);
    pay[MCS_OFF_THROUGHPUT + 2] = (uint8_t)(throughput_kbps >> 16);
    pay[MCS_OFF_THROUGHPUT + 3] = (uint8_t)(throughput_kbps >> 24);

    air_rate_reply(rate, pay, (int)sizeof pay, now);
}

static void reset(struct air_rate *rate, enum ml_rate_mode mode)
{
    memset(rate, 0, sizeof *rate);
    rate->mode = mode;
    rate->mcs = -1;
    rate->pending_mcs = -1;
    stub_bps = -1;
    stub_writes = 0;
    stub_ctrl_fail = 0;
    stub_cmd[0] = 0;
}

/** @brief The drop-fast / rise-slow asymmetry. */
static void check_hysteresis(void)
{
    struct air_rate rate;

    printf("  -- hysteresis --\n");
    reset(&rate, ML_RATE_ON);

    sample(&rate, 5, 10000, 1000);
    check(stub_writes == 1 && stub_bps == 3500000, "the first sample seeds the baseline at once");
    check(strcmp(stub_cmd, "bitrate 3500000\n") == 0,
          "the control line is the per-tile bitrate, halved from the derived total");

    sample(&rate, 7, 20000, 2000);
    check(stub_writes == 1, "a higher MCS writes nothing on arrival");
    sample(&rate, 7, 20000, 2000 + AIR_RATE_RISE_MS - 1);
    check(stub_writes == 1, "and still nothing one millisecond short of the settle window");
    sample(&rate, 7, 20000, 2000 + AIR_RATE_RISE_MS);
    check(stub_writes == 2 && stub_bps == 7000000, "the rise applies once the window has elapsed");

    sample(&rate, 3, 10000, 10000);
    check(stub_writes == 3 && stub_bps == 3500000, "a lower MCS applies immediately, no window");

    /* A rise that does not hold. Without this, a single noisy high sample followed by a return to
     * the current rate would still fire once the window expired.
     */
    sample(&rate, 6, 20000, 20000);
    sample(&rate, 3, 20000, 20100);
    sample(&rate, 6, 20000, 20000 + AIR_RATE_RISE_MS + 1);
    check(stub_writes == 3, "a rise interrupted by the current MCS is abandoned, not resumed");

    /* A different higher MCS restarts the window rather than inheriting the first one's age. */
    reset(&rate, ML_RATE_ON);
    sample(&rate, 3, 10000, 0);
    sample(&rate, 5, 20000, 100);
    sample(&rate, 6, 20000, 100 + AIR_RATE_RISE_MS);
    check(stub_writes == 1, "a second, different rise restarts the settle window");
    sample(&rate, 6, 20000, 100 + 2 * AIR_RATE_RISE_MS);
    check(stub_writes == 2, "and applies a full window after ITS own arrival");
}

/** @brief Only MCS transitions recompute; throughput alone does not. */
static void check_throughput_alone(void)
{
    struct air_rate rate;

    printf("  -- throughput is not a trigger --\n");
    reset(&rate, ML_RATE_ON);

    sample(&rate, 5, 10000, 0);
    check(stub_writes == 1, "baseline applied");
    sample(&rate, 5, 20000, 1000);
    sample(&rate, 5, 1000, 2000);
    check(stub_writes == 1 && stub_bps == 3500000,
          "the same MCS with a moved throughput does not recompute the rate");

    /* Two different inputs that derive the same number must not produce a second write: the
     * governor compares the derived value, not the inputs.
     */
    sample(&rate, 4, 10099, 3000);
    check(stub_writes == 1, "a drop deriving the already-applied rate writes nothing");
}

/** @brief The vendor derivation, including the parts that look wrong and are not. */
static void check_derivation(void)
{
    struct air_rate rate;

    printf("  -- derivation --\n");

    /* The integer divide by 100 happens BEFORE the multiply, so throughput is quantised to
     * 100 kbps steps and these two inputs are the same rate.
     */
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 10000, 0);
    check(stub_bps == 3500000, "10000 kbps derives 3.5 Mbps per tile");
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 10099, 0);
    check(stub_bps == 3500000, "10099 kbps derives the same: the divide quantises to 100 kbps");

    /* The cliff the quantisation creates. Below 100 kbps the derived total is zero, and zero is
     * replaced by the vendor's 8 Mbps default rather than by a floor near the measured rate. It
     * reads like a bug and is the transcribed behaviour; the assertion is here so a "fix" is a
     * deliberate divergence from the vendor rather than a tidy-up.
     */
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 99, 0);
    check(stub_bps == 4000000, "99 kbps derives ZERO and falls back to the 8 Mbps default");
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 100, 0);
    check(stub_bps == 35000, "100 kbps derives 70 kbps total, a 100x step across that boundary");

    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 0, 0);
    check(stub_bps == 4000000, "a zero throughput falls back to the 8 Mbps default");

    /* The clamp, and a throughput whose high bit is set. air_rate_reply widens the u32 into an int,
     * so a garbage reply arrives negative; it must land on the fallback, not on a negative rate.
     */
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 100000, 0);
    check(stub_bps == 10000000, "the total is clamped to AIR_RATE_MAX_KBPS");
    reset(&rate, ML_RATE_ON);
    sample(&rate, 5, 0x80000000u, 0);
    check(stub_bps == 4000000, "a throughput read as negative falls back rather than going below zero");

    /* The low-MCS branch is dead on a stock unit (Ar803xMinMcs is -1 and MCS is never negative),
     * but the constant is the switch, so it is exercised directly.
     */
    reset(&rate, ML_RATE_ON);
    sample(&rate, AIR_RATE_MIN_MCS, 10000, 0);
    check(stub_bps == 4000000, "at or below AIR_RATE_MIN_MCS the low-MCS ratio applies instead");
}

/** @brief Reply decoding and the modes that must stay silent. */
static void check_decode_and_modes(void)
{
    struct air_rate rate;
    uint8_t pay[8] = { 0 };

    printf("  -- decode and modes --\n");

    /* The +2 bias is the same constant the SET side writes with; a disagreement would silently
     * shift every derived rate by two MCS steps.
     */
    reset(&rate, ML_RATE_ON);
    sample(&rate, 0, 10000, 0);
    check(rate.mcs == 0, "MCS 0 arrives as the biased byte 2");
    reset(&rate, ML_RATE_ON);
    sample(&rate, 7, 10000, 0);
    check(rate.mcs == 7, "MCS 7 arrives as the biased byte 9");

    reset(&rate, ML_RATE_ON);
    air_rate_reply(&rate, pay, MCS_OFF_THROUGHPUT + 3, 0);
    check(stub_writes == 0 && rate.mcs == -1,
          "a reply too short to hold the throughput is dropped, not decoded from stack bytes");

    reset(&rate, ML_RATE_PROBE);
    sample(&rate, 5, 10000, 0);
    check(stub_writes == 0, "PROBE derives and logs but sends nothing");

    reset(&rate, ML_RATE_OFF);
    {
        struct air_bb bb = { .fd = -1 };

        stub_bb_sends = 0;
        air_rate_service(&rate, &bb, 100000);
        check(stub_bb_sends == 0 && bb.users == 0,
              "OFF does not so much as open the bb socket");
    }

    /* A control socket that does not answer must leave applied_bps alone. Recording the write
     * anyway would mean the next identical sample is suppressed as a duplicate and the encoder
     * never gets the rate at all.
     */
    reset(&rate, ML_RATE_ON);
    stub_ctrl_fail = 1;
    sample(&rate, 5, 10000, 0);
    check(rate.applied_bps == 0, "a refused control write is not recorded as applied");
    stub_ctrl_fail = 0;
    sample(&rate, 4, 10000, 1000);
    check(stub_writes == 1 && stub_bps == 3500000, "so the next sample retries and lands");
}

int main(void)
{
    check_hysteresis();
    check_throughput_alone();
    check_derivation();
    check_decode_and_modes();

    printf("%s\n", g_failed == 0 ? "air-rate-governor: all checks passed"
                                 : "air-rate-governor: FAILED");

    return g_failed == 0 ? 0 : 1;
}
