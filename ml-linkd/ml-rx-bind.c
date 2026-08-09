/*
 * ml-rx-bind.c - the RX role's pair sequence.
 *
 * A HUD bind request is queued from the UDP thread and run by the bb-socket TX thread; the GET_PAIR
 * replies it polls for are parsed by the reader thread into the snapshot below.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "../ml-shared/mlm.h"
#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-rx.h"
#include "ml-rx-bind.h"

/* Binding (the vendor RX pair loop, AR_AR8030_RX_BbPair @0x462ea8). The window matches the vendor
 * MID trigger's RX_BbPair(0x1e) = 30 s; the poll cadence and hit threshold are the disasm's
 * usleep(20000) and `iVar5 > 5`. The hit count is CUMULATIVE over the window (the disasm has no
 * reset on a zero read), and a hit is specifically the slot-0 bit of the reply bitmask. */
#define BIND_WINDOW_MS   30000            /* pair window before giving up */
#define BIND_POLL_US     20000            /* GET_PAIR poll spacing (vendor cadence) */
#define BIND_HITS        6                /* slot-0 hits before the lock (cumulative) */
#define BIND_REPLY_MS    40               /* max wait for one GET_PAIR reply */

/* Writes the locked peer into the config so it survives a power cycle. */
#define PERSIST_TOOL     "/usr/local/bin/ml-rf-persist"

/* The UDP thread queues the request (after the air-liveness gate) and the bb-socket owner (main
 * STEADY loop) runs it; the reader parses the GET_PAIR replies into the g_pair_* snapshot exactly
 * like the Get1V1Info pattern (seq bumped on every reply so the poll can wait for a fresh one).
 * g_pair_mac is written by the reader and read by main only while a poll it issued is in flight, so
 * the seq fence orders the accesses. */
static volatile int g_pending_bind;         /* a bind request is queued for the STEADY loop */
static volatile int g_bind_persist;         /* queued request wants persistence (arg != 0) */
static volatile unsigned g_pair_seq;        /* bumped on every GET_PAIR reply */
static volatile int g_pair_hit;             /* last reply's slot-0 candidate bit */
static uint8_t g_pair_mac[4];               /* last reply's slot-0 MAC, wire order */

/* An air unit is currently alive (fresh :10000 telemetry). Gates binding: pair-locking a new peer
 * while a bound air unit is up could re-pair away a flying quad, so a live link refuses the
 * command. Checked on receipt (fast feedback) and re-checked on the bb-socket thread
 * (authoritative: the queue hop is not atomic with the check). */
int rx_bind_air_alive(long now)
{
    return !g_air_lost && g_last_telem_ms && now - g_last_telem_ms < AIR_LOSS_MS;
}

/* Persist a locked peer MAC into the config candidate list by running the ml-rf-persist helper
 * (cJSON edit of the /usrdata config; it owns the dedup/FIFO logic and both band variants). Kept
 * out of ml-linkd itself: ml-linkd is the bb-socket owner and has no business parsing configs, and
 * the file edit needs no chip access. @return 0 if the helper exited 0, -1 otherwise. */
static int bind_persist(const uint8_t mac[4])
{
    char mac_str[9];
    pid_t pid, w;
    int status;

    snprintf(mac_str, sizeof mac_str, "%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3]);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, TAG " bind: fork: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execl(PERSIST_TOOL, PERSIST_TOOL, mac_str, (char *)NULL);
        _exit(127);
    }

    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, TAG " bind: %s failed (peer %s bound at runtime only)\n",
                PERSIST_TOOL, mac_str);
        return -1;
    }

    return 0;
}

/* Bind a new air unit: the vendor RX pair sequence (AR_AR8030_RX_BbPair @0x462ea8), byte-exact.
 * Enter pair mode, poll GET_PAIR every 20 ms for the slot-0 candidate bit, and after the 6th hit
 * read the peer MAC from the reply, exit pair mode, then pair-lock the MAC (vendor order: exit
 * BEFORE lock). The AU must be in its own pair mode (its bind button) for the chips to find each
 * other; the exchange itself is chip-autonomous. On timeout the only cleanup is exiting pair mode.
 *
 * The chip-runtime lock does not survive a power cycle; with g_bind_persist set, the locked MAC is
 * written into the config candidate list (ml-rf-persist) so it re-binds at the next insmod; dry-run
 * (g_bind_persist == 0) leaves only the runtime lock, which a power cycle reverts. Blocks the STEADY
 * cadence for up to BIND_WINDOW_MS, which is fine: the air-liveness gate means there is no session
 * to disturb. */
static void bind_run(uint8_t *frame, uint32_t *seq_link)
{
    uint8_t mac[4] = { 0 };
    int hits = 0;
    char what[96];
    const char *tag;

    if (rx_bind_air_alive(now_ms())) {
        link_event(MLM_LINK_BIND_FAIL, "bind refused: an air unit is connected");
        return;
    }

    link_event(MLM_LINK_BINDING, "pair mode on, waiting for an air unit in bind mode");
    send_frame(frame, bb_pair_mode(frame, 1, 0, (*seq_link)++), "pair-on");

    for (long t0 = now_ms(); g_run && now_ms() - t0 < BIND_WINDOW_MS && hits < BIND_HITS; ) {
        uint8_t poll[19];
        unsigned seq0 = g_pair_seq;
        long ts;

        bb_get(poll, GET_PAIR, (*seq_link)++);
        send_frame(poll, 19, "pair-poll");

        for (ts = now_ms(); g_pair_seq == seq0 && now_ms() - ts < BIND_REPLY_MS; ) {
            usleep(2000);
        }

        if (g_pair_seq != seq0) {
            /* pair with the RELEASE fence in the reader: once the new seq is visible, the matching
             * g_pair_hit + g_pair_mac are too. */
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (g_pair_hit) {
                hits++;
                memcpy(mac, g_pair_mac, sizeof mac);
            }
        }

        usleep(BIND_POLL_US);
    }

    send_frame(frame, bb_pair_mode(frame, 0, 0, (*seq_link)++), "pair-off");

    if (hits < BIND_HITS) {
        link_event(MLM_LINK_BIND_FAIL, "bind failed: no air unit found in the pair window");
        return;
    }

    usleep(BIND_POLL_US);
    send_frame(frame, bb_pair_lock(frame, mac, (*seq_link)++), "pair-lock");

    /* dry-run: chip-runtime lock only (power cycle reverts). persist: also write the config, and
     * report if that write failed so the peer is known to be runtime-only. */
    if (!g_bind_persist) {
        tag = " (dry-run)";
    } else if (bind_persist(mac) == 0) {
        tag = " (persisted)";
    } else {
        tag = " (persist FAILED, runtime only)";
    }

    snprintf(what, sizeof what, "bind ok, peer mac %02x%02x%02x%02x locked%s",
             mac[0], mac[1], mac[2], mac[3], tag);
    link_event_aux(MLM_LINK_BIND_OK,
                   (uint32_t)mac[0] << 24 | (uint32_t)mac[1] << 16
                   | (uint32_t)mac[2] << 8 | mac[3], what);
}

/* GET_PAIR reply (98 B): byte0 = candidate bitmask, slot-0 MAC at +1 in wire order. Snapshot for
 * the poll in bind_run; the seq bumps last so a fresh read sees the matching hit/MAC pair. */
void rx_bind_on_pair(const uint8_t *payload, int plen)
{
    if (plen < 5) {
        return;
    }

    g_pair_hit = payload[0] & 1;
    memcpy(g_pair_mac, payload + 1, sizeof g_pair_mac);
    /* publish g_pair_hit/g_pair_mac before the seq bump so a reader that observes the new
     * g_pair_seq is guaranteed to see the matching hit + MAC (aarch64 is weakly ordered; the
     * volatile seq alone does not order the plain g_pair_mac store). */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_pair_seq++;
}

/* Refused while an air unit is alive so nothing can re-pair mid-flight; the immediate BIND_FAIL
 * gives the HUD its failure cue without waiting on the queue. bind_run re-checks. */
void rx_bind_request(int persist, long now)
{
    if (rx_bind_air_alive(now)) {
        link_event(MLM_LINK_BIND_FAIL, "bind refused: an air unit is connected");
        return;
    }

    if (g_pending_bind) {
        fprintf(stderr, TAG " rfcmd: bind already queued, ignoring\n");
        return;
    }

    if (g_verbose) {
        printf(TAG " rfcmd: bind requested (%s)\n", persist ? "persist" : "dry-run");
        fflush(stdout);
    }

    g_bind_persist = persist;
    g_pending_bind = 1;
}

/* Run a queued bind from the bb-socket TX thread. */
void rx_bind_service(uint8_t *frame, uint32_t *seq_link)
{
    if (!g_pending_bind) {
        return;
    }

    g_pending_bind = 0;
    bind_run(frame, seq_link);
}
