/** @file linkcmd.c @brief See linkcmd.h. */
#include "linkcmd.h"

#include "../../../ml-shared/mlm.h"

/* One connectionless DGRAM to link.sock per call (mlm_rfcmd_send); ml-linkd translates the intent
 * into the air's SetTranParm datagram and re-applies it after a session restart. Dropped silently
 * if ml-linkd is down - the HUD re-asserts on the next link-up edge. */
void linkcmd_set_standby(int arm)
{
    mlm_rfcmd_send(MLM_RF_SET_STANDBY, arm ? 1 : 0);
}
