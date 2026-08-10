/*
 * ml-rfcmd - send one RF command to ml-linkd's link.sock.
 *
 * The same MLM_T_RFCMD datagram the HUD sends, from a shell. The HUD is otherwise the only sender,
 * which leaves the channel, scan and bind paths reachable only through the UI; this makes them
 * testable on a unit with no display, and reproducible in a script.
 *
 * Fire-and-forget: the datagram is unacknowledged, so a zero exit means it was sent, not that
 * ml-linkd accepted it. The outcome shows up in ml-linkd's log and, for the paths that have one,
 * as an MLM_T_LINK event.
 *
 * Usage: ml-rfcmd <command> [arg]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../ml-shared/mlm.h"

/* One accepted argument value, or ARG_ANY for a command that bounds its own. ml-linkd validates
 * everything again on receipt; the lists here exist so a typo fails at the shell rather than
 * silently on the far side of a socket. */
#define ARG_ANY   0xffffffffu
#define RF_ARG_MAX   3

struct rf_verb {
    const char *name;
    uint32_t cmd;
    int takes_arg;              /* 0 = arg ignored, 1 = required */
    uint32_t allowed[RF_ARG_MAX];  /* terminated by ARG_ANY, or first entry ARG_ANY for unbounded */
    const char *help;
};

static const struct rf_verb VERBS[] = {
    { "standby", MLM_RF_SET_STANDBY,    1, { 0, 1, ARG_ANY },   "0 disarm, 1 arm" },
    { "power",   MLM_RF_SET_POWER,      1, { 25, 100, 200 },    "air TX power in mW" },
    { "bitrate", MLM_RF_SET_BITRATE,    1, { 8, 16, 24 },       "air video bitrate in Mbps" },
    { "channel", MLM_RF_SELECT_CHANNEL, 1, { ARG_ANY },         "channel table index 0..18" },
    { "scan",    MLM_RF_SCAN,           0, { ARG_ANY },         "one-shot channel sweep" },
    { "bind",    MLM_RF_BIND,           1, { 0, 1, ARG_ANY },   "0 dry-run, 1 persist the peer" },
};
static const int VERBS_N = sizeof(VERBS) / sizeof(VERBS[0]);

static void usage(void)
{
    fprintf(stderr, "usage: ml-rfcmd <command> [arg]\n");
    for (int i = 0; i < VERBS_N; i++) {
        fprintf(stderr, "  %-8s %-6s %s\n", VERBS[i].name,
                VERBS[i].takes_arg ? "<arg>" : "", VERBS[i].help);
    }

    fprintf(stderr, "  raw      <cmd> <arg>  any enum mlm_rfcmd_type value, unchecked\n");
}

/* @return 0 when @p arg is in @p allowed, or the list is unbounded. */
static int arg_allowed(const uint32_t *allowed, uint32_t arg)
{
    for (int i = 0; i < RF_ARG_MAX; i++) {
        if (allowed[i] == ARG_ANY) {
            return i == 0 ? 0 : -1;   /* ARG_ANY first = unbounded, else end of list */
        }

        if (allowed[i] == arg) {
            return 0;
        }
    }

    return -1;
}

int main(int argc, char **argv)
{
    uint32_t arg = 0;

    if (argc < 2) {
        usage();
        return 2;
    }

    /* escape hatch for the selector-packed commands (camera, scale) and anything added later */
    if (!strcmp(argv[1], "raw")) {
        if (argc != 4) {
            usage();
            return 2;
        }

        return mlm_rfcmd_send((uint32_t)strtoul(argv[2], NULL, 0),
                              (uint32_t)strtoul(argv[3], NULL, 0)) == 0 ? 0 : 1;
    }

    for (int i = 0; i < VERBS_N; i++) {
        const struct rf_verb *verb = &VERBS[i];

        if (strcmp(argv[1], verb->name)) {
            continue;
        }

        if (verb->takes_arg) {
            char *end;

            if (argc != 3) {
                fprintf(stderr, "ml-rfcmd: %s needs an argument (%s)\n", verb->name, verb->help);
                return 2;
            }

            arg = (uint32_t)strtoul(argv[2], &end, 0);
            if (*end != '\0') {
                fprintf(stderr, "ml-rfcmd: '%s' is not a number\n", argv[2]);
                return 2;
            }

            if (arg_allowed(verb->allowed, arg) != 0) {
                fprintf(stderr, "ml-rfcmd: %s does not take %u (%s)\n", verb->name, arg, verb->help);
                return 2;
            }
        }

        if (mlm_rfcmd_send(verb->cmd, arg) != 0) {
            fprintf(stderr, "ml-rfcmd: send to %s failed (is ml-linkd up?)\n", MLM_LINK_SOCK);
            return 1;
        }

        return 0;
    }

    fprintf(stderr, "ml-rfcmd: unknown command '%s'\n", argv[1]);
    usage();
    return 2;
}
