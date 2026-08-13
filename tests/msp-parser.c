/**
 * @file msp-parser.c
 * @brief Host test: the FC UART's MSP v1 frame parser, fed malformed input.
 *
 * The flight controller's serial link is the only input in userspace that comes from third-party
 * firmware. Whatever Betaflight, INAV or a half-configured board actually puts on that wire lands
 * in a fixed 512-byte buffer and is indexed against a length byte the sender chose. msp-canvas-
 * roundtrip.c already covers the encoder, but it feeds frames it synthesised itself, so every
 * rejection path in the parser is unexercised by it: bad checksums, truncated frames, a length byte
 * that overruns what arrived, a DisplayPort record too short to hold its own header, and enough
 * records to fill the 384-byte accumulator.
 *
 * The parser is driven through a real file descriptor rather than by calling the internal scan
 * directly, because two of its behaviours only exist at that level: a frame split across two reads
 * has to be held and completed, and a buffer filled entirely with garbage has to drain rather than
 * wedge the link permanently.
 *
 * Failure here is not a crash in the ordinary case, it is a silent one: a blank OSD and a frozen
 * voltage in flight, which reads as a wiring fault.
 */
#include "../ml-linkd/ml-msp.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failed++;
    }
}

static int g_canvases;              /* canvases emitted through the callback */
static size_t g_canvas_len;         /* length of the last one */

static void on_canvas(const uint8_t *canvas, size_t len, void *ctx)
{
    (void)canvas;
    (void)ctx;
    g_canvases++;
    g_canvas_len = len;
}

/* The parser's end of a socketpair; the test writes FC bytes into the other end. Both ends are
 * non-blocking so the service loop's read drains to EAGAIN instead of blocking, which is how the
 * real UART fd is opened.
 */
static int g_fc;                    /* the test's end */

static void link_up(struct ml_msp *msp)
{
    int sv[2];

    ml_msp_init(msp, on_canvas, NULL);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("FAIL socketpair\n");
        g_failed++;

        return;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    msp->fd = sv[0];
    g_fc = sv[1];

    /* Park the poll cadence past every timestamp the test uses, so the only thing under test is
     * what arrives rather than what the parser asks for.
     */
    msp->next_status_ms = 1000000;
    msp->next_battery_ms = 1000000;
}

static void link_down(struct ml_msp *msp)
{
    close(msp->fd);
    close(g_fc);
    msp->fd = -1;
}

/** @brief Put @p n bytes on the wire and run one service pass. */
static void feed(struct ml_msp *msp, const uint8_t *bytes, size_t n, long now)
{
    uint8_t drain[64];

    if (n > 0 && write(g_fc, bytes, n) < 0) {
        printf("FAIL write to the fake UART\n");
        g_failed++;
    }
    ml_msp_service(msp, now);
    while (read(g_fc, drain, sizeof drain) > 0) {
        /* discard anything the parser requested, so the socket cannot fill */
    }
}

/** @brief Build one MSP v1 reply: '$' 'M' '>' len cmd payload... checksum. */
static size_t frame(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t checksum = len ^ cmd;

    out[0] = '$';
    out[1] = 'M';
    out[2] = '>';
    out[3] = len;
    out[4] = cmd;
    for (uint8_t i = 0; i < len; i++) {
        out[5 + i] = payload[i];
        checksum ^= payload[i];
    }
    out[5 + len] = checksum;

    return (size_t)len + 6;
}

#define MSP_STATUS         0x65
#define MSP_BATTERY_STATE  0x82
#define MSP_DISPLAYPORT    0xb6
#define DP_CLEAR_SCREEN    2
#define DP_WRITE_STRING    3
#define DP_DRAW_SCREEN     4

/** @brief Well-formed frames decode, so the malformed cases below mean something. */
static void check_good_frames(void)
{
    struct ml_msp msp;
    uint8_t buf[64];
    uint8_t status[7] = { 0 };
    uint8_t battery[11] = { 0 };
    size_t n;

    printf("  -- well-formed frames --\n");
    link_up(&msp);

    status[6] = 1;
    n = frame(buf, MSP_STATUS, status, sizeof status);
    feed(&msp, buf, n, 1000);
    check(msp.status.arm_flag == 1, "MSP_STATUS sets the arm flag from payload byte 6");
    check(msp.status.last_rx_ms == 1000, "and stamps the FC as heard from");

    /* Both battery fields are stored as tenths on the wire and scaled by 10 on the way in. */
    battery[4] = 0x2c;
    battery[5] = 0x01;      /* 300 -> 3000 */
    battery[9] = 0x9c;
    battery[10] = 0x00;     /* 156 -> 1560 mV */
    n = frame(buf, MSP_BATTERY_STATE, battery, sizeof battery);
    feed(&msp, buf, n, 2000);
    check(msp.status.mah_drawn_x10 == 3000, "MSP_BATTERY_STATE scales mAh drawn by ten");
    check(msp.status.voltage_mv == 1560, "and the voltage by ten");
    check(msp.status.last_battery_ms == 2000, "stamping the battery separately from the status");

    link_down(&msp);
}

/** @brief The length gates in front of each field read. */
static void check_short_payloads(void)
{
    struct ml_msp msp;
    uint8_t buf[64];
    uint8_t payload[10] = { 0 };
    size_t n;

    printf("  -- payloads too short for their fields --\n");
    link_up(&msp);

    /* A six-byte status has no byte 6. Reading it anyway would take the checksum as the arm flag. */
    payload[0] = 0xff;
    n = frame(buf, MSP_STATUS, payload, 6);
    feed(&msp, buf, n, 1000);
    check(msp.status.arm_flag == 0 && msp.status.last_rx_ms == 0,
          "a status frame one byte short is ignored, not read past its end");

    /* Ten bytes is one short of the voltage field at offset 9..10. */
    n = frame(buf, MSP_BATTERY_STATE, payload, 10);
    feed(&msp, buf, n, 2000);
    check(msp.status.voltage_mv == 0 && msp.status.last_battery_ms == 0,
          "a battery frame one byte short is ignored");

    /* An empty DisplayPort frame has no subcommand byte to switch on. */
    n = frame(buf, MSP_DISPLAYPORT, payload, 0);
    feed(&msp, buf, n, 3000);
    check(g_canvases == 0, "an empty DisplayPort frame emits nothing");

    link_down(&msp);
}

/** @brief Resynchronisation: garbage, bad checksums, and split frames. */
static void check_resync(void)
{
    struct ml_msp msp;
    uint8_t buf[128];
    uint8_t status[7] = { 0 };
    uint8_t junk[16];
    size_t n;

    printf("  -- resynchronisation --\n");
    link_up(&msp);

    /* Leading garbage is discarded a byte at a time until the preamble matches. */
    memset(junk, 0xa5, sizeof junk);
    junk[3] = '$';                  /* a false start, not followed by 'M' '>' */
    status[6] = 1;
    n = frame(buf, MSP_STATUS, status, sizeof status);
    feed(&msp, junk, sizeof junk, 1000);
    feed(&msp, buf, n, 1000);
    check(msp.status.arm_flag == 1, "a frame after leading garbage is still found");
    check(msp.rx_len == 0, "and the buffer is left empty once it is consumed");

    /* A corrupted checksum must not consume the frame as if it were good. */
    msp.status.arm_flag = 0;
    msp.status.last_rx_ms = 0;
    n = frame(buf, MSP_STATUS, status, sizeof status);
    buf[n - 1] ^= 0xff;
    feed(&msp, buf, n, 2000);
    check(msp.status.arm_flag == 0 && msp.status.last_rx_ms == 0,
          "a frame with a bad checksum is dropped");

    /* A frame arriving in two reads is held and completed rather than resynced away. */
    msp.rx_len = 0;
    n = frame(buf, MSP_STATUS, status, sizeof status);
    feed(&msp, buf, 5, 3000);
    check(msp.status.last_rx_ms == 0, "a frame split across reads is not acted on early");
    check(msp.rx_len == 5, "its bytes are held in the buffer");
    feed(&msp, buf + 5, n - 5, 3000);
    check(msp.status.arm_flag == 1 && msp.status.last_rx_ms == 3000,
          "and it decodes once the rest arrives");

    /* MSP v2. Neither Betaflight nor INAV should send it, and the parser is v1 only, so it must be
     * dropped rather than misread as a v1 frame with a wild length byte.
     */
    msp.status.last_rx_ms = 0;
    {
        uint8_t v2[] = { '$', 'X', '>', 0, 0x65, 0x00, 0x07, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };

        feed(&msp, v2, sizeof v2, 4000);
        check(msp.status.last_rx_ms == 0, "an MSP v2 frame is ignored");
    }

    link_down(&msp);
}

/** @brief The DisplayPort record accumulator and its bounds. */
static void check_displayport(void)
{
    struct ml_msp msp;
    uint8_t buf[128];
    uint8_t dp[32];
    size_t n;

    printf("  -- DisplayPort records --\n");
    link_up(&msp);
    g_canvases = 0;

    /* A write-string, then a draw-screen, is the normal sequence. */
    dp[0] = DP_WRITE_STRING;
    dp[1] = 5;                      /* row */
    dp[2] = 7;                      /* col */
    dp[3] = 0;                      /* attr */
    memcpy(dp + 4, "HELLO", 5);
    n = frame(buf, MSP_DISPLAYPORT, dp, 9);
    feed(&msp, buf, n, 1000);
    check(msp.records_len > 0, "a write-string is accumulated");

    dp[0] = DP_DRAW_SCREEN;
    n = frame(buf, MSP_DISPLAYPORT, dp, 1);
    feed(&msp, buf, n, 1000);
    check(g_canvases == 1, "a draw-screen emits one canvas");

    /* A record with no room for its own row/col/attr header. */
    msp.records_len = 0;
    dp[0] = DP_WRITE_STRING;
    n = frame(buf, MSP_DISPLAYPORT, dp, 3);
    feed(&msp, buf, n, 2000);
    check(msp.records_len == 0, "a write-string too short to hold its header is refused");

    /* Clear-screen drops what has accumulated. */
    dp[0] = DP_WRITE_STRING;
    memcpy(dp + 4, "XY", 2);
    n = frame(buf, MSP_DISPLAYPORT, dp, 6);
    feed(&msp, buf, n, 2000);
    check(msp.records_len > 0, "a record accumulates before the clear");
    dp[0] = DP_CLEAR_SCREEN;
    n = frame(buf, MSP_DISPLAYPORT, dp, 1);
    feed(&msp, buf, n, 2000);
    check(msp.records_len == 0, "clear-screen drops the accumulated records");

    /* More records than the 384-byte accumulator holds. The excess must be refused rather than
     * written past the end, and the canvas that follows must still be emitted from what fit.
     */
    g_canvases = 0;
    dp[0] = DP_WRITE_STRING;
    memcpy(dp + 4, "0123456789abcdef", 16);
    n = frame(buf, MSP_DISPLAYPORT, dp, 20);
    for (int i = 0; i < 60; i++) {
        dp[1] = (uint8_t)i;
        n = frame(buf, MSP_DISPLAYPORT, dp, 20);
        feed(&msp, buf, n, 3000);
    }
    check(msp.records_len <= sizeof msp.records,
          "the record accumulator never grows past its buffer");
    dp[0] = DP_DRAW_SCREEN;
    n = frame(buf, MSP_DISPLAYPORT, dp, 1);
    feed(&msp, buf, n, 3000);
    check(g_canvases == 1, "a canvas is still emitted after the accumulator filled");

    link_down(&msp);
}

/** @brief A receive buffer filled entirely with junk must drain, not wedge. */
static void check_buffer_never_wedges(void)
{
    struct ml_msp msp;
    uint8_t junk[256];
    uint8_t buf[64];
    uint8_t status[7] = { 0 };
    size_t n;

    printf("  -- the buffer drains under pure garbage --\n");
    link_up(&msp);

    /* 0x24 is '$', so this is the worst case: every byte starts a candidate frame that then fails
     * the preamble, which is what would pin a parser that only advances on success.
     */
    memset(junk, '$', sizeof junk);
    for (int i = 0; i < 8; i++) {
        feed(&msp, junk, sizeof junk, 1000);
    }
    check(msp.rx_len < sizeof msp.rx, "the buffer does not stay full of unparseable bytes");

    /* And the link recovers: a good frame after all that still decodes. */
    status[6] = 1;
    n = frame(buf, MSP_STATUS, status, sizeof status);
    feed(&msp, buf, n, 5000);
    check(msp.status.arm_flag == 1 && msp.status.last_rx_ms == 5000,
          "and a good frame after the flood still decodes");

    link_down(&msp);
}

int main(void)
{
    check_good_frames();
    check_short_payloads();
    check_resync();
    check_displayport();
    check_buffer_never_wedges();

    printf("%s\n", g_failed == 0 ? "msp-parser: all checks passed" : "msp-parser: FAILED");

    return g_failed == 0 ? 0 : 1;
}
