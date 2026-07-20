/** @file linkcmd.c @brief See linkcmd.h. */
#include "linkcmd.h"

#include <string.h>

#include "../../../ml-shared/mlm.h"

/* The air's three TX power levels, mapping each menu label to its mW value. This is the single
 * source of truth for the label <-> mW pairing; the menu's stepper options must match these labels.
 * ml-linkd owns the mW -> dBm step and rejects anything it does not recognise. */
static const struct { const char *label; int mw; } POWER_LEVELS[] = {
    { "25 mW",  25 },
    { "100 mW", 100 },
    { "200 mW", 200 },
};
#define POWER_DEFAULT_MW 100   /* used when a label does not match (e.g. a stale settings value) */

/* One connectionless DGRAM to link.sock per call (mlm_rfcmd_send); ml-linkd translates the intent
 * into the air's SetTranParm datagram and re-applies it after a session restart. Dropped silently
 * if ml-linkd is down - the HUD re-asserts on the next link-up edge. */
void linkcmd_set_standby(int arm)
{
    mlm_rfcmd_send(MLM_RF_SET_STANDBY, arm ? 1 : 0);
}

void linkcmd_set_power(const char *level)
{
    int mw = POWER_DEFAULT_MW;
    for (unsigned i = 0; level != NULL && i < sizeof POWER_LEVELS / sizeof POWER_LEVELS[0]; i++) {
        if (strcmp(level, POWER_LEVELS[i].label) == 0) {
            mw = POWER_LEVELS[i].mw;
            break;
        }
    }

    mlm_rfcmd_send(MLM_RF_SET_POWER, (uint32_t) mw);
}

void linkcmd_set_camera(unsigned sel, unsigned value)
{
    mlm_rfcmd_send(MLM_RF_SET_CAMERA, (sel << 16) | (value & 0xffffu));
}

void linkcmd_set_scale(int aspect_4_3, unsigned zoom_pct)
{
    mlm_rfcmd_send(MLM_RF_SET_SCALE, ((aspect_4_3 ? 1u : 0u) << 16) | (zoom_pct & 0xffffu));
}

void linkcmd_request_scan(void)
{
    mlm_rfcmd_send(MLM_RF_SCAN, 0);
}

void linkcmd_select_channel(unsigned idx)
{
    mlm_rfcmd_send(MLM_RF_SELECT_CHANNEL, idx);
}
