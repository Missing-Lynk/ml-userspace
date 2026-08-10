/*
 * ml-rx-udp.c - the RX role's UDP thread: :20001 hello/ack, :10000 handshake + telemetry, and the
 * link.sock seam the consumer READY gate and the HUD's RF commands arrive on.
 *
 * Everything the goggle sends to the air unit leaves from this thread, and everything the HUD asks
 * for lands here; the bb-socket work it triggers (a retune, a sweep, a bind) is queued for the
 * bb-socket TX thread instead of issued here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "../ml-shared/mlm.h"
#include "mp-cmd.h"
#include "ml-linkd.h"
#include "ml-rx.h"
#include "ml-rx-chan.h"
#include "ml-rx-bind.h"
#include "ml-rx-udp.h"

/* service intervals and timeouts (ms) */
#define READY_WINDOW_MS  6000             /* consumer heartbeat liveness window */
#define HELLO_IVL_MS     300              /* :20001 hello cadence (~3 Hz) */
#define PARAMS_IVL_MS    2000             /* :10000 request cadence (vendor 2 s) */
#define STANDBY_IVL_MS   2000             /* :10000 SetTranParm re-send cadence while armed (~2 s) */
#define LED_ASSERT_MS    1000             /* LED re-assert cadence (~1 Hz) */
#define LED_BREATHE_MS   3000             /* breathe-red period */
#define BIND_RETRY_MS    1000             /* unbound-socket rebind cadence */

/* :10000 message types + goggle->air builders live in mp-cmd.h. These are the receive-parse details
 * for the air's SetStandyMode (0x12) work-mode word. */
#define STANDBY_OFF_MODE 20               /* work-mode u32 at datagram offset 20 (body offset 0) */
#define STANDBY_MODE_ON  1                /* work-mode 1 = standby (0 = normal, 2 = airscrew/armed) */

/* HW-confirmed mW -> dBm values for SetTranParm body[0] (plans/rf-air-config.md §2). Only these three
 * levels are valid; a fabricated dBm can reboot the goggle, so map_power_dbm returns -1 for anything
 * else and the command is dropped. */
enum air_tx_dbm {
    AIR_TX_DBM_25  = 0x0e,   /* 25 mW  */
    AIR_TX_DBM_100 = 0x14,   /* 100 mW */
    AIR_TX_DBM_200 = 0x17,   /* 200 mW */
};
#define AIR_TX_DBM AIR_TX_DBM_100         /* vendor MID default, sent until the HUD commands a level */

/* Video-stall watch. The air-loss watch only sees :10000 silence, but an air-side link bounce
 * (chip LinkDown/LinkUp) tears down and rebuilds the air's video path WITHOUT ever silencing its
 * telemetry - the session then sits "acked" forever with zero video (HW post-mortem 2026-07-19).
 * The consumer's READY heartbeat carries its raw :10001 datagram counter (mlm_ready.rx_pkts); when
 * that counter stops advancing mid-session, the media handshake is re-run. All owned by this thread. */
static uint32_t g_rx_pkts_last;             /* last heartbeat's counter value */
static long g_rx_pkts_change_ms;            /* when it last advanced */
static int g_rx_counting;                   /* counter has advanced at least once this session */
#define MEDIA_STALL_MS 6000                 /* 3 heartbeats of a frozen counter = video is dead */
static volatile long g_last_ready_ms;       /* last MLM_T_READY heartbeat */

/* Air-unit RF config the HUD has commanded (MLM_T_RFCMD on link.sock). -1 = never commanded, so
 * nothing is pushed to the air until the HUD asserts it; the HUD re-asserts on every link-up edge.
 * ml-linkd re-sends the SetTranParm on a steady cadence while linked, so no per-change latch is
 * needed - a toggle or a returning air unit is picked up by the next tick. */
static volatile int g_standby_arm = -1;     /* 0/1 = HUD-commanded u8StandbyModeEn, -1 = unknown */
static volatile int g_power_dbm = -1;        /* HUD-commanded TX power (dBm) for SetTranParm body[0], -1 = unset */
static volatile int g_bitrate_mbps;         /* HUD-commanded bitrate (Mbps) for SetLdCfg bitrate_q, 0 = unset */

/* Camera/scale state the HUD has commanded (MLM_RF_SET_CAMERA / MLM_RF_SET_SCALE). Owned entirely
 * by this thread: the commands arrive on link.sock and the :10000 datagrams leave on params_sock,
 * both here. Each commanded selector is marked pending and sent as one live SetCameraInfo
 * (0x0C) once the session is up; the pending set is re-armed from the commanded set on every
 * (re)association, because the SetLdCfg the air latches there resets its ISP to the association
 * defaults. Scale (zoom + aspect) rides its own SetScaleMode (0x15) with the same latching. */
static struct mp_camera g_cam = MP_CAMERA_DEFAULTS;  /* full ISP state; defaults = air cold boot */
static uint32_t g_cam_pending;              /* bit per MLM_CAM_* selector awaiting a send */
static uint32_t g_cam_commanded;            /* bit per selector the HUD has commanded (re-assert set) */
static int g_scale_aspect = -1;             /* 0 = 16:9, 1 = 4:3; -1 = never commanded */
static int g_scale_zoom_pct = 100;          /* zoom factor in percent (100 or 70) */
static int g_scale_pending;

/* Map a HUD-commanded mW level to the air's SetTranParm dBm byte; -1 rejects anything not captured. */
static int map_power_dbm(uint32_t mw)
{
    switch (mw) {
        case 25: {
            return AIR_TX_DBM_25;
        } break;

        case 100: {
            return AIR_TX_DBM_100;
        } break;

        case 200: {
            return AIR_TX_DBM_200;
        } break;

        default: {
            return -1;
        } break;
    }
}

/* Drive the status LED (ml-ledd) off link state: breathe red while there is no video,
 * solid green once the params handshake is acked. Sent on the edges for latency and
 * re-asserted ~1 Hz (led_assert) so a late/restarted ml-ledd reconverges.
 */
static void led_cmd(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms)
{
    struct mlm_led led = { .mode = mode, .r = r, .g = g, .b = b, .period_ms = period_ms };

    mlm_pub(MLM_LED_SOCK, MLM_T_LED, &led, sizeof led);
}

static void led_assert(void)
{
    if (!g_params_acked || g_air_lost) {
        /* no usable link yet */
        led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
        return;
    }

    if (g_standby_state) {
        /* link up, air in standby */
        led_cmd(MLM_LED_BREATHE, 0xff, 0x50, 0x00, LED_BREATHE_MS);
        return;
    }

    /* link up, video flowing */
    led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);
}

/* The three sockets the thread owns plus the air's two peer addresses. Bound lazily (socks_bind). */
struct udp_socks {
    int hello;                  /* :20001 3-way hello */
    int params;                 /* :10000 params handshake + telemetry */
    int link;                   /* link.sock: consumer READY heartbeats + HUD RF commands */
    int hello_bound;
    int params_bound;
    int link_bound;
    unsigned bind_tries;
    long last_bind_ms;
    struct sockaddr_in air_hello;
    struct sockaddr_in air_params;
    struct sockaddr_un link_un;
};

/* Bind an AF_INET dgram socket to LOCAL_ADDR:port. */
static int bind_local(int sock, int port)
{
    struct sockaddr_in local;

    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    inet_pton(AF_INET, LOCAL_ADDR, &local.sin_addr);
    local.sin_port = htons(port);

    return bind(sock, (struct sockaddr *)&local, sizeof local);
}

static void socks_open(struct udp_socks *socks)
{
    int one = 1;

    memset(socks, 0, sizeof *socks);
    socks->hello = socket(AF_INET, SOCK_DGRAM, 0);
    socks->params = socket(AF_INET, SOCK_DGRAM, 0);
    socks->link = socket(AF_UNIX, SOCK_DGRAM, 0);
    socks->last_bind_ms = -BIND_RETRY_MS;

    setsockopt(socks->hello, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(socks->params, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    socks->air_hello.sin_family = AF_INET;
    inet_pton(AF_INET, AIR_ADDR, &socks->air_hello.sin_addr);
    socks->air_params = socks->air_hello;
    socks->air_hello.sin_port = htons(HELLO_PORT);
    socks->air_params.sin_port = htons(PARAMS_PORT);

    /* link.sock: the READY-gate seam; linkd binds, consumers send heartbeats */
    socks->link_un.sun_family = AF_UNIX;
    strncpy(socks->link_un.sun_path, MLM_LINK_SOCK, sizeof socks->link_un.sun_path - 1);
}

/* Retry the binds until every socket has one: sdio0's 10.0.0.1 exists only after ml-rf-bringup has
 * configured the interface, so a bind can fail at startup. Every send site is gated on the bound
 * flags, because a sendto on an unbound AF_INET socket would auto-bind it to an ephemeral port and
 * make the real bind fail forever. Logs the first failed round and then every OPEN_RETRY_EVERY-th. */
static void socks_bind(struct udp_socks *socks, long now)
{
    int log_this;

    if ((socks->hello_bound && socks->params_bound && socks->link_bound)
        || now - socks->last_bind_ms < BIND_RETRY_MS) {
        return;
    }

    log_this = (socks->bind_tries == 0 || (socks->bind_tries % OPEN_RETRY_EVERY) == 0);
    socks->last_bind_ms = now;
    socks->bind_tries++;
    if (!socks->hello_bound) {
        if (bind_local(socks->hello, HELLO_PORT) == 0) {
            socks->hello_bound = 1;
            if (socks->bind_tries > 1) {
                printf(TAG " bind :%d ok\n", HELLO_PORT);
                fflush(stdout);
            }
        } else if (log_this) {
            fprintf(stderr, TAG " bind :%d: %s (is sdio0 up as %s?), retrying\n",
                    HELLO_PORT, strerror(errno), LOCAL_ADDR);
        }
    }

    if (!socks->params_bound) {
        if (bind_local(socks->params, PARAMS_PORT) == 0) {
            socks->params_bound = 1;
            if (socks->bind_tries > 1) {
                printf(TAG " bind :%d ok\n", PARAMS_PORT);
                fflush(stdout);
            }
        } else if (log_this) {
            fprintf(stderr, TAG " bind :%d: %s, retrying\n", PARAMS_PORT, strerror(errno));
        }
    }

    if (!socks->link_bound) {
        mkdir(MLM_RUN_DIR, 0755);
        unlink(MLM_LINK_SOCK);
        if (bind(socks->link, (struct sockaddr *)&socks->link_un, sizeof socks->link_un) == 0) {
            socks->link_bound = 1;
            if (socks->bind_tries > 1) {
                printf(TAG " bind %s ok\n", MLM_LINK_SOCK);
                fflush(stdout);
            }
        } else if (log_this) {
            fprintf(stderr, TAG " bind %s: %s, retrying\n", MLM_LINK_SOCK, strerror(errno));
        }
    }
}

/* One HUD command off link.sock. The levers this records are pushed to the air by the cadence in
 * the thread below; the bb-socket work (a retune, a sweep, a bind) is queued for the bb-socket TX
 * owner, because issuing it here would race the steady poll and get lost. */
static void handle_rfcmd(const struct mlm_rfcmd *rfcmd, long now)
{
    switch (rfcmd->cmd) {
        case MLM_RF_SET_STANDBY: {
            int arm = rfcmd->arg ? 1 : 0;
            if (arm != g_standby_arm) {
                printf(TAG " rfcmd: standby arm=%d\n", arm);
                fflush(stdout);
            }
            g_standby_arm = arm;
        } break;

        case MLM_RF_SET_POWER: {
            int dbm = map_power_dbm(rfcmd->arg);
            if (dbm < 0) {
                fprintf(stderr, TAG " rfcmd: ignoring bad power %u mW\n", rfcmd->arg);
            } else {
                if (dbm != g_power_dbm) {
                    printf(TAG " rfcmd: power %u mW (0x%02x dBm)\n", rfcmd->arg, dbm);
                    fflush(stdout);
                }
                g_power_dbm = dbm;
            }
        } break;

        case MLM_RF_SET_BITRATE: {
            /* Applied via SetLdCfg at association (the air latches it there), so a change
             * takes effect on the next session; only the stock-menu levels are valid. */
            if (rfcmd->arg != 8 && rfcmd->arg != 16 && rfcmd->arg != 24) {
                fprintf(stderr, TAG " rfcmd: ignoring bad bitrate %u Mbps\n", rfcmd->arg);
            } else {
                if ((int)rfcmd->arg != g_bitrate_mbps) {
                    printf(TAG " rfcmd: bitrate %u Mbps (next session)\n", rfcmd->arg);
                    fflush(stdout);
                }
                g_bitrate_mbps = (int)rfcmd->arg;
            }
        } break;

        case MLM_RF_SELECT_CHANNEL: {
            /* Passed verbatim, no +1; the retune itself is queued for the bb-socket owner. */
            rx_chan_request_select(rfcmd->arg);
        } break;

        case MLM_RF_SET_CAMERA: {
            /* arg = (MLM_CAM_* selector << 16) | u16 value. Only the HW-captured selectors
             * are accepted; values are bounds-checked (an ISP value is not an RF byte, but
             * garbage still has no business on the wire). Applied by push_camera_scale. */
            unsigned sel = rfcmd->arg >> 16;
            unsigned value = rfcmd->arg & 0xffffu;
            int ok = 1;

            switch (sel) {
                case MLM_CAM_EXPOSURE: {
                    /* 0 = auto; else manual with the exposure time in us (down to 1/10000,
                     * up to 1/30 - the stock page never leaves that range) */
                    ok = value == 0 || (value >= 100 && value <= 33333);
                    if (ok) {
                        g_cam.exposure_manual = value != 0;
                        if (value != 0) {
                            g_cam.exposure_time = (uint16_t) value;
                        }
                    }
                } break;

                case MLM_CAM_SATURATION: {
                    ok = value <= 100;
                    if (ok) {
                        g_cam.saturation = (uint16_t) value;
                    }
                } break;

                case MLM_CAM_SHARPNESS: {
                    ok = value <= 100;
                    if (ok) {
                        g_cam.sharpness = (uint16_t) value;
                    }
                } break;

                case MLM_CAM_ROTATION: {
                    ok = value <= 1;
                    if (ok) {
                        g_cam.rotation = (uint16_t) value;
                    }
                } break;

                case MLM_CAM_NR3D: {
                    ok = value <= 1;
                    if (ok) {
                        g_cam.nr3d_en = (uint16_t) value;
                    }
                } break;

                default: {
                    ok = 0;
                } break;
            }

            if (!ok) {
                fprintf(stderr, TAG " rfcmd: ignoring bad camera sel=%u value=%u\n",
                        sel, value);
            } else {
                g_cam_commanded |= 1u << sel;
                g_cam_pending |= 1u << sel;
                if (g_verbose) {
                    printf(TAG " rfcmd: camera sel=%u value=%u\n", sel, value);
                    fflush(stdout);
                }
            }
        } break;

        case MLM_RF_SET_SCALE: {
            /* arg = (aspect << 16) | zoom_pct; only the two HW-captured zoom factors */
            unsigned zoom_pct = rfcmd->arg & 0xffffu;
            if (zoom_pct != 100 && zoom_pct != 70) {
                fprintf(stderr, TAG " rfcmd: ignoring bad zoom %u%%\n", zoom_pct);
            } else {
                g_scale_aspect = (rfcmd->arg >> 16) & 1;
                g_scale_zoom_pct = (int) zoom_pct;
                g_scale_pending = 1;
                if (g_verbose) {
                    printf(TAG " rfcmd: scale aspect=%d zoom=%d%%\n",
                           g_scale_aspect, g_scale_zoom_pct);
                    fflush(stdout);
                }
            }
        } break;

        case MLM_RF_SCAN: {
            rx_chan_request_scan();
        } break;

        case MLM_RF_BIND: {
            rx_bind_request(rfcmd->arg != 0, now);
        } break;

        default: {
            /* An unknown command is a HUD newer than this build. The contract in mlm.h is
             * additive, so ignore it rather than warn on every send. */
        } break;
    }
}

/* One consumer READY heartbeat off link.sock. */
static void handle_ready(const struct mlm_ready *ready, long now)
{
    if (!g_ready) {
        g_ready = 1;
        if (g_verbose) {
            printf(TAG " consumer READY\n");
            fflush(stdout);
        }
    }

    g_last_ready_ms = now;
    if (ready->frames_seen && g_params_acked && !g_video_confirmed) {
        g_video_confirmed = 1;
        if (g_verbose) {
            printf(TAG " consumer confirms frames arriving\n");
            fflush(stdout);
        }
    }

    /* feed the video-stall watch: note every advance of the consumer's datagram counter */
    if (ready->rx_pkts != g_rx_pkts_last) {
        g_rx_pkts_last = ready->rx_pkts;
        g_rx_pkts_change_ms = now;
        g_rx_counting = 1;
    }
}

/* One :10000 datagram from the air: the params reply that starts video, or telemetry to republish. */
static void handle_params(struct udp_socks *socks, const uint8_t *datagram, ssize_t n,
                          uint32_t stamp_us)
{
    uint32_t msg_type;

    if ((size_t)n < 4) {
        return;
    }

    memcpy(&msg_type, datagram, 4);       /* LE u32 msg type, common to both families */
    if (msg_type == MP_REPLY) {     /* MEDIA_PARAMS reply -> [SetLdCfg] -> IDR request, video starts */
        /* Match the vendor's per-cycle sequence exactly: after the air's 0x02 reply the goggle
         * sends SetLdCfg (0x0A), THEN the 0x03 ack that starts video (capture: 01 -> 02 -> 0a ->
         * 03 -> 15). Sending it here - as part of the handshake, before video - is the vendor
         * placement; sending it post-ack (mid-stream) reconfigured a live encoder and wedged the
         * air. Sent whenever the HUD has commanded a lever it carries (TX power and/or bitrate;
         * both stay at the captured base when unset). The !g_params_acked guard makes it fire
         * ONCE per session (the first 0x02): the poll runs for the whole session, 0x02 replies
         * arrive every 2 s, and re-sending SetLdCfg on each would be exactly that mid-stream
         * reconfig. */
        if (!g_params_acked && (g_power_dbm >= 0 || g_bitrate_mbps > 0)) {
            uint8_t cfg[MP_LDCFG_LEN];
            uint8_t dbm = g_power_dbm >= 0 ? (uint8_t) g_power_dbm : 0;
            /* standby: armed only when the HUD commanded it; never the base's 1 (an
             * uncommanded first association must not arm the standby trap) */
            sendto(socks->params, cfg,
                   mp_set_ld_cfg(cfg, dbm, (uint8_t) g_bitrate_mbps, g_standby_arm > 0,
                                 stamp_us),
                   MSG_DONTWAIT, (struct sockaddr *)&socks->air_params, sizeof socks->air_params);
            /* Power is the lever the air honours; the blob's bitrate_q is stored-but-ignored
             * on the air (HW-confirmed), so only power is worth noting. */
            if (g_verbose) {
                fprintf(stderr, TAG " tx SetLdCfg (power=0x%02x)\n", dbm);
            }
        }

        /* Ask the air for a keyframe. It streams one IDR at session start and P-frames
         * after, so a receiver that was not listening then needs a fresh one to decode
         * (mp-cmd.h, mp_idr_request). Sent while video is not confirmed flowing and stopped
         * once it is, which keeps the stream from going needlessly intra-heavy. Both the
         * air-loss watch and the video-stall watch clear g_video_confirmed, so requests
         * resume by themselves whenever video needs repairing. */
        if (!g_video_confirmed) {
            uint8_t frame[MP_HDR_LEN];
            sendto(socks->params, frame, mp_idr_request(frame, stamp_us), MSG_DONTWAIT,
                   (struct sockaddr *)&socks->air_params, sizeof socks->air_params);

            if (g_verbose) {
                fprintf(stderr, TAG " tx MEDIA_IDR_REQUEST\n");
            }
        }

        if (!g_params_acked) {
            g_params_acked = 1;
            link_event(MLM_LINK_PARAMS_ACKED, "MEDIA_PARAMS acked, video should start");
            led_cmd(MLM_LED_SOLID, 0x00, 0xff, 0x00, 0);

            /* The SetLdCfg the air just latched reset its ISP to the association defaults:
             * re-arm every commanded camera selector (and the scale pair) so the live
             * push below re-applies the HUD's state on top. */
            g_cam_pending = g_cam_commanded;
            g_scale_pending = g_scale_aspect >= 0;
        }
    } else if (msg_type == MP_MSP) {
        mlm_pub(MLM_OSD_SOCK, MLM_T_MSP, datagram, n);
    } else if (msg_type == MP_STATUS_A || msg_type == MP_STATUS_B) {
        mlm_pub(MLM_TELEMETRY_SOCK, MLM_T_STATUS, datagram, n);
    } else if (msg_type == MP_STANDBY && n >= STANDBY_OFF_MODE + 4) {
        /* the air reports its live work-mode on every change; latch it for the HUD icon */
        uint32_t wm;
        memcpy(&wm, datagram + STANDBY_OFF_MODE, 4);
        g_standby_state = (wm == STANDBY_MODE_ON);
        if (g_verbose) {
            fprintf(stderr, TAG " standby work-mode=%u\n", wm);
        }

        /* Ack a standby entry so the air actually drops to its low-power fps - it gates that
         * drop on this 0x1b and holds full fps until it arrives (see MP_STBACK). The air only
         * emits 0x12 on a work-mode change, so one ack per standby-mode report matches stock. */
        if (wm == STANDBY_MODE_ON) {
            uint8_t frame[MP_HDR_LEN];
            sendto(socks->params, frame, mp_stb_ack(frame, stamp_us), MSG_DONTWAIT,
                   (struct sockaddr *)&socks->air_params, sizeof socks->air_params);
            if (g_verbose) {
                fprintf(stderr, TAG " tx StbAck (0x1b)\n");
            }
        }
    }
}

/* Live camera/scale pushes: one SetCameraInfo (0x0C) per pending selector plus one SetScaleMode
 * (0x15), the vendor's own live-set path (one datagram per menu change). The caller gates this on
 * an established session: the air only applies ISP sets with a running encoder, and anything
 * commanded earlier stays pending until the params ack re-arms it and it lands here. */
static void push_camera_scale(struct udp_socks *socks, uint32_t stamp_us)
{
    while (g_cam_pending != 0) {
        unsigned sel = (unsigned) __builtin_ctz(g_cam_pending);
        uint8_t frame[MP_CAM_LEN];

        g_cam_pending &= g_cam_pending - 1;
        sendto(socks->params, frame, mp_set_camera_info(frame, sel, &g_cam, stamp_us),
               MSG_DONTWAIT, (struct sockaddr *)&socks->air_params, sizeof socks->air_params);
        if (g_verbose) {
            fprintf(stderr, TAG " tx SetCameraInfo sel=%u\n", sel);
        }
    }

    if (g_scale_pending) {
        uint8_t frame[MP_SCALE_LEN];

        g_scale_pending = 0;
        sendto(socks->params, frame,
               mp_set_scale_mode(frame, g_scale_aspect == 1,
                                 (float) g_scale_zoom_pct / 100.0f, stamp_us),
               MSG_DONTWAIT, (struct sockaddr *)&socks->air_params, sizeof socks->air_params);
        if (g_verbose) {
            fprintf(stderr, TAG " tx SetScaleMode aspect=%d zoom=%d%%\n",
                    g_scale_aspect, g_scale_zoom_pct);
        }
    }
}

void *rx_udp_thread(void *arg)
{
    struct udp_socks socks;
    uint8_t hello[HELLO_LEN] = { 0 };
    long last_hello = 0, last_req = 0, last_led = 0, last_stp = 0;

    (void)arg;
    socks_open(&socks);

    while (g_run) {
        long now = now_ms();
        uint32_t stamp_us;
        struct timespec t;
        uint8_t buf[PKT_MAX];
        ssize_t n;

        socks_bind(&socks, now);

        clock_gettime(CLOCK_MONOTONIC, &t);
        stamp_us = (uint32_t)(t.tv_sec * 1000000ULL + t.tv_nsec / 1000);

        /* link.sock datagrams: consumer READY heartbeats + HUD RF commands (nonblocking drain) */
        while ((n = recv(socks.link, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
            struct mlm_hdr *hdr = (struct mlm_hdr *)buf;

            if ((size_t)n < sizeof *hdr || hdr->magic != MLM_MAGIC) {
                continue;
            }

            if (hdr->type == MLM_T_RFCMD && (size_t)n >= sizeof *hdr + sizeof(struct mlm_rfcmd)) {
                handle_rfcmd((struct mlm_rfcmd *)(buf + sizeof *hdr), now);
            } else if (hdr->type == MLM_T_READY
                       && (size_t)n >= sizeof *hdr + sizeof(struct mlm_ready)) {
                handle_ready((struct mlm_ready *)(buf + sizeof *hdr), now);
            }
        }

        /* Video-stall watch: the session is "acked" and video was confirmed flowing, but the
         * consumer's :10001 counter has frozen while the air's telemetry stays alive - the
         * air-side link bounce rebuilt its video path and now waits for a media handshake this
         * session already latched past. Re-arm the handshake: the ungated 2 s poll keeps running,
         * and the next 0x02 reply re-fires SetLdCfg + the type-3 ack (and the camera re-assert). */
        if (g_params_acked && g_video_confirmed && g_rx_counting && !g_air_lost
            && now - g_rx_pkts_change_ms > MEDIA_STALL_MS) {
            g_params_acked = 0;
            g_video_confirmed = 0;
            g_rx_counting = 0;
            link_event(MLM_LINK_SESSION_RESTART,
                       "video datagrams stalled, re-running the media handshake");
        }

        if (g_ready && now - g_last_ready_ms > READY_WINDOW_MS) {
            g_ready = 0;
            printf(TAG " consumer READY lost (heartbeat timeout)\n");
            fflush(stdout);
        }

        /* :20001 hello until the 3-way is done (vendor goes quiet after) */
        if (socks.hello_bound && !g_hs_done && now - last_hello >= HELLO_IVL_MS) {
            sendto(socks.hello, hello, sizeof hello, MSG_DONTWAIT,
                   (struct sockaddr *)&socks.air_hello, sizeof socks.air_hello);
            last_hello = now;
        }

        while ((n = recvfrom(socks.hello, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL)) > 0) {
            if (buf[0] == 1) {               /* air's type-1 identity: echo the 2-byte ACK diff */
                uint8_t ack[PKT_MAX];

                memcpy(ack, buf, n);
                ack[0] = 0x02;
                if (n > 5) {
                    ack[5] = 0x00;
                }

                sendto(socks.hello, ack, n, MSG_DONTWAIT,
                       (struct sockaddr *)&socks.air_hello, sizeof socks.air_hello);
                if (!g_hs_done) {
                    g_hs_done = 1;
                    if (g_verbose) {
                        printf(TAG " :20001 3-way done (air identity %zd B, type2 ACK sent)\n", n);
                        fflush(stdout);
                    }
                }
            }
        }

        /* :10000 poll: a type-1 request every 2 s for the WHOLE session, matching the vendor's
         * ParamsTimerEvent (which never stops). The air replies 0x02 to each - harmless, no air-side
         * state change (RE of AR_FSM_TX_ProcessParamsRequest) - and those replies keep :10000 RX
         * fresh so the air-loss watch below only trips on a real baseband drop. We used to STOP this
         * poll once the consumer reported frames (g_video_confirmed); that let the air's :10000
         * telemetry go quiet and tripped a FALSE air-loss every ~5 s, so video cut out and re-
         * handshook in a ~10 s loop. Pre-media the poll is READY-gated so the air's first IDR lands
         * with a bound consumer; once acked it is the ungated keepalive, and it is also what paces
         * the IDR request (which rides the 0x02 reply). */
        if (socks.params_bound && g_hs_done && (g_no_gate || g_ready || g_params_acked)
            && now - last_req >= PARAMS_IVL_MS) {
            uint8_t frame[MP_HDR_LEN];

            sendto(socks.params, frame, mp_params_request(frame, stamp_us), MSG_DONTWAIT,
                   (struct sockaddr *)&socks.air_params, sizeof socks.air_params);
            last_req = now;

            if (g_verbose) {
                fprintf(stderr, TAG " tx MEDIA_PARAMS_REQUEST\n");
            }
        }

        /* drain :10000: params reply + telemetry/OSD; all of it counts as air liveness */
        while ((n = recvfrom(socks.params, buf, sizeof buf, MSG_DONTWAIT, NULL, NULL)) > 0) {
            g_last_telem_ms = now;
            if (g_air_lost) {
                g_air_lost = 0;
                link_event(MLM_LINK_SESSION_RESTART, "TX unit returned, re-handshaking");
                led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
                /* the periodic sender re-applies the standby-arm on its own once the air is back */
            }

            handle_params(&socks, buf, n, stamp_us);
        }

        /* air-loss watch (only meaningful once telemetry has flowed in STEADY) */
        if (g_steady && !g_air_lost && g_last_telem_ms
            && now - g_last_telem_ms > AIR_LOSS_MS) {
            g_air_lost = 1;
            g_hs_done = 0;                  /* re-arm the :20001 3-way for the next session */
            g_params_acked = 0;
            g_video_confirmed = 0;
            link_event(MLM_LINK_TX_LOST, "no :10000 traffic for 5 s");
            led_cmd(MLM_LED_BREATHE, 0xff, 0x00, 0x00, LED_BREATHE_MS);
        }

        /* Keep the air's TX power + standby-arm asserted while it is reachable. Both ride this one
         * SetTranParm (body[0] = TX power dBm, body[8] = u8StandbyModeEn). The air only enters standby
         * when the arm bit is set AND its own RC link is disarmed, and it re-evaluates on every frame,
         * so we re-send on a steady cadence - a single frame can race association or be dropped. Send
         * once either has been commanded by the HUD; the other falls back to a safe default (vendor
         * 100 mW / disarmed) so a fabricated byte never reaches the air. */
        if (socks.params_bound && (g_standby_arm >= 0 || g_power_dbm >= 0)
            && g_hs_done && !g_air_lost && now - last_stp >= STANDBY_IVL_MS) {
            uint8_t frame[MP_STP_LEN];
            uint8_t power   = (g_power_dbm >= 0) ? (uint8_t) g_power_dbm : AIR_TX_DBM;
            uint8_t standby = (g_standby_arm >= 0) ? (uint8_t) g_standby_arm : 0;

            sendto(socks.params, frame, mp_set_tran_parm(frame, power, standby, stamp_us), MSG_DONTWAIT,
                   (struct sockaddr *)&socks.air_params, sizeof socks.air_params);
            last_stp = now;

            if (g_verbose) {
                fprintf(stderr, TAG " tx SetTranParm standby=%d power=0x%02x\n", standby, power);
            }
        }

        /* (SetLdCfg is sent in the MEDIA_PARAMS 0x02-reply handler, before the 0x03 ack, to match
         * the vendor's per-cycle 01->02->0a->03 sequence - not here.) */

        if (socks.params_bound && g_params_acked && !g_air_lost) {
            push_camera_scale(&socks, stamp_us);
        }

        /* re-assert the LED ~1 Hz so a late-started/restarted ml-ledd reconverges;
         * also paints breathe-red from the first tick, before any link is up */
        if (now - last_led > LED_ASSERT_MS) {
            last_led = now;
            led_assert();
        }

        usleep(UDP_TICK_US);
    }

    close(socks.hello);
    close(socks.params);
    close(socks.link);
    unlink(MLM_LINK_SOCK);

    return NULL;
}
