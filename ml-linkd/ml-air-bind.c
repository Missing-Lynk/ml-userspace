/*
 * ml-air-bind.c - bind button and the DEV-role pair window.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/input.h>

#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-air-bb.h"
#include "ml-air-bind.h"

/* Write @p val to @p led / @p attr. Returns 0 on success, -1 on error; the caller ignores it, since
 * the LED nodes are absent until artosyn_gpio binds and the blink is indication only. */
static int air_led_write(const char *led, const char *attr, const char *val)
{
    char path[192];
    ssize_t written;
    int fd;

    snprintf(path, sizeof path, "%s/%s", led, attr);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    written = write(fd, val, strlen(val));
    close(fd);

    return written < 0 ? -1 : 0;
}

/* Read @p attr from @p led. Returns the value, or @p dflt if the node is absent or unreadable. */
static int air_led_read(const char *led, const char *attr, int dflt)
{
    char path[192];
    char buf[32];
    int fd;
    ssize_t n;

    snprintf(path, sizeof path, "%s/%s", led, attr);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return dflt;
    }

    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) {
        return dflt;
    }

    buf[n] = '\0';

    return (int)strtol(buf, NULL, 10);
}

/* Both lines share one series resistor, so lighting green starves the red: the indication is a
 * switch between the colours, never a combination. Green's level is sampled on the way in and
 * written back on the way out.
 *
 * delay_on/delay_off only exist once the timer trigger is selected, so the trigger write comes
 * first. Clearing the trigger back to none leaves the brightness where the trigger left it. */
static void air_bind_led_on(struct air_bind *button)
{
    char ms[16];

    button->green_saved = air_led_read(AIR_BIND_LED_GREEN, "brightness", -1);
    air_led_write(AIR_BIND_LED_GREEN, "brightness", "0");

    snprintf(ms, sizeof ms, "%d", AIR_BIND_LED_MS);
    air_led_write(AIR_BIND_LED_RED, "trigger", "timer");
    air_led_write(AIR_BIND_LED_RED, "delay_on", ms);
    air_led_write(AIR_BIND_LED_RED, "delay_off", ms);
}

static void air_bind_led_off(struct air_bind *button)
{
    char level[16];

    air_led_write(AIR_BIND_LED_RED, "trigger", "none");
    air_led_write(AIR_BIND_LED_RED, "brightness", "0");

    if (button->green_saved < 0) {
        return;
    }

    snprintf(level, sizeof level, "%d", button->green_saved);
    air_led_write(AIR_BIND_LED_GREEN, "brightness", level);
    button->green_saved = -1;
}

/* Find the bind button by its device name and open it non-blocking. Returns the fd, or -1. */
static int air_bind_open(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    int fd = -1;

    if (d == NULL) {
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        char path[280];
        char name[64] = { 0 };
        int cand;

        if (strncmp(e->d_name, "event", 5) != 0) {
            continue;
        }

        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        cand = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (cand < 0) {
            continue;
        }

        if (ioctl(cand, EVIOCGNAME(sizeof name - 1), name) >= 0
            && strcmp(name, AIR_BIND_DEV_NAME) == 0) {
            fd = cand;
            break;
        }

        close(cand);
    }

    closedir(d);

    return fd;
}

/* Read the persisted peer MAC out of the baseband config: the value of the "ap_mac" key, 8 hex
 * digits in wire order. The sibling AP entry is keyed "mac", so "ap_mac" is unambiguous. Returns 0
 * and fills @p mac on success.
 *
 * This is the baked binding. A peer committed at runtime and not persisted is not represented here,
 * which is the known gap in using it as the restore source. */
static int air_bind_saved_mac(uint8_t mac[4])
{
    char buf[16384];
    const char *p;
    int fd = open(AIR_BIND_CFG_USR, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        fd = open(AIR_BIND_CFG, O_RDONLY | O_CLOEXEC);
    }

    if (fd < 0) {
        return -1;
    }

    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }

    buf[n] = '\0';
    p = strstr(buf, "\"ap_mac\"");
    if (p == NULL) {
        return -1;
    }

    p = strchr(p + 8, ':');
    if (p == NULL) {
        return -1;
    }

    while (*p == ':' || *p == ' ' || *p == '"') {
        p++;
    }

    for (int i = 0; i < 4; i++) {
        char byte[3] = { p[i * 2], p[i * 2 + 1], '\0' };
        char *end;
        long v;

        if (byte[0] == '\0' || byte[1] == '\0') {
            return -1;
        }

        v = strtol(byte, &end, 16);
        if (*end != '\0') {
            return -1;
        }

        mac[i] = (uint8_t)v;
    }

    return 0;
}

static int air_bind_set_mac(struct air_bb *bb, const uint8_t mac[4], const char *what)
{
    uint8_t frame[32];

    if (air_bb_send(bb, frame, bb_set_ap_mac(frame, mac, bb->seq++), what) != 0) {
        fprintf(stderr, TAG " bind: %s -> %02x%02x%02x%02x FAILED\n",
                what, mac[0], mac[1], mac[2], mac[3]);
        return -1;
    }

    if (g_verbose) {
        fprintf(stderr, TAG " bind: %s -> %02x%02x%02x%02x\n",
                what, mac[0], mac[1], mac[2], mac[3]);
    }

    return 0;
}

/* Ask the chip whether a link is up, so a bind can be refused while one is.
 *
 * GET_MCS throughput is the discriminator, measured on this hardware in both states: zero with no
 * peer associated, and the live link rate (tens of Mbps) once associated. It is read from the chip
 * rather than inferred from the goggle's datagrams, so a peer whose userspace has stopped still
 * reads as linked and cannot be unbound by a stray press.
 *
 * @return 1 link up, 0 no link, -1 no usable answer.
 *
 * The reply is read directly rather than through the shared drain, because the answer is needed
 * before the gesture can proceed. Any other frame read here is discarded; the only loss is at most
 * one rate-governor sample, which it re-polls on its own cadence.
 */
static int air_bind_link_up(struct air_bb *bb)
{
    uint8_t frame[32];
    uint8_t buf[4096];

    if (air_bb_send(bb, frame, bb_get(frame, GET_MCS, bb->seq++), "gate GET_MCS") != 0) {
        return -1;
    }

    for (int waited_ms = 0; waited_ms < AIR_BIND_GATE_WAIT_MS; waited_ms += AIR_BIND_GATE_POLL_MS) {
        struct pollfd pfd = { .fd = bb->fd, .events = POLLIN };
        ssize_t n;

        if (poll(&pfd, 1, AIR_BIND_GATE_POLL_MS) <= 0) {
            continue;
        }

        n = read(bb->fd, buf, sizeof buf);
        if (n <= 0) {
            continue;
        }

        for (ssize_t i = 0; i + 18 < n; i++) {
            int plen;

            if (buf[i] != 0xaa) {
                continue;
            }

            plen = buf[i + 1] | (buf[i + 2] << 8);
            if (plen < MCS_OFF_THROUGHPUT + 4 || i + 18 + plen >= n) {
                continue;
            }

            if (buf[i + 18 + plen] != 0xbb || buf[i + 5] != BB_GET || buf[i + 8] != GET_MCS) {
                continue;
            }

            const uint8_t *pay = buf + i + 18;
            uint32_t kbps = (uint32_t)pay[MCS_OFF_THROUGHPUT]
                            | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 1] << 8)
                            | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 2] << 16)
                            | ((uint32_t)pay[MCS_OFF_THROUGHPUT + 3] << 24);

            if (g_verbose) {
                fprintf(stderr, TAG " bind: link check: mcs %d, throughput %u kbps\n",
                        (int)pay[MCS_OFF_INDEX] - MCS_INDEX_BIAS, kbps);
            }

            return kbps > 0 ? 1 : 0;
        }
    }

    return -1;
}

/* Write the committed peer into the air config so it survives a power cycle, by running the same
 * helper the AP role uses in its --air mode. Kept out of the daemon: ml-linkd owns the bb socket and
 * has no business parsing configs, and the edit needs no chip access.
 * @return 0 if the helper exited 0. */
static int air_bind_persist(const uint8_t mac[4])
{
    char mac_str[9];
    pid_t pid;
    int status;
    pid_t w;

    snprintf(mac_str, sizeof mac_str, "%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3]);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, TAG " bind: fork: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execl(AIR_BIND_PERSIST, AIR_BIND_PERSIST, "--air", mac_str, (char *)NULL);
        _exit(127);
    }

    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }

    return 0;
}

/* Open the pair window: broadcast the ap_mac, then enter pair mode on slot 0. The broadcast is sent
 * before pair mode and its failure aborts with no cleanup, since pair mode was never entered. */
static void air_bind_start(struct air_bind *button, struct air_bb *bb, long now)
{
    static const uint8_t broadcast[4] = { 0xff, 0xff, 0xff, 0xff };
    uint8_t frame[32];
    int link;

    if (air_bb_acquire(bb, now, "pair window") != 0) {
        printf(TAG " bind: bb socket unavailable, pair window not opened\n");
        fflush(stdout);
        return;
    }

    button->holding = 1;

    /* Refuse while a link is up, and refuse when the chip will not say. A bind is a deliberate act
     * the operator can repeat; an unbind mid-flight costs the video feed for the window, so the
     * uncertain case fails closed. */
    link = air_bind_link_up(bb);
    if (link != 0) {
        air_bb_release(bb);
        button->holding = 0;
        printf(TAG " bind: refused, %s\n",
               link > 0 ? "a link is up" : "chip did not report link state");
        fflush(stdout);
        return;
    }

    button->have_saved = (air_bind_saved_mac(button->saved) == 0);
    if (button->have_saved) {
        if (g_verbose) {
            fprintf(stderr, TAG " bind: saved ap_mac %02x%02x%02x%02x for the timeout path\n",
                    button->saved[0], button->saved[1], button->saved[2], button->saved[3]);
        }
    } else {
        fprintf(stderr, TAG " bind: no saved ap_mac; a timeout will leave the unit broadcast-bound\n");
    }

    if (air_bind_set_mac(bb, broadcast, "ap_mac broadcast") != 0) {
        air_bb_release(bb);
        button->holding = 0;
        printf(TAG " bind: broadcast failed, pair window not opened\n");
        fflush(stdout);
        return;
    }

    button->slot = 0;
    button->hits = 0;
    button->last_poll_ms = 0;
    button->restore_tries = 0;
    button->polls = 0;
    button->replies = 0;
    memset(button->peer, 0, sizeof button->peer);

    /* From here the chip is on the broadcast MAC. If pair mode cannot be entered there is no window
     * to run and nothing will restore it later, so undo it now. */
    if (air_bb_send(bb, frame, bb_pair_mode(frame, 1, (uint8_t)button->slot, bb->seq++), "pair-on") != 0) {
        if (button->have_saved) {
            air_bind_set_mac(bb, button->saved, "restore ap_mac");
        }
        air_bb_release(bb);
        button->holding = 0;
        printf(TAG " bind: pair mode refused, pair window not opened\n");
        fflush(stdout);

        return;
    }

    button->phase = PAIR_POLL;
    button->window_until_ms = now + AIR_BIND_WINDOW_MS;
    air_bind_led_on(button);

    printf(TAG " bind: pair window open for %d s\n", AIR_BIND_WINDOW_MS / 1000);
    fflush(stdout);
}

static void air_bind_stop(struct air_bind *button, struct air_bb *bb, const char *why)
{
    button->window_until_ms = 0;
    button->phase = PAIR_IDLE;

    if (button->holding) {
        air_bb_release(bb);
        button->holding = 0;
    }

    air_bind_led_off(button);

    printf(TAG " bind: pair window closed (%s): %d polls, %d replies, %d hits\n",
           why, button->polls, button->replies, button->hits);
    fflush(stdout);
}

/* True while a pair window is open. */
int air_bind_active(const struct air_bind *button)
{
    return button->window_until_ms != 0;
}

/* Act on one release: the hold decides between the pair window and the reserved long press. */
static void air_bind_gesture(struct air_bind *button, struct air_bb *bb, long hold_ms, long now)
{
    /* The evdev timestamps are CLOCK_REALTIME, so a clock step between press and release can put
     * the hold outside the window either way; a negative one is not a short press. */
    if (hold_ms < 0 || hold_ms > AIR_BIND_HOLD_MAX_MS) {
        if (g_verbose) {
            fprintf(stderr, TAG " bind: %ld ms hold, reserved, ignored\n", hold_ms);
        }

        return;
    }

    if (air_bind_active(button)) {
        if (g_verbose) {
            fprintf(stderr, TAG " bind: %ld ms hold, pair window already open, ignored\n", hold_ms);
        }

        return;
    }

    if (g_verbose) {
        fprintf(stderr, TAG " bind: %ld ms hold\n", hold_ms);
    }

    air_bind_start(button, bb, now);
}

/* One GET_PAIR reply. Byte 0 is the candidate bitmask; the lowest set bit is the slot, overwriting
 * the requested one, and the peer MAC follows at 1 + slot*4 in wire order. Hits accumulate and are
 * never reset by a zero read. */
void air_bind_pair_reply(struct air_bind *button, const uint8_t *pay, int plen)
{
    int slot;

    if (button->phase != PAIR_POLL || plen < 1) {
        return;
    }

    button->replies++;

    if (pay[0] == 0) {
        return;
    }

    for (slot = 0; slot < 8; slot++) {
        if ((pay[0] & (1u << slot)) != 0) {
            break;
        }
    }

    if (slot == 8 || plen < 1 + slot * 4 + 4) {
        return;
    }

    button->slot = slot;
    button->hits++;
    memcpy(button->peer, pay + 1 + slot * 4, sizeof button->peer);

    if (g_verbose) {
        fprintf(stderr, TAG " bind: candidate bitmask 0x%02x slot %d hit %d/%d\n",
                pay[0], slot, button->hits, AIR_BIND_HITS);
    }
}

/* Tear a window down in one go: exit pair mode and put the saved ap_mac back, without waiting for
 * the tick the normal path uses to space those two writes. For the cases that cannot wait, process
 * shutdown and the socket disappearing, where leaving the chip in pair mode on the broadcast MAC
 * would cost a power cycle to undo. */
void air_bind_abort(struct air_bind *button, struct air_bb *bb, const char *why)
{
    uint8_t frame[32];

    if (button->phase == PAIR_POLL) {
        air_bb_send(bb, frame, bb_pair_mode(frame, 0, (uint8_t)button->slot, bb->seq++), "pair-off");
    }

    if (button->phase != PAIR_COMMIT && button->have_saved) {
        air_bind_set_mac(bb, button->saved, "restore ap_mac");
    }

    air_bind_stop(button, bb, why);
}

/* Drive the pair sequence one tick. Pair mode is exited before the commit, matching the vendor
 * order, and the write that follows lands on the next tick. */
void air_bind_pair_service(struct air_bind *button, struct air_bb *bb, long now)
{
    uint8_t frame[32];

    if (button->phase == PAIR_IDLE) {
        return;
    }

    /* The socket went away under an open window (a failed write drops it). Every remaining step
     * needs it, and the unit is sitting on the broadcast MAC until one of them lands. */
    if (bb->fd < 0) {
        button->holding = 0;
        fprintf(stderr, TAG " bind: bb socket lost mid-window; ap_mac may be left broadcast,"
                        " power-cycle to fall back to the baked config\n");
        air_bind_stop(button, bb, "socket lost");

        return;
    }

    if (button->phase == PAIR_COMMIT) {
        const char *tag;

        if (air_bind_set_mac(bb, button->peer, "commit peer ap_mac") != 0) {
            /* The chip never took the peer, so persisting it would record a binding the unit does
             * not have. Put the previous one back instead. */
            button->phase = PAIR_RESTORE;
            return;
        }

        tag = air_bind_persist(button->peer) == 0 ? "persisted" : "runtime only, persist FAILED";
        printf(TAG " bind: paired with %02x%02x%02x%02x on slot %d (%s)\n",
               button->peer[0], button->peer[1], button->peer[2], button->peer[3], button->slot, tag);
        air_bind_stop(button, bb, "paired");

        return;
    }

    if (button->phase == PAIR_RESTORE) {
        /* The write that puts the binding back. Backpressure here would otherwise leave the unit
         * broadcast-bound until a power cycle, so it is retried across ticks rather than attempted
         * once. */
        if (button->have_saved && air_bind_set_mac(bb, button->saved, "restore ap_mac") != 0) {
            if (++button->restore_tries < AIR_BIND_RESTORE_TRIES) {
                return;
            }

            fprintf(stderr, TAG " bind: ap_mac restore failed %d times, giving up;"
                            " unit is broadcast-bound until a power cycle\n", button->restore_tries);
        }

        air_bind_stop(button, bb, "timeout");
        return;
    }

    if (button->hits >= AIR_BIND_HITS) {
        air_bb_send(bb, frame, bb_pair_mode(frame, 0, (uint8_t)button->slot, bb->seq++), "pair-off");
        button->phase = PAIR_COMMIT;

        return;
    }

    if (now >= button->window_until_ms) {
        air_bb_send(bb, frame, bb_pair_mode(frame, 0, (uint8_t)button->slot, bb->seq++), "pair-off");
        button->phase = PAIR_RESTORE;

        return;
    }

    if (now - button->last_poll_ms >= AIR_BIND_POLL_MS) {
        button->last_poll_ms = now;
        if (air_bb_send(bb, frame, bb_get(frame, GET_PAIR, bb->seq++), "pair-poll") == 0) {
            button->polls++;
        }
    }
}

/* Resolve the device if it is not open yet, drain its events, and expire an open window. */
void air_bind_service(struct air_bind *button, struct air_bb *bb, long now)
{
    struct input_event ev;

    if (button->fd < 0) {
        if (button->last_open_ms != 0 && now - button->last_open_ms < AIR_BIND_OPEN_RETRY_MS) {
            return;
        }

        button->last_open_ms = now;
        button->fd = air_bind_open();
        if (button->fd < 0) {
            if (!button->warned_open) {
                fprintf(stderr, TAG " bind: %s not present (artosyn_gpio not bound?), retrying\n",
                        AIR_BIND_DEV_NAME);
                button->warned_open = 1;
            }

            return;
        }

        if (g_verbose) {
            fprintf(stderr, TAG " bind: watching %s\n", AIR_BIND_DEV_NAME);
        }
        button->warned_open = 0;
    }

    for (int got = 0; got < AIR_BIND_EV_BURST_MAX; got++) {
        ssize_t n = read(button->fd, &ev, sizeof ev);
        long ev_ms;

        if (n != (ssize_t)sizeof ev) {
            /* Anything but an empty queue means the device is gone: drop the fd so the resolve
             * above picks it up again. */
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                break;
            }

            fprintf(stderr, TAG " bind: %s read failed (%s), reopening\n",
                    AIR_BIND_DEV_NAME, strerror(errno));
            close(button->fd);
            button->fd = -1;
            button->press_ms = 0;

            return;
        }

        if (ev.type != EV_KEY || ev.code != KEY_CONNECT) {
            continue;
        }

        ev_ms = (long)ev.time.tv_sec * 1000 + ev.time.tv_usec / 1000;

        if (ev.value == 1) {
            button->press_ms = ev_ms;
        } else if (ev.value == 0 && button->press_ms != 0) {
            air_bind_gesture(button, bb, ev_ms - button->press_ms, now);
            button->press_ms = 0;
        }
    }

}
