/*
 * ml-rx-reader.c - the RX role's bb-socket reader thread.
 *
 * Deframes the AA | plen | ... | BB envelope the chip sends on /dev/artosyn_sdio, keeps the ch05
 * chip log drained, and hands every recognised GET reply to the module that owns it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-rx.h"
#include "ml-rx-chan.h"
#include "ml-rx-bind.h"
#include "ml-rx-reader.h"

#define READ_IDLE_US     2000             /* backoff between empty device reads */

/* Replies arrive on the request channel BB_GET (0x01), port = selector. */
#define REPLY_CH         0x01

static char g_chiplog[16384];
static int g_chiplog_n;

/* Dump a raw bb-socket frame (header + payload + trailer) as offset/hex/ascii rows to stdout, so a
 * bench capture can confirm an un-RE'd reply envelope byte-for-byte. Only reached under --scan-probe. */
static void hexdump_frame(const char *what, const uint8_t *frame, int n)
{
    printf(TAG " %s (%d B):\n", what, n);
    for (int off = 0; off < n; off += 16) {
        printf("  %04x: ", off);
        for (int k = 0; k < 16; k++) {
            if (off + k < n) {
                printf("%02x ", frame[off + k]);
            } else {
                printf("   ");
            }
        }

        printf(" |");
        for (int k = 0; k < 16 && off + k < n; k++) {
            uint8_t c = frame[off + k];

            putchar(c >= 32 && c < 127 ? c : '.');
        }

        printf("|\n");
    }

    fflush(stdout);
}

void *rx_reader_thread(void *arg)
{
    uint8_t buf[8192];
    (void)arg;

    while (g_run) {
        ssize_t n = read(g_fd, buf, sizeof buf);

        if (n <= 0) {
            if (!g_run) {
                break;
            }

            usleep(READ_IDLE_US);
            continue;
        }

        for (ssize_t i = 0; i + 18 < n; i++) {
            int plen;

            if (buf[i] != 0xAA) {
                continue;
            }

            plen = buf[i + 1] | (buf[i + 2] << 8);
            if (plen < 0 || i + 18 + plen >= n) {
                continue;
            }

            if (buf[i + 18 + plen] != 0xBB) {
                continue;
            }

            const uint8_t *payload = buf + i + 18;

            if (buf[i + 5] == 0x05 || (buf[i + 5] == 0x03 && buf[i + 8] == 0x06)) {
                for (int k = 0; k < plen && g_chiplog_n < (int)sizeof(g_chiplog) - 1; k++) {
                    char ch = buf[i + 18 + k];

                    if (ch == '\n' || ch == '\r') {
                        if (g_chiplog_n) {
                            g_chiplog[g_chiplog_n] = 0;
                            /* ch05 chip log is a verbose RF-debug stream (a down link spews it fast
                             * enough to fill the log tmpfs), so forward it only under -v. */
                            if (g_verbose) {
                                printf("[chip] %s\n", g_chiplog);
                            }
                            g_chiplog_n = 0;
                        }
                    } else if (ch >= 32 && ch < 127) {
                        g_chiplog[g_chiplog_n++] = ch;
                    }
                }
                fflush(stdout);
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_1V1INFO) {
                rx_chan_on_1v1(payload, plen);
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_PAIR) {
                rx_bind_on_pair(payload, plen);
            } else if (buf[i + 5] == REPLY_CH && buf[i + 8] == GET_SCAN_RESULT) {
                /* Seed the channel table for the sweep; --scan-probe also dumps the raw frame so the
                 * reply envelope can be re-checked on the bench (e.g. normal-mode layout). */
                if (g_scan_probe) {
                    hexdump_frame("get-scan reply", buf + i, 19 + plen);
                }

                rx_chan_on_scan_result(payload, plen);
            }

            i += 18 + plen;
        }
    }

    return NULL;
}
