/**
 * @file mp-params-reply.c
 * @brief Host test: the MEDIA_PARAMS reply the air sends, byte for byte.
 *
 * This frame is the one place in the protocol where a receiver we cannot debug decides whether the
 * session proceeds. A vendor goggle tests the declared body length against 0x48 before looking at
 * anything else and silently drops the datagram otherwise, it refuses to bring its pipeline up
 * while the source-ready word is zero, and it creates decoder channel 0 alone while the pipeline
 * flag is zero. All three failures happen inside software we do not run, on a unit we deliberately
 * never modify, so the wire bytes are asserted here instead.
 *
 * The primary assertion is a whole-frame comparison against a real vendor reply captured off a
 * working session (archive/out/rf-capture/assoc-arm-sdio0.pcap, the single type-2 datagram in it).
 * The per-field checks below it are not redundant: they carry the RE citation that says WHY each
 * field has the value it has, so a future edit has to argue with the reason rather than just
 * re-baseline an opaque byte array. Every offset and constant is quoted from the vendor consumer,
 * cited in mp-cmd.h against archive/re/ghidra/out/ar_lowdelay-full.txt. If one of these assertions
 * is edited, the RE citation for that field has to be re-read first: the assertion is the only
 * thing standing in for a test against real vendor software.
 */
#include "../ml-linkd/mp-cmd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The captured vendor reply, 1920x1080p60. Its timestamp and geometry are fed back into our
 * builder below so the two frames are comparable.
 */
static const uint8_t g_vendor[MP_PARAMS_TOTAL] = {
    0x02, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x13, 0x44, 0x8d, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x48, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,  0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0xe5, 0xff, 0x6f, 0x42,
    0x80, 0x07, 0x00, 0x00,  0x38, 0x04, 0x00, 0x00,  0x3c, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x80, 0x3f,  0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
};

#define VENDOR_STAMP    0x008d4413
#define VENDOR_WIDTH    1920
#define VENDOR_HEIGHT   1080
#define VENDOR_FPS      60

static int g_failed;

static void check(int cond, const char *what)
{
    printf("%-72s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) {
        g_failed++;
    }
}

static uint32_t u32_at(const uint8_t *frame, int off)
{
    uint32_t v;

    memcpy(&v, frame + off, 4);

    return v;
}

static float f32_at(const uint8_t *frame, int off)
{
    float v;

    memcpy(&v, frame + off, 4);

    return v;
}

/** @brief Every word in [from, to) is zero. */
static int words_zero(const uint8_t *frame, int from, int to)
{
    for (int off = from; off < to; off += 4) {
        if (u32_at(frame, off) != 0) {
            return 0;
        }
    }

    return 1;
}

/* The one field our builder is expected to differ on: the vendor's high-precision rate came from
 * an HDMI source running 59.99988, ours is a camera driven at an exact whole rate. The receiver
 * substitutes its configured rate when it reads 0.0 and otherwise takes what it is given, so the
 * difference is tolerated by the consumer at ar_lowdelay-full.txt:18882.
 */
static int byte_is_exempt(int off)
{
    return off >= MP_PR_OFF_FPS_F && off < MP_PR_OFF_FPS_F + 4;
}

/** @brief Compare our frame to the captured vendor one; reports the first differing offset. */
static void check_matches_vendor(const uint8_t *frame)
{
    char what[128];

    for (int off = 0; off < MP_PARAMS_TOTAL; off++) {
        if (byte_is_exempt(off) || frame[off] == g_vendor[off]) {
            continue;
        }
        snprintf(what, sizeof what, "byte %d is 0x%02x, the vendor capture has 0x%02x",
                 off, frame[off], g_vendor[off]);
        check(0, what);

        return;
    }

    check(1, "every byte outside the float rate matches the captured vendor reply");
}

int main(void)
{
    uint8_t frame[MP_PARAMS_TOTAL];
    int len = mp_params_reply(frame, VENDOR_WIDTH, VENDOR_HEIGHT, VENDOR_FPS, VENDOR_STAMP);

    check(len == MP_PARAMS_TOTAL,
          "the reply is 104 bytes: a 20-byte header, a 72-byte body and a 12-byte tail");
    check(MP_PARAMS_BODY_LEN == 0x48, "the body length constant is the 0x48 the receiver demands");
    check_matches_vendor(frame);

    /* Header. A wrong type or a wrong length field is the failure that produces
     * "receive params size[%d] error , really size[72]!!" on the vendor side and nothing else.
     */
    check(u32_at(frame, MP_OFF_TYPE) == MP_REPLY, "msg_type is 2 (MEDIA_PARAMS)");
    check(u32_at(frame, MP_OFF_LEN) == MP_PARAMS_BODY_LEN, "the declared body length is 0x48");
    check(u32_at(frame, MP_OFF_STAMP) == VENDOR_STAMP, "the timestamp round-trips at offset 8");

    /* The pair that decides how many decoder channels the receiver creates. With the flag at 20
     * zero, AR_LDRT_RX_VDEC_Enable leaves its channel count at 1, so channel 1 is never created,
     * its bitstream-buffer size stays 0, and every SendStream on it is rejected as
     * "invalid frame, bs_size = 0".
     */
    check(u32_at(frame, MP_PR_OFF_PIPE_FLAG) == 1,
          "the pipeline flag at 20 is 1, so the receiver takes the pattern's channel count");
    check(u32_at(frame, MP_PR_OFF_PIPE_VAL) == 1, "the pipeline value at 24 is 1");

    /* The gate that turns a correctly sized body of zeros into a pipeline that never starts:
     * AR_FSM_RX_RealTimeInit refuses with "Tx Sns Is Not Ready..." while this word is 0.
     */
    check(u32_at(frame, MP_PR_OFF_SRC_READY) != 0, "the source-ready word at 28 is non-zero");
    check(u32_at(frame, MP_PR_OFF_SRC_MODE) == 0,
          "the word at 32 is zero, as it is on the wire; only the format-change path sets it");

    /* Geometry and rate, the fields the receiver acts on. */
    check(u32_at(frame, MP_PR_OFF_WIDTH) == VENDOR_WIDTH, "width at 48");
    check(u32_at(frame, MP_PR_OFF_HEIGHT) == VENDOR_HEIGHT, "height at 52");
    check(u32_at(frame, MP_PR_OFF_FPS) == VENDOR_FPS,
          "whole frame rate at 56, which drives SetCurInputFps");
    check(f32_at(frame, MP_PR_OFF_FPS_F) == (float) VENDOR_FPS,
          "high-precision frame rate at 44 is the same rate as a float");
    check(f32_at(frame, MP_PR_OFF_SCALE_F) == 1.0f, "the scale float at 88 is 1.0");

    /* 1280x720 is the other geometry the vendor decoder names explicitly (0x500 x 0x2d0), so it is
     * worth one pass to show the fields are written from the arguments rather than baked.
     */
    len = mp_params_reply(frame, 1280, 720, 30, 0);
    check(len == MP_PARAMS_TOTAL, "a second geometry produces the same 104-byte frame");
    check(u32_at(frame, MP_PR_OFF_WIDTH) == 0x500 && u32_at(frame, MP_PR_OFF_HEIGHT) == 0x2d0,
          "720p geometry lands as 0x500 x 0x2d0");
    check(u32_at(frame, MP_PR_OFF_FPS) == 30, "the frame rate follows the argument");
    check(u32_at(frame, MP_OFF_LEN) == MP_PARAMS_BODY_LEN,
          "the declared length is fixed at 0x48 regardless of content");

    /* The fields we deliberately leave zero, asserted so that a future change to any of them is a
     * deliberate edit to this list rather than an accident. 36, 40 and 60..87 are unidentified; 40
     * reaches the decoder create, so it is the first to look at if a vendor goggle accepts the
     * reply and then fails to bring its pipeline up. 92..103 is the tail past the body.
     */
    check(words_zero(frame, 36, 44), "the unidentified words at 36/40 are zero");
    check(words_zero(frame, 60, MP_PR_OFF_SCALE_F), "the unidentified words 60..87 are zero");
    check(words_zero(frame, MP_PR_OFF_SCALE_F + 4, MP_PARAMS_TOTAL),
          "the 12-byte tail at 92..103 is zero");

    printf("%s\n", g_failed == 0 ? "mp-params-reply: all checks passed" : "mp-params-reply: FAILED");

    return g_failed == 0 ? 0 : 1;
}
