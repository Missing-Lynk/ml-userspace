/**
 * @file mp-frame-builders.c
 * @brief Host test: the :10000 message-plane frames, byte for byte against a vendor session.
 *
 * mp-params-reply.c covers the MEDIA_PARAMS reply, the one frame a vendor goggle length-checks
 * before it will start a session. This covers the rest of the message plane: the goggle's polls,
 * the standby ack, and the three status frames the air transmits.
 *
 * Every fixture is a real datagram from archive/out/rf-capture/assoc-arm-sdio0.pcap, a vendor air
 * talking to a vendor goggle. For the air->goggle frames that makes the capture the specification:
 * it is what a vendor goggle was observed to accept, and our air has to produce the same shape to
 * be interchangeable with it.
 *
 * One divergence is recorded rather than asserted away, see check_standby_report below: our 0x12
 * SetStandyMode is 8 bytes shorter than the vendor's. The assertions state what we send today so a
 * change is deliberate; closing the gap is a decision about vendor parity, not a test fix.
 */
#include "../ml-linkd/mp-cmd.h"

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

/* Goggle->air polls and the standby ack: the common header alone, no body. */
static const uint8_t g_vendor_params_request[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0xed, 0x4c, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t g_vendor_idr_request[] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x40, 0x61, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t g_vendor_stb_ack[] = {
    0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa5, 0xe7, 0xcc, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Air->goggle 0x11 periodic status, from a bench unit with no FC telemetry: the 6-byte body is all
 * zero, so this fixture pins the envelope and the trailer rather than the field values.
 */
static const uint8_t g_vendor_status_periodic[] = {
    0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0xd5, 0x74, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
};

/* Air->goggle 0x09 version/info status. hw "V1.0" at body 0, fw "1.0.44.rel" at body 32, the
 * voltage word at body 96 reading zero on a bench unit, and 39 C at body 98.
 */
static const uint8_t g_vendor_status_version[] = {
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x07, 0x81, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x56, 0x31, 0x2e, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x31, 0x2e, 0x30, 0x2e, 0x34, 0x34, 0x2e, 0x72,
    0x65, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x27, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

/** @brief The bodyless goggle->air frames. */
static void check_header_only(void)
{
    uint8_t frame[MP_HDR_LEN];

    check_frame(frame, mp_params_request(frame, 0x034ced27),
                g_vendor_params_request, (int) sizeof g_vendor_params_request,
                "mp_params_request matches the captured vendor frame");
    check_frame(frame, mp_idr_request(frame, 0x04614080),
                g_vendor_idr_request, (int) sizeof g_vendor_idr_request,
                "mp_idr_request matches the captured vendor frame");
    check_frame(frame, mp_stb_ack(frame, 0x04cce7a5),
                g_vendor_stb_ack, (int) sizeof g_vendor_stb_ack,
                "mp_stb_ack matches the captured vendor frame");

    /* All three are the same 24 bytes with one byte changed, so the type byte is the only thing
     * distinguishing an IDR request from a standby ack on the wire.
     */
    check(mp_params_request(frame, 0) == MP_HDR_LEN, "a bodyless frame is exactly the 24-byte header");
    check(frame[MP_OFF_LEN] == 0, "and declares a zero body length");
}

/** @brief The air's periodic and version status frames. */
static void check_status_frames(void)
{
    uint8_t frame[MP_STATUS_A_TOTAL];
    int len;

    len = mp_status_periodic(frame, 0, 0, 0, 0x0074d583);
    check_frame(frame, len, g_vendor_status_periodic, (int) sizeof g_vendor_status_periodic,
                "mp_status_periodic matches the captured vendor frame");

    /* The field placement the zeroed capture cannot show. The goggle reads these at the offsets in
     * ml-hud/src/channel/osd_proto.h, and the two u16s are unaligned in the body.
     */
    len = mp_status_periodic(frame, 1, 0x1234, 0x4d2, 0);
    check(len == MP_STATUS_B_TOTAL && frame[MP_OFF_LEN] == 6,
          "the periodic status is 38 bytes with a 6-byte body");
    check(frame[MP_OFF_BODY] == 1, "the arm flag is body byte 0");
    check(frame[MP_OFF_BODY + 2] == 0x34 && frame[MP_OFF_BODY + 3] == 0x12,
          "mAh drawn is a little-endian u16 at body 2");
    check(frame[MP_OFF_BODY + 4] == 0xd2 && frame[MP_OFF_BODY + 5] == 0x04,
          "the voltage is a little-endian u16 at body 4");

    /* The strings go through sized buffers rather than literals so that strnlen's 16-byte bound is
     * provably within the object; against a short literal gcc reports the bound as an overread.
     */
    {
        char hw[16] = "V1.0";
        char fw[16] = "1.0.44.rel";

        len = mp_status_version(frame, hw, fw, 0, 39, 0x00810727);
        check_frame(frame, len, g_vendor_status_version, (int) sizeof g_vendor_status_version,
                    "mp_status_version matches the captured vendor frame");
    }

    /* The version strings are fixed-width fields in a pre-zeroed body, so an over-long string must
     * be truncated rather than run into the next field.
     */
    {
        char hw[24] = "0123456789abcdefOVERRUN";
        char fw[16] = "fw";

        len = mp_status_version(frame, hw, fw, 0, 0, 0);
    }
    check(memcmp(frame + MP_OFF_BODY + MP_STATUS_A_OFF_HW, "0123456789abcdef", 16) == 0,
          "a hardware string longer than 16 bytes is truncated to the field");
    check(frame[MP_OFF_BODY + 16] == 0, "and does not run past it");
}

/** @brief The 0x12 work-mode report, and the one place we differ from the vendor. */
static void check_standby_report(void)
{
    uint8_t frame[MP_STANDBY_TOTAL];
    int len = mp_standby_report(frame, MP_STANDBY_ON, 0x008f6f65);

    check(len == MP_STANDBY_TOTAL, "the standby report is 36 bytes");
    check(frame[MP_OFF_TYPE] == MP_STANDBY, "msg_type is 0x12");
    check(frame[MP_OFF_LEN] == 4, "it declares a 4-byte body");
    check(frame[MP_OFF_BODY] == MP_STANDBY_ON && frame[MP_OFF_BODY + 1] == 0,
          "the work mode is a little-endian u32 at body 0");

    len = mp_standby_report(frame, MP_STANDBY_NORMAL, 0);
    check(frame[MP_OFF_BODY] == 0, "normal work mode is zero");
    check(MP_STANDBY_NORMAL == 0 && MP_STANDBY_ON == 1, "the two modes we send are 0 and 1");
    for (int i = MP_OFF_BODY + 4; i < len; i++) {
        if (frame[i] != 0) {
            check(0, "the 12-byte trailer is zero");

            return;
        }
    }
    check(1, "the 12-byte trailer is zero");

    /* KNOWN DIVERGENCE, stated here because this file is where it would be noticed. The vendor air
     * in the same capture sends this frame as 44 bytes with a 12-byte body:
     *
     *   16  0c 00 00 00   declared body length, 12
     *   20  01 00 00 00   work mode, the field we do send
     *   24  0f 00 06 00   undecoded
     *   28  05 00 00 00   undecoded
     *
     * Ours is 36 bytes with a 4-byte body. Our own goggle dispatches on msg_type and ignores the
     * rest, so the pairing we ship works. Whether a VENDOR goggle length-checks 0x12 the way it
     * length-checks MEDIA_PARAMS is not known, and that is the risk: the MEDIA_PARAMS reply failed
     * exactly this way, silently, until the declared length was made to match. Deciding this needs
     * the RE for the 0x12 consumer, not a test edit.
     */
    check(MP_STANDBY_TOTAL == 36,
          "we send 36 bytes where the vendor sends 44: see the note above before changing this");
}

/** @brief SetTranParm and the canvas frame, whose shapes come from RE rather than this capture. */
static void check_tran_parm_and_canvas(void)
{
    uint8_t frame[256];
    const uint8_t canvas[5] = { 1, 2, 3, 4, 5 };
    int len;

    len = mp_set_tran_parm(frame, 23, 1, 0x11223344);
    check(len == MP_STP_LEN, "SetTranParm is 34 bytes");
    check(frame[MP_OFF_TYPE] == MP_SETTRANPARM && frame[MP_OFF_LEN] == MP_STP_BODY_LEN,
          "it declares its 10-byte body");
    check(frame[MP_STP_OFF_DBM] == 23, "the power dBm is body byte 0");
    check(frame[MP_STP_OFF_STANDBY] == 1, "the standby flag is body byte 8");

    /* body[1] is a constant in every captured frame. It is asserted because the rest of this body
     * is the HW-confirmed vendor tuple: fabricating a byte here has rebooted a goggle.
     */
    check(frame[MP_OFF_BODY + 1] == 0x04, "body byte 1 is the constant 0x04 the captures all carry");

    len = mp_msp_canvas(frame, canvas, sizeof canvas, 0);
    check(len == MP_OFF_BODY + (int) sizeof canvas + MP_TRAILER_LEN,
          "a canvas frame is header + canvas + the 12-byte trailer");
    check(frame[MP_OFF_LEN] == sizeof canvas, "and declares the canvas length");
    check(memcmp(frame + MP_OFF_BODY, canvas, sizeof canvas) == 0, "the canvas rides verbatim");
    check(frame[MP_OFF_BODY + sizeof canvas] == 0, "with the trailer zeroed after it");
}

int main(void)
{
    check_header_only();
    check_status_frames();
    check_standby_report();
    check_tran_parm_and_canvas();

    printf("%s\n", g_failed == 0 ? "mp-frame-builders: all checks passed"
                                 : "mp-frame-builders: FAILED");

    return g_failed == 0 ? 0 : 1;
}
