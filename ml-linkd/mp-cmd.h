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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../ml-shared/mlm.h"   /* MLM_CAM_DEF_* camera defaults shared with the HUD */

/* :10000 message types (LE u32 at byte 0). Goggle->air are the ones we build below; air->goggle are
 * listed too so the receive-side switch reads from one canonical map. */
enum mp_type {
    MP_REQUEST     = 0x01,   /* MEDIA_PARAMS_REQUEST      (goggle->air) */
    MP_REPLY       = 0x02,   /* MEDIA_PARAMS_REDAY reply  (air->goggle) */
    MP_IDR_REQUEST = 0x03,   /* MEDIA_IDR_REQUEST         (goggle->air) */
    MP_STATUS_A    = 0x09,   /* air status frame          (air->goggle) */
    MP_SETLDCFG    = 0x0a,   /* low-delay/video config    (goggle->air) */
    MP_SETCAMERA   = 0x0c,   /* camera ISP selector set   (goggle->air) */
    MP_SETTRANPARM = 0x0d,   /* TX power + standby arm    (goggle->air) */
    MP_MSP         = 0x10,   /* MSP DisplayPort canvas    (air->goggle) */
    MP_STATUS_B    = 0x11,   /* air status frame          (air->goggle) */
    MP_STANDBY     = 0x12,   /* SetStandyMode work-mode   (air->goggle) */
    MP_SETSCALE    = 0x15,   /* VIN scale (zoom/aspect)   (goggle->air) */
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

/* MEDIA_PARAMS handshake: the goggle's poll and the air's reply. */
static inline int mp_params_request(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_REQUEST, stamp);
}

/* The reply carries a 72-byte body and MUST declare that length. A vendor receiver tests the
 * declared length against 0x48 before anything else (AR_FSM_RX_CommonMessageProcessThread,
 * archive/re/ghidra/out/ar_lowdelay-full.txt:21814) and drops the datagram with
 * "receive params size[%d] error , really size[72]" on a mismatch, so a header-only reply never
 * raises PARAMS_RECEIVED: the receiver then never runs its RealTimeInit and never asks for the
 * keyframe that starts video. Our own goggle dispatches on msg_type alone and takes its geometry
 * from SetLdCfg, so it ignores everything below and accepts this frame unchanged.
 *
 * Field map. Values are a real vendor reply captured off a working session
 * (archive/out/rf-capture/assoc-arm-sdio0.pcap, the one type-2 in it): 104 bytes, so there IS a
 * 12-byte tail past the body. Consumers are the stores into g_stRxParams (:21921) and the readers
 * in AR_FSM_RX_RealTimeInit (:18752). Offsets are absolute into the datagram:
 *
 *   16  u32  48 00 00 00   declared body length, 72.
 *   20  u32  01 00 00 00   pipeline flag. MUST BE 1 - see below.
 *   24  u32  01 00 00 00   pipeline value, forwarded to the pipeline create (:18783).
 *   28  u32  01 00 00 00   source ready. NON-ZERO IS MANDATORY - RealTimeInit refuses with "Tx Sns
 *                          Is Not Ready..." while this is 0, so a correctly sized body of zeros
 *                          fails one rung later than a header-only one. The vendor sets it when its
 *                          HDMI source connects (:15365), clears it on disconnect (:15442); our
 *                          source is the camera, live whenever there is anything to answer with.
 *   32  u32  00 00 00 00   zero, though the vendor's format-change path sets it (:15479).
 *   44  f32  e5 ff 6f 42   59.99988, frame rate, high precision. Zero is tolerated - the receiver
 *                          substitutes its configured rate when it reads 0.0 (:18882).
 *   48  u32  80 07 00 00   1920. The receiver decodes 48/52 into "720P60" and friends at :23182.
 *   52  u32  38 04 00 00   1080.
 *   56  u32  3c 00 00 00   60, frame rate, whole. Read by AR_LOWDELAY_RX_SYSCTRL_SetCurInputFps.
 *   88  f32  00 00 80 3f   1.0.
 *   92..103                zero, the tail.
 *
 * Offsets 36, 40 and 60..87 are unidentified. 40 reaches the decoder create (:18880), so it is the
 * first field to look at if a vendor goggle accepts the reply and then fails to bring up its
 * pipeline.
 *
 * With the flag at 20 set to zero, a vendor goggle accepts the reply, brings up decoder channel 0,
 * and fails every SendStream on channel 1 with "invalid frame, bs_size = 0" until it resets its
 * pipeline. AR_LDRT_RX_VDEC_Enable (libldrt_pipeline.so @0x38a68) seeds a channel count of 1 and
 * replaces it with the pattern's count only when this flag reads 1:
 *
 *    38ac0:  mov  w1, #0x1          ; count = 1
 *    38ac4:  ldr  w0, [x20]         ; arg[0] = this flag
 *    38acc:  cmp  w0, w1
 *    38ad0:  b.ne 38adc             ; leave count at 1
 *    38ad4:  ldrh w0, [x25, #40]    ; count = pattern channel count
 *
 * That count bounds the AR_MPI_VDEC_CreateChn loop (@0x38c14, bound reloaded @0x38ce0), so a zero
 * flag creates decoder channel 0 and nothing else. The demux still feeds channel 1, whose
 * per-channel bitstream-buffer size is therefore 0; AR_MPI_VDEC_SendStream loads that from the
 * channel table at +92 and rejects any frame with frame_size >= bs_size, returning 0x80058403 -
 * the VDEC illegal-parameter code, not a verdict on our bitstream.
 */
#define MP_PARAMS_BODY_LEN   0x48                                  /* 72, checked by the receiver */
#define MP_PARAMS_TAIL       12                                    /* zero bytes past the body */
#define MP_PARAMS_TOTAL      (MP_OFF_BODY + MP_PARAMS_BODY_LEN + MP_PARAMS_TAIL)  /* 104 */

#define MP_PR_OFF_PIPE_FLAG  (MP_OFF_BODY + 0)    /* 20 */
#define MP_PR_OFF_PIPE_VAL   (MP_OFF_BODY + 4)    /* 24 */
#define MP_PR_OFF_SRC_READY  (MP_OFF_BODY + 8)    /* 28 */
#define MP_PR_OFF_SRC_MODE   (MP_OFF_BODY + 12)   /* 32, zero on the wire */
#define MP_PR_OFF_FPS_F      (MP_OFF_BODY + 24)   /* 44 */
#define MP_PR_OFF_WIDTH      (MP_OFF_BODY + 28)   /* 48 */
#define MP_PR_OFF_HEIGHT     (MP_OFF_BODY + 32)   /* 52 */
#define MP_PR_OFF_FPS        (MP_OFF_BODY + 36)   /* 56 */
#define MP_PR_OFF_SCALE_F    (MP_OFF_BODY + 68)   /* 88, float 1.0 */

static inline int mp_params_reply(uint8_t *frame, uint32_t width, uint32_t height, uint32_t fps,
                                  uint32_t stamp)
{
    uint32_t one = 1;
    float fps_f = (float) fps;
    float scale = 1.0f;

    mp_stamp(frame, MP_PARAMS_TOTAL, MP_REPLY, stamp, MP_PARAMS_BODY_LEN);
    /* The pipeline flag/value pair. Zero at the flag creates decoder channel 0 only. */
    memcpy(frame + MP_PR_OFF_PIPE_FLAG, &one, 4);
    memcpy(frame + MP_PR_OFF_PIPE_VAL, &one, 4);
    memcpy(frame + MP_PR_OFF_SRC_READY, &one, 4);
    /* MP_PR_OFF_SRC_MODE stays zero: the wire capture has zero there. */
    memcpy(frame + MP_PR_OFF_SCALE_F, &scale, 4);
    memcpy(frame + MP_PR_OFF_FPS_F, &fps_f, 4);
    memcpy(frame + MP_PR_OFF_WIDTH, &width, 4);
    memcpy(frame + MP_PR_OFF_HEIGHT, &height, 4);
    memcpy(frame + MP_PR_OFF_FPS, &fps, 4);

    return MP_PARAMS_TOTAL;
}

/* MEDIA_IDR_REQUEST: the receiver asking the air for a keyframe, which is what starts video at
 * session start and what repairs a late join. RE: the goggle's AR_FSM_RX_ProcessIdrRequest
 * @0x42de70 pushes a wire message with [0]=3, us-timestamp at +8 and length at +16 - the :10000
 * header - as AR_FSM_RX_ProcessParamsRequest @0x42d8c0 does with [0]=1. The air routes it to TX FSM
 * message 8, AR_FSM_TX_ProcessIdrRequest @0x428938, which calls AR_LDRT_TX_PIPELINE_IdrEnable while
 * already streaming. */
static inline int mp_idr_request(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_IDR_REQUEST, stamp);
}

/* STB_EVENT_ACK: the empty-body ack that COMPLETES the air's standby entry. Without it the air holds
 * at full fps - it hard-gates its fps/power drop on receiving this (air StbThread checks handle+0x188,
 * set only by StbAck / FSM case 0x1b). Verified by HW capture (slota-airconfig `1b0000...`) and static
 * RE (AR_LOWDELAY_RX_SYSCTRL_StbThread @004392a8; air StbAck @42327 gates fps @40566). */
static inline int mp_stb_ack(uint8_t *frame, uint32_t stamp)
{
    return mp_header_only(frame, MP_STBACK, stamp);
}

/* Air->goggle status frames the air transmits on :10000. They share the common 20-byte header
 * (type@0, timestamp@8, body_len@16, body@20) and carry a 12-byte zero trailer after the body. The
 * goggle parses them at the offsets in ml-hud/src/channel/osd_proto.h. */
#define MP_TRAILER_LEN     12

/* 0x11 periodic status: 6-byte body relayed from the FC link. A bench air unit still sends this
 * frame with ADC voltage fallback while FC telemetry is stale. */
#define MP_STATUS_B_TOTAL  (MP_OFF_BODY + 6 + MP_TRAILER_LEN)   /* 38 */
static inline int mp_status_periodic(uint8_t *frame, uint8_t arm_flag, uint16_t mah_drawn_x10,
                                     uint16_t voltage_mv, uint32_t stamp)
{
    mp_stamp(frame, MP_STATUS_B_TOTAL, MP_STATUS_B, stamp, 6);
    frame[MP_OFF_BODY] = arm_flag;
    memcpy(frame + MP_OFF_BODY + 2, &mah_drawn_x10, sizeof mah_drawn_x10);
    memcpy(frame + MP_OFF_BODY + 4, &voltage_mv, sizeof voltage_mv);

    return MP_STATUS_B_TOTAL;
}

/* 0x12 SetStandyMode: the air's live work mode, a LE u32 at body offset 0. The air sends one on
 * every change; the goggle latches it for the HUD icon and answers a standby report with the 0x1b
 * ack that gates the air's fps and power drop. Work mode 0 = normal, 1 = standby; the goggle also
 * decodes 2 (airscrew/armed), which belongs to the FC-gated behaviour this stack does not
 * implement, so only 0 and 1 are ever sent.
 */
#define MP_STANDBY_TOTAL   (MP_OFF_BODY + 4 + MP_TRAILER_LEN)   /* 36 */
#define MP_STANDBY_NORMAL  0
#define MP_STANDBY_ON      1
static inline int mp_standby_report(uint8_t *frame, uint32_t work_mode, uint32_t stamp)
{
    mp_stamp(frame, MP_STANDBY_TOTAL, MP_STANDBY, stamp, 4);
    memcpy(frame + MP_OFF_BODY, &work_mode, sizeof work_mode);

    return MP_STANDBY_TOTAL;
}

/* 0x10 MSP DisplayPort canvas frame. `canvas` is the vendor-packed body generated from the FC's
 * MSP_DISPLAYPORT drawScreen update. */
static inline int mp_msp_canvas(uint8_t *frame, const uint8_t *canvas, uint8_t canvas_len,
                                uint32_t stamp)
{
    mp_stamp(frame, MP_OFF_BODY + canvas_len + MP_TRAILER_LEN, MP_MSP, stamp, canvas_len);
    memcpy(frame + MP_OFF_BODY, canvas, canvas_len);
    return MP_OFF_BODY + canvas_len + MP_TRAILER_LEN;
}

/* 0x09 version/info status: 128-byte body carrying the hw/fw version strings, the battery voltage,
 * and the air-unit temperature. Only the fields the goggle reads are set; the buffer is pre-zeroed
 * by mp_stamp, so the 16-byte string fields stay NUL-padded. */
#define MP_STATUS_A_TOTAL      (MP_OFF_BODY + 128 + MP_TRAILER_LEN)   /* 160 */
#define MP_STATUS_A_OFF_HW     0
#define MP_STATUS_A_OFF_FW     32
#define MP_STATUS_A_OFF_VOLT   96
#define MP_STATUS_A_OFF_TEMP   98
static inline int mp_status_version(uint8_t *frame, const char *hw, const char *fw,
                                    uint16_t voltage_mv, uint16_t temp_c, uint32_t stamp)
{
    uint8_t *body = frame + MP_OFF_BODY;

    mp_stamp(frame, MP_STATUS_A_TOTAL, MP_STATUS_A, stamp, 128);
    memcpy(body + MP_STATUS_A_OFF_HW, hw, strnlen(hw, 16));
    memcpy(body + MP_STATUS_A_OFF_FW, fw, strnlen(fw, 16));
    memcpy(body + MP_STATUS_A_OFF_VOLT, &voltage_mv, sizeof voltage_mv);
    memcpy(body + MP_STATUS_A_OFF_TEMP, &temp_c, sizeof temp_c);

    return MP_STATUS_A_TOTAL;
}

/* SetTranParm (0x0D): TX power + standby-arm. 34-byte frame, 10-byte body at offset 20: body[0]=dBm,
 * body[1]=0x04 (const in every captured frame), body[8]=u8StandbyModeEn. Only power and standby are
 * varied; the rest is the HW-confirmed vendor tuple. See plans/rf-air-config.md. */
#define MP_STP_LEN           34
#define MP_STP_BODY_LEN      0x0a
#define MP_STP_OFF_DBM       (MP_OFF_BODY + 0)   /* TX power dBm */
#define MP_STP_OFF_STANDBY   (MP_OFF_BODY + 8)   /* u8StandbyModeEn (0/1) */
static inline int mp_set_tran_parm(uint8_t *frame, uint8_t dbm, uint8_t standby, uint32_t stamp)
{
    mp_stamp(frame, MP_STP_LEN, MP_SETTRANPARM, stamp, MP_STP_BODY_LEN);
    frame[MP_STP_OFF_DBM] = dbm;
    frame[MP_OFF_BODY + 1] = 0x04;      /* body[1]: const */
    frame[MP_STP_OFF_STANDBY] = standby;
    return MP_STP_LEN;
}

/* SetLdCfg (0x0A): the low-delay/video config the goggle sends at ASSOCIATION. It seeds the air's
 * handle+0x118 (from the power byte) on every (re)association, so it is the DURABLE TX-power lever -
 * SetTranParm's live write is otherwise clobbered here.
 *
 * The datagram is a 20-byte header + a 192-byte (0xC0) BODY at wire offset 0x14 + 4 ignored trailer
 * bytes = 216. The air does memcpy(handle+0xb0, body, 0xc0), so struct offset S -> handle 0xb0+S. The
 * body is decoded below as `struct mp_ldcfg` (field-by-field RE in re/ghidra ar_lowdelay SetLdCfg
 * @004487a0 + cross-diff of glue/capture/out/ldcfg-*.hex; full table in plans/rf-air-config.md).
 *
 * We only SET the two CONFIRMED, cleanly-decoded levers (tx_power_dbm, bitrate_q); everything else is
 * sent verbatim from a known-good capture, because most fields are camera/OSD/rate-control state whose
 * exact encoding is undecoded and a fabricated RF/config byte can reboot the goggle. */
#define MP_LDCFG_LEN       216
#define MP_LDCFG_BODY_OFF  0x14   /* body (struct mp_ldcfg) starts here in the datagram */

/* Decoded SetLdCfg body (192 B). Only fields marked [SET] are varied by ml-linkd; the rest ride the
 * captured default. Named fields are CONFIRMED (HW cross-diff or a direct handle-offset RE read);
 * `reservedNN` gaps are undecoded vendor state sent byte-for-byte. Offsets are asserted below. */
/* [C]=CONFIRMED (HW cross-diff / direct handle-offset RE); [I]=INFERRED (an ar_lowdelay setter writes
 * this handle offset); [?]=undecoded, sent verbatim. Only [SET] fields are varied by ml-linkd. */
struct __attribute__((packed)) mp_ldcfg {
    uint8_t  valid;             /* 0x00  config-present/version flag (=1)                         [I] */
    uint8_t  caps_flags1;       /* 0x01  session capability bitfield (=0x0f; NOT the ROI enable)  [C] */
    uint16_t brightness;        /* 0x02  ISP brightness (SetCameraInfo sel0)                      [I] */
    uint16_t exposure_manual;   /* 0x04  ISP AE manual flag (sel1)                                [I] */
    uint16_t exposure_time;     /* 0x06  ISP exposure us (sel1; =16666 ~ 1/60 s)                  [I] */
    uint16_t cam_unk08;         /* 0x08  ISP misc (=14)                                           [?] */
    uint16_t reserved0a;        /* 0x0a                                                           [?] */
    uint16_t saturation;        /* 0x0c  ISP saturation (sel2)                                    [I] */
    uint16_t sharpness;         /* 0x0e  ISP sharpness (sel3)                                     [I] */
    uint16_t wb_manual;         /* 0x10  ISP white-balance manual flag (sel4)                     [I] */
    uint16_t white_balance;     /* 0x12  ISP WB color temp K (sel4; =5000)                        [I] */
    uint16_t rotation;          /* 0x14  image rotation (sel5)                                    [I] */
    uint16_t aspect_ratio;      /* 0x16  aspect ratio (sel6)                                      [I] */
    uint16_t nr3d_en;           /* 0x18  3D-DNR enable (sel7)                                     [I] */
    uint16_t nr2d_en;           /* 0x1a  2D-DNR enable (sel8)                                     [I] */
    uint16_t iso;               /* 0x1c  ISO (sel9; =100)                                         [I] */
    uint16_t cam_unk1e;         /* 0x1e  ISO/gain related (=6300)                                 [?] */
    uint16_t cam_unk20;         /* 0x20  (=100)                                                   [?] */
    uint16_t contrast;          /* 0x22  contrast (sel10)                                         [I] */
    uint8_t  scale_mode;        /* 0x24  VIN scale/aspect flag                                    [I] */
    uint8_t  scale_unk25;       /* 0x25                                                           [I] */
    uint16_t reserved26;        /* 0x26                                                           [?] */
    float    zoom_factor;       /* 0x28  VIN zoom (=1.0 = none)                                   [I] */
    uint8_t  reserved2c[4];     /* 0x2c  (=01 01 06 00)                                           [?] */
    uint16_t osd_unk30;         /* 0x30  OSD param block                                          [I] */
    uint16_t osd_unk32;         /* 0x32  OSD param block                                          [I] */
    uint16_t canvas_w;          /* 0x34  OSD canvas width  (=53; UpdateCanvasInfo)                [I] */
    uint16_t canvas_h;          /* 0x36  OSD canvas height (=20; UpdateCanvasInfo)                [I] */
    uint16_t reserved38;        /* 0x38                                                           [?] */
    uint16_t osd_rect[4];       /* 0x3a  OSD/viewfinder rect (=475,270,910,545; per-session)      [I] */
    uint8_t  reserved42;        /* 0x42                                                           [?] */
    uint8_t  roi_enable;        /* 0x43  air ROI/focus gate (=0 off; handle+0xf3)                 [C] */
    uint8_t  unk44;             /* 0x44  codec/scan? (=1)                                         [?] */
    uint8_t  unk45;             /* 0x45  framerate? (=4)                                          [?] */
    uint16_t rec_width;         /* 0x46  encode/record width  (=1920)                             [C] */
    uint16_t rec_height;        /* 0x48  encode/record height (=1080)                             [C] */
    uint16_t unk4a;             /* 0x4a  (=316)                                                   [?] */
    uint16_t reserved4c;        /* 0x4c                                                           [?] */
    uint16_t unk4e;             /* 0x4e  (=264)                                                   [?] */
    uint32_t reserved50;        /* 0x50                                                           [?] */
    uint32_t threshold54;       /* 0x54  auto-ROI/skip threshold (=0x7fffffff = off/max)          [I] */
    uint8_t  venc_flags58[13];  /* 0x58  VENC rate-control flags (undecoded)                      [?] */
    uint16_t bitrate_q;         /* 0x65  bitrate in 250 kbps units (= Mbps * 4)          [C][SET]     */
    uint8_t  reserved67;        /* 0x67                                                           [?] */
    uint8_t  tx_power_dbm;      /* 0x68  TX power dBm (25=0x0e,100=0x14,200=0x17)        [C][SET]     */
    uint8_t  tran_bw_mcs;       /* 0x69  SetTranParm mirror [1] bandwidth/MCS (verbatim)          [C] */
    uint16_t tran_blk2;         /* 0x6a  SetTranParm mirror [2:3] (cold-boot=1920; verbatim)      [C] */
    uint16_t tran_blk4;         /* 0x6c  SetTranParm mirror [4:5] (cold-boot=1080; verbatim)      [C] */
    uint8_t  tran_blk6;         /* 0x6e  SetTranParm mirror [6] (verbatim)                        [I] */
    uint8_t  tran_blk7;         /* 0x6f  SetTranParm mirror [7] (verbatim)                        [I] */
    uint8_t  standby_mode_en;   /* 0x70  u8StandbyModeEn (handle+0x120)                  [C][SET]     */
    uint8_t  flags71[2];        /* 0x71  (=01 01)                                                 [?] */
    uint8_t  caps_flags2;       /* 0x73  session capability bitfield (=0x12; NOT ROI)             [C] */
    uint8_t  unk74;             /* 0x74  (=0x10)                                                  [?] */
    char     dvr_path[0x40];    /* 0x75  air DVR record dir ("/tmp/sdcard/", NUL-terminated)      [C] */
    uint8_t  tail_flags[11];    /* 0xB5  trailing flags (01 at 0xB5,0xB6,0xBB)                    [?] */
};
_Static_assert(sizeof(struct mp_ldcfg) == 0xC0, "mp_ldcfg body must be 192 bytes");
_Static_assert(offsetof(struct mp_ldcfg, roi_enable)      == 0x43, "roi_enable offset");
_Static_assert(offsetof(struct mp_ldcfg, rec_width)       == 0x46, "rec_width offset");
_Static_assert(offsetof(struct mp_ldcfg, bitrate_q)       == 0x65, "bitrate_q offset");
_Static_assert(offsetof(struct mp_ldcfg, tx_power_dbm)    == 0x68, "tx_power_dbm offset");
_Static_assert(offsetof(struct mp_ldcfg, standby_mode_en) == 0x70, "standby_mode_en offset");
_Static_assert(offsetof(struct mp_ldcfg, dvr_path)        == 0x75, "dvr_path offset");

static const uint8_t mp_ldcfg_base[MP_LDCFG_LEN] = {
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x01, 0x00,
    0x00, 0x00, 0x1a, 0x41, 0x0e, 0x00, 0x00, 0x00, 0x32, 0x00, 0x37, 0x00,
    0x00, 0x00, 0x88, 0x13, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x64, 0x00, 0x9c, 0x18, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x3f, 0x01, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x35, 0x00, 0x14, 0x00, 0x00, 0x00, 0xdb, 0x01, 0x0e, 0x01, 0x8e, 0x03,
    0x21, 0x02, 0x00, 0x00, 0x01, 0x04, 0x80, 0x07, 0x38, 0x04, 0x3c, 0x01,
    0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x7f,
    0x00, 0xc8, 0x00, 0x00, 0x01, 0x00, 0x0a, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x60, 0x00, 0x00, 0x17, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x12, 0x10, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x73, 0x64,
    0x63, 0x61, 0x72, 0x64, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Build a SetLdCfg from the captured base, varying only the decoded fields via the struct overlay.
 * Pass dbm/mbps = 0 (or standby_arm < 0) to keep the base value for that field.
 *
 * standby_arm MUST track the commanded standby state: the air seeds u8StandbyModeEn (handle+0x120)
 * from this blob on EVERY association, so the captured base's 1 silently re-arms standby each
 * session no matter what SetTranParm byte[8] said - and an armed, disarmed-quad air drops to 5 dBm
 * / duty-cycled TX, which kills the video downlink (HW post-mortem 2026-07-19). */
static inline int mp_set_ld_cfg(uint8_t *frame, uint8_t dbm, uint8_t mbps, int standby_arm,
                                uint32_t stamp)
{
    memcpy(frame, mp_ldcfg_base, MP_LDCFG_LEN);
    memcpy(frame + MP_OFF_STAMP, &stamp, 4);

    struct mp_ldcfg *cfg = (struct mp_ldcfg *) (frame + MP_LDCFG_BODY_OFF);
    if (dbm) {
        cfg->tx_power_dbm = dbm;
    }

    if (mbps) {
        cfg->bitrate_q = (uint16_t) (mbps * 4);      /* 250 kbps units */
    }

    if (standby_arm >= 0) {
        cfg->standby_mode_en = standby_arm ? 1 : 0;
    }

    return MP_LDCFG_LEN;
}

/* SetCameraInfo (0x0C): one live camera/ISP set. The body is a selector-tagged union: body[0] (u32)
 * names the ONE field this message sets, and the air's handler (SetCameraInfo @00447138, a switch on
 * the selector) reads and applies exactly that field, ignoring the rest of the struct - so the
 * non-selected fields safely ride the sender's cached state. Body = 0x28 bytes at datagram offset 24
 * (the [20..23] word is zero on the wire - HW capture 2026-07-14); header length field = 0x28 (the
 * goggle builder SetCameraSettingToTx @0043bc88 copies exactly 40 bytes).
 *
 * Air-side value handling (RE): rotation and the NR enables are booleans (nonzero = on; the NR
 * strength is the air's own constant 50); exposure_manual nonzero puts the AEC in manual mode with
 * exposure_time in us, 0 returns it to auto; iso only applies while the AEC is already manual;
 * banding accepts only 0 (off) / 50 / 60 (Hz, anti-flicker) and forces anything else to off;
 * brightness (sel 0) is stored but drives no ISP call. Saturation/sharpness/WB pass straight to the
 * ISP layer, which owns the clamping.
 *
 * Body offset: 20, the same MP_OFF_BODY as every other :10000 frame (SetLdCfg's body is at 20 too).
 * An earlier capture note claimed 24 for 0x0C; HW-refuted 2026-07-19: with the selector at 24 the
 * air parsed every message as selector 0 (brightness, no ISP apply) - the air reads the selector at
 * [20..23], matching the RE event-struct mapping (payload at event+0x1c = wire 20). */
#define MP_CAM_LEN       60      /* 20-byte header + 0x28 body */
#define MP_CAM_BODY_OFF  MP_OFF_BODY
#define MP_CAM_BODY_LEN  0x28

/* The selector values are mlm.h's enum mlm_cam_sel, sent verbatim as body[0]: there is exactly one
 * selector namespace and it IS the wire encoding. The sel numbers on each field below are the air's
 * full union map (sel 10 is anti-flicker banding, NOT contrast: air log "u16Banding"). */
struct __attribute__((packed)) mp_camera {
    uint32_t selector;         /* 0x00  mlm_cam_sel: the ONE field this message applies */
    uint16_t brightness;       /* 0x04  sel 0 (air: stored only, no ISP apply) */
    uint16_t exposure_manual;  /* 0x06  sel 1: 0 = auto AEC, nonzero = manual */
    uint16_t exposure_time;    /* 0x08  sel 1: manual exposure time in us */
    uint16_t unk0a;            /* 0x0a */
    uint16_t unk0c;            /* 0x0c */
    uint16_t iso;              /* 0x0e  sel 9 (air: applied only while the AEC is manual) */
    uint16_t unk10;            /* 0x10 */
    uint16_t unk12;            /* 0x12 */
    uint16_t saturation;       /* 0x14  sel 2 */
    uint16_t sharpness;        /* 0x16  sel 3 */
    uint16_t wb_manual;        /* 0x18  sel 4: 0 = auto */
    uint16_t white_balance;    /* 0x1a  sel 4: color temperature K */
    uint16_t rotation;         /* 0x1c  sel 5: nonzero = 180 degrees */
    uint16_t aspect_ratio;     /* 0x1e  sel 6 (unused here) */
    uint16_t nr3d_en;          /* 0x20  sel 7: nonzero = on */
    uint16_t nr2d_en;          /* 0x22  sel 8: nonzero = on */
    uint16_t banding;          /* 0x24  sel 10: 0 = off, 50 / 60 = mains Hz */
    uint16_t unk26;            /* 0x26 */
};
_Static_assert(sizeof(struct mp_camera) == MP_CAM_BODY_LEN, "mp_camera body must be 0x28 bytes");

/* The air's cold-boot ISP state, from the captured SetLdCfg base's camera block: a full mp_camera
 * seeded from this plus the commanded fields matches what the air is already running, so a message
 * never carries a fabricated value in the fields it does not select. */
#define MP_CAMERA_DEFAULTS { \
    .selector = 0, \
    .brightness = MLM_CAM_DEF_BRIGHTNESS, \
    .exposure_manual = MLM_CAM_DEF_EXPOSURE, \
    .exposure_time = MLM_CAM_DEF_EXPOSURE_US, \
    .iso = MLM_CAM_DEF_ISO, \
    .saturation = MLM_CAM_DEF_SATURATION, \
    .sharpness = MLM_CAM_DEF_SHARPNESS, \
    .wb_manual = MLM_CAM_DEF_WB_MANUAL, \
    .white_balance = MLM_CAM_DEF_WB_KELVIN, \
    .rotation = MLM_CAM_DEF_ROTATION, \
    .aspect_ratio = MLM_CAM_DEF_ASPECT, \
    .nr3d_en = MLM_CAM_DEF_NR3D, \
    .nr2d_en = MLM_CAM_DEF_NR2D, \
    .banding = 0 }

/* Build a SetCameraInfo applying selector `sel` on top of the full cached state. */
static inline int mp_set_camera_info(uint8_t *frame, unsigned sel,
                                     const struct mp_camera *state, uint32_t stamp)
{
    mp_stamp(frame, MP_CAM_LEN, MP_SETCAMERA, stamp, MP_CAM_BODY_LEN);

    struct mp_camera *body = (struct mp_camera *) (frame + MP_CAM_BODY_OFF);
    *body = *state;
    body->selector = sel;

    return MP_CAM_LEN;
}

/* SetScaleMode (0x15): the air's VIN scale, zoom + aspect in one message. Body = 12 bytes at
 * datagram offset 20 (MP_OFF_BODY, like 0x0C - see the offset note there): aspect flag (u32,
 * 1 = 4:3, 0 = 16:9), zoom enable (u32), zoom ratio (f32). The air treats ratio 1.0 (or 0.0) as
 * zoom-off regardless of the enable flag and accepts any other ratio as-is (RE:
 * ProcessVinScaleEvent); the stock UI only ever uses 1.0 and 0.7. A change with media running tears
 * down and rebuilds the air's pipeline: expect a brief feed blip. */
#define MP_SCALE_LEN       32     /* 20-byte header + 12-byte body */
#define MP_SCALE_BODY_OFF  MP_OFF_BODY
#define MP_SCALE_BODY_LEN  0x0c

static inline int mp_set_scale_mode(uint8_t *frame, int aspect_4_3, float zoom, uint32_t stamp)
{
    uint32_t aspect = aspect_4_3 ? 1 : 0;
    uint32_t enable = (zoom > 0.0f && zoom != 1.0f) ? 1 : 0;

    mp_stamp(frame, MP_SCALE_LEN, MP_SETSCALE, stamp, MP_SCALE_BODY_LEN);
    memcpy(frame + MP_SCALE_BODY_OFF + 0, &aspect, 4);
    memcpy(frame + MP_SCALE_BODY_OFF + 4, &enable, 4);
    memcpy(frame + MP_SCALE_BODY_OFF + 8, &zoom, 4);

    return MP_SCALE_LEN;
}

#endif /* MP_CMD_H */
