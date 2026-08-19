/**
 * @file air-cam-banding.c
 * @brief Host test: the air's SetCameraInfo banding parse, against the goggle's own builder.
 *
 * The frames come from mp_set_camera_info, the exact builder ml-linkd's goggle role transmits
 * with, so the two ends of the wire are tested against each other rather than against a copy of
 * the same offsets. Covers: the accepted values, the vendor's force-to-off rule for anything
 * else, foreign selectors, foreign message types, and the short-datagram guard.
 */
#include "../ml-linkd/mp-cmd.h"
#include "../ml-linkd/ml-air-cam.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ml-air-cam.c links against ml-linkd's globals; the test provides them. */
int g_verbose;

static int g_failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failed = 1;
    }
}

static int build_banding(uint8_t *frame, uint16_t hz)
{
    struct mp_camera state = MP_CAMERA_DEFAULTS;

    state.banding = hz;

    return mp_set_camera_info(frame, MLM_CAM_BANDING, &state, 0x12345678);
}

int main(void)
{
    uint8_t frame[MP_CAM_LEN];
    unsigned int hz;
    int len;

    len = build_banding(frame, 50);
    check(len == MP_CAM_LEN, "builder emits the captured frame length");
    check(air_cam_parse_banding(frame, len, &hz) == 1 && hz == 50, "banding 50 parses as 50");

    len = build_banding(frame, 60);
    check(air_cam_parse_banding(frame, len, &hz) == 1 && hz == 60, "banding 60 parses as 60");

    len = build_banding(frame, 0);
    check(air_cam_parse_banding(frame, len, &hz) == 1 && hz == 0, "banding 0 parses as off");

    /* The vendor's handler forces any other value to off rather than rejecting the datagram. */
    len = build_banding(frame, 47);
    check(air_cam_parse_banding(frame, len, &hz) == 1 && hz == 0, "banding 47 is forced to off");

    /* Another selector rides the same union; its banding field is cached sender state and must
     * not be applied. */
    {
        struct mp_camera state = MP_CAMERA_DEFAULTS;

        state.banding = 50;
        len = mp_set_camera_info(frame, MLM_CAM_SATURATION, &state, 0);
        hz = 77;
        check(air_cam_parse_banding(frame, len, &hz) == 0 && hz == 77,
              "a saturation set does not apply its cached banding");
    }

    /* A short datagram must not be read past. */
    len = build_banding(frame, 50);
    check(air_cam_parse_banding(frame, len - 1, &hz) == 0, "a truncated frame is refused");
    check(air_cam_parse_banding(frame, 3, &hz) == 0, "a runt is refused");

    /* A different message type with plausible length. */
    frame[0] = MP_SETLDCFG;
    check(air_cam_parse_banding(frame, MP_CAM_LEN, &hz) == 0, "a foreign type is refused");

    if (g_failed) {
        return 1;
    }

    printf("air-cam-banding OK\n");

    return 0;
}
