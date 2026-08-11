/**
 * @file mp-params-reply.c
 * @brief Host test: the MEDIA_PARAMS reply the air sends, byte for byte.
 *
 * This frame is the one place in the protocol where a receiver we cannot debug decides whether the
 * session proceeds. A vendor goggle tests the declared body length against 0x48 before looking at
 * anything else and silently drops the datagram otherwise, and it then refuses to bring its
 * pipeline up while the source-ready word is zero. Both failures happen inside software we do not
 * run, on a unit we deliberately never modify, so the wire bytes are asserted here instead.
 *
 * Every offset and constant below is quoted from the vendor consumer, cited in mp-cmd.h against
 * archive/re/ghidra/out/ar_lowdelay-full.txt. If one of these assertions is edited, the RE citation
 * for that field has to be re-read first: the assertion is the only thing standing in for a test
 * against real vendor software.
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

static uint32_t u32_at(const uint8_t *frame, int off)
{
    uint32_t v;

    memcpy(&v, frame + off, 4);

    return v;
}

int main(void)
{
    uint8_t frame[MP_PARAMS_TOTAL];
    int len = mp_params_reply(frame, 1920, 1080, 60, 0x11223344);
    float fps_f;

    check(len == 92, "the reply is 92 bytes: the 20-byte header plus a 72-byte body");
    check(MP_PARAMS_BODY_LEN == 0x48, "the body length constant is the 0x48 the receiver demands");

    /* Header. A wrong type or a wrong length field is the failure that produces
     * "receive params size[%d] error , really size[72]!!" on the vendor side and nothing else. */
    check(u32_at(frame, MP_OFF_TYPE) == MP_REPLY, "msg_type is 2 (MEDIA_PARAMS)");
    check(u32_at(frame, MP_OFF_LEN) == MP_PARAMS_BODY_LEN, "the declared body length is 0x48");
    check(u32_at(frame, MP_OFF_STAMP) == 0x11223344, "the timestamp round-trips at offset 8");

    /* The gate that turns a correctly sized body of zeros into a pipeline that never starts:
     * AR_FSM_RX_RealTimeInit refuses with "Tx Sns Is Not Ready..." while this word is 0. */
    check(u32_at(frame, MP_PR_OFF_SRC_READY) != 0, "the source-ready word at 28 is non-zero");
    check(u32_at(frame, MP_PR_OFF_SRC_MODE) == 1, "the word at 32 is 1, as the vendor sets it");

    /* Geometry and rate, the fields the receiver acts on. */
    check(u32_at(frame, MP_PR_OFF_WIDTH) == 1920, "width at 48");
    check(u32_at(frame, MP_PR_OFF_HEIGHT) == 1080, "height at 52");
    check(u32_at(frame, MP_PR_OFF_FPS) == 60, "whole frame rate at 56, which drives SetCurInputFps");

    memcpy(&fps_f, frame + MP_PR_OFF_FPS_F, 4);
    check(fps_f == 60.0f, "high-precision frame rate at 44 is the same rate as a float");

    /* 1280x720 is the other geometry the vendor decoder names explicitly (0x500 x 0x2d0), so it is
     * worth one pass to show the fields are written from the arguments rather than baked. */
    len = mp_params_reply(frame, 1280, 720, 30, 0);
    check(len == 92, "a second geometry produces the same 92-byte frame");
    check(u32_at(frame, MP_PR_OFF_WIDTH) == 0x500 && u32_at(frame, MP_PR_OFF_HEIGHT) == 0x2d0,
          "720p geometry lands as 0x500 x 0x2d0");
    check(u32_at(frame, MP_PR_OFF_FPS) == 30, "the frame rate follows the argument");
    check(u32_at(frame, MP_OFF_LEN) == MP_PARAMS_BODY_LEN,
          "the declared length is fixed at 0x48 regardless of content");

    /* The fields we deliberately leave zero, asserted so that a future change to any of them is a
     * deliberate edit to this list rather than an accident. 20/24 are the pipeline-create flag pair
     * (zero is the receiver's "off" branch); 36, 40 and 60..91 are unidentified. */
    check(u32_at(frame, MP_OFF_BODY) == 0 && u32_at(frame, MP_OFF_BODY + 4) == 0,
          "the pipeline-create flag pair at 20/24 is zero");
    check(u32_at(frame, 36) == 0 && u32_at(frame, 40) == 0, "the unidentified words at 36/40 are zero");

    for (int off = 60; off < MP_PARAMS_TOTAL; off += 4) {
        if (u32_at(frame, off) != 0) {
            check(0, "the unidentified tail 60..91 is zero");
            break;
        }
    }
    check(1, "the unidentified tail 60..91 is zero");

    printf("%s\n", g_failed == 0 ? "mp-params-reply: all checks passed" : "mp-params-reply: FAILED");

    return g_failed == 0 ? 0 : 1;
}
