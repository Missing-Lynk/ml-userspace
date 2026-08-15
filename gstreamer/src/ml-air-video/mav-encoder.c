/**
 * @file mav-encoder.c
 * @brief The direct V4L2 encoder instances: bring-up, queueing, draining and recovery.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

int air_vbv_for_bitrate(int bitrate)
{
    guint64 limit = AIR_TX_MAX - AIR_VPH_HEADER - AIR_VPH_TAIL;
    guint64 vbv;

    if (bitrate <= 0) {
        return 10;
    }

    vbv = limit * 8000u * 3u / 4u / (guint64)bitrate;
    if (vbv < 10) {
        vbv = 10;
    } else if (vbv > 3000) {
        vbv = 3000;
    }

    return (int)vbv;
}

/** Set one integer encoder control, reporting but not failing on a control the driver lacks. */
static void air_enc_ctrl(struct air_tile *tile, guint32 id, gint32 val, const char *name)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;

    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.value = val;
    cs.count = 1;
    cs.controls = &c;
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;

    if (ioctl(tile->enc_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        g_printerr(TAG " tile %d: %s: %s\n", tile->chn, name, strerror(errno));
    }
}

/** Set one integer encoder control on a live instance, reporting the failure to the caller. */
int air_enc_set_int(struct air_tile *tile, guint32 id, gint32 val, const char *name)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;

    memset(&c, 0, sizeof c);
    memset(&cs, 0, sizeof cs);
    c.id = id;
    c.value = val;
    cs.count = 1;
    cs.controls = &c;
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;

    if (ioctl(tile->enc_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        g_printerr(TAG " tile %d: live %s %d: %s\n",
                   tile->chn, name, val, strerror(errno));
        return -1;
    }

    return 0;
}

int air_enc_set_bitrate(struct air_tile *tile, int bitrate)
{
    if (!tile->active || tile->enc_fd < 0 || tile->enc_bitrate == bitrate) {
        return 0;
    }

    if (air_enc_set_int(tile, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate") != 0) {
        return -1;
    }

    tile->enc_bitrate = bitrate;
    return 0;
}

int air_enc_set_vbv(struct air_tile *tile, int vbv)
{
    if (!tile->active || tile->enc_fd < 0 || tile->enc_vbv == vbv) {
        return 0;
    }

    if (air_enc_set_int(tile, V4L2_CID_MPEG_VIDEO_VBV_SIZE, vbv, "vbv") != 0) {
        return -1;
    }

    tile->enc_vbv = vbv;
    return 0;
}

int air_enc_set_fps(struct air_tile *tile, int fps)
{
    struct v4l2_streamparm sp;

    if (!tile->active || tile->enc_fd < 0) {
        return 0;
    }

    if (tile->enc_fps == fps) {
        return 0;
    }

    memset(&sp, 0, sizeof sp);
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = (guint32)fps;
    if (ioctl(tile->enc_fd, VIDIOC_S_PARM, &sp) != 0) {
        g_printerr(TAG " tile %d: live fps %d: %s\n",
                   tile->chn, fps, strerror(errno));
        return -1;
    }

    tile->fps_pending = fps;
    tile->enc_fps = fps;
    return 0;
}

/** Find the node that encodes to HEVC, by capability rather than by index.
 *
 * The camera node is skipped by path: it is already open, and a node that offers HEVC on its
 * capture queue is what makes an encoder, so probing is the only thing that stays true if the
 * enumeration order moves. ML_AIR_ENC_NODE overrides the search. */
int air_enc_find_node(const char *camera, char *out, size_t len)
{
    const char *forced = getenv("ML_AIR_ENC_NODE");

    if (forced != NULL) {
        g_strlcpy(out, forced, len);
        return 0;
    }

    for (int i = 0; i < 16; i++) {
        struct v4l2_fmtdesc d;
        char path[32];
        int fd;
        int hit = 0;

        g_snprintf(path, sizeof path, "/dev/video%d", i);
        if (camera != NULL && strcmp(path, camera) == 0) {
            continue;
        }

        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        memset(&d, 0, sizeof d);
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        for (d.index = 0; hit == 0 && ioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++) {
            if (d.pixelformat == V4L2_PIX_FMT_HEVC) {
                hit = 1;
            }
        }

        close(fd);

        if (hit) {
            g_strlcpy(out, path, len);
            return 0;
        }
    }

    g_printerr(TAG " no node offers HEVC on its capture queue\n");
    return -1;
}

/** Open and configure one encoder instance for this tile.
 *
 * The whole reason this path exists is the OUTPUT S_FMT below: bytesperline comes from the
 * capture node, not from the picture width, and the driver's answer is checked. A stride the
 * encoder silently rewrote produced two sessions of sheared video that every counter reported
 * as healthy, so a mismatch fails the run here rather than becoming a picture nobody decodes. */
int air_enc_open(struct air_tile *tile, const char *dev, int fps)
{
    struct v4l2_format f;
    struct v4l2_requestbuffers rb;
    struct v4l2_streamparm sp;

    /* Vendor AR_8030_TX_GetBitRate() derives the encoder target from live RF throughput
     * (throughput * Ar803xThroutputRate, capped at ArMaxBitRate) and returns 8000 kbps when
     * throughput reads zero. 8000 kbps is the total across both tiles, so half it here. Link
     * adaptation should drive the live control path rather than move this default. */
    int bitrate = tile->enc_bitrate > 0 ? tile->enc_bitrate
                                        : atoi(air_env_or("ML_AIR_BITRATE", "4000000"));
    int vbv = tile->enc_vbv > 0 ? tile->enc_vbv : atoi(air_env_or("ML_AIR_VBV", "0"));
    enum v4l2_buf_type otype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int plane;
    int i;

    /* O_NONBLOCK is load bearing: air_enc_drain empties both queues by dequeuing until DQBUF
     * fails, which on a blocking fd never happens. */
    tile->enc_fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (tile->enc_fd < 0) {
        g_printerr(TAG " tile %d: open %s: %s\n", tile->chn, dev, strerror(errno));
        return -1;
    }

    memset(&f, 0, sizeof f);
    f.type = otype;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    f.fmt.pix_mp.width = AIR_COMP_W;
    f.fmt.pix_mp.height = (guint32)tile->height;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = AIR_CAP_PLANES;
    for (plane = 0; plane < AIR_CAP_PLANES; plane++) {
        f.fmt.pix_mp.plane_fmt[plane].bytesperline = g_cap_stride[plane];
        f.fmt.pix_mp.plane_fmt[plane].sizeimage = (guint32)air_cap_len(tile, plane);
    }

    if (ioctl(tile->enc_fd, VIDIOC_S_FMT, &f) != 0) {
        g_printerr(TAG " tile %d: S_FMT OUTPUT: %s\n", tile->chn, strerror(errno));
        return -1;
    }

    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420M ||
        f.fmt.pix_mp.num_planes != AIR_CAP_PLANES) {
        g_printerr(TAG " tile %d: encoder substituted the source format\n", tile->chn);
        return -1;
    }

    for (plane = 0; plane < AIR_CAP_PLANES; plane++) {
        guint32 bpl = f.fmt.pix_mp.plane_fmt[plane].bytesperline;
        guint32 want = g_cap_stride[plane];

        g_printerr(TAG " tile %d: enc plane %d bytesperline %u (asked %u), "
                   "sizeimage %u (buffer has %zu)\n",
                   tile->chn, plane, bpl, want, f.fmt.pix_mp.plane_fmt[plane].sizeimage,
                   air_cap_len(tile, plane));

        if (bpl != want) {
            g_printerr(TAG " tile %d: encoder rewrote the source stride; "
                       "refusing to encode a sheared picture\n", tile->chn);
            return -1;
        }

        if (f.fmt.pix_mp.plane_fmt[plane].sizeimage > air_cap_len(tile, plane)) {
            g_printerr(TAG " tile %d: encoder demands more than the capture "
                       "buffer holds on plane %d\n", tile->chn, plane);
            return -1;
        }
    }

    if (fps > 0) {
        memset(&sp, 0, sizeof sp);
        sp.type = otype;
        sp.parm.output.timeperframe.numerator = 1;
        sp.parm.output.timeperframe.denominator = (guint32)fps;
        if (ioctl(tile->enc_fd, VIDIOC_S_PARM, &sp) != 0) {
            g_printerr(TAG " tile %d: S_PARM %d fps: %s\n",
                       tile->chn, fps, strerror(errno));
            return -1;
        }
    }

    memset(&f, 0, sizeof f);
    f.type = ctype;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    f.fmt.pix_mp.width = AIR_COMP_W;
    f.fmt.pix_mp.height = (guint32)tile->height;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;

    if (ioctl(tile->enc_fd, VIDIOC_S_FMT, &f) != 0) {
        g_printerr(TAG " tile %d: S_FMT CAPTURE HEVC: %s\n", tile->chn, strerror(errno));
        return -1;
    }

    if (vbv < 10) {
        vbv = air_vbv_for_bitrate(bitrate);
    }

    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_VBV_SIZE, vbv, "vbv size");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
                 V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                 atoi(air_env_or("ML_AIR_GOP", "0")), "gop size");

    /* Repeat VPS/SPS/PPS on every IDR. The stream carries one IDR at session start and parameter
     * sets are sent once, so a receiver that joins later has neither - a forced keyframe on its own
     * gives it a picture it cannot configure a decoder for. Seq-init parameter, so it has to be set
     * before streaming starts, not alongside the keyframe request. */
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "prepend sps/pps to idr");

    /* No access-unit delimiters. wave5 defaults this control to 1, prepending a 7-byte AUD to every
     * access unit; the vendor encoder emits none.
     *
     * AR_LDRT_RX_VDEC_RecvStreamCheck in libldrt_pipeline.so validates the IDR access-unit head
     * against twelve hard-coded bytes - start code, 0x40 0x01 (VPS) at [4], a second start code at
     * [39], 0x42 0x01 (SPS) - and on mismatch logs "Parser IDR Stream Error", resets the decoder and
     * drops the frame. The AUD shifts the VPS by 7 bytes and fails five of the twelve. Our own
     * goggle ignores AUDs, so this is invisible on our own link. */
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_AU_DELIMITER, 0, "au delimiter");

    /* Level 4.0, which is what a vendor IDR carries (general_level_idc 120). Unset, our VPS goes out
     * with general_level_idc 10: not a defined HEVC level (valid are 30/60/63/90/93/120), and read
     * as Level 1 it permits 36864 luma samples against the 1,075,200 of a 1920x560 tile. A receiver
     * sizing decode or display buffers from the level then sizes for QCIF. Our own goggle sizes
     * from the SPS dimensions, so this is invisible on our own link.
     *
     * wave5 maps LEVEL_4 to 40 * 3 = 120. Verify on the wire (glue/capture/vph-sniff.c): the
     * registered default LEVEL_1 should give 30 and measured 10. */
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_MPEG_VIDEO_HEVC_LEVEL_4, "hevc level");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "frame rc");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE,
                 atoi(air_env_or("ML_AIR_MBRC", "1")), "mb rc");
    /* The QP floor bounds peak access-unit size on this path. 15 is the vendor air unit's value for
     * both I and P (qpMinI / qpMinP in stVencTxMap, the table its ar_lowdelay feeds to
     * AR_CFG_VENC_LoadDefault; qpMax 51 already matches). The MinIQp 0 in the rcParam comparison in
     * docs/air-video-benchmark.md is venc8, the GOGGLE's RTSP re-encode channel, not this one.
     *
     * Nothing else bounds a picture here. VBV is derived to three quarters of a datagram but shapes
     * the average, not the peak (measured 5.6x over budget on hard content). The vendor's
     * u8KeyFrameMultiplier / u8NonKeyFrameMultiplier are bitstream BUFFER sizing, not rate limits:
     * libmpp_service computes align16(multiplier * u32BufSize / 8), ~806 kB for a 1920x560 tile.
     * Bitrate is not the lever either - the vendor stream runs 5.35 Mbit/s per tile, above our
     * default, and peaks at 18919 B (0.14 bpp) where a floor of 0 gave a 75970 B IDR (0.57 bpp). */
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,
                 atoi(air_env_or("ML_AIR_MINQP", "15")), "min qp");
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
                 atoi(air_env_or("ML_AIR_MAXQP", "51")), "max qp");

    /* The QP the first intra picture of the instance is coded at, and the reason the first IDR of
     * a session is the largest access unit the link ever carries. Rate control has no history at
     * picture 0, so wave5 opens with initial_rc_qp -1 and the firmware codes that picture at
     * intra_qp; from picture 1 the loop takes over. The vendor's own default template carries
     * intraQP 35 (ar_video_h26x_enc_exe_gen_default_param in libmpp_service.so; the recovered
     * table is in docs/air-video-benchmark.md) and its session-opening IDR measures exactly QP 35
     * on the wire, so 35 is the vendor's value rather than an inference from it. Read it back off
     * a dump with glue/capture/hevc-slice-qp.py.
     */
    tile->enc_qp = atoi(air_env_or("ML_AIR_IQP", "35"));
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, tile->enc_qp, "i frame qp");

    /* Vendor chromaCbQpOffset / chromaCrQpOffset, both -2. One control drives both, and the
     * driver registers it under its H.264 name for every codec it offers.
     */
    air_enc_ctrl(tile, V4L2_CID_MPEG_VIDEO_H264_CHROMA_QP_INDEX_OFFSET,
                 atoi(air_env_or("ML_AIR_CHROMAQP", "-2")), "chroma qp offset");

    memset(&rb, 0, sizeof rb);
    rb.type = otype;
    rb.memory = V4L2_MEMORY_DMABUF;
    rb.count = AIR_ENC_OUT_BUFS;
    if (ioctl(tile->enc_fd, VIDIOC_REQBUFS, &rb) != 0) {
        g_printerr(TAG " tile %d: REQBUFS OUTPUT: %s\n", tile->chn, strerror(errno));
        return -1;
    }
    tile->enc_out_n = (int)rb.count;

    memset(&rb, 0, sizeof rb);
    rb.type = ctype;
    rb.memory = V4L2_MEMORY_MMAP;
    rb.count = AIR_ENC_CAP_BUFS;
    if (ioctl(tile->enc_fd, VIDIOC_REQBUFS, &rb) != 0) {
        g_printerr(TAG " tile %d: REQBUFS CAPTURE: %s\n", tile->chn, strerror(errno));
        return -1;
    }

    tile->enc_cap_n = (int)rb.count;
    for (i = 0; i < tile->enc_cap_n; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane pl;

        memset(&b, 0, sizeof b);
        memset(&pl, 0, sizeof pl);
        b.type = ctype;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned int)i;
        b.length = 1;
        b.m.planes = &pl;
        if (ioctl(tile->enc_fd, VIDIOC_QUERYBUF, &b) != 0) {
            g_printerr(TAG " tile %d: QUERYBUF %d: %s\n", tile->chn, i, strerror(errno));
            return -1;
        }

        tile->enc_cap_len[i] = pl.length;
        tile->enc_cap_map[i] = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, tile->enc_fd,
                                 (off_t)pl.m.mem_offset);
        if (tile->enc_cap_map[i] == MAP_FAILED) {
            tile->enc_cap_map[i] = NULL;
            g_printerr(TAG " tile %d: mmap capture %d: %s\n",
                       tile->chn, i, strerror(errno));
            return -1;
        }

        if (ioctl(tile->enc_fd, VIDIOC_QBUF, &b) != 0) {
            g_printerr(TAG " tile %d: QBUF capture %d: %s\n",
                       tile->chn, i, strerror(errno));
            return -1;
        }
    }

    if (ioctl(tile->enc_fd, VIDIOC_STREAMON, &otype) != 0 ||
        ioctl(tile->enc_fd, VIDIOC_STREAMON, &ctype) != 0) {
        g_printerr(TAG " tile %d: STREAMON: %s\n", tile->chn, strerror(errno));
        return -1;
    }

    /* Both are what recovery needs to rebuild this instance, and the stamp is what keeps the
     * stall watchdog from reading an unstarted encoder as one that has been silent since boot. */
    if (dev != tile->enc_node) {
        g_strlcpy(tile->enc_node, dev, sizeof tile->enc_node);
    }
    tile->enc_fps = fps;
    tile->enc_bitrate = bitrate;
    tile->enc_vbv = vbv;
    tile->enc_progress_us = g_get_monotonic_time();

    return 0;
}

/** Queue one capture buffer into this tile's encoder at the tile's row offset. Takes a
 * reference on the buffer, released when the OUTPUT buffer comes back. */
int air_enc_queue(struct air_tile *tile, struct air_cap_buf *capbuf)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[AIR_CAP_PLANES];
    int idx = -1;

    for (int i = 0; i < tile->enc_out_n; i++) {
        if (tile->enc_out_cb[i] == NULL) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        return -1;
    }

    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = (unsigned int)idx;
    b.length = AIR_CAP_PLANES;
    b.m.planes = pl;

    for (int plane = 0; plane < AIR_CAP_PLANES; plane++) {
        pl[plane].m.fd = capbuf->fd[plane];
        pl[plane].length = (unsigned int)capbuf->len[plane];
        pl[plane].data_offset = (unsigned int)air_cap_off(tile, plane);
        pl[plane].bytesused = pl[plane].data_offset + (unsigned int)air_cap_len(tile, plane);
    }

    if (ioctl(tile->enc_fd, VIDIOC_QBUF, &b) != 0) {
        tile->lost++;
        return -1;
    }

    tile->enc_out_cb[idx] = capbuf;
    tile->pushed++;
    return 0;
}

/** Drain whatever this tile's encoder has ready: coded access units out, spent source buffers
 * back. Non-blocking; the caller polls. */
void air_enc_drain(struct air_tile *tile)
{
    struct v4l2_buffer b;
    struct v4l2_plane pl[AIR_CAP_PLANES];

    for (;;) {
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_DMABUF;
        b.length = AIR_CAP_PLANES;
        b.m.planes = pl;
        if (ioctl(tile->enc_fd, VIDIOC_DQBUF, &b) != 0) {
            break;
        }
        tile->enc_progress_us = g_get_monotonic_time();

        if (b.index < (unsigned int)tile->enc_out_n && tile->enc_out_cb[b.index] != NULL) {
            air_cap_release(tile->enc_out_cb[b.index]);
            tile->enc_out_cb[b.index] = NULL;
        }
    }

    for (;;) {
        memset(&b, 0, sizeof b);
        memset(pl, 0, sizeof pl);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = 1;
        b.m.planes = pl;
        if (ioctl(tile->enc_fd, VIDIOC_DQBUF, &b) != 0) {
            break;
        }

        tile->enc_progress_us = g_get_monotonic_time();

        if (b.index < (unsigned int)tile->enc_cap_n && tile->enc_cap_map[b.index] != NULL &&
            pl[0].bytesused > 0) {
            air_emit_au(tile, tile->enc_cap_map[b.index] + pl[0].data_offset,
                        pl[0].bytesused - pl[0].data_offset, tile->enc_seq++,
                        (b.flags & V4L2_BUF_FLAG_KEYFRAME) ? 1 : 0);
        }

        if (ioctl(tile->enc_fd, VIDIOC_QBUF, &b) != 0) {
            g_printerr(TAG " tile %d: requeue capture %u: %s\n",
                       tile->chn, b.index, strerror(errno));
            break;
        }
    }
}

/** Capture frames this encoder has taken and not yet given back. */
int air_enc_held(const struct air_tile *tile)
{
    int held = 0;
    int i;

    for (i = 0; i < tile->enc_out_n; i++) {
        if (tile->enc_out_cb[i] != NULL) {
            held++;
        }
    }

    return held;
}

/** Recover a stalled encoder by rebuilding the instance, and keep the stream alive either way.
 *
 * Cycling only the OUTPUT queue would be cheaper and was tried first; it does not work. wave5's
 * stop_streaming calls switch_state(inst, VPU_INST_STATE_STOP) whenever both queues are
 * streaming, and start_streaming's re-initialisation is guarded by inst->state ==
 * VPU_INST_STATE_OPEN, so a re-STREAMON on a STOP instance skips initialize_sequence and
 * prepare_fb and never reaches PIC_RUN again. The driver has no in-place restart: state leaves
 * NONE once and only a fresh open returns there.
 *
 * So this closes and re-opens. That does build a new encoder instance, which is the operation
 * this part has historically watchdogged on, but the alternative is an encoder that is already
 * dead. Attempts are capped: past AIR_ENC_MAX_RESTARTS the tile is retired rather than thrashed,
 * and the other tile and the capture loop carry on without it.
 *
 * Either way the held capture frames are released. They are the reason this cannot be ignored:
 * until they come back g_cap_inflight never falls, and a stall on one tile would starve the
 * whole pipeline instead of just that tile. Frames in flight are lost; the stream continues. */
void air_enc_restart(struct air_tile *tile)
{
    if (tile->enc_restarts >= AIR_ENC_MAX_RESTARTS) {
        g_printerr(TAG " tile %d: %u recoveries, retiring the tile\n",
                   tile->chn, tile->enc_restarts);
        air_enc_close(tile);
        tile->active = 0;
        return;
    }

    tile->enc_restarts++;
    air_enc_close(tile);

    if (air_enc_open(tile, tile->enc_node, tile->enc_fps) != 0) {
        g_printerr(TAG " tile %d: recovery re-open failed, retiring the tile\n",
                   tile->chn);
        air_enc_close(tile);
        tile->active = 0;
        return;
    }

    tile->enc_progress_us = g_get_monotonic_time();
}

/** STREAMOFF both queues and close. STREAMOFF returns every queued buffer, so the capture
 * references this tile still holds are dropped here rather than leaked; wave5 needs the
 * instance torn down cleanly or its next open finds the firmware busy. */
void air_enc_close(struct air_tile *tile)
{
    enum v4l2_buf_type otype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int i;

    if (tile->enc_fd < 0) {
        return;
    }

    ioctl(tile->enc_fd, VIDIOC_STREAMOFF, &otype);
    ioctl(tile->enc_fd, VIDIOC_STREAMOFF, &ctype);

    for (i = 0; i < tile->enc_out_n; i++) {
        if (tile->enc_out_cb[i] != NULL) {
            air_cap_release(tile->enc_out_cb[i]);
            tile->enc_out_cb[i] = NULL;
        }
    }

    for (i = 0; i < tile->enc_cap_n; i++) {
        if (tile->enc_cap_map[i] != NULL) {
            munmap(tile->enc_cap_map[i], tile->enc_cap_len[i]);
            tile->enc_cap_map[i] = NULL;
        }
    }

    close(tile->enc_fd);
    tile->enc_fd = -1;
}
