/*
 * bb-cmd.h - named interface to the local AR8030 baseband chip.
 *
 * These builders emit the bb-socket control-plane frames written to /dev/artosyn_sdio; they
 * configure THIS unit's radio and are NOT commands to the air unit (air config rides UDP :10000,
 * see docs/rf-video-downlink.md).
 *
 * Each builder writes one frame into `frame` (which must hold plen + 19 bytes) and returns its
 * length, so the caller owns transmission:
 *   send_frame(frame, bb_set_power(frame, RF_TX, 23, seq), "tx-power")
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
    GET_CANDIDATES    = 0x03,   /* GetCandidates */
    GET_DISTANCE      = 0x05,   /* GetDistanceResult (RF ranging); reply = u32 metres at payload 0 */
    GET_STATUS_PARAMS = 0x04,   /* GetStatusParams */
    GET_MCS           = 0x06,   /* GetRxMcs / GetTxMcs / bitrate / framerate */
    GET_POWER         = 0x08,
    GET_SCAN_RESULT   = 0x0a,   /* GET_ScanResult */
    GET_TIME          = 0x0c,
    GET_1V1INFO       = 0x73,   /* Get1V1Info (link stats) */
};

/* SET selectors (channel BB_SET). */
enum bb_set_sel {
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

/* GET request (channel BB_GET, empty payload), seq = request id. The reply returns async on
 * ch03/ch05; decoding the reply payloads into typed values is not yet done (structs un-RE'd).
 */
static inline int bb_get(uint8_t *frame, enum bb_get_sel selector, uint32_t seq)
{
    return bb_build_frame(frame, BB_GET, 0, 0, selector, seq, NULL, 0);
}

/* PROVEN: local output power for one chain. dir = RF_TX / RF_RX, dbm in dBm. */
static inline int bb_set_power(uint8_t *frame, enum rf_dir dir, uint8_t dbm, uint32_t seq)
{
    const uint8_t payload[2] = { dir, dbm };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_POWER, seq, payload, 2);
}

/* PROVEN: enable the chip's power self-adjust. */
static inline int bb_set_power_auto(uint8_t *frame, uint8_t enable, uint32_t seq)
{
    const uint8_t payload[1] = { enable };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_POWER_AUTO, seq, payload, 1);
}

/* DECODED: retune the local RX to a channel table index (0..18); the air re-associates
 * autonomously. The index is the table position, passed verbatim - not the OSD channel number. */
static inline int bb_select_channel(uint8_t *frame, uint8_t chan_idx, uint32_t seq)
{
    const uint8_t payload[2] = { 0x02, chan_idx };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_CHNIDX, seq, payload, 2);
}

/* DECODED: set channel bandwidth. */
static inline int bb_set_bandwidth(uint8_t *frame, uint8_t bandwidth, uint32_t seq)
{
    const uint8_t payload[3] = { 0x00, 0x01, bandwidth };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_BANDWIDTH, seq, payload, 3);
}

/* DECODED: set channel mode. */
static inline int bb_set_chnmode(uint8_t *frame, uint8_t mode, uint32_t seq)
{
    const uint8_t payload[1] = { mode };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_CHNMODE, seq, payload, 1);
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

    return bb_build_frame(frame, BB_SET, 0, 0, SET_MCS_MODE, seq, payload, 2);
}

static inline int bb_set_mcs_value(uint8_t *frame, uint8_t mcs, uint32_t seq)
{
    const uint8_t payload[2] = { 0, (uint8_t)(mcs + 2) };

    return bb_build_frame(frame, BB_SET, 0, 0, SET_MCS, seq, payload, 2);
}

#endif /* BB_CMD_H */
