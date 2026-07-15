/*
 * mp-cmd.h - named builders for the :10000 message-plane frames the goggle sends to the air unit.
 *
 * These are the UDP "Msg channel" datagrams (port 10000) carrying goggle->air config and the
 * media-params handshake - distinct from the local AR8030 bb-socket control plane (bb-cmd.h) and the
 * :20001 hello. Every frame shares a 24-byte header: msg_type (LE u32 @0), timestamp us (LE u32 @8),
 * body length (@16), body at offset 20. Each builder writes one frame into `frame` and returns its
 * length, so the caller owns transmission:
 *   sendto(sock, frame, mp_stb_ack(frame, stamp), 0, ...)
 *
 * RF-value safety: SetTranParm's non-power/standby bytes are the HW-confirmed vendor tuple; fabricating
 * them can reboot the goggle, so only power (body[0]) and standby (body[8]) are ever varied.
 */
#ifndef MP_CMD_H
#define MP_CMD_H

#include <stdint.h>
#include <string.h>

/* :10000 message types (LE u32 at byte 0). Goggle->air are the ones we build below; air->goggle are
 * listed too so the receive-side switch reads from one canonical map. */
enum mp_type {
    MP_REQUEST     = 0x01,   /* MEDIA_PARAMS request      (goggle->air) */
    MP_REPLY       = 0x02,   /* MEDIA_PARAMS reply        (air->goggle) */
    MP_ACK         = 0x03,   /* MEDIA_PARAMS type-3 ack   (goggle->air) */
    MP_STATUS_A    = 0x09,   /* air status frame          (air->goggle) */
    MP_SETTRANPARM = 0x0d,   /* TX power + standby arm    (goggle->air) */
    MP_MSP         = 0x10,   /* MSP DisplayPort canvas    (air->goggle) */
    MP_STATUS_B    = 0x11,   /* air status frame          (air->goggle) */
    MP_STANDBY     = 0x12,   /* SetStandyMode work-mode   (air->goggle) */
    MP_STBACK      = 0x1b,   /* STB_EVENT_ACK             (goggle->air) */
};

/* header layout (offsets into the datagram payload) */
#define MP_HDR_LEN     24        /* the common header; a bodyless frame is exactly this long */
#define MP_OFF_TYPE    0         /* msg_type (LE u32) */
#define MP_OFF_STAMP   8         /* timestamp us (LE u32) */
#define MP_OFF_LEN     16        /* body length (LE u32) */
#define MP_OFF_BODY    20        /* body starts here (0x0D uses the [20..23] word) */

/* Zero `frame` to `total`, then stamp the common header: msg_type, timestamp, body length. */
static inline void mp_stamp(uint8_t *frame, int total, enum mp_type type, uint32_t stamp, uint8_t body_len)
{
    memset(frame, 0, total);
    frame[MP_OFF_TYPE] = (uint8_t) type;   /* every type < 256; the high bytes stay 0 */
    memcpy(frame + MP_OFF_STAMP, &stamp, 4);
    frame[MP_OFF_LEN] = body_len;
}

/* A bodyless :10000 frame: the 24-byte header only. */
static inline int mp_header_only(uint8_t *frame, enum mp_type type, uint32_t stamp)
{
    mp_stamp(frame, MP_HDR_LEN, type, stamp, 0);
    return MP_HDR_LEN;
}

/* MEDIA_PARAMS handshake: request (poll for video params) and the type-3 ack that starts video. */
static inline int mp_params_request(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_REQUEST, stamp);
}

static inline int mp_params_ack(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_ACK, stamp);
}

/* STB_EVENT_ACK: the empty-body ack that COMPLETES the air's standby entry. Without it the air holds
 * at full fps - it hard-gates its fps/power drop on receiving this (air StbThread checks handle+0x188,
 * set only by StbAck / FSM case 0x1b). Verified by HW capture (slota-airconfig `1b0000...`) and static
 * RE (AR_LOWDELAY_RX_SYSCTRL_StbThread @004392a8; air StbAck @42327 gates fps @40566). */
static inline int mp_stb_ack(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_STBACK, stamp);
}

/* SetTranParm (0x0D): TX power + standby-arm. 34-byte frame, 10-byte body at offset 20: body[0]=dBm,
 * body[1]=0x04 (const in every captured frame), body[8]=u8StandbyModeEn. Only power and standby are
 * varied; the rest is the HW-confirmed vendor tuple. See plans/rf-air-config.md. */
#define MP_STP_LEN       34
#define MP_STP_BODY_LEN  0x0a
static inline int mp_set_tran_parm(uint8_t *frame, uint8_t dbm, uint8_t standby, uint32_t stamp)
{
    mp_stamp(frame, MP_STP_LEN, MP_SETTRANPARM, stamp, MP_STP_BODY_LEN);
    frame[MP_OFF_BODY + 0] = dbm;       /* body[0]: TX power dBm */
    frame[MP_OFF_BODY + 1] = 0x04;      /* body[1]: const */
    frame[MP_OFF_BODY + 8] = standby;   /* body[8]: u8StandbyModeEn (0/1) */
    return MP_STP_LEN;
}

#endif /* MP_CMD_H */
