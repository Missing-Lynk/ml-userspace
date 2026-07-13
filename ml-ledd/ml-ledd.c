// ml-ledd - the status RGB LED indicator daemon.
//
// Owns the goggle's WS2812 status LED (a 3-pixel chain, all driven the same, on the DesignWare SPI
// master, /dev/spidev*.0, CS0; see kernel/docs/status-led.md). The WS2812 has no
// hardware brightness ramp, so this daemon renders the animation itself: it holds a
// current pattern (off/solid/breathe/blink) and repaints the LED on a fixed tick.
//
// It is a command sink, not a link-state mirror: it binds MLM_LED_SOCK and any producer
// sends it an MLM_T_LED record (last command wins). ml-linkd is the first producer
// (breathe red until video, solid green once the link is up). Because it only needs
// /run (mounted in sysinit) and /dev/spidev (built-in, present once devtmpfs mounts),
// it starts in the boot runlevel, earlier than every other daemon, so breathe-red is
// one of the first signs of life.
//
// Default at startup is breathe red, so the LED is meaningful before any command arrives.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>
#include <signal.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <linux/spi/spidev.h>

#include "../ml-shared/mlm.h"

#define NLED      3            // WS2812 pixels in the chain (all three populated, driven the same)
#define DATA      (NLED * 24)  // 72 SPI bytes, one per WS2812 data bit
#define NRESET    320          // trailing low bytes: ~410 us MOSI-low latch after each frame
                               // (WS2812B/SK6812 need >=280 us; the classic 50 us is not enough)
#define FRAME     (DATA + NRESET)
#define T0        0x80         // WS2812 "0" bit as one SPI byte at 6.25 MHz (~0.16 us high)
#define T1        0xFC         // WS2812 "1" bit as one SPI byte at 6.25 MHz (~0.96 us high)
#define SPEED     6250000      // 6.25 MHz, per status-led.md
#define TICK_MS   30           // ~33 Hz repaint; smooth breathe at negligible CPU

static volatile int g_run = 1;

static void on_sig(int s)
{
    (void)s;
    g_run = 0;
}

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// Encode one pixel into 24 SPI bytes: WS2812 wire order is G,R,B, MSB first.
static void encode_pixel(uint8_t *out, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };

    for (int c = 0; c < 3; c++) {
        for (int bit = 7; bit >= 0; bit--) {
            *out++ = ((grb[c] >> bit) & 1) ? T1 : T0;
        }
    }
}

// Build a full 72-byte frame with every pixel set to the same color.
static void encode_frame(uint8_t *frame, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < NLED; i++) {
        encode_pixel(frame + i * 24, r, g, b);
    }
}

static int spi_send(int fd, const uint8_t *frame)
{
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)frame,
        .len = FRAME,
        .speed_hz = SPEED,
        .bits_per_word = 8,
    };

    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}

// Open the status-LED spidev. The bus id is dynamically assigned (/dev/spidev32765.0,
// not a fixed index), so glob rather than hard-code; SPIDEV=/dev/spidevN.0 overrides.
// Retries briefly: at boot ml-ledd can race devtmpfs population.
static int open_spidev(void)
{
    const char *env = getenv("SPIDEV");

    for (int attempt = 0; attempt < 50 && g_run; attempt++) {
        char path[64];
        glob_t gl;

        path[0] = 0;
        if (env) {
            snprintf(path, sizeof path, "%s", env);
        } else if (glob("/dev/spidev*.0", 0, NULL, &gl) == 0 && gl.gl_pathc > 0) {
            snprintf(path, sizeof path, "%s", gl.gl_pathv[0]);
            globfree(&gl);
        }

        if (path[0]) {
            int fd = open(path, O_RDWR | O_CLOEXEC);

            if (fd >= 0) {
                uint8_t mode = SPI_MODE_0;
                uint8_t bits = 8;
                uint32_t speed = SPEED;

                ioctl(fd, SPI_IOC_WR_MODE, &mode);
                ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
                ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
                fprintf(stderr, "[ml-ledd] LED on %s @ %d Hz, %d pixels\n", path, SPEED, NLED);

                return fd;
            }
        }

        usleep(100000);
    }

    fprintf(stderr, "[ml-ledd] no /dev/spidev*.0 found; LED disabled\n");
    return -1;
}

// Bind the command socket. ml-ledd is the consumer, producers sendto() it. Best-effort:
// on failure the LED still animates its default, it just takes no commands.
static int open_cmd_sock(void)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (sock < 0) {
        return -1;
    }

    mkdir(MLM_RUN_DIR, 0755);
    unlink(MLM_LED_SOCK);
    strncpy(addr.sun_path, MLM_LED_SOCK, sizeof addr.sun_path - 1);
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[ml-ledd] bind %s: %s\n", MLM_LED_SOCK, strerror(errno));
        close(sock);

        return -1;
    }

    return sock;
}

// Scale a channel by a 0..255 brightness with a squared curve, so a breathe looks like
// an ease rather than a flat triangle. Integer only, no libm dependency.
static uint8_t dim(uint8_t v, int level)
{
    int eased = level * level / 255; // ease-in: emphasises the low end of the fade
    return (uint8_t)(v * eased / 255);
}

// Compute the frame for the current pattern at time now (ms).
static void render(uint8_t *frame, const struct mlm_led *led, long now)
{
    switch (led->mode) {
        case MLM_LED_SOLID: {
            encode_frame(frame, led->r, led->g, led->b);
        } break;

        case MLM_LED_BREATHE: {
            int period = led->period_ms ? led->period_ms : 2000;
            int phase = (int)(now % period);
            int half = period / 2;
            int level; // 0..255 triangle over the period, eased by dim()

            if (phase < half) {
                level = phase * 255 / half;
            } else {
                level = (period - phase) * 255 / half;
            }

            encode_frame(frame, dim(led->r, level), dim(led->g, level), dim(led->b, level));
        } break;

        case MLM_LED_BLINK: {
            int period = led->period_ms ? led->period_ms : 500;

            if ((now % period) < period / 2) {
                encode_frame(frame, led->r, led->g, led->b);
            } else {
                encode_frame(frame, 0, 0, 0);
            }
        } break;

        default: {
            // MLM_LED_OFF and anything unknown
            encode_frame(frame, 0, 0, 0);
        } break;
    }
}

int main(void)
{
    struct mlm_led led = { .mode = MLM_LED_BREATHE, .r = 0xff, .g = 0, .b = 0, .period_ms = 3000 };
    uint8_t frame[FRAME];
    uint8_t last_frame[FRAME];
    int have_last = 0;
    int spi;
    int sock;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    spi = open_spidev();
    if (spi < 0) {
        return 1;
    }

    sock = open_cmd_sock();

    // render() only fills the DATA region; zero the whole buffer once so the NRESET
    // trailing bytes stay low, giving every SPI transfer a >50 us reset that latches the
    // frame. Without this the WS2812 never latches on continuous refresh and the GRB bytes
    // shift across frame boundaries (LED flickers through random colours).
    memset(frame, 0, sizeof frame);

    while (g_run) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        long now;
        int ready = 0;

        // Wait one tick, waking early on a command. With no socket, just sleep the tick
        // so the default animation keeps running without spinning the CPU.
        if (sock >= 0) {
            ready = poll(&pfd, 1, TICK_MS) > 0 && (pfd.revents & POLLIN);
        } else {
            usleep(TICK_MS * 1000);
        }

        if (ready) {
            uint8_t rx[128];
            ssize_t n;

            // Drain the queue; the last valid command wins.
            while ((n = recv(sock, rx, sizeof rx, 0)) > 0) {
                struct mlm_hdr *h = (struct mlm_hdr *)rx;

                if ((size_t)n < sizeof *h + sizeof(struct mlm_led)) {
                    continue;
                }

                if (h->magic != MLM_MAGIC || h->type != MLM_T_LED) {
                    continue;
                }

                memcpy(&led, rx + sizeof *h, sizeof led);
                have_last = 0; // force a repaint on the new pattern
            }
        }

        now = now_ms();
        render(frame, &led, now);

        // Static patterns (solid/off) only need writing on change; animated ones repaint
        // every tick. This keeps a solid LED from re-driving the bus 33x a second.
        if (!have_last || memcmp(frame, last_frame, FRAME) != 0) {
            if (spi_send(spi, frame) < 0) {
                fprintf(stderr, "[ml-ledd] SPI write: %s\n", strerror(errno));
            }

            memcpy(last_frame, frame, FRAME);
            have_last = 1;
        }
    }

    // Leave the LED off on a clean exit so a stopped service is not misread as a live state.
    encode_frame(frame, 0, 0, 0);
    spi_send(spi, frame);
    if (sock >= 0) {
        close(sock);
        unlink(MLM_LED_SOCK);
    }
    close(spi);

    return 0;
}
