/**
 * @file bb-frame-builders.c
 * @brief Host test: the AR8030 bb-socket frames ml-linkd builds, byte for byte.
 *
 * These frames configure the local radio. A malformed one is not a dropped datagram: bb_ioctl
 * stamps a command's table length on the wire regardless of what the caller filled in, and a short
 * or mis-padded frame can crash the chip firmware, which on this hardware means a watchdog reset.
 *
 * bb-cmd.h divides its builders into two groups, and this test exists mainly for the second:
 *
 *   PROVEN   issued by ml-linkd's bring-up and confirmed against hardware.
 *   DECODED  payloads recovered by RE that have NEVER been sent on the open stack. Each one
 *            retunes or repowers a live radio, so nothing validates them today. A byte assertion
 *            is the only check they will get before someone RAM-boots them.
 *
 * The anchor is a real vendor session (archive/out/rf-capture/assoc-arm-lo.pcap), which carries
 * both directions of the vendor daemon's bb traffic. Three distinct HOST->CHIP commands appear in
 * it, and they pin the shared machinery every other builder rides on: the header layout, the
 * big-endian sequence number, the checksum, and bb_build_cmd's zero-padding to the table length.
 * With those pinned by vendor bytes, the per-builder assertions below only have to state each
 * payload, which is what the RE actually recovered.
 *
 * Reply-side decoding is not covered here. It belongs with the reader that consumes it.
 */
#include "../ml-linkd/bb-cmd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failed;

static void check(int cond, const char *what)
{
    printf("%-72s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) {
        g_failed++;
    }
}

/** @brief Compare a built frame to captured vendor bytes; reports the first difference. */
static void check_frame(const uint8_t *got, int got_len, const uint8_t *want, int want_len,
                        const char *what)
{
    char detail[160];

    if (got_len != want_len) {
        snprintf(detail, sizeof detail, "%s: built %d bytes, the capture has %d",
                 what, got_len, want_len);
        check(0, detail);

        return;
    }

    for (int i = 0; i < want_len; i++) {
        if (got[i] != want[i]) {
            snprintf(detail, sizeof detail, "%s: byte %d is 0x%02x, the capture has 0x%02x",
                     what, i, got[i], want[i]);
            check(0, detail);

            return;
        }
    }

    check(1, what);
}

/* SET_POWER, the RX chain at 23 dBm, sequence 0. The vendor's own bring-up frame. */
static const uint8_t g_vendor_set_power[] = {
    0xaa, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5d, 0x08, 0x17, 0xbb,
};

/* GET_STATUS, sequence 0. Its two payload bytes are pad: the table gives this GET an in_len of 2
 * and bb_get supplies none, so the zeros are bb_build_cmd's doing and not the caller's.
 */
static const uint8_t g_vendor_get_status[] = {
    0xaa, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0xbb,
};

/* An unnamed one-byte BB_SET selector, sequence 0. No builder wraps 0x1a; it is here because it is
 * the only captured command with a payload shorter than its header, which pins the trailer position
 * against an off-by-one that the two-byte frames above cannot distinguish.
 */
static const uint8_t g_vendor_set_1a[] = {
    0xaa, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x1a, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x06, 0xbb,
};

/** @brief The builders whose exact bytes a vendor capture can settle. */
static void check_against_capture(void)
{
    uint8_t frame[64];
    const uint8_t sel_1a_payload[1] = { 0x06 };
    int len;

    len = bb_set_power(frame, RF_RX, 23, 0);
    check_frame(frame, len, g_vendor_set_power, (int) sizeof g_vendor_set_power,
                "bb_set_power(RF_RX, 23) matches the captured vendor frame");

    len = bb_get(frame, GET_STATUS, 0);
    check_frame(frame, len, g_vendor_get_status, (int) sizeof g_vendor_get_status,
                "bb_get(GET_STATUS) matches the captured vendor frame");

    len = bb_build_cmd(frame, BB_SET, 0x1a, sel_1a_payload, 1, 0);
    check_frame(frame, len, g_vendor_set_1a, (int) sizeof g_vendor_set_1a,
                "bb_build_cmd(BB_SET, 0x1a) matches the captured vendor frame");
}

/** @brief The wire envelope, as the captured frames above fix it. */
static void check_frame_layout(void)
{
    uint8_t frame[64];
    uint8_t csum = 0;
    int len = bb_set_power(frame, RF_TX, 23, 0x11223344);

    check(len == 2 + BB_FRAME_EXTRA, "a 2-byte payload builds a 21-byte frame");
    check(frame[BB_OFF_MAGIC] == 0xaa, "the frame opens with 0xaa");
    check(frame[len - 1] == 0xbb, "the frame closes with 0xbb after the payload");
    check(frame[BB_OFF_PLEN] == 2 && frame[BB_OFF_PLEN + 1] == 0,
          "the payload length is a little-endian u16 at offset 1");
    check(frame[BB_OFF_CLASS] == BB_SET, "the class byte is at offset 5");
    check(frame[BB_OFF_OPCODE] == 0 && frame[BB_OFF_SLOT] == 0,
          "GET/SET commands carry opcode 0 and slot 0");
    check(frame[BB_OFF_SELECTOR] == SET_POWER, "the selector is at offset 8");

    /* Big-endian, unlike every length and payload field in this protocol. The captured GET_TIME
     * and Get1V1Info replies carry their request ids the same way round.
     */
    check(frame[9] == 0x11 && frame[10] == 0x22 && frame[11] == 0x33 && frame[12] == 0x44,
          "the sequence number is big-endian at offsets 9..12");
    check(frame[13] == 0 && frame[14] == 0 && frame[15] == 0 && frame[16] == 0,
          "the four reserved bytes at 13..16 are zero");

    for (int i = 0; i < 17; i++) {
        csum ^= frame[i];
    }
    csum = (uint8_t) ~csum;
    check(frame[17] == csum, "the checksum at 17 is ~XOR of bytes 0..16");

    /* The checksum covers the header ALONE. Both captured BB_SET 0x1a frames carry checksum 0x4c
     * with different payload bytes, so extending it over the payload would look like a tightening
     * and would in fact make every frame we send unparseable.
     */
    {
        uint8_t other[64];

        bb_set_power(other, RF_RX, 5, 0x11223344);
        check(other[17] == frame[17], "the checksum does not cover the payload");
    }
}

/** @brief bb_build_cmd pads to the table length and refuses what it cannot pad. */
static void check_padding_and_rejects(void)
{
    uint8_t frame[64];
    const uint8_t two[2] = { 0xa5, 0x5a };
    int len;

    /* Pair-mode is the documented partial fill: 2 meaningful bytes of a 14-byte wire payload. */
    len = bb_pair_mode(frame, 1, 0, 0);
    check(len == 14 + BB_FRAME_EXTRA, "bb_pair_mode pads its 2 bytes to the 14-byte wire length");
    check(frame[BB_OFF_PLEN] == 14, "the padded length is what the frame declares");
    for (int i = BB_OFF_PAYLOAD + 2; i < len - 1; i++) {
        if (frame[i] != 0) {
            check(0, "bb_pair_mode zero-fills the pad");

            return;
        }
    }
    check(1, "bb_pair_mode zero-fills the pad");

    check(bb_build_cmd(frame, BB_SET, 0xff, two, 2, 0) == -1,
          "an unmapped selector is refused rather than sent unpadded");
    check(bb_build_cmd(frame, BB_SET, SET_POWER_AUTO, two, 2, 0) == -1,
          "a payload longer than the command's wire length is refused");
    check(bb_build_cmd(frame, BB_SET, 0x0a, two, 2, 0) == -1,
          "a wire length past BB_CMD_PAYLOAD_MAX is refused, not written past the buffer");
    check(bb_cmd_in_len(BB_SET, SET_PAIR_LOCK) <= BB_CMD_PAYLOAD_MAX,
          "BB_CMD_PAYLOAD_MAX covers the longest command a builder actually issues");
}

/* Every named builder, with the wire length its table row demands. A builder that stops agreeing
 * with the table is one bb_ioctl would zero-pad or truncate on the way to the chip.
 */
struct builder_case {
    const char *name;
    enum bb_class cls;
    uint8_t selector;
    int len;
};

/** @brief Each builder's frame declares its table in_len. */
static void check_builder_lengths(void)
{
    uint8_t frame[64];
    const uint8_t mac[4] = { 0xde, 0xad, 0xbe, 0xef };
    struct builder_case cases[] = {
        { "bb_get(GET_1V1INFO)",  BB_GET, GET_1V1INFO,    bb_get(frame, GET_1V1INFO, 1) },
        { "bb_get(GET_MCS)",      BB_GET, GET_MCS,        bb_get(frame, GET_MCS, 1) },
        { "bb_get(GET_PAIR)",     BB_GET, GET_PAIR,       bb_get(frame, GET_PAIR, 1) },
        { "bb_get(GET_SCAN)",     BB_GET, GET_SCAN_RESULT, bb_get(frame, GET_SCAN_RESULT, 1) },
        { "bb_get(GET_TIME)",     BB_GET, GET_TIME,       bb_get(frame, GET_TIME, 1) },
        { "bb_get_power",         BB_GET, GET_POWER,      bb_get_power(frame, RF_TX, 1) },
        { "bb_set_power",         BB_SET, SET_POWER,      bb_set_power(frame, RF_TX, 23, 1) },
        { "bb_set_power_auto",    BB_SET, SET_POWER_AUTO, bb_set_power_auto(frame, 1, 1) },
        { "bb_select_channel",    BB_SET, SET_CHNIDX,     bb_select_channel(frame, 3, 1) },
        { "bb_set_bandwidth",     BB_SET, SET_BANDWIDTH,  bb_set_bandwidth(frame, 1, 1) },
        { "bb_pair_mode",         BB_SET, SET_PAIR_MODE,  bb_pair_mode(frame, 1, 0, 1) },
        { "bb_set_ap_mac",        BB_SET, SET_AP_MAC,     bb_set_ap_mac(frame, mac, 1) },
        { "bb_pair_lock",         BB_SET, SET_PAIR_LOCK,  bb_pair_lock(frame, mac, 1) },
        { "bb_set_chnmode",       BB_SET, SET_CHNMODE,    bb_set_chnmode(frame, 0, 1) },
        { "bb_set_mcs_mode",      BB_SET, SET_MCS_MODE,   bb_set_mcs_mode(frame, MCS_MODE_AUTO, 1) },
        { "bb_set_mcs_value",     BB_SET, SET_MCS,        bb_set_mcs_value(frame, 5, 1) },
    };
    char what[128];

    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int want = bb_cmd_in_len(cases[i].cls, cases[i].selector);

        snprintf(what, sizeof what, "%s builds its table wire length (%d)", cases[i].name, want);
        check(want >= 0 && cases[i].len == want + BB_FRAME_EXTRA, what);
    }
}

/* The payloads the RE recovered, for the builders that have never been sent. Each assertion is the
 * transcription being restated independently of the builder that writes it.
 */
static void check_decoded_payloads(void)
{
    uint8_t frame[64];
    const uint8_t *body = frame + BB_OFF_PAYLOAD;
    const uint8_t mac[4] = { 0xde, 0xad, 0xbe, 0xef };

    bb_set_power(frame, RF_RX, 23, 0);
    check(body[0] == RF_RX && body[1] == 23, "bb_set_power writes {dir, dBm}");
    check(RF_TX == 0x00 && RF_RX == 0x08, "the direction bytes are TX 0x00 and RX 0x08");

    bb_select_channel(frame, 3, 0);
    check(body[0] == 0x02 && body[1] == 3,
          "bb_select_channel writes a leading 0x02 then the table index verbatim");

    bb_set_bandwidth(frame, 7, 0);
    check(body[0] == 0x00 && body[1] == 0x01 && body[2] == 7,
          "bb_set_bandwidth writes {0x00, 0x01, bandwidth}");

    bb_pair_mode(frame, 1, 0, 0);
    check(body[0] == 1 && body[1] == 0x01, "pair mode on slot 0 sets bit 0 of the slot mask");
    bb_pair_mode(frame, 0, 3, 0);
    check(body[0] == 0 && body[1] == 0x08, "pair mode off slot 3 sets bit 3 and clears the enable");
    bb_pair_mode(frame, 1, 8, 0);
    check(body[1] == 0x01, "the slot index is masked to 3 bits, so slot 8 folds onto slot 0");

    bb_set_ap_mac(frame, mac, 0);
    check(memcmp(body, mac, 4) == 0, "bb_set_ap_mac writes the MAC in wire order, not byte-swapped");

    bb_pair_lock(frame, mac, 0);
    check(body[0] == 0x00 && body[1] == 0x01, "bb_pair_lock prefixes the u16 0x0100");
    check(memcmp(body + 2, mac, 4) == 0, "bb_pair_lock carries the MAC in wire order");

    /* The vendor writes mode << 8, so the mode lands in the HIGH byte of a little-endian u16 and
     * byte 0 stays zero. Writing it at byte 0 would read as manual-vs-auto inverted.
     */
    bb_set_mcs_mode(frame, MCS_MODE_AUTO, 0);
    check(body[0] == 0 && body[1] == MCS_MODE_AUTO, "bb_set_mcs_mode puts the mode in the high byte");
    check(MCS_MODE_MANUAL == 0 && MCS_MODE_AUTO == 1, "manual is 0 and auto is 1");

    /* The write bias and the GET_MCS read bias are the same constant seen from two sides; if they
     * ever disagree, a commanded MCS and the MCS reported back differ by a silent 2.
     */
    bb_set_mcs_value(frame, 5, 0);
    check(body[0] == 0 && body[1] == 5 + MCS_INDEX_BIAS,
          "bb_set_mcs_value offsets the index by the same bias GET_MCS decodes with");
}

int main(void)
{
    check_against_capture();
    check_frame_layout();
    check_padding_and_rejects();
    check_builder_lengths();
    check_decoded_payloads();

    printf("%s\n", g_failed == 0 ? "bb-frame-builders: all checks passed"
                                 : "bb-frame-builders: FAILED");

    return g_failed == 0 ? 0 : 1;
}
