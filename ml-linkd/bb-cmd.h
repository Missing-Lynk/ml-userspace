/*
 * bb-cmd.h - named interface to the local AR8030 baseband chip.
 *
 * These builders emit the bb-socket control-plane frames written to /dev/artosyn_sdio; they
 * configure THIS unit's radio and are NOT commands to the air unit (air config rides UDP :10000,
 * see userspace/docs/rf-video-downlink.md).
 *
 * Each builder writes one frame into `frame` (which must hold the command's wire payload length + 19
 * bytes) and returns its length, so the caller owns transmission:
 *   send_frame(frame, bb_set_power(frame, RF_TX, 23, seq), "tx-power")
 * The GET/SET builders pack through bb_build_cmd, which pads each payload to the command's fixed wire
 * length from bb_cmd_lens (below); the bring-up/link frames use bb_build_frame directly.
 *
 * PROVEN builders are issued by ml-linkd's bring-up, byte-identical to the captured vendor frames.
 * DECODED builders have RE'd payloads but have NEVER been sent on the open stack; each one retunes
 * or repowers a live radio, so validate it on a RAM-boot before wiring it into any path.
 */
#ifndef BB_CMD_H
#define BB_CMD_H

#include <stdint.h>
#include <string.h>

/* frame class (channel byte). BB_GET/BB_SET/BB_LIFECYCLE are the config-command classes; the rest
 * are the link bring-up channels (association handshake, chip log, link-management pings).
 */
enum bb_class {
    BB_GET       = 0x01,   /* GET family: selector picks the value to read */
    BB_SET       = 0x02,   /* SET family: selector picks the setter */
    BB_ASSOC     = 0x03,   /* association / RPC control (opcode 0x01 = countdown) */
    BB_LOG       = 0x05,   /* chip-log channel (reader() drains it) */
    BB_LIFECYCLE = 0x0b,   /* bb_start / bb_stop / bb_init / bb_deinit */
    BB_LINK      = 0xff,   /* link-management pings */
};

/* GET selectors (channel BB_GET). ml-linkd's steady cadence polls GET_1V1INFO + GET_TIME. */
enum bb_get_sel {
    GET_STATUS        = 0x00,   /* GetStatus */
    GET_PAIR          = 0x01,   /* pair state: 98-byte reply, byte0 = candidate bitmask (bit n =
                                 * slot n), 4-byte AR8030 MAC per slot at 1 + slot*4, wire order */
    GET_CANDIDATES    = 0x03,   /* GetCandidates */
    GET_STATUS_PARAMS = 0x04,   /* GetStatusParams */
    GET_DISTANCE      = 0x05,   /* GetDistanceResult; reply u32 at +0 is a 1 kHz tick counter
                                 * (vendor-unused; OSD distance is Get1V1Info +0x08) */
    GET_MCS           = 0x06,   /* GetRxMcs / GetTxMcs / bitrate / framerate */
    GET_POWER         = 0x08,   /* GetPower */
    GET_SCAN_RESULT   = 0x0a,   /* GET_ScanResult */
    GET_TIME          = 0x0c,
    GET_1V1INFO       = 0x73,   /* Get1V1Info (link stats) */
};

/* SET selectors (channel BB_SET). */
enum bb_set_sel {
    SET_PAIR_MODE  = 0x02,
    SET_PAIR_LOCK  = 0x04,
    SET_CHNMODE    = 0x05,
    SET_CHNIDX     = 0x06,
    SET_POWER      = 0x08,
    SET_POWER_AUTO = 0x09,
    SET_MCS_MODE   = 0x0c,
    SET_MCS        = 0x0d,
    SET_BANDWIDTH  = 0x16,
};

/* SET_POWER direction: which chain of the local transceiver to set. The vendor exposes these as
 * two functions (TX_SET_POWER writes 0x00, RX_SET_POWER writes 0x08), same selector.
 */
enum rf_dir {
    RF_TX = 0x00,   /* transmit chain (uplink) */
    RF_RX = 0x08,   /* receive chain */
};

/* Pack one bb-socket frame:
 *   AA | plen(u16 LE) | 00 00 | channel | opcode | slot | port | seq(4 BE) | reserved(4, 0) |
 *   cksum = ~XOR(bytes[0..16]) | payload[plen] | BB
 * Returns the frame length (plen + 19).
 */
static inline int bb_build_frame(uint8_t *frame, uint8_t channel, uint8_t opcode, uint8_t slot,
                                 uint8_t port, uint32_t seq, const uint8_t *payload, int plen)
{
    uint8_t csum = 0;

    memset(frame, 0, 18);
    frame[0] = 0xaa;
    frame[1] = plen & 0xff;
    frame[2] = (plen >> 8) & 0xff;
    frame[5] = channel;
    frame[6] = opcode;
    frame[7] = slot;
    frame[8] = port;
    frame[9] = seq >> 24;
    frame[10] = seq >> 16;
    frame[11] = seq >> 8;
    frame[12] = seq;
    for (int i = 0; i < 17; i++) {
        csum ^= frame[i];
    }
    frame[17] = (uint8_t)~csum;
    if (plen > 0) {
        memcpy(frame + 18, payload, plen);
    }
    frame[18 + plen] = 0xbb;

    return plen + 19;
}

/* The vendor's full bb_ioctl command-length table (get_bb_ioctl_cmdiptlen @0x4afa40 -> the 92-entry
 * {code, in_len, out_len} array @0x4eb480 in ar_lowdelay), transcribed straight from the binary.
 * bb_ioctl stamps a command's `in_len` on the wire regardless of how many bytes the caller filled
 * in, zero-padding the rest; a short frame is malformed and can crash the firmware. bb_build_cmd()
 * looks the length up so a builder supplies only its meaningful bytes and never hardcodes the pad.
 *
 * Keyed by (cls, selector) - the selector alone is ambiguous (0x06 is GET_MCS or SET_CHNIDX).
 * Selectors we have mapped use their enum name; the rest keep the raw selector, and the classes
 * with no enum (0x00 init, 0x06 config blob, 0x0a) keep the raw channel byte. `out_len` is the
 * reply payload length (reference; bb_build_cmd only consumes in_len). Sorted by (cls, selector).
 * When a new command is mapped, name its selector in the enums above and this row picks it up. */
struct bb_cmd_len {
    uint8_t cls;      /* enum bb_class, or the raw channel byte for classes with no enum */
    uint8_t selector; /* enum bb_get_sel / bb_set_sel where mapped, else the raw selector */
    uint16_t in_len;  /* host->chip payload bytes on the wire */
    uint16_t out_len; /* chip->host reply payload bytes (reference) */
};

static const struct bb_cmd_len bb_cmd_lens[] = {
    { 0x00        , 0x02             ,  312,    0 },
    { BB_GET      , GET_STATUS       ,    2,  300 },
    { BB_GET      , GET_PAIR         ,    0,   98 },
    { BB_GET      , 0x02             ,    0,    4 },
    { BB_GET      , GET_CANDIDATES   ,    1,   21 },
    { BB_GET      , GET_STATUS_PARAMS,    4,   80 },
    { BB_GET      , GET_DISTANCE     ,    1,   32 },
    { BB_GET      , GET_MCS          ,    2,    8 },
    { BB_GET      , 0x07             ,    0,    1 },
    { BB_GET      , GET_POWER        ,    1,    2 },
    { BB_GET      , 0x09             ,    0,    1 },
    { BB_GET      , GET_SCAN_RESULT  ,    0,  264 },
    { BB_GET      , 0x0b             ,    2,   64 },
    { BB_GET      , GET_TIME         ,    0,    4 },
    { BB_GET      , 0x0d             ,   32,   32 },
    { BB_GET      , 0x0e             ,    8,  132 },
    { BB_GET      , 0x0f             ,    0,    4 },
    { BB_GET      , 0x10             ,    4,  648 },
    { BB_GET      , 0x11             ,    4,   16 },
    { BB_GET      , 0x64             ,    4,  256 },
    { BB_GET      , 0x65             ,    8, 1024 },
    { BB_GET      , 0x66             ,    0,    1 },
    { BB_GET      , 0x69             ,    0,  136 },
    { BB_GET      , 0x6a             ,    1,  128 },
    { BB_GET      , 0x6b             ,    4,  208 },
    { BB_GET      , 0x6c             ,    4,    1 },
    { BB_GET      , 0x6d             ,    4,    1 },
    { BB_GET      , 0x6e             ,    0,    1 },
    { BB_GET      , 0x6f             ,    0,    1 },
    { BB_GET      , 0x70             ,    0,    4 },
    { BB_GET      , 0x71             ,    4,    4 },
    { BB_GET      , 0x72             ,    0,    8 },
    { BB_GET      , GET_1V1INFO      ,    0,   44 },
    { BB_GET      , 0xc8             ,  256,  256 },
    { BB_SET      , SET_PAIR_MODE    ,   14,    0 },
    { BB_SET      , 0x03             ,    4,    0 },
    { BB_SET      , SET_PAIR_LOCK    ,   22,    0 },
    { BB_SET      , SET_CHNMODE      ,    1,    0 },
    { BB_SET      , SET_CHNIDX       ,    2,    0 },
    { BB_SET      , 0x07             ,    1,    0 },
    { BB_SET      , SET_POWER        ,    2,    0 },
    { BB_SET      , SET_POWER_AUTO   ,    1,    0 },
    { BB_SET      , 0x0a             , 1024,    4 },
    { BB_SET      , 0x0b             ,   16,    4 },
    { BB_SET      , SET_MCS_MODE     ,    2,    0 },
    { BB_SET      , SET_MCS          ,    2,    0 },
    { BB_SET      , 0x0e             ,    4,    0 },
    { BB_SET      , 0x0f             ,    1,    0 },
    { BB_SET      , 0x10             ,    1,    0 },
    { BB_SET      , 0x11             ,    1,    0 },
    { BB_SET      , 0x12             ,    1,    0 },
    { BB_SET      , 0x13             ,    1,    0 },
    { BB_SET      , 0x14             ,    0,    0 },
    { BB_SET      , 0x15             ,  136,    0 },
    { BB_SET      , SET_BANDWIDTH    ,    3,    0 },
    { BB_SET      , 0x17             ,   20,    0 },
    { BB_SET      , 0x18             ,    3,    0 },
    { BB_SET      , 0x19             ,    1,    0 },
    { BB_SET      , 0x1a             ,    1,    0 },
    { BB_SET      , 0x1b             ,    4,    0 },
    { BB_SET      , 0x1c             ,    1,    0 },
    { BB_SET      , 0x1d             ,    1,    0 },
    { BB_SET      , 0x1e             ,   32,    0 },
    { BB_SET      , 0x1f             ,    2,    0 },
    { BB_SET      , 0x20             ,    2,    0 },
    { BB_SET      , 0x21             ,    4,    0 },
    { BB_SET      , 0x22             ,   10,    0 },
    { BB_SET      , 0x64             ,  260,    0 },
    { BB_SET      , 0x65             , 1024,    0 },
    { BB_SET      , 0x66             ,    0,    0 },
    { BB_SET      , 0x67             ,    3,    0 },
    { BB_SET      , 0x68             ,    1,    0 },
    { BB_SET      , 0x69             ,    8,    0 },
    { BB_SET      , 0x6a             ,    2,    0 },
    { BB_SET      , 0x6b             ,    2,    0 },
    { BB_SET      , 0x6c             ,    1,    0 },
    { BB_SET      , 0x6d             ,    1,    0 },
    { BB_SET      , 0x6e             ,    4,    0 },
    { BB_SET      , 0x6f             ,    0,    0 },
    { BB_SET      , 0x70             ,    0,    0 },
    { BB_SET      , 0x72             ,    0,    0 },
    { BB_SET      , 0x73             ,    0,    0 },
    { BB_SET      , 0xc8             ,  256,    0 },
    { BB_SET      , 0xc9             , 1024,    0 },
    { BB_SET      , 0xca             , 1024,    0 },
    { BB_SET      , 0xcb             , 1024,    0 },
    { 0x06        , 0x00             , 1031, 1031 },
    { 0x0a        , 0x04             ,    8,   16 },
    { BB_LIFECYCLE, 0x00             ,    0,    0 },
    { BB_LIFECYCLE, 0x01             ,    0,    0 },
    { BB_LIFECYCLE, 0x02             ,    0,    0 },
    { BB_LIFECYCLE, 0x03             ,    0,    0 },
};

/* @return the command's fixed wire payload length, or -1 if (cls, selector) is not in the table. */
static inline int bb_cmd_in_len(enum bb_class cls, uint8_t selector)
{
    for (unsigned i = 0; i < sizeof bb_cmd_lens / sizeof bb_cmd_lens[0]; i++) {
        if (bb_cmd_lens[i].cls == cls && bb_cmd_lens[i].selector == selector) {
            return bb_cmd_lens[i].in_len;
        }
    }

    return -1;
}

/* Largest wire payload bb_build_cmd zero-pads to on the stack; covers every mapped GET/SET command
 * (max in_len 22, the pair-lock). Bump it if a longer command is added to bb_cmd_lens. */
#define BB_CMD_PAYLOAD_MAX 32

/* Build one bb_ioctl GET/SET frame (opcode 0, slot 0, port = selector). @p payload holds the @p len
 * MEANINGFUL bytes the caller filled (always `sizeof payload` at the call sites); this pads them up
 * to the command's fixed WIRE length from bb_cmd_lens with zeros. The two lengths differ when a
 * command is partly filled (pair-mode = 2 meaningful of 14 on the wire). @return the frame length,
 * or -1 if the command is unmapped, @p len exceeds its wire length, or the wire length is larger
 * than BB_CMD_PAYLOAD_MAX. Callers must size @p frame to hold that wire length + 19.
 */
static inline int bb_build_cmd(uint8_t *frame, enum bb_class cls, uint8_t selector,
                               const uint8_t *payload, int len, uint32_t seq)
{
    int wire = bb_cmd_in_len(cls, selector);
    uint8_t buf[BB_CMD_PAYLOAD_MAX] = { 0 };

    if (wire < 0 || len < 0 || len > wire || wire > (int)sizeof buf) {
        return -1;
    }

    if (len > 0) {
        memcpy(buf, payload, (size_t)len);
    }

    return bb_build_frame(frame, cls, 0, 0, selector, seq, buf, wire);
}

/* GET request (channel BB_GET, port = selector), seq = request id. The request payload length is
 * the table's (0 for every GET ml-linkd polls). The reply returns async on the request's channel;
 * decoding the reply payloads into typed values is done per-selector in the reader.
 */
static inline int bb_get(uint8_t *frame, enum bb_get_sel selector, uint32_t seq)
{
    return bb_build_cmd(frame, BB_GET, selector, NULL, 0, seq);
}

/* PROVEN: local output power for one chain. dir = RF_TX / RF_RX, dbm in dBm. */
static inline int bb_set_power(uint8_t *frame, enum rf_dir dir, uint8_t dbm, uint32_t seq)
{
    const uint8_t payload[2] = { dir, dbm };

    return bb_build_cmd(frame, BB_SET, SET_POWER, payload, sizeof payload, seq);
}

/* PROVEN: enable the chip's power self-adjust. */
static inline int bb_set_power_auto(uint8_t *frame, uint8_t enable, uint32_t seq)
{
    const uint8_t payload[1] = { enable };

    return bb_build_cmd(frame, BB_SET, SET_POWER_AUTO, payload, sizeof payload, seq);
}

/* DECODED: retune the local RX to a channel table index (0..18); the air re-associates
 * autonomously. The index is the table position, passed verbatim - not the OSD channel number. */
static inline int bb_select_channel(uint8_t *frame, uint8_t chan_idx, uint32_t seq)
{
    const uint8_t payload[2] = { 0x02, chan_idx };

    return bb_build_cmd(frame, BB_SET, SET_CHNIDX, payload, sizeof payload, seq);
}

/* DECODED: set channel bandwidth. */
static inline int bb_set_bandwidth(uint8_t *frame, uint8_t bandwidth, uint32_t seq)
{
    const uint8_t payload[3] = { 0x00, 0x01, bandwidth };

    return bb_build_cmd(frame, BB_SET, SET_BANDWIDTH, payload, sizeof payload, seq);
}

/* DECODED: enter (enable = 1) / exit (enable = 0) the chip's pair mode for slot 0. Byte 0 is the
 * enable flag, byte 1 the slot bitmask (bit 0 = slot 0); bb_build_cmd zero-pads to the command's
 * 14-byte wire length. While pair mode is on the chip broadcasts/answers pairing over the air
 * autonomously; the host polls GET_PAIR for a candidate (AR_AR8030_RX_BbPair @0x462ea8). */
static inline int bb_pair_mode(uint8_t *frame, uint8_t enable, uint32_t seq)
{
    const uint8_t payload[2] = { enable ? 1 : 0, 0x01 };

    return bb_build_cmd(frame, BB_SET, SET_PAIR_MODE, payload, sizeof payload, seq);
}

/* DECODED: lock the pairing to @p mac (4 bytes, exactly as read from the GET_PAIR reply, NOT
 * byte-swapped). Payload = u16 0x0100 (LE wire bytes 00 01) + the MAC; bb_build_cmd zero-pads to
 * the command's 22-byte wire length. Sent AFTER exiting pair mode, matching the vendor order
 * (exit at :1058, lock at :1067). */
static inline int bb_pair_lock(uint8_t *frame, const uint8_t mac[4], uint32_t seq)
{
    const uint8_t payload[6] = { 0x00, 0x01, mac[0], mac[1], mac[2], mac[3] };

    return bb_build_cmd(frame, BB_SET, SET_PAIR_LOCK, payload, sizeof payload, seq);
}

/* DECODED: set channel mode. */
static inline int bb_set_chnmode(uint8_t *frame, uint8_t mode, uint32_t seq)
{
    const uint8_t payload[1] = { mode };

    return bb_build_cmd(frame, BB_SET, SET_CHNMODE, payload, sizeof payload, seq);
}

/* DECODED: manual MCS is two frames - set the mode, then the value. Call
 * bb_set_mcs_mode(MCS_MODE_MANUAL, seq) then bb_set_mcs_value(mcs, seq + 1). The value is offset +2
 * on the wire. The mode byte is the HIGH byte of the u16 payload (the vendor writes mode << 8).
 */
enum bb_mcs_mode {
    MCS_MODE_MANUAL = 0,   /* fixed rate, whatever bb_set_mcs_value last set */
    MCS_MODE_AUTO   = 1,   /* chip picks the rate (the normal running mode) */
};

static inline int bb_set_mcs_mode(uint8_t *frame, enum bb_mcs_mode mode, uint32_t seq)
{
    const uint8_t payload[2] = { 0, (uint8_t)mode };

    return bb_build_cmd(frame, BB_SET, SET_MCS_MODE, payload, sizeof payload, seq);
}

static inline int bb_set_mcs_value(uint8_t *frame, uint8_t mcs, uint32_t seq)
{
    const uint8_t payload[2] = { 0, (uint8_t)(mcs + 2) };

    return bb_build_cmd(frame, BB_SET, SET_MCS, payload, sizeof payload, seq);
}

#endif /* BB_CMD_H */
