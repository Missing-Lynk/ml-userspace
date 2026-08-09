/*
 * ml-air-bb.c - reference-counted access to the AR8030 bb control socket.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-air-bb.h"

/* Take a reference, opening the node if this is the first. Returns 0 when the socket is usable.
 * The open is retried on a cadence so a caller that asks before artosyn_sdio has probed is not a
 * hard failure. */
int air_bb_acquire(struct air_bb *bb, long now, const char *why)
{
    if (bb->fd >= 0) {
        bb->users++;
        return 0;
    }

    /* The cadence throttles retries after a FAILED open, so a node that is not there yet is not
     * hammered every tick. A successful open must not arm it: the socket is opened and closed once
     * per pair window, and throttling that would refuse a legitimate press for the wrong reason. */
    if (bb->last_open_ms != 0 && now - bb->last_open_ms < AIR_BB_OPEN_RETRY_MS) {
        return -1;
    }

    bb->fd = open(AIR_BB_NODE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (bb->fd < 0) {
        bb->last_open_ms = now;
        if (!bb->warned_open) {
            fprintf(stderr, TAG " bb: open(%s): %s, retrying\n", AIR_BB_NODE, strerror(errno));
            bb->warned_open = 1;
        }

        return -1;
    }

    bb->last_open_ms = 0;
    bb->users++;
    bb->warned_open = 0;

    if (g_verbose) {
        fprintf(stderr, TAG " bb: opened %s (%s)\n", AIR_BB_NODE, why);
    }

    return 0;
}

void air_bb_release(struct air_bb *bb)
{
    if (bb->users > 0) {
        bb->users--;
    }

    if (bb->users == 0 && bb->fd >= 0) {
        close(bb->fd);
        bb->fd = -1;

        if (g_verbose) {
            fprintf(stderr, TAG " bb: closed %s\n", AIR_BB_NODE);
        }
    }
}

/* Write one built frame. Returns 0 on success.
 *
 * The socket is non-blocking so the reply drain can run from the service tick, which means a write
 * returns EAGAIN whenever the driver's command TX queue has not drained yet; back-to-back frames
 * hit this routinely. EAGAIN is a busy socket, not a broken one, so the frame is retried against
 * POLLOUT (the driver reports it once cmd_txq empties) within a bounded budget, and the socket is
 * kept either way. Only a hard error drops the fd, since a half-written frame leaves the chip's
 * parser out of step.
 *
 * The budget bounds how long one frame can stall the tick, because ml_msp_service() has to keep
 * draining the FC UART. */
int air_bb_send(struct air_bb *bb, const uint8_t *frame, int len, const char *what)
{
    struct pollfd pfd = { .fd = bb->fd, .events = POLLOUT };
    int waited_ms;
    int sent = 0;

    if (bb->fd < 0 || len <= 0) {
        return -1;
    }

    for (waited_ms = 0; waited_ms <= AIR_BB_WRITE_WAIT_MS; waited_ms += AIR_BB_WRITE_POLL_MS) {
        ssize_t n = write(bb->fd, frame, (size_t)len);

        if (n == (ssize_t)len) {
            sent = 1;
            break;
        }

        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            pfd.revents = 0;
            poll(&pfd, 1, AIR_BB_WRITE_POLL_MS);
            continue;
        }

        fprintf(stderr, TAG " bb: %s write failed (%s), dropping the socket\n",
                what, strerror(errno));
        close(bb->fd);
        bb->fd = -1;
        bb->users = 0;

        return -1;
    }

    if (!sent) {
        fprintf(stderr, TAG " bb: %s not accepted in %d ms, frame dropped\n",
                what, AIR_BB_WRITE_WAIT_MS);
        return -1;
    }

    if (waited_ms > 0 && g_verbose) {
        fprintf(stderr, TAG " bb: %s accepted after %d ms of backpressure\n", what, waited_ms);
    }

    if (g_verbose) {
        fprintf(stderr, TAG " bb: sent %s (%d B)\n", what, len);
    }

    return 0;
}

/* Drain whatever the bb socket has and route each reply by selector. Frames of any other class (the
 * ch05 chip log in particular) are handed over too and the callback ignores them: on the air unit
 * nothing else reads this node, so the log has to be consumed here or it backs up.
 *
 * Bounded per tick: the socket must never be able to hold the service loop. */
void air_bb_drain(struct air_bb *bb, air_bb_reply_fn cb, void *ctx)
{
    struct pollfd pfd = { .fd = bb->fd, .events = POLLIN };
    uint8_t buf[4096];
    int reads;

    if (bb->fd < 0) {
        return;
    }

    for (reads = 0; reads < AIR_BB_DRAIN_MAX; reads++) {
        if (poll(&pfd, 1, 0) <= 0 || (pfd.revents & POLLIN) == 0) {
            return;
        }

        ssize_t n = read(bb->fd, buf, sizeof buf);

        if (n <= 0) {
            return;
        }

        for (ssize_t i = 0; i + 18 < n; i++) {
            const uint8_t *pay;
            int plen;

            if (buf[i] != 0xaa) {
                continue;
            }

            plen = buf[i + 1] | (buf[i + 2] << 8);
            if (plen < 1 || i + 18 + plen >= n) {
                continue;
            }

            if (buf[i + 18 + plen] != 0xbb || buf[i + 5] != BB_GET) {
                continue;
            }

            pay = buf + i + 18;

            cb(buf[i + 8], pay, plen, ctx);
        }
    }
}
