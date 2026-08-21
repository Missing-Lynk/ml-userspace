/**
 * @file air-msg-seen.c
 * @brief Host test: the report-once latch behind the air's unhandled-message logging.
 *
 * The latch is what keeps a goggle control that re-sends every session from filling the log, so
 * its two guarantees are worth pinning: the first offer of a code reports and later ones do not,
 * and a full record stops reporting rather than growing. Covers the selector names alongside it,
 * since the same log line carries them.
 */
#include "../ml-linkd/ml-air-seen.h"
#include "../ml-linkd/ml-air-cam.h"

#include <stdio.h>
#include <string.h>

static int g_failed;

static void check(int is_ok, const char *what)
{
    printf("%s %s\n", is_ok ? "ok  " : "FAIL", what);
    if (!is_ok) {
        g_failed = 1;
    }
}

int main(void)
{
    struct air_seen seen = { 0 };
    unsigned int i;
    int reported;

    check(air_seen_first(&seen, 0x15) == 1, "a code is reported the first time");
    check(air_seen_first(&seen, 0x15) == 0, "the same code is not reported again");
    check(air_seen_first(&seen, 0x0c) == 1, "a second code is reported on its own first time");
    check(air_seen_first(&seen, 0x15) == 0, "the first code stays reported after the second");

    /* Selector 0 is a real selector (brightness), so the record must hold it like any other and
     * not read as an empty slot. */
    check(air_seen_first(&seen, 0) == 1, "selector 0 is reported once");
    check(air_seen_first(&seen, 0) == 0, "selector 0 is not reported again");

    /* Fill the record from empty, then offer one code past it. */
    memset(&seen, 0, sizeof seen);
    reported = 0;
    for (i = 0; i < AIR_SEEN_MAX; i++) {
        reported += air_seen_first(&seen, 0x1000 + i);
    }
    check(reported == AIR_SEEN_MAX, "every code up to the record size is reported");
    check(air_seen_first(&seen, 0x2000) == 0, "a full record reports no further code");
    check(air_seen_first(&seen, 0x1000) == 0, "a full record still suppresses what it holds");

    check(strcmp(air_cam_selector_name(10), "banding") == 0, "selector 10 names banding");
    check(strcmp(air_cam_selector_name(5), "rotation") == 0, "selector 5 names rotation");
    check(strcmp(air_cam_selector_name(0), "brightness") == 0, "selector 0 names brightness");
    check(strcmp(air_cam_selector_name(11), "unknown") == 0, "a selector past the union is unknown");

    printf("%s\n", g_failed ? "FAILED" : "all passed");

    return g_failed;
}
