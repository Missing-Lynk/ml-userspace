/*
 * ml-msp-echo - FC UART receive test.
 *
 * Opens the air-unit FC UART as raw 115200 8N1 and writes every received byte to stdout.
 */
#include "../ml-linkd/ml-msp.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int g_running = 1;

static void on_sig(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv)
{
    const char *tty = ML_MSP_DEFAULT_TTY;
    struct ml_msp msp;
    uint8_t buf[256];

    if (argc == 2) {
        tty = argv[1];
    } else if (argc > 2) {
        fprintf(stderr, "usage: ml-msp-echo [/dev/ttyS1]\n");
        return 2;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    ml_msp_init(&msp, NULL, NULL);
    if (ml_msp_open(&msp, tty) != 0) {
        fprintf(stderr, "ml-msp-echo: open %s: %s\n", tty, strerror(errno));
        return 1;
    }

    while (g_running) {
        ssize_t n = read(msp.fd, buf, sizeof buf);

        if (n > 0) {
            ssize_t off = 0;

            while (off < n) {
                ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
                if (w < 0) {
                    ml_msp_close(&msp);
                    return 1;
                }

                off += w;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            fprintf(stderr, "ml-msp-echo: read %s: %s\n", tty, strerror(errno));
            ml_msp_close(&msp);
            return 1;
        } else {
            usleep(20000);
        }
    }

    ml_msp_close(&msp);
    return 0;
}
