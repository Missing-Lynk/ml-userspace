/**
 * @file mav-ctrl.c
 * @brief The live control socket and the whole-pipeline commands it accepts.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/* The control socket, owned here: opened by air_ctrl_open, torn down by air_ctrl_close. */
static int g_ctrl_fd = -1;
static char g_ctrl_path[108];

static int air_active_encoder_count(void)
{
    int count = 0;

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0) {
            count++;
        }
    }

    return count;
}

int air_set_bitrate_all(int bitrate, int vbv)
{
    int ret = 0;
    int active = air_active_encoder_count();
    int changed = 0;

    if (bitrate < 1 || bitrate > 700000000 || vbv < 10 || vbv > 3000) {
        return -1;
    }

    if (active == 0) {
        g_printerr("[ml-air-video] live bitrate needs the direct V4L2 encoder path\n");
        return -1;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0 &&
            (g_tile[i].enc_bitrate != bitrate || g_tile[i].enc_vbv != vbv)) {
            changed++;
        }

        /* Bitrate before the window: going down, the pair is briefly the new low rate against
         * the old short window, which is the constrained direction. The reverse order would
         * leave the old high rate against the new long window and permit a burst. */
        if (air_enc_set_bitrate(&g_tile[i], bitrate) != 0) {
            ret = -1;
        }

        if (air_enc_set_vbv(&g_tile[i], vbv) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] live bitrate %d bps/tile, vbv %d ms%s\n",
                   bitrate, vbv, changed == 0 ? " (unchanged)" : "");
    }

    return ret;
}

/* Give a receiver a decodable entry point, which is either starting the encoders or forcing an IDR.
 *
 * This stream carries exactly one IDR, at FrameId 0, and P-frames for the rest of the session, so a
 * receiver that was not listening at session start cannot decode and no amount of motion repairs it:
 * the encoder picks intra-vs-inter against its OWN reference, which is correct, so it never notices
 * that the receiver's is not. The vendor covers this with an on-demand keyframe
 * (AR_LOWDELAY_MESSAGE_MEDIA_IDR_REQUEST -> AR_FSM_TX_ProcessIdrRequest), and that handler branches
 * on whether it is already streaming: not streaming runs PIPELINE_Start, streaming adds
 * AR_LDRT_TX_PIPELINE_IdrEnable. Both branches are here, so one request covers both cases.
 *
 * Under ML_AIR_ON_DEMAND the first request opens the encoders, and their own first picture is the
 * session IDR, so nothing extra is forced. V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME is a button control,
 * so the value is ignored.
 */
int air_force_keyframe_all(void)
{
    int ret = 0;
    int forced = 0;

    if (g_on_demand && !g_atomic_int_get(&g_enc_up)) {
        if (air_enc_start_request() != 0) {
            g_printerr("[ml-air-video] keyframe: encoders did not come up\n");
            return -1;
        }

        return 0;
    }

    if (air_active_encoder_count() == 0) {
        g_printerr("[ml-air-video] keyframe needs the direct V4L2 encoder path\n");
        return -1;
    }

    for (int i = 0; i < AIR_NCHN; i++) {
        if (!g_tile[i].active || g_tile[i].enc_fd < 0) {
            continue;
        }

        if (air_enc_set_int(&g_tile[i], V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, "keyframe") != 0) {
            ret = -1;
        } else {
            forced++;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] forced a keyframe on %d tile(s)\n", forced);
    }

    return ret;
}

int air_set_fps_all(int fps)
{
    int ret = 0;
    int active = air_active_encoder_count();
    int changed = 0;

    if (fps < 1 || fps > 240) {
        return -1;
    }

    if (active == 0) {
        g_printerr("[ml-air-video] live fps needs the direct V4L2 encoder path\n");
        return -1;
    }

    /* Slow the feeder before telling rate control about the new rate. The encoder budgets
     * bitrate/fps per picture, so the window where the two disagree either under-spends (feeder
     * already slow, rate control still on the old high rate) or over-spends. Under-spending is
     * the safe side of an RF link that is sized for the requested rate. */
    g_atomic_int_set(&g_cap_fps, fps);

    for (int i = 0; i < AIR_NCHN; i++) {
        if (g_tile[i].active && g_tile[i].enc_fd >= 0 && g_tile[i].enc_fps != fps) {
            changed++;
        }

        if (air_enc_set_fps(&g_tile[i], fps) != 0) {
            ret = -1;
        }
    }

    if (ret == 0) {
        g_printerr("[ml-air-video] live fps %d%s\n", fps, changed == 0 ? " (unchanged)" : "");
    }

    return ret;
}

int air_set_rate_all(int bitrate, int fps, int vbv)
{
    int ret;

    /* Range-check both halves before applying either, so a rejected fps cannot leave the
     * encoders running at the new bitrate while the caller is told the command failed. */
    if (fps < 1 || fps > 240) {
        return -1;
    }

    ret = air_set_bitrate_all(bitrate, vbv);
    if (ret != 0) {
        return ret;
    }

    return air_set_fps_all(fps);
}

gboolean air_on_ctrl(G_GNUC_UNUSED int fd, G_GNUC_UNUSED GIOCondition cond,
                     G_GNUC_UNUSED gpointer user)
{
    char buf[256];
    char reply[128];
    int cfd;
    struct pollfd pfd;
    ssize_t n;
    int bitrate;
    int fps;
    int vbv = -1;
    int ret;

    cfd = accept4(g_ctrl_fd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) {
        return G_SOURCE_CONTINUE;
    }

    memset(&pfd, 0, sizeof pfd);
    pfd.fd = cfd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 100) <= 0) {
        close(cfd);
        return G_SOURCE_CONTINUE;
    }

    n = read(cfd, buf, sizeof buf - 1);
    if (n <= 0) {
        close(cfd);
        return G_SOURCE_CONTINUE;
    }
    buf[n] = '\0';

    ret = sscanf(buf, "rate %d %d %d", &bitrate, &fps, &vbv);
    if (ret >= 2) {
        if (vbv < 0) {
            vbv = air_vbv_for_bitrate(bitrate);
        }
        ret = air_set_rate_all(bitrate, fps, vbv);
        snprintf(reply, sizeof reply, "%s bitrate=%d fps=%d vbv=%d\n",
                 ret == 0 ? "ok" : "err", bitrate, fps, vbv);
    } else if (sscanf(buf, "bitrate %d %d", &bitrate, &vbv) >= 1) {
        if (vbv < 0) {
            vbv = air_vbv_for_bitrate(bitrate);
        }
        ret = air_set_bitrate_all(bitrate, vbv);
        snprintf(reply, sizeof reply, "%s bitrate=%d vbv=%d\n", ret == 0 ? "ok" : "err",
                 bitrate, vbv);
    } else if (sscanf(buf, "fps %d", &fps) == 1) {
        ret = air_set_fps_all(fps);
        snprintf(reply, sizeof reply, "%s fps=%d\n", ret == 0 ? "ok" : "err", fps);
    } else if (strncmp(buf, "keyframe", 8) == 0) {
        ret = air_force_keyframe_all();
        snprintf(reply, sizeof reply, "%s keyframe\n", ret == 0 ? "ok" : "err");
    } else {
        ret = -1;
        snprintf(reply, sizeof reply,
                 "err expected: bitrate <bps> [vbv] | fps <fps> | rate <bps> <fps> [vbv]"
                 " | keyframe\n");
    }

    (void)write(cfd, reply, strlen(reply));
    close(cfd);

    return G_SOURCE_CONTINUE;
}

int air_ctrl_open(const char *path)
{
    struct sockaddr_un addr;
    char *dir;

    if (strlen(path) >= sizeof addr.sun_path) {
        g_printerr("[ml-air-video] control socket path too long: %s\n", path);
        return -1;
    }

    dir = g_path_get_dirname(path);
    if (dir != NULL && g_mkdir_with_parents(dir, 0755) != 0) {
        g_printerr("[ml-air-video] mkdir %s: %s\n", dir, strerror(errno));
        g_free(dir);
        return -1;
    }
    g_free(dir);

    g_ctrl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_ctrl_fd < 0) {
        g_printerr("[ml-air-video] control socket: %s\n", strerror(errno));
        return -1;
    }

    unlink(path);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    g_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    if (bind(g_ctrl_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(g_ctrl_fd, 4) != 0) {
        g_printerr("[ml-air-video] bind %s: %s\n", path, strerror(errno));
        close(g_ctrl_fd);
        g_ctrl_fd = -1;

        return -1;
    }

    g_strlcpy(g_ctrl_path, path, sizeof g_ctrl_path);
    g_unix_fd_add(g_ctrl_fd, G_IO_IN, air_on_ctrl, NULL);
    g_printerr("[ml-air-video] control socket %s\n", path);

    return 0;
}

/** Close the control socket and remove its filesystem entry. */
void air_ctrl_close(void)
{
    if (g_ctrl_fd < 0) {
        return;
    }

    close(g_ctrl_fd);
    g_ctrl_fd = -1;

    if (g_ctrl_path[0] != '\0') {
        unlink(g_ctrl_path);
        g_ctrl_path[0] = '\0';
    }
}
