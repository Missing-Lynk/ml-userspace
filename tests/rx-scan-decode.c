/**
 * @file rx-scan-decode.c
 * @brief Host test: decoding the chip's GetScanResult reply into the HUD's channel table.
 *
 * This decode turns a variable-length chip reply into a fixed 19-entry table that the HUD indexes
 * and that a channel select is issued from. It is all bounds arithmetic over attacker-adjacent
 * input: the count comes from one byte of the reply, the entry count is whatever the chip says, and
 * the valid-channel bitmap is read from the END of the payload rather than a fixed offset. A
 * mis-clamped count writes past a fixed-size array, and a wrong index reaches a SelectChn that
 * retunes the receiver away from the aircraft.
 *
 * The reply layouts here are SYNTHESISED from the offsets recorded in ml-rx-chan.c, not lifted from
 * a capture: no GetScanResult round-trip appears in the sessions under archive/out/rf-capture, only
 * Get1V1Info, GetTime and GetStatus. So this pins our decoder against our own reading of the
 * format, and would not catch a misreading of the format itself. It is the bounds behaviour that is
 * being asserted, not the field map. If a scan reply is ever captured, the full-table case below is
 * the one to re-seed from it.
 *
 * Order matters in main(): the decoder latches a "a table has landed" flag that never clears, so
 * the rejection cases have to run before the first accepted reply.
 */
#include "../ml-shared/mlm.h"
#include "../ml-linkd/ml-rx-chan.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Stubs for ml-linkd.c, ml-rx-udp.c and the bb socket. */
int g_verbose;
int g_scan_probe;
atomic_int g_hs_done;
atomic_int g_air_lost;

long now_ms(void)
{
    return 0;
}

int send_frame(const uint8_t *frame, int n, const char *tag)
{
    (void)frame;
    (void)n;
    (void)tag;

    return 0;
}

static struct mlm_scan stub_scan;
static int stub_publishes;

void mlm_pub(const char *path, uint16_t type, const void *payload, size_t n)
{
    (void)path;

    if (type == MLM_T_SCAN && n == sizeof stub_scan) {
        memcpy(&stub_scan, payload, sizeof stub_scan);
        stub_publishes++;
    }
}

static int g_failed;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failed++;
    }
}

/* A GetScanResult reply: the entry count in byte 0, the channel frequencies as little-endian u32
 * kHz from byte 4, and the valid-channel bitmap as the payload's trailing u32. GET_SCAN_RESULT's
 * table out_len is 264, which is the full-size case.
 */
#define SCAN_REPLY_LEN   264
#define SCAN_OFF_COUNT   0
#define SCAN_OFF_FREQ    4

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/** @brief Build a reply of @p plen bytes declaring @p count channels, with @p bmp as the mask. */
static int build_reply(uint8_t *out, int plen, uint8_t count, uint32_t bmp, uint32_t first_khz)
{
    memset(out, 0, (size_t)plen);
    out[SCAN_OFF_COUNT] = count;
    for (int i = 0; i < count; i++) {
        int foff = SCAN_OFF_FREQ + i * 4;

        if (foff + 4 > plen) {
            break;
        }
        put_u32(out + foff, first_khz + (uint32_t)i * 20000u);
    }
    put_u32(out + plen - 4, bmp);

    return plen;
}

/** @brief Replies the decoder must refuse outright. Runs first: the ready flag never clears. */
static void check_rejects_before_any_table(void)
{
    uint8_t reply[SCAN_REPLY_LEN];

    printf("  -- rejected replies --\n");
    check(stub_publishes == 0, "nothing is published before any reply has landed");

    build_reply(reply, SCAN_REPLY_LEN, 19, 0x7ffff, 5180000);
    rx_chan_on_scan_result(reply, SCAN_OFF_FREQ + 3);
    rx_chan_table_publish();
    check(stub_publishes == 0, "a reply too short to hold one frequency publishes no table");
    check(rx_chan_valid_bmp() == 0, "and leaves the valid mask unread, so selects stay permitted");
}

/** @brief A full-size reply decodes field for field. */
static void check_full_table(void)
{
    uint8_t reply[SCAN_REPLY_LEN];
    const uint32_t bmp = 0x0005a5a5;
    int ok_freq = 1;
    int ok_index = 1;
    int ok_valid = 1;
    int ok_snr = 1;

    printf("  -- a full table --\n");
    build_reply(reply, SCAN_REPLY_LEN, MLM_SCAN_MAX_CH, bmp, 5180000);
    rx_chan_on_scan_result(reply, SCAN_REPLY_LEN);
    rx_chan_table_publish();

    check(stub_publishes == 1, "a well-formed reply publishes one table");
    check(stub_scan.count == MLM_SCAN_MAX_CH, "all 19 entries are decoded");
    check(stub_scan.valid_bmp == bmp, "the valid mask is the payload's trailing u32");
    check(rx_chan_valid_bmp() == bmp, "and is readable by the select gate on the UDP thread");
    check(stub_scan.active_idx == 0xff,
          "with no channel tuned the active index is 0xff, which highlights nothing");
    check(stub_scan.measured == 0, "a table publish is not a measured sweep");

    for (int i = 0; i < MLM_SCAN_MAX_CH; i++) {
        if (stub_scan.chan[i].freq_mhz != 5180 + i * 20) {
            ok_freq = 0;
        }
        if (stub_scan.chan[i].index != i) {
            ok_index = 0;
        }
        if (stub_scan.chan[i].valid != ((bmp >> i) & 1)) {
            ok_valid = 0;
        }
        if (stub_scan.chan[i].snr_db != MLM_SCAN_SIGNAL_NONE
            || stub_scan.chan[i].snr_raw != MLM_SCAN_RAW_NONE) {
            ok_snr = 0;
        }
    }
    check(ok_freq, "each frequency converts from kHz to MHz");
    check(ok_index, "each entry carries its own table index, which is what a select is issued with");
    check(ok_valid, "each entry's valid flag is its bit in the mask");

    /* The reply carries no per-channel SNR; the sweep measures it later by visiting each channel.
     * Decoding anything into these would put a number on the HUD that no measurement produced.
     */
    check(ok_snr, "no entry claims an SNR: the reply carries none");
}

/** @brief The bounds: a lying count, a truncated table, junk frequencies. */
static void check_bounds(void)
{
    uint8_t reply[SCAN_REPLY_LEN];
    int publishes;

    printf("  -- bounds --\n");

    /* The count is one unvalidated byte of the reply and the destination array is fixed at 19. */
    build_reply(reply, SCAN_REPLY_LEN, 200, 0xffffffff, 5180000);
    reply[SCAN_OFF_COUNT] = 200;
    rx_chan_on_scan_result(reply, SCAN_REPLY_LEN);
    rx_chan_table_publish();
    check(stub_scan.count == MLM_SCAN_MAX_CH,
          "a count of 200 is clamped to the 19 the table holds");

    /* A reply that stops mid-table: the loop must stop at the payload end, not at the count. At
     * plen 24 the frequency slots run 4..23, so entries 0..4 are covered and 5..18 are not. Entry 4
     * is not asserted here: its four bytes are also the payload's trailing u32, so the mask write
     * lands on top of it. That aliasing is the subject of the last case below.
     */
    build_reply(reply, 24, 19, 0xffffffff, 5180000);
    rx_chan_on_scan_result(reply, 24);
    rx_chan_table_publish();
    check(stub_scan.count == 19, "a truncated reply still declares the count the chip sent");
    check(stub_scan.chan[3].freq_mhz == 5240, "entries the payload covers are decoded");
    check(stub_scan.chan[5].freq_mhz == 0 && stub_scan.chan[18].freq_mhz == 0,
          "entries past the payload end stay zero rather than reading past it");

    /* Out-of-band frequencies are zeroed but the entry survives, so the table stays index-aligned:
     * dropping the entry instead would shift every later channel's index by one.
     */
    build_reply(reply, SCAN_REPLY_LEN, 3, 0x7, 5180000);
    put_u32(reply + SCAN_OFF_FREQ + 4, 2400000);
    rx_chan_on_scan_result(reply, SCAN_REPLY_LEN);
    rx_chan_table_publish();
    check(stub_scan.chan[1].freq_mhz == 0, "a frequency outside the 5000..6100 MHz gate reads zero");
    check(stub_scan.chan[1].index == 1 && stub_scan.chan[2].freq_mhz == 5220,
          "but the entry keeps its index, so later channels do not shift");

    /* The minimum accepted length. The bitmap is read from the payload's LAST four bytes, so at
     * exactly this length it is the same four bytes as the first frequency. Asserted as the
     * boundary the current reading produces, not as a behaviour worth relying on: a real reply is
     * the table's 264 bytes, and this case only arises from a malformed one.
     */
    publishes = stub_publishes;
    build_reply(reply, SCAN_OFF_FREQ + 4, 1, 5180000, 0);
    rx_chan_on_scan_result(reply, SCAN_OFF_FREQ + 4);
    rx_chan_table_publish();
    check(stub_publishes == publishes + 1, "the shortest accepted reply is one frequency long");
    check(stub_scan.valid_bmp == 5180000 && stub_scan.chan[0].freq_mhz == 5180,
          "at that length one u32 is read as BOTH the mask and the first frequency");
}

int main(void)
{
    check_rejects_before_any_table();
    check_full_table();
    check_bounds();

    printf("%s\n", g_failed == 0 ? "rx-scan-decode: all checks passed"
                                 : "rx-scan-decode: FAILED");

    return g_failed == 0 ? 0 : 1;
}
