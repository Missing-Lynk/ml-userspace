/*
 * ml-air-seen.h - report-once latch for message kinds the air drains without acting on.
 *
 * A goggle re-sends a command every session and, for some, every few seconds, so a line per
 * datagram would bury the log. This keeps the codes already reported so each kind is announced
 * once per run, which is what makes an unbuilt actuator a fact in the log instead of a silence.
 *
 * Header-only and pure, so both the :10000 dispatcher and the SetCameraInfo handler share it with
 * no object file of their own and the host test exercises it directly.
 */
#ifndef ML_AIR_SEEN_H
#define ML_AIR_SEEN_H

#include <stdint.h>

/*
 * Distinct codes remembered. The wire has ten SetCameraInfo selectors and a dozen :10000 types,
 * so this holds every code either dispatcher can legitimately meet, with room over.
 */
#define AIR_SEEN_MAX 32

struct air_seen {
    uint32_t codes[AIR_SEEN_MAX];
    unsigned int count;
};

/*
 * @return 1 the first time @p code is offered to @p s, 0 on every later offer of the same code.
 *
 * A full record also returns 0: the codes come off the network, so a peer sending an unbounded
 * spread of them must not be able to drive an unbounded number of log lines.
 */
static inline int air_seen_first(struct air_seen *s, uint32_t code)
{
    for (unsigned int i = 0; i < s->count; i++) {
        if (s->codes[i] == code) {
            return 0;
        }
    }

    if (s->count == AIR_SEEN_MAX) {
        return 0;
    }

    s->codes[s->count++] = code;

    return 1;
}

#endif /* ML_AIR_SEEN_H */
