/*
 * ml-rx-chan.c - the RX role's RF channel.
 *
 * Owns everything the two channel-bearing GET replies carry: GetScanResult gives the band's channel
 * table and its valid mask, Get1V1Info gives the link measurements (SNR, distance, throughput) and
 * the working channel the per-channel sweep gates on. The channel the goggle is tuned to is tracked
 * here too, so every SelectChn goes out from one place.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#include "../ml-shared/mlm.h"
#include "bb-cmd.h"
#include "ml-linkd.h"
#include "ml-rx.h"
#include "ml-rx-chan.h"

/* Get1V1Info (0x73) carries a linear SNR at +0x06 (dB = 10*log10(raw/36); zero until a video link
 * is up) and the distance in metres at +0x08. */
#define V1V1_OFF_SNR     0x06

/* Get1V1Info +0x25 carries the chip's current working channel. The vendor's scan sweep clobbers this
 * byte to 0xff before each read and then refuses any reply whose +0x25 != the channel it just
 * selected (AR_MID_RX_WIRELESS_GET_SCAN_RESULT_IMPL, ar_lowdelay-full.txt:58393-58440). That gate
 * only makes sense because Get1V1Info keeps returning the PREVIOUS channel's cached snapshot for a
 * while after SelectChn - without it a sweep reads the active link's SNR on every channel.
 * The +0x25 = working-channel binding is inferred from that usage, not confirmed by a symbol. */
#define V1V1_OFF_CHAN    0x25

/* Get1V1Info +0x08 is the OSD distance: signed i32, metres, negative = no fix. The vendor's
 * AR_MID_GET_REALTIME_SYS_INFO (ar_lowdelay-full.txt:59482-59486) reads exactly this field,
 * clamps < 0 to 0 and truncates to u16; no scaling. GetDistanceResult (0x05) is NOT the source:
 * it has no call sites in the vendor stack and its u32 at +0 ticks at 1 kHz (a ms counter). */
#define V1V1_OFF_DIST    0x08

/* Get1V1Info +0x0c: measured PHY link throughput (rx_throughput), u32 LE kbps. This is link
 * capacity, NOT the encoder bitrate (HW-confirmed: constant ~20.9 Mbps while the encoded rate and
 * the netdev rate vary). The vendor OSD draws this same field as "Bitrate", dividing it by a
 * hardcoded 6 in standby - we publish it raw. Reads 0 until a video link is up, so it rides the
 * same raw>0 gate as SNR/distance. Offset HW-verified against a live 0x73 reply (be 51 = 20926). */
#define V1V1_OFF_THROUGHPUT 0x0c

/* Per-channel SNR sweep timings, from the vendor sweep (ar_lowdelay-full.txt:58393-58440). Per
 * channel it polls Get1V1Info every 10 ms until the reply's working channel matches (500 ms budget,
 * "timeout 500ms" in its own log), then settles 50 ms and takes one sample. A locked channel matches
 * almost at once, so the common cost is ~1 poll + the dwell; only dead channels burn the full
 * budget. Worst case over the 16 Race channels is ~8 s, which EXCEEDS AIR_LOSS_MS - the sweep blocks
 * the STEADY cadence, so a fully dead band re-runs the handshake afterwards. */
#define SWEEP_DWELL_MS   50               /* settle after the channel gate matches, before the sample */
#define SWEEP_LOCK_MS    500              /* budget for the reply's working channel to match */
#define SWEEP_GATE_US    10000            /* spacing between gate polls */
#define SWEEP_REPLY_MS   40               /* max wait for a single Get1V1Info reply */
#define SWEEP_SCAN_MS    300              /* max wait for the GetScanResult that seeds the table */

/* A first sample below this is taken again once. The value is the vendor's bucket-2 floor (raw 160,
 * ~6.5 dB): the level below which a channel has no usable link, so a healthy channel never pays the
 * extra reply. */
#define SWEEP_RETRY_RAW  160

/* GetScanResult raw reply layout (struct-of-arrays). count at [0]; freq[] u32 LE kHz at [4] (the full
 * channel table, up to 19); the valid-channel bitmap is the trailing u32 of the payload. Captured in
 * race mode (16-channel); the normal-mode payload layout is unverified.
 *
 * SCAN_OFF_SIGNAL is the reply's rssi[] (s32 LE, packed over the valid channels in table order):
 * ambient energy, not link quality. Kept as reference only - HW showed it pinned near the noise floor
 * (-104..-108 dBm) and unmoved by a locked air unit on the channel, so the tiles use the Get1V1Info
 * sweep instead. The array at [80] is always zero even with a live link. */
#define SCAN_OFF_COUNT   0
#define SCAN_OFF_FREQ    4
#define SCAN_OFF_SIGNAL  144
#define SCAN_FREQ_MHZ_MIN 5000        /* freq sanity gate (reject crossed-transaction junk) */
#define SCAN_FREQ_MHZ_MAX 6100

/* Local baseband link metrics, parsed from the GET replies in the reader thread and published for
 * the HUD's System OSD. MLM_LINKINFO_NONE until a reply lands. */
static atomic_int g_snr_db = MLM_LINKINFO_NONE;
static atomic_int g_distance_m = MLM_LINKINFO_NONE;
static atomic_int g_throughput_kbps;        /* measured PHY link throughput (Get1V1Info +0x0c); 0 = no link */

/* Raw Get1V1Info sampling for the channel sweep. g_snr_db deliberately holds the last GOOD value
 * (raw 0 replies do not overwrite it, so the OSD does not flicker), which makes it unusable for the
 * sweep: a dead channel would silently inherit the previous channel's SNR. g_v1v1_seq increments on
 * EVERY reply including raw 0, so the sweep can wait for a fresh sample and tell "no lock" (raw 0)
 * apart from "no reply at all". Single writer (reader), single reader (main sweep). */
static atomic_uint g_v1v1_seq;              /* bumped on every Get1V1Info reply */
static atomic_int g_v1v1_raw;               /* raw linear SNR of the last reply (0 = no lock) */
static atomic_int g_v1v1_chan = -1;         /* working channel (+0x25) of the last reply, -1 = absent */

/* Last Get1V1Info reply payload, for --scan-probe. The +0x25 working-channel offset the sweep gates
 * on is INFERRED, so the sweep dumps one reply per run: on a live link the byte holding the current
 * channel index must equal the tuned channel, which locates it without trusting the inference. */
#define V1V1_PAY_MAX     64
static uint8_t g_v1v1_pay[V1V1_PAY_MAX];
static atomic_int g_v1v1_plen;

/* The parsed channel table, owned by the main STEADY thread. The reader parses a GetScanResult reply
 * into it and sets g_scan_ready; the sweep then fills in snr_db per channel and publishes it. Not
 * published from the reader: the SNR only exists after the main thread has visited each channel. */
static struct mlm_scan g_scan;
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_scan_ready;             /* a GetScanResult reply has landed in g_scan */

/* The current band's valid-channel mask (the config JSON's chan_valid_bmp, echoed by the chip in
 * every GetScanResult reply). Its own global rather than a read of g_scan: the select gate reads it
 * off the UDP thread, and g_scan belongs to the main thread. 0 = not read back yet, in which case
 * the band is unknown and selects are allowed through rather than all rejected. */
static atomic_uint g_valid_bmp;

/* Channel select: the HUD queues a retune here (UDP thread), the bb-socket owner (main STEADY loop)
 * issues it once and clears it back to -1. One-shot, NOT a latch: re-issuing SelectChn on a cadence
 * would retune the RX continuously. The select must come from the bb-socket TX thread only; issuing
 * it from the UDP thread would race the steady poll and get lost (RE of the stock picker). */
static atomic_int g_pending_chnidx = -1;    /* HUD-requested channel table index (0..18), -1 = none */
/* Channel the local RX is tuned to; tracks every SelectChn we issue, for the OSD and the sweep's
 * restore. -1 until rx_chan_open has read the band and chosen one, which is also the state left
 * behind when GetScanResult does not answer: we have set no channel, so we do not claim one. */
static atomic_int g_cur_chnidx = -1;
static atomic_int g_pending_scan;           /* HUD requested a one-shot scan (MLM_RF_SCAN); STEADY fires it */

/* mlm_scan.active_idx is a u8 and the HUD highlights the tile whose index equals it. No table index
 * is 0xff, so that is the encoding for "no channel set yet" and the grid highlights nothing. */
#define ACTIVE_IDX_NONE  0xff

static uint8_t active_idx(void)
{
    int cur_chnidx = atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed);

    return cur_chnidx >= 0 ? (uint8_t)cur_chnidx : ACTIVE_IDX_NONE;
}

int rx_chan_index(void)
{
    int cur_chnidx = atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed);

    return cur_chnidx >= 0 ? cur_chnidx : MLM_LINKINFO_NONE;
}

uint32_t rx_chan_valid_bmp(void)
{
    return atomic_load_explicit(&g_valid_bmp, memory_order_relaxed);
}

int rx_chan_snr_db(void)
{
    return atomic_load_explicit(&g_snr_db, memory_order_relaxed);
}

int rx_chan_distance_m(void)
{
    return atomic_load_explicit(&g_distance_m, memory_order_relaxed);
}

int rx_chan_throughput_kbps(void)
{
    return atomic_load_explicit(&g_throughput_kbps, memory_order_relaxed);
}

/* Parse a raw GetScanResult reply (struct-of-arrays, HW-decoded) into g_scan for the sweep. The chip
 * returns the full channel table (freq[]) regardless of Standard/Race mode; the trailing bitmap is
 * the current mode's valid mask. snr_db is left unset here: the reply carries no per-channel SNR
 * (its rssi[] at SCAN_OFF_SIGNAL is ambient energy that did not move even with a locked air unit on
 * the channel, so it cannot drive the tiles), and sweep_run() measures it per channel instead. */
void rx_chan_on_scan_result(const uint8_t *payload, int plen)
{
    struct mlm_scan scan;
    uint32_t bmp;
    int count;

    if (plen < SCAN_OFF_FREQ + 4) {
        return;
    }

    count = payload[SCAN_OFF_COUNT];
    if (count > MLM_SCAN_MAX_CH) {
        count = MLM_SCAN_MAX_CH;
    }

    /* the valid-channel bitmap is the trailing u32 of the payload */
    bmp = (uint32_t)payload[plen - 4] | ((uint32_t)payload[plen - 3] << 8)
        | ((uint32_t)payload[plen - 2] << 16) | ((uint32_t)payload[plen - 1] << 24);

    memset(&scan, 0, sizeof scan);
    scan.valid_bmp = bmp;
    atomic_store_explicit(&g_valid_bmp, bmp, memory_order_relaxed);
    scan.count = (uint8_t)count;
    scan.active_idx = active_idx();

    for (int i = 0; i < count; i++) {
        int foff = SCAN_OFF_FREQ + i * 4;
        uint32_t fkhz;
        uint16_t mhz;
        int valid;

        if (foff + 4 > plen) {
            break;
        }

        fkhz = (uint32_t)payload[foff] | ((uint32_t)payload[foff + 1] << 8)
             | ((uint32_t)payload[foff + 2] << 16) | ((uint32_t)payload[foff + 3] << 24);
        mhz = (uint16_t)(fkhz / 1000);
        valid = (bmp >> i) & 1;

        scan.chan[i].freq_mhz = (mhz >= SCAN_FREQ_MHZ_MIN && mhz <= SCAN_FREQ_MHZ_MAX) ? mhz : 0;
        scan.chan[i].index = (uint8_t)i;
        scan.chan[i].valid = (uint8_t)valid;
        scan.chan[i].snr_db = MLM_SCAN_SIGNAL_NONE;
        scan.chan[i].snr_raw = MLM_SCAN_RAW_NONE;
    }

    pthread_mutex_lock(&g_scan_lock);
    g_scan = scan;
    pthread_mutex_unlock(&g_scan_lock);
    atomic_store_explicit(&g_scan_ready, 1, memory_order_release);
    if (g_verbose) {
        printf(TAG " scan: count=%d valid_bmp=0x%08x\n", count, bmp);
        fflush(stdout);
    }
}

/* Get1V1Info reply: linear SNR at +0x06 -> dB. Empty inter-poll replies (raw 0, seen when video is
 * idle or during a brief Tx-link blip) keep the last good value so the OSD does not flicker to No
 * Link; a real link loss blanks it via the air_lost gate. */
void rx_chan_on_1v1(const uint8_t *payload, int plen)
{
    unsigned raw;
    int kept;

    if (plen < V1V1_OFF_SNR + 2) {
        return;
    }

    raw = payload[V1V1_OFF_SNR] | (payload[V1V1_OFF_SNR + 1] << 8);

    /* publish every sample (raw 0 included) for the sweep before the last-good filter, with the
     * working channel the sample belongs to so the sweep can reject stale ones */
    kept = plen > V1V1_PAY_MAX ? V1V1_PAY_MAX : plen;

    atomic_store_explicit(&g_v1v1_raw, (int)raw, memory_order_relaxed);
    atomic_store_explicit(&g_v1v1_chan,
                          plen > V1V1_OFF_CHAN ? (int)payload[V1V1_OFF_CHAN] : -1,
                          memory_order_relaxed);
    memcpy(g_v1v1_pay, payload, (size_t)kept);
    atomic_store_explicit(&g_v1v1_plen, kept, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_v1v1_seq, 1, memory_order_release);

    if (raw > 0) {
        atomic_store_explicit(&g_snr_db, (int)lroundf(10.0f * log10f((float)raw / 36.0f)),
                              memory_order_relaxed);

        /* distance rides the same populated reply: i32 at +0x08, metres, vendor-style clamp of
         * negative (no fix) to 0. Empty inter-poll replies keep the last good value, same as SNR. */
        if (plen >= V1V1_OFF_DIST + 4) {
            int32_t dist = (int32_t)((uint32_t)payload[V1V1_OFF_DIST]
                         | ((uint32_t)payload[V1V1_OFF_DIST + 1] << 8)
                         | ((uint32_t)payload[V1V1_OFF_DIST + 2] << 16)
                         | ((uint32_t)payload[V1V1_OFF_DIST + 3] << 24));

            atomic_store_explicit(&g_distance_m, dist < 0 ? 0 : (int)dist,
                                  memory_order_relaxed);
        }

        /* measured PHY link throughput rides the same populated reply: u32 kbps at +0x0c. Empty
         * inter-poll replies keep the last good value, same as SNR. */
        if (plen >= V1V1_OFF_THROUGHPUT + 4) {
            atomic_store_explicit(&g_throughput_kbps,
                                  (int)((uint32_t)payload[V1V1_OFF_THROUGHPUT]
                                  | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 1] << 8)
                                  | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 2] << 16)
                                  | ((uint32_t)payload[V1V1_OFF_THROUGHPUT + 3] << 24)),
                                  memory_order_relaxed);
        }
    }

    if (g_verbose) {
        printf(TAG " 1v1info snr_raw=%u snr_db=%d dist_m=%d thr_kbps=%d\n", raw,
               atomic_load_explicit(&g_snr_db, memory_order_relaxed),
               atomic_load_explicit(&g_distance_m, memory_order_relaxed),
               atomic_load_explicit(&g_throughput_kbps, memory_order_relaxed));
        fflush(stdout);
    }
}

/* Publish the cached channel table (measured = 0) so the HUD's channel grid shows every channel
 * without a sweep. Carries whatever SNRs g_scan holds - after a sweep those are the measured readings
 * - but never the measured=1 edge, which sweep_run emits exactly once on completion so the HUD's
 * "scanning" spinner clears only on a real sweep, not on one of these table publishes. A no-op until
 * the table has been seeded once. Same STEADY thread as sweep_run, so it never runs mid-sweep. */
void rx_chan_table_publish(void)
{
    struct mlm_scan scan;

    if (!atomic_load_explicit(&g_scan_ready, memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&g_scan_lock);
    g_scan.active_idx = active_idx();
    g_scan.measured = 0;
    scan = g_scan;
    pthread_mutex_unlock(&g_scan_lock);

    mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_SCAN, &scan, sizeof scan);
}

/* Publish a completed sweep: active_idx = @p restore, measured = 1 for this one publish only (a
 * one-shot edge, so the cadence's table publishes stay measured=0 and never clear the HUD spinner). */
static void scan_publish_swept(int restore)
{
    struct mlm_scan scan;

    pthread_mutex_lock(&g_scan_lock);
    g_scan.active_idx = restore >= 0 ? (uint8_t)restore : ACTIVE_IDX_NONE;
    g_scan.measured = 1;
    scan = g_scan;
    g_scan.measured = 0;
    pthread_mutex_unlock(&g_scan_lock);

    mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_SCAN, &scan, sizeof scan);
}

/* @return the lowest channel index set in @bmp, or -1 if none is. */
static int first_valid_idx(uint32_t bmp)
{
    for (int i = 0; i < MLM_SCAN_MAX_CH; i++) {
        if ((bmp >> i) & 1) {
            return i;
        }
    }

    return -1;
}

/* Parse ML_OPEN_CHNIDX (the saved open channel ml-video read from the rf-channel marker). Returns the
 * index, or -1 when it is unset, empty, or malformed so the open falls back to the band's first valid
 * channel. */
static int read_open_chnidx(void)
{
    const char *value = getenv("ML_OPEN_CHNIDX");
    char *end;
    long idx;

    if (value == NULL || *value == '\0') {
        return -1;
    }

    idx = strtol(value, &end, 10);
    if (*end != '\0' || idx < 0 || idx >= MLM_SCAN_MAX_CH) {
        return -1;
    }

    return (int)idx;
}

/* Choose and set the RX's channel at OPEN, from the chip's own band.
 *
 * The band is only known from the chip - the config JSON's chan_valid_bmp, echoed in every
 * GetScanResult reply - and the two channel modes share no indices (race is 3..18, non-race 0..2),
 * so the table is read BEFORE the channel is set and the select is issued once, from the answer:
 * the saved channel (ML_OPEN_CHNIDX) when it is valid for this band, else the band's first valid
 * channel. Setting a fixed index first and correcting it afterwards, as the vendor's RX_Init does,
 * means a non-race chip is briefly tuned outside its own valid set.
 *
 * With no reply inside SWEEP_SCAN_MS the band is unknown and NO channel is set: the chip keeps
 * whatever its own config left it on, which beats a guess that is valid in only one of the two
 * modes. The HUD re-asserts its saved channel on every link-up edge, so that path recovers.
 *
 * Runs once during OPEN, before the air is associated: selecting here costs nothing, whereas doing
 * it later would drop a working link. Cheap - the raw GetScanResult does not sweep.
 *
 * Publishes the seeded table so the HUD's channel grid shows every channel from the start without a
 * sweep; the STEADY cadence keeps it fresh for a HUD that starts later.
 */
void rx_chan_open(uint8_t *frame, uint32_t *seq_link)
{
    int saved = read_open_chnidx();
    uint8_t poll[19];
    int target;

    atomic_store_explicit(&g_scan_ready, 0, memory_order_relaxed);
    bb_get(poll, GET_SCAN_RESULT, (*seq_link)++);
    send_frame(poll, 19, "get-scan");
    for (long t0 = now_ms();
         !atomic_load_explicit(&g_scan_ready, memory_order_acquire); ) {
        if (now_ms() - t0 >= SWEEP_SCAN_MS) {
            printf(TAG " band: no GetScanResult reply, no channel set\n");
            fflush(stdout);
            return;
        }

        usleep(5000);
    }

    pthread_mutex_lock(&g_scan_lock);
    uint32_t valid_bmp = g_scan.valid_bmp;
    pthread_mutex_unlock(&g_scan_lock);

    if (saved >= 0 && ((valid_bmp >> saved) & 1)) {
        target = saved;
    } else {
        target = first_valid_idx(valid_bmp);
    }

    if (target < 0) {
        printf(TAG " band: valid_bmp=0x%08x has no channel, none set\n", valid_bmp);
        fflush(stdout);

        return;
    }

    /* The fallback is operator-actionable (the saved channel no longer belongs to this band), so it
     * stays unconditional; opening on the expected channel is routine.
     */
    if (saved >= 0 && target != saved) {
        printf(TAG " band: saved ch%d outside valid_bmp=0x%08x, opening on first valid ch%d\n",
               saved, valid_bmp, target);
        fflush(stdout);
    } else if (g_verbose) {
        printf(TAG " band: opening on ch%d (valid_bmp=0x%08x)\n", target, valid_bmp);
        fflush(stdout);
    }

    send_frame(frame, bb_select_channel(frame, (uint8_t)target, (*seq_link)++), "open-chn");
    atomic_store_explicit(&g_cur_chnidx, target, memory_order_relaxed);
    usleep(OPEN_STEP_US);

    rx_chan_table_publish();   /* active_idx reflects the channel just chosen */
}

/* Send one Get1V1Info and wait for its reply. @return 1 on a fresh reply (g_v1v1_raw / g_v1v1_chan
 * updated), 0 on timeout. g_snr_db cannot serve here: it holds the last GOOD value by design, so it
 * cannot tell a dead channel from a missing reply. */
static int v1v1_poll(uint32_t *seq_link)
{
    uint8_t poll[19];
    unsigned seq0 = atomic_load_explicit(&g_v1v1_seq, memory_order_acquire);

    bb_get(poll, GET_1V1INFO, (*seq_link)++);
    send_frame(poll, 19, "sweep-1v1");

    for (long t0 = now_ms();
         atomic_load_explicit(&g_v1v1_seq, memory_order_acquire) == seq0; ) {
        if (now_ms() - t0 >= SWEEP_REPLY_MS) {
            return 0;
        }

        usleep(2000);
    }

    return 1;
}

/* Measure one swept channel's raw SNR, gating on the reply's working channel (+0x25) so the sample
 * belongs to @p idx and not to the channel we just left: poll every SWEEP_GATE_US until the reply's
 * channel matches (SWEEP_LOCK_MS budget), then settle SWEEP_DWELL_MS and sample. Without the gate the
 * chip's cached snapshot makes every channel read as the active link's SNR, saturating the top bucket
 * everywhere. The gate does not fully close the race, so a low sample is re-read once (below).
 *
 * @return the raw linear SNR (>0), MLM_SCAN_RAW_NOLOCK when the chip never reports the channel
 * (the vendor's strength-0 timeout case), or MLM_SCAN_RAW_NONE when Get1V1Info stops replying. */
static int sweep_measure(uint32_t *seq_link, int chnidx)
{
    long t0 = now_ms();

    while (now_ms() - t0 < SWEEP_LOCK_MS) {
        if (!v1v1_poll(seq_link)) {
            return MLM_SCAN_RAW_NONE;
        }

        if (atomic_load_explicit(&g_v1v1_chan, memory_order_relaxed) == chnidx) {
            int raw;

            usleep(SWEEP_DWELL_MS * 1000);
            if (!v1v1_poll(seq_link)) {
                return MLM_SCAN_RAW_NONE;
            }

            raw = atomic_load_explicit(&g_v1v1_raw, memory_order_relaxed);

            /* Re-read an implausibly low sample. The chip reports the new working channel at +0x25
             * before it has finished recomputing the SNR, so the gate does not fully close the race
             * and a single post-dwell read intermittently returns ~0 on a healthy channel (HW: ch9
             * read 5730, 5342, then 21 over three sweeps; the vendor, reading once after a bare
             * 50 ms, hits this ~5x more often and paints those tiles red). Keep the better of the
             * two: a genuinely dead channel reads ~0 twice, so this cannot mask one. */
            if (raw < SWEEP_RETRY_RAW) {
                usleep(SWEEP_DWELL_MS * 1000);
                int next_raw;

                if (v1v1_poll(seq_link)) {
                    next_raw = atomic_load_explicit(&g_v1v1_raw, memory_order_relaxed);
                    if (next_raw > raw) {
                        raw = next_raw;
                    }
                }
            }

            return raw;
        }

        usleep(SWEEP_GATE_US);
    }

    return MLM_SCAN_RAW_NOLOCK;
}

/* One full channel scan for the HUD: seed the table from GetScanResult, then measure each valid
 * channel's SNR and publish it as MLM_T_SCAN. The raw reply carries no per-channel SNR, so the only
 * source is Get1V1Info read while tuned to each channel - the vendor does the same (FUN_0045c108).
 * The air unit follows the retune over the chip-to-chip management link, so each reading is the link
 * SNR actually achievable on that channel; with no air unit up, every channel reads no-lock.
 *
 * Visits the current mode's valid channels starting at the active one and wrapping around, so the
 * active channel is measured while its link is still up and is the last one left tuned. Video is
 * interrupted for the length of the sweep and resumes when the active channel is restored, so this
 * must stay a one-shot on an explicit HUD request - never a cadence. Runs on the main STEADY thread,
 * the only bb-socket TX owner; issuing these selects from another thread would race the poll. */
static void sweep_run(uint8_t *frame, uint32_t *seq_link)
{
    uint8_t poll[19];
    int order[MLM_SCAN_MAX_CH];
    int n = 0;
    int restore = atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed);

    /* seed freq[] + the current mode's valid bitmap */
    atomic_store_explicit(&g_scan_ready, 0, memory_order_relaxed);
    bb_get(poll, GET_SCAN_RESULT, (*seq_link)++);
    send_frame(poll, 19, "get-scan");
    for (long t0 = now_ms();
         !atomic_load_explicit(&g_scan_ready, memory_order_acquire); ) {
        if (now_ms() - t0 >= SWEEP_SCAN_MS) {
            printf(TAG " scan: no GetScanResult reply, sweep skipped\n");
            fflush(stdout);
            return;
        }

        usleep(5000);
    }

    /* Locate the working-channel byte on the active channel before retuning anywhere: any offset
     * holding restore is a candidate for the gate, and V1V1_OFF_CHAN must be among them. On a dead
     * link the whole struct reads zero and this proves nothing, so the link state is printed too. */
    if (g_scan_probe && v1v1_poll(seq_link)) {
        int plen = atomic_load_explicit(&g_v1v1_plen, memory_order_relaxed);

        printf(TAG " scan: 1v1 on active ch%d (hs=%d air_lost=%d) plen=%d raw=%d\n",
               restore, rx_state_get(&g_hs_done), rx_state_get(&g_air_lost),
               plen, atomic_load_explicit(&g_v1v1_raw, memory_order_relaxed));
        for (int i = 0; i < plen; i += 16) {
            printf(TAG " scan: 1v1[%02x]", i);
            for (int k = 0; k < 16 && i + k < plen; k++) {
                printf(" %02x", g_v1v1_pay[i + k]);
            }

            printf("\n");
        }

        printf(TAG " scan: offsets holding the active channel (%d):", restore);
        for (int i = 0; i < plen; i++) {
            if (g_v1v1_pay[i] == (uint8_t)restore) {
                printf(" +0x%02x", i);
            }
        }

        printf("   (gate uses +0x%02x)\n", V1V1_OFF_CHAN);
        fflush(stdout);
    }

    /* Visit order: the valid channels, rotated to start at the active one. With no channel set yet
     * (rx_chan_open got no reply) there is nothing to rotate to or restore, so start at 0. */
    for (int k = 0; k < g_scan.count; k++) {
        int i = (restore >= 0 ? restore + k : k) % g_scan.count;

        if (g_scan.chan[i].valid) {
            order[n++] = i;
        }
    }

    /* Without an air unit every channel would gate-timeout to NOLOCK anyway, at SWEEP_LOCK_MS each:
     * over the 16 Race channels that is ~8 s of blocked STEADY cadence, past AIR_LOSS_MS, which
     * would fake an air loss and tear down the handshake. Publish the same all-NOLOCK answer at
     * once instead - the table and bitmap are already seeded, only the readings are missing. */
    if (!rx_state_get(&g_hs_done) || rx_state_get(&g_air_lost)) {
        for (int k = 0; k < n; k++) {
            g_scan.chan[order[k]].snr_raw = MLM_SCAN_RAW_NOLOCK;
        }

        scan_publish_swept(restore);
        printf(TAG " scan: no air unit (hs=%d air_lost=%d), %d channels reported unmeasured\n",
               rx_state_get(&g_hs_done), rx_state_get(&g_air_lost), n);
        fflush(stdout);

        return;
    }

    /* Force MCS 0 for the sweep, exactly as the vendor does (SetMcs(0) at ar_lowdelay-full.txt:58363,
     * SetMcsMode(1) restoring auto at :58503). This is not cosmetic: the vendor's buckets top out at
     * raw 1100 (~15 dB), yet under auto MCS the chip reports 4000-11500 on any healthy link, so every
     * tile saturates. The scan is the only place the vendor pins the rate, which is the one regime
     * difference that can explain the scale gap - plausibly because an SNR estimate read off the
     * coarse MCS-0 constellation cannot resolve high SNR and saturates into the bucketed range. */
    send_frame(frame, bb_set_mcs_mode(frame, MCS_MODE_MANUAL, (*seq_link)++), "sweep-mcs-manual");
    send_frame(frame, bb_set_mcs_value(frame, 0, (*seq_link)++), "sweep-mcs-0");

    for (int k = 0; k < n; k++) {
        int idx = order[k];
        int raw;

        /* the active channel is already tuned on the first visit */
        if (idx != atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed)) {
            send_frame(frame, bb_select_channel(frame, (uint8_t)idx, (*seq_link)++), "sweep-sel");
            atomic_store_explicit(&g_cur_chnidx, idx, memory_order_relaxed);
        }

        raw = sweep_measure(seq_link, idx);
        g_scan.chan[idx].snr_raw = (int16_t)raw;
        g_scan.chan[idx].snr_db = raw > 0
            ? (int16_t)lroundf(10.0f * log10f((float)raw / 36.0f))
            : MLM_SCAN_SIGNAL_NONE;

        /* One line per channel, -v: the sweep publishes its result to the HUD and reports the
         * summary below. chan is the reply's +0x25 working channel and must equal the swept index,
         * which is the check that the inferred +0x25 binding is real - if it never matches, every
         * channel times out to NOLOCK and the gate is keyed on the wrong byte. */
        if (g_verbose) {
            printf(TAG " scan: ch%-2d %u MHz raw=%-6d chan=%-3d snr=%d\n", idx,
                   g_scan.chan[idx].freq_mhz, raw,
                   atomic_load_explicit(&g_v1v1_chan, memory_order_relaxed),
                   g_scan.chan[idx].snr_db);
            fflush(stdout);
        }
    }

    /* restore the active channel and hand the rate back to the chip: the link and video resume here.
     * Auto MCS must be restored on every exit or the link stays pinned to MCS 0 after a scan. */
    if (restore >= 0 && atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed) != restore) {
        send_frame(frame, bb_select_channel(frame, (uint8_t)restore, (*seq_link)++), "sweep-restore");
        atomic_store_explicit(&g_cur_chnidx, restore, memory_order_relaxed);
    }

    send_frame(frame, bb_set_mcs_mode(frame, MCS_MODE_AUTO, (*seq_link)++), "sweep-mcs-auto");

    scan_publish_swept(restore);
    if (restore >= 0) {
        printf(TAG " scan: swept %d channels, active %d restored\n", n, restore);
    } else {
        printf(TAG " scan: swept %d channels, left on %d (none was set)\n", n,
               atomic_load_explicit(&g_cur_chnidx, memory_order_relaxed));
    }

    fflush(stdout);
}

/* Queue a HUD retune. The bound is the channel TABLE size: indices run 0..18, and Race's valid set
 * is 3..18, so a 0..15 bound would reject CH16/17/18 (5420/5380/5340 MHz).
 *
 * The band is enforced here rather than left to the chip: the chip accepts an index outside its own
 * chan_valid_bmp and simply tunes there (a Normal-band chip sat on Race ch5 until the band tune was
 * added). The HUD re-asserts a saved channel on every link-up, so without this a channel saved under
 * Race would retune a Normal-band chip straight back off its band. Rejecting leaves the band's first
 * valid channel in place. */
void rx_chan_request_select(unsigned chnidx)
{
    if (chnidx >= MLM_SCAN_MAX_CH) {
        fprintf(stderr, TAG " rfcmd: ignoring bad channel index %u\n", chnidx);
        return;
    }

    uint32_t valid_bmp = atomic_load_explicit(&g_valid_bmp, memory_order_relaxed);

    if (valid_bmp != 0 && !((valid_bmp >> chnidx) & 1)) {
        fprintf(stderr, TAG " rfcmd: channel %u outside band valid_bmp=0x%08x, ignoring\n",
                chnidx, valid_bmp);
        return;
    }

    /* the queued request is the step; rx_chan_service logs the select it actually issued */
    if (g_verbose) {
        printf(TAG " rfcmd: select channel %u\n", chnidx);
        fflush(stdout);
    }

    atomic_store_explicit(&g_pending_chnidx, (int)chnidx, memory_order_release);
}

/* Queue a one-shot sweep; read-only and self-restoring, so it needs no gate of its own. */
void rx_chan_request_scan(void)
{
    atomic_store_explicit(&g_pending_scan, 1, memory_order_release);
    if (g_verbose) {
        printf(TAG " rfcmd: scan requested\n");
        fflush(stdout);
    }
}

/* Run whatever the HUD has queued, from the bb-socket TX thread. The retune is issued once and
 * cleared; the sweep retunes across every valid channel and interrupts video for its duration, so
 * both stay request-driven and never ride the cadence. */
void rx_chan_service(uint8_t *frame, uint32_t *seq_link)
{
    int chnidx;

    /* Clear before sending so a request that arrives during the tune is not lost to the clear. The
     * tune is async and the air follows transparently; g_cur_chnidx tracks it so the published OSD
     * channel stays correct. */
    chnidx = atomic_exchange_explicit(&g_pending_chnidx, -1, memory_order_acquire);
    if (chnidx >= 0) {
        send_frame(frame, bb_select_channel(frame, (uint8_t)chnidx, (*seq_link)++), "select-chn");
        atomic_store_explicit(&g_cur_chnidx, chnidx, memory_order_relaxed);
        if (g_verbose) {
            printf(TAG " selected channel %d\n", chnidx);
            fflush(stdout);
        }
    }

    if (atomic_exchange_explicit(&g_pending_scan, 0, memory_order_acquire)) {
        sweep_run(frame, seq_link);
    }
}
