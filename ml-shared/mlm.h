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
};

struct mlm_hdr {
    uint32_t magic;
    uint16_t type;  /* enum mlm_type */
    uint16_t flags;
} __attribute__((packed));

struct mlm_framestats {
    uint32_t frame_id;
    uint32_t reserved;
    uint64_t pts_ns; /* GStreamer buffer PTS (GST_CLOCK_TIME_NONE = ~0ull) */
} __attribute__((packed));

struct mlm_ready {
    uint32_t frames_seen; /* 0 = pipeline up but no video datagrams yet; 1 = frames arriving */
    uint32_t reserved;
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
};

struct mlm_cmd {
    uint32_t cmd; /* enum mlm_cmd_type */
    uint32_t arg; /* command-specific; 0 when unused, permille for SEEK */
} __attribute__((packed));
/* MLM_CMD_PLAY carries the file path as bytes after the mlm_cmd in the same datagram; the
 * full frame is { mlm_hdr, mlm_cmd, char path[] } (NUL-terminated, bounded by MLM_PATH_MAX). */
#define MLM_PATH_MAX 512

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

#endif
