/*
 * mlm.h - the MissingLynk Messaging (MLM) cross-component wire contracts. "MLM" is the
 * protocol name; its frame magic is "MLM1" (see MLM_MAGIC). Lives in the top-level
 * ml-shared/ folder; every component (gstreamer/src tools, ml-linkd, ml-ledd, hud)
 * includes it from here.
 *
 * Two seams, both under /run/missinglynk:
 *  - drm.sock       SOCK_STREAM; ml-drmfd passes the shared DRM master fd via SCM_RIGHTS.
 *  - telemetry.sock SOCK_DGRAM; one datagram = one mlm_hdr-framed record, consumer binds,
 *    producer sends MSG_DONTWAIT and drops on error (video never blocks on the HUD).
 *  - osd.sock       same datagram contract, MSP DisplayPort records (the HUD binds; ml-linkd,
 *                    or ml-hud/tools/osd-replay on the bench, sends).
 */
#ifndef MLM_H
#define MLM_H

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MLM_RUN_DIR        "/run/missinglynk"

#define MLM_DRM_SOCK       MLM_RUN_DIR "/drm.sock"
#define MLM_TELEMETRY_SOCK MLM_RUN_DIR "/telemetry.sock"
#define MLM_OSD_SOCK       MLM_RUN_DIR "/osd.sock"
#define MLM_LINK_SOCK      MLM_RUN_DIR "/link.sock"   /* consumer -> ml-linkd: READY gate */
#define MLM_LED_SOCK       MLM_RUN_DIR "/led.sock"    /* any producer -> ml-ledd: LED command */
#define MLM_CTRL_SOCK      MLM_RUN_DIR "/ctrl.sock"   /* HUD -> ml-pipeline: control commands (ml-pipeline binds) */

#define MLM_MAGIC 0x314d4c4du /* "MLM1" little-endian */

/* record types; new types append, consumers skip unknown ones */
enum mlm_type {
    MLM_T_MSP        = 0x0001, /* raw MSP DisplayPort frame off :10000 */
    MLM_T_STATUS     = 0x0002, /* raw TX-unit binary status frame off :10000 */
    MLM_T_FRAMESTATS = 0x0003, /* per-video-frame stats (SEI-derived later) */
    MLM_T_READY      = 0x0004, /* consumer -> ml-linkd: pipeline bound on :10001 (heartbeat) */
    MLM_T_LINK       = 0x0005, /* ml-linkd -> HUD: link state change (associated/params/lost) */
    MLM_T_LED        = 0x0006, /* any producer -> ml-ledd: set the status LED pattern */
    MLM_T_CMD        = 0x0007, /* HUD -> ml-pipeline (ctrl.sock): a control command */
    MLM_T_STATE      = 0x0008, /* ml-pipeline -> HUD (telemetry.sock): current pipeline mode */
    MLM_T_LINKINFO   = 0x0009, /* ml-linkd -> HUD (telemetry.sock): local baseband link metrics */
    MLM_T_RFCMD      = 0x000a, /* HUD -> ml-linkd (link.sock): air-unit RF config command */
    MLM_T_SCAN       = 0x000b, /* ml-linkd -> HUD (telemetry.sock): RF channel scan result */
};

struct mlm_hdr {
    uint32_t magic;
    uint16_t type;  /* enum mlm_type */
    uint16_t flags;
} __attribute__((packed));

struct mlm_framestats {
    uint32_t frame_id;
    uint32_t br_kbps; /* combined air-encoder bitrate, kbps (0 = no SEI yet). Was `reserved`. */
    uint64_t pts_ns;  /* GStreamer buffer PTS (GST_CLOCK_TIME_NONE = ~0ull) */
    uint32_t qp;      /* air-encoder QP, tile 0 (0 = no SEI yet) */
    /* br_kbps/qp are SEI-derived (in-band H.265 PREFIX_SEI, NAL 39, ASCII "... BR <kbps> QP <n>"):
     * br_kbps sums both tiles, qp is tile 0. br_kbps reuses the old `reserved` slot and qp is
     * appended, so {frame_id, pts_ns} keep their offsets and a pre-SEI consumer reading only that
     * prefix stays compatible. */
} __attribute__((packed));

struct mlm_ready {
    uint32_t frames_seen; /* 0 = pipeline up but no video datagrams yet; 1 = frames arriving (latched) */
    uint32_t rx_pkts;     /* low 32 bits of the pipeline's raw :10001 datagram counter. Advances iff
                           * the air is actually sending video (independent of decode/playback), so
                           * ml-linkd uses it to catch a mid-session video stall the :10000 telemetry
                           * hides (an air-side link bounce rebuilds the air's encoder but never
                           * silences telemetry, so the air-loss watch cannot see it) and re-runs the
                           * media handshake. Old producers send 0 here; a never-changing counter
                           * disables the stall watch, so the field is backward compatible. */
} __attribute__((packed));

/* MLM_T_LINK payload (ml-linkd -> telemetry.sock consumer) */
enum mlm_link_state {
    MLM_LINK_ASSOCIATED      = 1, /* bb-socket bring-up done, steady cadence running */
    MLM_LINK_PARAMS_ACKED    = 2, /* :10000 params handshake acked, video should start */
    MLM_LINK_TX_LOST         = 3, /* >5 s of :10000 silence from the TX unit */
    MLM_LINK_SESSION_RESTART = 4, /* air returned after a loss, re-handshaking */
};

struct mlm_link {
    uint32_t state; /* enum mlm_link_state */
    uint32_t reserved;
} __attribute__((packed));

/* MLM_T_LINKINFO payload (ml-linkd -> HUD on telemetry.sock). The local AR8030 baseband link
 * metrics ml-linkd reads from its GET replies, republished ~1 Hz so the HUD's System OSD can show
 * the air-unit side. These are LOCAL-goggle readings (RF ranging + link quality), distinct from the
 * air unit's own :10000 status frames (voltage/temperature) that ride MLM_T_STATUS. Each field is
 * self-describing: a sentinel means "not known yet / no link", so the HUD renders a dim placeholder.
 */
/* channel/snr/distance sentinel: unknown or no link. INT32_MIN (not -1) so it never collides with a
 * valid reading - SNR in dB can legitimately be negative on a weak link. */
#define MLM_LINKINFO_NONE INT32_MIN

struct mlm_linkinfo {
    int32_t  channel;      /* channel table index the RX is tuned to (the select value), or MLM_LINKINFO_NONE */
    int32_t  snr_db;       /* link SNR in dB (from Get1V1Info), or MLM_LINKINFO_NONE */
    int32_t  distance_m;   /* RF-ranging distance in metres, or MLM_LINKINFO_NONE (not ranging) */
    uint32_t flags;        /* MLM_LINKINFO_F_* validity/state bits */
    uint32_t throughput_kbps; /* measured PHY link throughput / capacity (Get1V1Info +0x0c); NOT the
                            * encoder bitrate. 0 = unknown / no video link. Appended, older readers ignore. */
} __attribute__((packed));

/* The air unit is currently in standby (its work-mode sync, :10000 SetStandyMode 0x12, reports
 * standby - the quad is disarmed and standby is armed). The HUD shows the standby glyph. */
#define MLM_LINKINFO_F_STANDBY 0x1

/* MLM_T_SCAN payload (ml-linkd -> HUD on telemetry.sock). One RF channel scan. The frequency table
 * and `valid_bmp` come from the raw GetScanResult bb-socket reply (struct-of-arrays: count + freq[]
 * + valid bitmap); the chip returns the FULL channel table (up to 19) regardless of the
 * Standard/Race mode, and `valid_bmp` is the current mode's valid-channel mask (bit i = index i
 * valid). `snr_db` does NOT come from that reply, which carries no per-channel SNR: ml-linkd
 * measures it by visiting each valid channel and reading Get1V1Info there, so only valid channels
 * carry a value and out-of-mode entries carry MLM_SCAN_SIGNAL_NONE. */
#define MLM_SCAN_MAX_CH     19
#define MLM_SCAN_SIGNAL_NONE INT16_MIN     /* snr_db has no value (snr_raw <= 0) */

/* snr_raw sentinels. The vendor buckets the RAW value, so it is the wire's source of truth and
 * snr_db is only a display convenience. */
#define MLM_SCAN_RAW_NOLOCK  0   /* the chip never reported the swept channel: no usable link there */
#define MLM_SCAN_RAW_NONE   (-1) /* no Get1V1Info reply came back at all */

struct mlm_scan_chan {
    uint16_t freq_mhz;  /* channel centre frequency in MHz */
    int16_t  snr_db;    /* snr_raw expressed in dB for display, or MLM_SCAN_SIGNAL_NONE */
    int16_t  snr_raw;   /* raw linear Get1V1Info SNR (+0x06), or MLM_SCAN_RAW_NOLOCK / _RAW_NONE */
    uint8_t  index;     /* channel table index 0..18 = the value passed to a channel select */
    uint8_t  valid;     /* 1 = in the current mode's valid set (from valid_bmp) */
} __attribute__((packed));

struct mlm_scan {
    uint32_t valid_bmp;  /* current mode's valid-channel bitmap (bit i set = index i selectable now) */
    uint8_t  count;      /* number of entries in chan[] */
    uint8_t  active_idx; /* table index the local RX is currently tuned to (for the active highlight) */
    uint8_t  pad[2];
    struct mlm_scan_chan chan[MLM_SCAN_MAX_CH];
} __attribute__((packed));

/* MLM_T_LED payload (any producer -> ml-ledd). ml-ledd renders the animation itself
 * (the WS2812 LED has no hardware ramp), so a command is just the target pattern; the
 * last command wins. Producers re-assert periodically so a restarted ml-ledd reconverges.
 */
enum mlm_led_mode {
    MLM_LED_OFF     = 0,
    MLM_LED_SOLID   = 1, /* steady color */
    MLM_LED_BREATHE = 2, /* fade in/out over period_ms */
    MLM_LED_BLINK   = 3, /* hard on/off over period_ms (50% duty) */
};

struct mlm_led {
    uint8_t  mode;      /* enum mlm_led_mode */
    uint8_t  r, g, b;   /* target color, 0..255 per channel */
    uint16_t period_ms; /* breathe/blink cycle; ignored for solid/off */
    uint16_t reserved;
} __attribute__((packed));

/* MLM_T_CMD payload (HUD -> ml-pipeline on ctrl.sock). ml-pipeline is the source of truth for what
 * it is doing, so commands express intent, not absolute state: REC_TOGGLE flips recording and the
 * pipeline reports the result back as MLM_T_STATE. New commands (playback: PLAY/PAUSE/SEEK/STOP)
 * append; the pipeline ignores unknown cmd values. `arg` is command-specific (unused for the toggle).
 */
enum mlm_cmd_type {
    MLM_CMD_REC_TOGGLE = 1, /* start recording if idle, stop if recording */
    MLM_CMD_PLAY       = 2, /* play a file: the NUL-terminated path follows the mlm_cmd in the datagram */
    MLM_CMD_PAUSE      = 3, /* pause playback (hold current frame) */
    MLM_CMD_RESUME     = 4, /* resume paused playback */
    MLM_CMD_SEEK       = 5, /* seek within the current clip; arg = permille (0..1000) of duration */
    MLM_CMD_STOP       = 6, /* stop playback and return to the live stream */
    MLM_CMD_SPEED      = 7, /* set play speed; arg = signed multiplier (int32 bit-cast): 1 = normal,
                            *  2/4/8 fast-forward, -2/-4/-8 rewind. Realised as a rate seek. */
    MLM_CMD_SHOW_IDLE  = 8, /* park the display on the no-signal splash instead of the last decoded
                            *  frame (the live RF link dropped). The HUD sends this off ml-linkd's
                            *  connection state; a returning video frame clears it. Ignored during
                            *  playback (a file owns the display). */
    MLM_CMD_SRT_TEXT   = 9, /* one telemetry subtitle line: the NUL-terminated text follows the
                            *  mlm_cmd (like PLAY's path). While recording, ml-pipeline appends it
                            *  to the recording's .srt sidecar (same basename as the .mp4) stamped
                            *  with the recording-relative time; ignored otherwise. The sender (the
                            *  HUD, off its telemetry cache) gates on the dvr.save_srt setting, so
                            *  no datagrams means no sidecar file. */
    MLM_CMD_DVR_RES    = 10, /* DVR recording format: arg = (height << 16) | fps (720/1080, 30/60).
                             *  Latched by ml-pipeline and applied at the NEXT recording start (a
                             *  running recording keeps its format). The HUD sends it off the
                             *  dvr.resolution setting: on change and again just before each
                             *  record-start toggle, so a restarted pipeline reconverges. */
    MLM_CMD_OSD_CELL   = 11, /* one rendered BTFL OSD glyph cell for the DVR burn-in: a struct
                             *  mlm_osd_cell followed by w*h RGBA pixels rides after the mlm_cmd.
                             *  The HUD renders the cell with its own loaded MSP font (so the
                             *  recording uses the exact glyphs on the panel) and sends only
                             *  changed cells; ml-pipeline caches them and, while recording,
                             *  overwrites the opaque pixels into each composite before the
                             *  encoder. A header with no pixel payload clears that cell; row =
                             *  col = MLM_OSD_CLEAR_ALL clears the whole cache. The sender (the
                             *  HUD) gates on the dvr.record_osd setting + recording state. */
};

struct mlm_cmd {
    uint32_t cmd; /* enum mlm_cmd_type */
    uint32_t arg; /* command-specific; 0 when unused, permille for SEEK */
} __attribute__((packed));
/* MLM_CMD_PLAY carries the file path as bytes after the mlm_cmd in the same datagram; the
 * full frame is { mlm_hdr, mlm_cmd, char path[] } (NUL-terminated, bounded by MLM_PATH_MAX). */
#define MLM_PATH_MAX 512

/* MLM_CMD_OSD_CELL payload: one BTFL OSD grid cell, pre-rendered by the HUD at composite
 * resolution. The rect is the cell's luma-pixel rectangle in the 1920x1080 composite (the same
 * rectangle the HUD draws on the overlay plane, so the burned glyphs sit exactly under the
 * displayed ones). w*h RGBA pixels follow the struct: alpha >= 128 = opaque glyph pixel, else
 * transparent (the HUD's own binary-alpha draw rule); ml-pipeline only ever writes the opaque
 * pixels. A frame with no pixel bytes clears the cell.
 */
#define MLM_OSD_ROWS      20    /* BTFL HD DisplayPort grid (mirrors BTFL_OSD_ROWS/COLS) */
#define MLM_OSD_COLS      53
#define MLM_OSD_CLEAR_ALL 0xffff /* row = col = this: clear every cached cell */
#define MLM_OSD_CELL_WMAX 64    /* rect bounds a receiver must enforce */
#define MLM_OSD_CELL_HMAX 64
#define MLM_OSD_CELL_MAX  (sizeof(struct mlm_osd_cell) + \
                           (size_t) MLM_OSD_CELL_WMAX * MLM_OSD_CELL_HMAX * 4)

struct mlm_osd_cell {
    uint16_t row, col;  /* grid cell being replaced (or MLM_OSD_CLEAR_ALL / MLM_OSD_CLEAR_ALL) */
    uint16_t x, y;      /* cell rect top-left in composite luma pixels */
    uint16_t w, h;      /* cell rect size; w*h*4 RGBA bytes follow (0 bytes = cell cleared) */
} __attribute__((packed));

/* MLM_T_STATE payload (ml-pipeline -> HUD on telemetry.sock). The pipeline broadcasts its current
 * mode on every change and re-asserts periodically, so a restarted HUD (or one that missed a
 * datagram) reconverges. New modes (PLAYBACK, ...) append; the HUD treats unknown values as IDLE.
 */
enum mlm_state_mode {
    MLM_STATE_IDLE      = 0, /* receiving/decoding the live stream only */
    MLM_STATE_RECORDING = 1, /* a DVR recording is active */
    MLM_STATE_PLAYBACK  = 2, /* a file is playing back (preempting the live stream) */
};

struct mlm_state {
    uint32_t state;    /* enum mlm_state_mode */
    uint32_t flags;    /* MLM_STATE_F_* (playback: paused vs playing) */
    uint32_t pos_ms;   /* playback: current position in ms (0 otherwise) */
    uint32_t dur_ms;   /* playback: clip duration in ms (0 if unknown / not playing) */
} __attribute__((packed));

#define MLM_STATE_F_PAUSED 0x1 /* playback is paused (only meaningful in MLM_STATE_PLAYBACK) */
#define MLM_STATE_F_ENDED  0x2 /* playback reached end-of-clip: last frame held, position at duration,
                                *  awaiting the user (replay or exit). Still MLM_STATE_PLAYBACK. */
#define MLM_STATE_F_RENDERING 0x4 /* first decoded frame is on the display (the clip is visible); before
                                   *  this the HUD keeps the menu up with a loading spinner. */
#define MLM_STATE_F_VIDEO_LIVE 0x8 /* the pipeline is actively flipping frames to the display (a flip
                                    *  event within the last 500 ms). Video commits latch the whole VO
                                    *  shadow state including the HUD overlay's pixels, so while this
                                    *  is fresh the HUD suppresses its own per-present plane re-assert
                                    *  (a blocking SETPLANE commit that stalls the next video flip a
                                    *  full vblank in DRM's stall_checks). */

/* MLM_T_RFCMD payload (HUD -> ml-linkd on link.sock). ml-linkd owns /dev/artosyn_sdio and the
 * :10000 message channel, so the HUD never touches the air directly: it sends intent, and ml-linkd
 * translates it into the air's config datagrams and re-applies it after a session restart. The HUD
 * re-asserts on every link-up edge so the air converges to the menu's state (a default that was
 * never toggled still needs pushing). New cmds append; ml-linkd ignores unknown values.
 */
enum mlm_rfcmd_type {
    MLM_RF_SET_STANDBY = 1, /* arg = 1 arm standby, 0 disarm. Rides SetTranParm (:10000 msg 0x0D)
                             * byte[8] = u8StandbyModeEn; the air's automatic arm/disarm handshake
                             * (0x12/0x1b) does the actual power-save entry/exit. */
    MLM_RF_SET_POWER   = 2, /* arg = the air's TX power in mW, one of {25,100,200}. Rides the same
                             * SetTranParm (:10000 msg 0x0D) byte[0]; ml-linkd maps mW -> dBm
                             * (25->0x0e, 100->0x14, 200->0x17) and rejects any other value. */
    MLM_RF_SET_BITRATE = 3, /* arg = the air's video bitrate in Mbps, one of {8,16,24}. Rides
                             * SetLdCfg (:10000 msg 0x0A) bitrate_q (Mbps * 4, 250 kbps units);
                             * the air latches it at association, so a change takes effect on the
                             * next session. ml-linkd rejects any other value. */
    MLM_RF_SELECT_CHANNEL = 4, /* arg = the channel table index (0..18) to tune the local RX to,
                             * passed verbatim to the bb-socket SelectChn (SET_CHNIDX): the same
                             * index the scan reports and the tiles show, no +1. Unlike the three
                             * above this is LOCAL: it sends nothing to the air, which follows the
                             * retune transparently (no re-association, no video drop). ml-linkd
                             * rejects an arg outside the table (0..MLM_SCAN_MAX_CH-1); whether the
                             * index is valid in the current band is left to the chip. */
    MLM_RF_SCAN         = 5, /* trigger a one-shot RF channel scan (GetScanResult); ml-linkd fires it
                             * on its bb-socket thread and publishes MLM_T_SCAN. arg ignored. Read-only:
                             * the sweep self-restores the active channel. */
    MLM_RF_SET_CAMERA   = 6, /* arg = (selector << 16) | value, selector = enum mlm_cam_sel, value a
                             * u16. Rides SetCameraInfo (:10000 msg 0x0C), the air's selector-tagged
                             * ISP union: one datagram sets one field, applied live (seamless except
                             * rotation, which blips the feed). Exposure packs both of its fields into
                             * the one value: 0 = auto, nonzero = manual with that exposure time in
                             * microseconds. ml-linkd accepts only the HW-captured selectors and
                             * clamps/rejects out-of-range values. */
    MLM_RF_SET_SCALE    = 7, /* arg = (aspect << 16) | zoom_pct. Rides SetScaleMode (:10000 msg 0x15),
                             * the air's VIN scale: aspect 0 = 16:9, 1 = 4:3; zoom_pct = zoom factor
                             * in percent, one of {100, 70} (the two HW-captured stock values).
                             * Applied live; a change blips the feed (geometry restart). */
};

/* MLM_RF_SET_CAMERA selectors, the air's SetCameraInfo union tags. Only the ones the stock Camera
 * page exposed and the slot-A capture confirmed are listed; the others (brightness 0, WB 4,
 * aspect-as-0x0C 6, NR2D 8, ISO 9, banding 10) exist on the air but were never captured, so
 * ml-linkd does not send them. */
enum mlm_cam_sel {
    MLM_CAM_EXPOSURE   = 1,  /* value: 0 = auto, else manual exposure time in us */
    MLM_CAM_SATURATION = 2,  /* value: 0..100 (stock default 50) */
    MLM_CAM_SHARPNESS  = 3,  /* value: 0..100 (stock default 55) */
    MLM_CAM_ROTATION   = 5,  /* value: 0 = normal, 1 = 180 degrees */
    MLM_CAM_NR3D       = 7,  /* value: 0 = off, 1 = on (3D noise reduction) */
};

/* The air's cold-boot camera defaults (from the captured SetLdCfg base's camera block): the values
 * a (re)association resets the air's ISP to. Shared so the HUD's menu defaults and reset action,
 * and ml-linkd's cached wire state (mp-cmd.h MP_CAMERA_DEFAULTS), cannot drift apart. */
#define MLM_CAM_DEF_BRIGHTNESS   1
#define MLM_CAM_DEF_EXPOSURE     0      /* auto */
#define MLM_CAM_DEF_EXPOSURE_US  16666  /* manual exposure time seed (1/60 s) */
#define MLM_CAM_DEF_SATURATION   50
#define MLM_CAM_DEF_SHARPNESS    55
#define MLM_CAM_DEF_WB_MANUAL    0
#define MLM_CAM_DEF_WB_KELVIN    5000
#define MLM_CAM_DEF_ISO          100
#define MLM_CAM_DEF_ROTATION     0
#define MLM_CAM_DEF_NR3D         1
#define MLM_CAM_DEF_NR2D         1
#define MLM_CAM_DEF_ZOOM_PCT     100
#define MLM_CAM_DEF_ASPECT       0      /* 16:9 */

struct mlm_rfcmd {
    uint32_t cmd; /* enum mlm_rfcmd_type */
    uint32_t arg; /* command-specific */
} __attribute__((packed));

/* Send one MLM_T_RFCMD to ml-linkd's link.sock. Connectionless DGRAM: returns 0 if the datagram
 * went out, -1 otherwise (a down ml-linkd just means nobody is bound; the HUD re-asserts on the
 * next link-up edge regardless). */
static inline int mlm_rfcmd_send(uint32_t cmd, uint32_t arg)
{
    struct { struct mlm_hdr h; struct mlm_rfcmd cmd; } __attribute__((packed)) frame;
    frame.h.magic = MLM_MAGIC;
    frame.h.type = MLM_T_RFCMD;
    frame.h.flags = 0;
    frame.cmd.cmd = cmd;
    frame.cmd.arg = arg;

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_LINK_SOCK, sizeof addr.sun_path - 1);
    int ok = sendto(sock, &frame, sizeof frame, 0, (struct sockaddr *)&addr, sizeof addr)
             == (ssize_t) sizeof frame ? 0 : -1;
    close(sock);

    return ok;
}

/* SCM_RIGHTS fd passing over drm.sock */
static inline int mlm_send_fd(int sock, int fd)
{
    char byte = 'F';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = control.buf, .msg_controllen = sizeof control.buf };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}

static inline int mlm_recv_fd(int sock)
{
    char byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof control);
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = control.buf, .msg_controllen = sizeof control.buf };

    if (recvmsg(sock, &msg, 0) != 1) {
        return -1;
    }

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
            return fd;
        }
    }

    return -1;
}

/* connect to ml-drmfd and fetch the shared master fd (-1 on failure) */
static inline int mlm_get_drm_fd(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_DRM_SOCK, sizeof addr.sun_path - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(sock);
        return -1;
    }

    int fd = mlm_recv_fd(sock);
    close(sock);

    return fd;
}

/* Send one MLM_T_CMD to ml-pipeline's ctrl.sock. `path` (PLAY only, else NULL) is appended,
 * NUL-terminated. Connectionless DGRAM: returns 0 if the datagram was sent, -1 otherwise (a
 * down pipeline just means nobody is bound). */
static inline int mlm_ctrl_send(uint32_t cmd, uint32_t arg, const char *path)
{
    struct { struct mlm_hdr h; struct mlm_cmd cmd; char path[MLM_PATH_MAX]; } __attribute__((packed)) frame;
    size_t len = sizeof frame.h + sizeof frame.cmd;

    frame.h.magic = MLM_MAGIC;
    frame.h.type = MLM_T_CMD;
    frame.h.flags = 0;
    frame.cmd.cmd = cmd;
    frame.cmd.arg = arg;
    if (path) {
        size_t n = strnlen(path, MLM_PATH_MAX - 1);
        memcpy(frame.path, path, n);
        frame.path[n] = '\0';
        len += n + 1;
    }

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_CTRL_SOCK, sizeof addr.sun_path - 1);
    int ok = sendto(sock, &frame, len, 0, (struct sockaddr *)&addr, sizeof addr) == (ssize_t)len ? 0 : -1;
    close(sock);

    return ok;
}

/* Send one MLM_T_CMD with an arbitrary binary payload after the mlm_cmd (MLM_CMD_OSD_CELL's
 * cell frame). Same connectionless-DGRAM contract as mlm_ctrl_send; @p len is bounded by
 * MLM_OSD_CELL_MAX (the largest defined payload).
 */
static inline int mlm_ctrl_send_blob(uint32_t cmd, uint32_t arg, const void *data, size_t len)
{
    struct { struct mlm_hdr h; struct mlm_cmd cmd; unsigned char blob[MLM_OSD_CELL_MAX]; }
        __attribute__((packed)) frame;
    size_t total = sizeof frame.h + sizeof frame.cmd + len;

    if (len > sizeof frame.blob) {
        return -1;
    }

    frame.h.magic = MLM_MAGIC;
    frame.h.type = MLM_T_CMD;
    frame.h.flags = 0;
    frame.cmd.cmd = cmd;
    frame.cmd.arg = arg;
    if (len > 0) {
        memcpy(frame.blob, data, len);
    }

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_CTRL_SOCK, sizeof addr.sun_path - 1);
    int ok = sendto(sock, &frame, total, 0, (struct sockaddr *)&addr, sizeof addr)
             == (ssize_t) total ? 0 : -1;
    close(sock);

    return ok;
}

#endif
