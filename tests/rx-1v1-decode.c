/**
 * @file rx-1v1-decode.c
 * @brief Host test: decoding the chip's Get1V1Info reply into the System OSD's link metrics.
 *
 * Get1V1Info is the goggle's only source for SNR, ranging distance and measured link throughput, so
 * this decode is what the pilot actually reads while flying. Two things about it are easy to break
 * and impossible to notice on a bench:
 *
 *   1. the last-good filter. The chip answers the steady poll with an EMPTY reply whenever video is
 *      idle or the Tx link blips: same length, plausible-looking bytes, but a zero raw SNR. Writing
 *      those through would flicker the OSD to "No Link" on a healthy link. A real link loss blanks
 *      the fields by a different route (the air_lost gate), so the filter here is not hiding one.
 *   2. the field offsets, which are packed at byte granularity and were recovered by RE.
 *
 * The fixtures are three real replies from a vendor session (archive/out/rf-capture/assoc-arm-lo.pcap,
 * which carries 320 of them across 160 distinct payloads): one empty, two populated at different
 * signal levels. That makes the offsets and the dB conversion vendor-anchored. Cases the captured
 * session cannot show - it ranged at zero distance throughout and never moved its throughput - are
 * synthesised, and say so.
 *
 * Order matters in main(): the decoder holds its last good values in file statics that never reset,
 * which is the behaviour under test.
 */
#include "../ml-shared/mlm.h"
#include "../ml-linkd/ml-rx-chan.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Stubs for ml-linkd.c, ml-rx-udp.c and the bb socket. */
int g_verbose;
int g_scan_probe;
atomic_int g_hs_done;
atomic_int g_air_lost;

long now_ms(void)
{
    return 0;
}

int send_frame(const uint8_t *frame, int n, const char *tag)
{
    (void)frame;
    (void)n;
    (void)tag;

    return 0;
}

void mlm_pub(const char *path, uint16_t type, const void *payload, size_t n)
{
    (void)path;
    (void)type;
    (void)payload;
    (void)n;
}

static int g_failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failed++;
    }
}

/* Field offsets into the reply payload, restated here so the test fails if they move in the source
 * without the captured bytes below being re-read.
 */
#define V1V1_OFF_SNR         0x06
#define V1V1_OFF_DIST        0x08
#define V1V1_OFF_THROUGHPUT  0x0c
#define V1V1_LEN             44

/* An EMPTY reply: raw SNR zero at +6. Note it still carries a plausible throughput at +0x0c, so
 * "empty" is a property of the SNR field alone, not of the datagram being blank.
 */
static const uint8_t g_vendor_empty[V1V1_LEN] = {
    0xd5, 0xca, 0x00, 0x00, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xbe, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x49, 0x5a, 0x00, 0xa0, 0x76, 0x57, 0x00, 0x29,
    0x40, 0x01, 0x0c, 0x0a, 0x05, 0x17, 0x17, 0xc5, 0x07, 0x00, 0x00,
};

/* A populated reply: raw 4194 at +6, which is 21 dB. */
static const uint8_t g_vendor_mid[V1V1_LEN] = {
    0x45, 0x46, 0x01, 0x00, 0x01, 0x1c, 0x62, 0x10, 0x00, 0x00, 0x00,
    0x00, 0xbe, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x49, 0x5a, 0x00, 0xa0, 0x76, 0x57, 0x00, 0x23,
    0x21, 0x01, 0x0c, 0x0a, 0x05, 0x17, 0x17, 0x5f, 0x15, 0x00, 0x00,
};

/* A stronger one: raw 8873, which is 24 dB. */
static const uint8_t g_vendor_high[V1V1_LEN] = {
    0x07, 0x27, 0x01, 0x00, 0x01, 0x1d, 0xa9, 0x22, 0x00, 0x00, 0x00,
    0x00, 0xbe, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x48, 0x49, 0x5a, 0x00, 0xa0, 0x76, 0x57, 0x00, 0x28,
    0x28, 0x01, 0x0c, 0x0a, 0x05, 0x17, 0x0e, 0x9f, 0x24, 0x00, 0x00,
};

/** @brief A synthesised reply carrying @p raw, @p dist and @p thr at the RE'd offsets. */
static void synth(uint8_t *out, uint16_t raw, int32_t dist, uint32_t thr)
{
    memset(out, 0, V1V1_LEN);
    out[V1V1_OFF_SNR] = (uint8_t)raw;
    out[V1V1_OFF_SNR + 1] = (uint8_t)(raw >> 8);
    out[V1V1_OFF_DIST + 0] = (uint8_t)((uint32_t)dist);
    out[V1V1_OFF_DIST + 1] = (uint8_t)((uint32_t)dist >> 8);
    out[V1V1_OFF_DIST + 2] = (uint8_t)((uint32_t)dist >> 16);
    out[V1V1_OFF_DIST + 3] = (uint8_t)((uint32_t)dist >> 24);
    out[V1V1_OFF_THROUGHPUT + 0] = (uint8_t)thr;
    out[V1V1_OFF_THROUGHPUT + 1] = (uint8_t)(thr >> 8);
    out[V1V1_OFF_THROUGHPUT + 2] = (uint8_t)(thr >> 16);
    out[V1V1_OFF_THROUGHPUT + 3] = (uint8_t)(thr >> 24);
}

/** @brief Nothing is claimed before a reply has landed. Runs first: the statics never reset. */
static void check_initial_state(void)
{
    printf("  -- before any reply --\n");
    check(rx_chan_snr_db() == MLM_LINKINFO_NONE, "SNR reads NONE, not zero, before any reply");
    check(rx_chan_distance_m() == MLM_LINKINFO_NONE, "distance reads NONE before any reply");
    check(rx_chan_throughput_kbps() == 0, "throughput reads zero, which is its no-link value");

    /* Too short to hold the SNR word: dropped whole, rather than decoded from whatever follows. */
    rx_chan_on_1v1(g_vendor_mid, V1V1_OFF_SNR + 1);
    check(rx_chan_snr_db() == MLM_LINKINFO_NONE, "a reply too short to hold the SNR is dropped");
}

/** @brief The captured replies, decoded field for field. */
static void check_vendor_replies(void)
{
    printf("  -- captured vendor replies --\n");

    rx_chan_on_1v1(g_vendor_mid, V1V1_LEN);
    check(rx_chan_snr_db() == 21, "raw 4194 at +0x06 converts to 21 dB");
    check(rx_chan_throughput_kbps() == 20926, "throughput is the u32 kbps at +0x0c");
    check(rx_chan_distance_m() == 0, "distance is the i32 at +0x08, zero throughout this session");

    rx_chan_on_1v1(g_vendor_high, V1V1_LEN);
    check(rx_chan_snr_db() == 24, "a stronger reply, raw 8873, converts to 24 dB");
}

/** @brief The last-good filter: an empty reply must not disturb a good reading. */
static void check_last_good_filter(void)
{
    uint8_t reply[V1V1_LEN];

    printf("  -- the last-good filter --\n");

    /* Seed known-good values that differ from the empty reply's own bytes, so holding them can be
     * told apart from re-decoding the empty reply.
     */
    synth(reply, 3600, 1234, 9999);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == 20 && rx_chan_distance_m() == 1234
          && rx_chan_throughput_kbps() == 9999, "a populated reply sets all three");

    rx_chan_on_1v1(g_vendor_empty, V1V1_LEN);
    check(rx_chan_snr_db() == 20, "the captured empty reply leaves the SNR at its last good value");
    check(rx_chan_distance_m() == 1234, "and leaves the distance alone");

    /* The one worth stating plainly: the empty reply carries 20926 kbps in its throughput field,
     * and that value is deliberately NOT taken. The gate is the raw SNR, not the field's contents.
     */
    check(rx_chan_throughput_kbps() == 9999,
          "and leaves the throughput alone, though the empty reply carries one");

    /* A reply long enough for the SNR but not the distance updates what it can and holds the rest,
     * rather than reading past its end for the fields it lacks.
     */
    synth(reply, 360, 4321, 5555);
    rx_chan_on_1v1(reply, V1V1_OFF_SNR + 2);
    check(rx_chan_snr_db() == 10, "a reply holding only the SNR still updates it");
    check(rx_chan_distance_m() == 1234 && rx_chan_throughput_kbps() == 9999,
          "and leaves the fields it does not reach at their last good values");
}

/** @brief Conversion and clamping. Synthesised: the captured session never moved these. */
static void check_conversion_and_clamps(void)
{
    uint8_t reply[V1V1_LEN];

    printf("  -- conversion and clamps --\n");

    /* 36 is the divisor in 10*log10(raw/36), so it is the 0 dB reference point. */
    synth(reply, 36, 0, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == 0, "raw 36 is the 0 dB reference");

    synth(reply, 360, 0, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == 10, "a ten-fold raw SNR is 10 dB");

    /* Below the reference the result is negative. A clamp at zero here would misreport a marginal
     * link as a usable one.
     */
    synth(reply, 18, 0, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == -3, "a raw SNR below the reference reports negative dB, not zero");

    /* The sweep's retry floor, quoted in ml-rx-chan.c as the vendor's bucket-2 boundary at about
     * 6.5 dB. Asserting it keeps that comment honest.
     */
    synth(reply, 160, 0, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == 6, "the sweep's raw-160 retry floor is the documented ~6.5 dB");

    /* Ranging reports negative when it has no fix. The vendor clamps that to zero rather than
     * showing a negative distance on the OSD.
     */
    synth(reply, 3600, -5, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_distance_m() == 0, "a negative distance clamps to zero, as the vendor does");

    synth(reply, 3600, 100000, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_distance_m() == 100000, "a large distance is not clamped");

    /* The SNR field is a u16, so its top half must not be read as a sign bit. */
    synth(reply, 65535, 0, 1);
    rx_chan_on_1v1(reply, V1V1_LEN);
    check(rx_chan_snr_db() == 33, "the maximum raw SNR decodes as unsigned, giving 33 dB");
}

int main(void)
{
    check_initial_state();
    check_vendor_replies();
    check_last_good_filter();
    check_conversion_and_clamps();

    printf("%s\n", g_failed == 0 ? "rx-1v1-decode: all checks passed"
                                 : "rx-1v1-decode: FAILED");

    return g_failed == 0 ? 0 : 1;
}
