/**
 * @file mav-capture.c
 * @brief The ar-cvisp capture node and the feeder thread that owns the encoder fds.
 *
 * Part of ml-air-video; shared types and cross-file declarations in ml-air-video.h.
 */
#include "ml-air-video.h"

/*
 * Zero-copy camera capture.
 *
 * The capture node is driven directly rather than through v4l2src, for one reason: the buffer
 * count. The block completes a buffer only when another is queued, so every buffer the encoder
 * holds is one the rotation cannot use, and the count is what decides whether capture and encode
 * pipeline at all. REQBUFS takes it as an argument; gst-v4l2 derives it from a negotiation whose
 * inputs (the driver's V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, which this node does not implement, and
 * a downstream allocation query appsink does not answer) are not ours to set. Driving the node
 * also removes the appsink queue and the videorate element from the latency path.
 *
 * Each plane of each buffer is exported once with EXPBUF and wrapped in a GstMemory that lives
 * for the process. A frame is then two GstBuffers of three gst_memory_share() views into those
 * memories, at the tile's row offset. No allocation, no copy, no CPU read.
 *
 * The share carries its offset into V4L2 as the plane's data_offset, and the video meta's wider
 * stride makes gst-v4l2 re-run S_FMT on the encoder's OUTPUT queue at 2048 bytes, which is what
 * wave5_widen_src_stride() exists to accept.
 */

/* The capture node and its buffer rotation, owned here. Declared in ml-air-video.h. */
int g_cap_fd = -1;
static struct air_cap_buf g_cap[AIR_CAP_BUFS_MAX];
static int g_cap_n;
int g_cap_inflight_max;
int g_cap_fps;
gint g_cap_inflight;
guint32 g_cap_stride[AIR_CAP_PLANES];
volatile int g_cap_stop;
volatile guint64 g_cap_frames;
volatile guint64 g_cap_skipped;

static void air_cap_qbuf(struct air_cap_buf *capbuf)
{
    struct v4l2_plane planes[AIR_CAP_PLANES];
    struct v4l2_buffer b;

    /* Tile buffers outlive the node: the encoder pipelines are torn down after air_cap_close,
     * so the last releases arrive with the fd already gone. Not an error, just late. */
    if (g_cap_fd < 0) {
        return;
    }

    memset(planes, 0, sizeof planes);
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = capbuf->index;
    b.length = AIR_CAP_PLANES;
    b.m.planes = planes;

    if (ioctl(g_cap_fd, VIDIOC_QBUF, &b) != 0) {
        perror("[ml-air-video] capture QBUF");
    }
}

/** Last tile buffer sharing this capture buffer was finalized: the encoders are done reading it,
 * so it goes back to the rotation. Runs on whichever thread finalized the tile buffer. */
void air_cap_release(gpointer data)
{
    struct air_cap_buf *capbuf = data;

    if (g_atomic_int_dec_and_test(&capbuf->refs)) {
        g_atomic_int_add(&g_cap_inflight, -1);
        air_cap_qbuf(capbuf);
    }
}

/** Byte offset of a tile's first row within a capture plane. */
gsize air_cap_off(const struct air_tile *tile, int plane)
{
    if (plane == 0) {
        return (gsize)tile->crop_y * g_cap_stride[0];
    }

    return (gsize)(tile->crop_y / 2) * g_cap_stride[plane];
}

/** Bytes of a capture plane a tile covers.
 *
 * The ALIGNED height, not the coded one. The encoder aligns its source geometry to 16 rows, so
 * s_fmt on a 552-row tile reports a 560-row plane, gst-v4l2 accumulates its plane offsets from
 * those sizeimages, and the alignment set on the video meta below computes 560-row plane sizes.
 * Sharing only 552 rows would make all three disagree with each other.
 *
 * The extra rows are read into the encoder's source buffer and never coded, because the coded
 * height stays the requested 552 (kernel patch 0280 item 6). Tile 1 then ends flush against the
 * end of the capture plane: 528 + 560 = 1088 rows, which is exactly what the block allocates.
 */
gsize air_cap_len(const struct air_tile *tile, int plane)
{
    if (plane == 0) {
        return (gsize)tile->alloc_h * g_cap_stride[0];
    }

    return (gsize)(tile->alloc_h / 2) * g_cap_stride[plane];
}

/** Build one tile's encoder input as three shared views of @p capbuf, with no copy.
 *
 * The video meta carries the capture strides, not the picture width. That is what makes
 * gst-v4l2 re-negotiate the encoder's OUTPUT format to a 2048-byte luma stride; without it the
 * encoder would read 1920-byte rows out of a 2048-byte-pitch frame and shear the picture.
 */
static GstBuffer *air_cap_share(struct air_tile *tile, struct air_cap_buf *capbuf, GstClockTime pts)
{
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, 0, 0, 0 };
    gint stride[GST_VIDEO_MAX_PLANES] = { 0, 0, 0, 0 };
    GstVideoAlignment align;
    GstVideoMeta *meta;
    GstBuffer *buf = gst_buffer_new();
    gsize flat = 0;

    for (int plane = 0; plane < AIR_CAP_PLANES; plane++) {
        gsize len = air_cap_len(tile, plane);

        gst_buffer_append_memory(buf,
                                 gst_memory_share(capbuf->mem[plane],
                                                  (gssize)air_cap_off(tile, plane),
                                                  (gssize)len));
        offset[plane] = flat;
        stride[plane] = (gint)g_cap_stride[plane];
        flat += len;
    }

    GST_BUFFER_PTS(buf) = pts;
    meta = gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_I420,
                                          AIR_COMP_W, tile->height, AIR_CAP_PLANES, offset, stride);

    /* The padding has to be stated, not just implied by the strides. A meta carrying offsets and
     * strides but no alignment fails its own consistency check: gst_video_meta_get_plane_size
     * recomputes the strides from the picture size with zero alignment, gets 1920/960/960 against
     * our 2048/1024/1024, and returns FALSE. gst_video_meta_get_plane_height then returns FALSE
     * without writing anything, and gst-v4l2 passes a zero padded_height into
     * gst_v4l2_object_match_buffer_layout, which drops back to the current format height and
     * never sets obj->align.padding_bottom.
     *
     * padding_right is in pixels: 2048 - 1920 = 128 gives luma stride 2048 and chroma 1024.
     * padding_bottom is the 16-row source alignment the encoder applies anyway, 8 rows on the
     * 552 tile and 0 on the 560 one. With both set the meta reproduces exactly the plane sizes
     * air_cap_len shares, so plane_height comes out as the aligned height and every number
     * gst-v4l2 derives matches what the driver reports.
     */
    gst_video_alignment_reset(&align);
    align.padding_right = g_cap_stride[0] - AIR_COMP_W;
    align.padding_bottom = (guint)(tile->alloc_h - tile->height);
    if (!gst_video_meta_set_alignment(meta, align)) {
        g_printerr("[ml-air-video] tile %d: video meta rejected padding %u right %u bottom\n",
                   tile->chn, align.padding_right, align.padding_bottom);
    }

    gst_mini_object_set_qdata(GST_MINI_OBJECT(buf), g_recycle_quark, capbuf, air_cap_release);
    return buf;
}

/** Copy one tile out of the capture buffer into a pool buffer, packed at the coded height.
 *
 * The same layout air_fill_tile produces, so air_wrap_tile takes it unchanged. Reads come out of
 * a no-map coherent carveout and are therefore uncached, which is the whole cost: about 1.6 MB
 * per tile per frame, against zero for a share.
 */
static void air_cap_fill(struct air_tile *tile, int idx, struct air_cap_buf *capbuf)
{
    guint8 *dst = tile->pool[idx].map;
    const int ys = AIR_COMP_W;
    const int cs = AIR_COMP_W / 2;
    guint8 *d_y = dst;
    guint8 *d_cb = dst + (gsize)ys * tile->height;
    guint8 *d_cr = d_cb + (gsize)cs * (tile->height / 2);
    int row;

    air_dmabuf_sync(tile->pool[idx].fd, 1);

    for (row = 0; row < tile->height; row++) {
        memcpy(d_y + (gsize)row * ys,
               capbuf->map[0] + (gsize)(tile->crop_y + row) * g_cap_stride[0], AIR_COMP_W);
    }

    for (row = 0; row < tile->height / 2; row++) {
        memcpy(d_cb + (gsize)row * cs,
               capbuf->map[1] + (gsize)(tile->crop_y / 2 + row) * g_cap_stride[1], cs);
        memcpy(d_cr + (gsize)row * cs,
               capbuf->map[2] + (gsize)(tile->crop_y / 2 + row) * g_cap_stride[2], cs);
    }

    air_dmabuf_sync(tile->pool[idx].fd, 0);
}

/** Open the capture node, export every plane of every buffer, and start streaming.
 * Returns 0 on success. */
int air_cap_open(const char *dev, int want)
{
    struct v4l2_requestbuffers req;
    struct v4l2_format fmt;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (want < 2) {
        want = 2;
    }

    if (want > AIR_CAP_BUFS_MAX) {
        want = AIR_CAP_BUFS_MAX;
    }

    g_cap_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (g_cap_fd < 0) {
        g_printerr("[ml-air-video] open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    /* The node's geometry is not negotiable (it is the geometry the block is configured for),
     * so this reads the format rather than proposing one, and the strides come from the driver
     * instead of from a constant here that could drift from it. */
    memset(&fmt, 0, sizeof fmt);
    fmt.type = type;
    if (ioctl(g_cap_fd, VIDIOC_G_FMT, &fmt) != 0) {
        g_printerr("[ml-air-video] %s G_FMT: %s\n", dev, strerror(errno));
        return -1;
    }

    if (fmt.fmt.pix_mp.num_planes != AIR_CAP_PLANES ||
        fmt.fmt.pix_mp.width != AIR_COMP_W || fmt.fmt.pix_mp.height != AIR_COMP_H) {
        g_printerr("[ml-air-video] %s is %ux%u in %u planes, expected %dx%d in %d\n",
                   dev, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
                   fmt.fmt.pix_mp.num_planes, AIR_COMP_W, AIR_COMP_H, AIR_CAP_PLANES);
        return -1;
    }

    for (int plane = 0; plane < AIR_CAP_PLANES; plane++) {
        g_cap_stride[plane] = fmt.fmt.pix_mp.plane_fmt[plane].bytesperline;
    }

    memset(&req, 0, sizeof req);
    req.count = (unsigned int)want;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_cap_fd, VIDIOC_REQBUFS, &req) != 0) {
        g_printerr("[ml-air-video] %s REQBUFS %d: %s\n", dev, want, strerror(errno));
        return -1;
    }

    if (req.count > AIR_CAP_BUFS_MAX) {
        req.count = AIR_CAP_BUFS_MAX;
    }
    g_cap_n = (int)req.count;

    for (int i = 0; i < g_cap_n; i++) {
        struct v4l2_plane planes[AIR_CAP_PLANES];
        struct v4l2_buffer b;
        struct air_cap_buf *capbuf = &g_cap[i];

        memset(planes, 0, sizeof planes);
        memset(&b, 0, sizeof b);
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (unsigned int)i;
        b.length = AIR_CAP_PLANES;
        b.m.planes = planes;
        if (ioctl(g_cap_fd, VIDIOC_QUERYBUF, &b) != 0) {
            g_printerr("[ml-air-video] QUERYBUF %d: %s\n", i, strerror(errno));
            return -1;
        }

        capbuf->index = (unsigned int)i;
        for (int plane = 0; plane < AIR_CAP_PLANES; plane++) {
            struct v4l2_exportbuffer exp;

            memset(&exp, 0, sizeof exp);
            exp.type = type;
            exp.index = (unsigned int)i;
            exp.plane = (unsigned int)plane;
            exp.flags = O_RDWR | O_CLOEXEC;
            if (ioctl(g_cap_fd, VIDIOC_EXPBUF, &exp) != 0) {
                g_printerr("[ml-air-video] EXPBUF %d plane %d: %s\n", i, plane, strerror(errno));
                return -1;
            }

            capbuf->len[plane] = planes[plane].length;
            /* gst_dmabuf_allocator_alloc takes the fd, so the V4L2 path gets its own. */
            capbuf->fd[plane] = dup(exp.fd);
            capbuf->mem[plane] = gst_dmabuf_allocator_alloc(g_dmabuf_alloc, exp.fd,
                                                            capbuf->len[plane]);
            if (capbuf->fd[plane] < 0) {
                g_printerr("[ml-air-video] dup(dmabuf) %d plane %d: %s\n",
                           i, plane, strerror(errno));
                return -1;
            }

            /* Only the ML_AIR_COPY path reads this. Mapped unconditionally because it costs one
             * mmap per plane at startup and nothing per frame, and because a tile switching to
             * the copy path mid-run would otherwise need the node re-opened. */
            capbuf->map[plane] = mmap(NULL, capbuf->len[plane], PROT_READ, MAP_SHARED, g_cap_fd,
                              (off_t)planes[plane].m.mem_offset);
            if (capbuf->map[plane] == MAP_FAILED) {
                capbuf->map[plane] = NULL;
            }
        }

        air_cap_qbuf(capbuf);
    }

    /* A tile that runs off the end of a plane is rejected by the kernel at QBUF time, one frame
     * at a time and with nothing said about why. Check it once, here, where the numbers are. */
    for (int i = 0; i < AIR_NCHN; i++) {
        struct air_tile *tile = &g_tile[i];

        if (!tile->active) {
            continue;
        }

        for (int plane = 0; plane < AIR_CAP_PLANES; plane++) {
            if (tile->copy && g_cap[0].map[plane] == NULL) {
                g_printerr("[ml-air-video] tile %d is ML_AIR_COPY but plane %d did not mmap\n",
                           tile->chn, plane);
                return -1;
            }

            if (air_cap_off(tile, plane) + air_cap_len(tile, plane) > g_cap[0].len[plane]) {
                g_printerr("[ml-air-video] tile %d plane %d: rows %d..%d at stride %u needs "
                           "%" G_GSIZE_FORMAT " B of a %" G_GSIZE_FORMAT " B plane\n",
                           tile->chn, plane, tile->crop_y, tile->crop_y + tile->height,
                           g_cap_stride[plane],
                           air_cap_off(tile, plane) + air_cap_len(tile, plane),
                           g_cap[0].len[plane]);
                return -1;
            }
        }
    }

    if (ioctl(g_cap_fd, VIDIOC_STREAMON, &type) != 0) {
        g_printerr("[ml-air-video] STREAMON: %s\n", strerror(errno));
        return -1;
    }

    g_printerr("[ml-air-video] camera %s: %d buffers, strides %u/%u/%u, up to %d in flight\n",
               dev, g_cap_n, g_cap_stride[0], g_cap_stride[1], g_cap_stride[2],
               g_cap_inflight_max);
    return 0;
}

/** Build the feeder's poll set: the camera first, then whichever encoders are open.
 *
 * Called again after an on-demand bring-up, so the set is no longer fixed for the life of the
 * thread. Only the feeder may call it: it owns both the set and the encoder fds. */
int air_cap_poll_build(struct pollfd *pfd, int *enc_slot)
{
    int nfd = 1;

    memset(pfd, 0, sizeof *pfd * (1 + AIR_NCHN));
    pfd[0].fd = g_cap_fd;
    pfd[0].events = POLLIN;

    for (int i = 0; i < AIR_NCHN; i++) {
        enc_slot[i] = -1;

        if (g_enc_direct && g_tile[i].active && g_tile[i].enc_fd >= 0) {
            enc_slot[i] = nfd;
            pfd[nfd].fd = g_tile[i].enc_fd;
            pfd[nfd].events = POLLIN | POLLOUT;
            nfd++;
        }
    }

    return nfd;
}

/** Open every active tile's encoder. Feeder-thread only.
 *
 * All or nothing: a tile that fails takes the others down with it, so a later request retries from
 * a clean state rather than running half a pipeline. The encoder pair gets one clean open per boot,
 * so this must not be called again once it has succeeded, and nothing tears it down on receiver
 * loss. */
static int air_enc_start_all(void)
{
    int opened = 0;

    for (int i = 0; i < AIR_NCHN; i++) {
        if (!g_tile[i].active) {
            continue;
        }

        if (g_tile[i].enc_fd < 0 &&
            air_enc_open(&g_tile[i], g_tile[i].enc_node, g_tile[i].enc_fps) != 0) {
            for (int j = 0; j < AIR_NCHN; j++) {
                air_enc_close(&g_tile[j]);
            }

            return -1;
        }

        opened++;
    }

    return opened > 0 ? 0 : -1;
}

/* Hand-off for the deferred encoder bring-up, owned here: the request is raised by whoever calls
 * air_enc_start_request and serviced on the feeder thread, which owns the capture fds. */
static GMutex g_enc_start_lock;
static GCond g_enc_start_cond;
static int g_enc_start_want;

/** Ask the feeder to bring the encoders up, and wait for the answer.
 *
 * Called from the main loop (the control socket). Returns 0 once they are streaming. Single-flight:
 * a second caller arriving mid-bring-up waits on the same attempt rather than starting another. A
 * failure returns the request to idle so the next one retries, which matters because the receiver
 * keeps asking. */
int air_enc_start_request(void)
{
    gint64 deadline;
    int ret;

    if (g_atomic_int_get(&g_enc_up)) {
        return 0;
    }

    g_mutex_lock(&g_enc_start_lock);
    g_enc_start_want = 1;
    deadline = g_get_monotonic_time() + (gint64)AIR_ENC_START_MS * 1000;

    while (g_enc_start_want) {
        if (!g_cond_wait_until(&g_enc_start_cond, &g_enc_start_lock, deadline)) {
            break;
        }
    }

    ret = g_atomic_int_get(&g_enc_up) ? 0 : -1;
    g_mutex_unlock(&g_enc_start_lock);

    return ret;
}

/** Capture thread: dequeue, share into both tiles, push. A frame that is not wanted (rate limit)
 * or cannot be afforded (no credit) goes straight back to the driver, which is the cheapest
 * possible drop and keeps the rotation fed. */
gpointer air_cap_feed(gpointer user)
{
    struct pollfd pfd[1 + AIR_NCHN];
    int enc_slot[AIR_NCHN];
    gint64 base_us = 0;
    gint64 due_us = 0;
    gint64 cap_err_us = 0;
    /* Silence is only the encoder's fault while frames are still arriving for it. A quiet camera
     * would otherwise read as a stalled encoder and buy a rebuild that fixes nothing. */
    gint64 last_cap_us = 0;
    int nfd;

    (void)user;

    /* Slot 0 is the camera; the direct path's encoders take the slots after it. Rebuilt after an
     * on-demand bring-up, which is the one way a tile gains an fd after start-up. */
    nfd = air_cap_poll_build(pfd, enc_slot);

    while (!g_cap_stop) {
        struct v4l2_plane planes[AIR_CAP_PLANES];
        struct v4l2_buffer b;
        struct air_cap_buf *capbuf;
        GstBuffer *buf[AIR_NCHN] = { NULL, NULL };
        gpointer pool[AIR_NCHN] = { NULL, NULL };
        int cap_fps;
        gint64 period;
        gint64 ts_us;
        gint64 now_us;
        int nshare = 0;
        int starved = 0;

        /* Serviced before the poll, so a camera producing nothing cannot hold a bring-up. */
        g_mutex_lock(&g_enc_start_lock);
        if (g_enc_start_want) {
            int rc;

            g_mutex_unlock(&g_enc_start_lock);
            rc = air_enc_start_all();
            if (rc == 0) {
                nfd = air_cap_poll_build(pfd, enc_slot);
                g_atomic_int_set(&g_enc_up, 1);
                g_printerr("[ml-air-video] encoders up on request\n");
            } else {
                g_printerr("[ml-air-video] encoder bring-up failed\n");
            }

            g_mutex_lock(&g_enc_start_lock);
            g_enc_start_want = 0;
            g_cond_broadcast(&g_enc_start_cond);
        }
        g_mutex_unlock(&g_enc_start_lock);

        for (int i = 0; i < nfd; i++) {
            pfd[i].revents = 0;
        }

        /* Polled rather than blocking in DQBUF, so the exit path does not depend on a frame
         * arriving to be noticed. */
        if (poll(pfd, nfd, 200) <= 0) {
            continue;
        }

        /* The encoders are in the same poll set as the camera, so a coded frame is collected on
         * its own readiness rather than waiting for the next capture frame, and output keeps
         * flowing if capture stalls. v4l2-mem2mem raises POLLOUT for a finished source buffer and
         * POLLIN for a finished coded one, and only ever from a done list, so this cannot spin.
         * Draining is also what lowers g_cap_inflight, which is why it must stay ahead of the
         * in-flight gate below: behind it, reaching the ceiling would deadlock the loop. */
        now_us = g_get_monotonic_time();

        for (int i = 0; i < AIR_NCHN; i++) {
            struct air_tile *tile = &g_tile[i];

            if (enc_slot[i] < 0) {
                continue;
            }

            if (pfd[enc_slot[i]].revents & (POLLIN | POLLOUT)) {
                air_enc_drain(tile);
            }

            /* Nothing here ends the stream. A stalled encoder costs the frames it is holding
             * and nothing else: an error on the fd, or silence for AIR_ENC_STALL_US while
             * source buffers are outstanding, cycles its OUTPUT queue and carries on. The
             * alternative is worse than a glitch, because the frames it holds are capture
             * frames and g_cap_inflight cannot fall until they are back. */
            if (pfd[enc_slot[i]].revents & (POLLERR | POLLNVAL)) {
                g_printerr("[ml-air-video] tile %d: encoder poll 0x%x, recovering\n",
                           tile->chn, pfd[enc_slot[i]].revents);
                air_enc_restart(tile);
            } else if (air_enc_held(tile) > 0 &&
                       now_us - tile->enc_progress_us > AIR_ENC_STALL_US &&
                       now_us - last_cap_us < AIR_ENC_STALL_US) {
                g_printerr("[ml-air-video] tile %d: encoder silent for %" G_GINT64_FORMAT " ms "
                           "holding %d frames, recovering\n",
                           tile->chn, (now_us - tile->enc_progress_us) / 1000, air_enc_held(tile));
                air_enc_restart(tile);
            } else {
                continue;
            }

            /* Recovery replaced the fd, or retired the tile. Either way the poll set is stale. */
            pfd[enc_slot[i]].fd = tile->active ? tile->enc_fd : -1;
        }

        if ((pfd[0].revents & POLLIN) == 0) {
            continue;
        }

        memset(planes, 0, sizeof planes);
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.length = AIR_CAP_PLANES;
        b.m.planes = planes;

        /* A failed dequeue costs this frame, not the stream. The loop goes back to poll and
         * picks up whenever the camera produces again; only g_cap_stop ends it. Reported at most
         * once a second so a persistent fault cannot flood a 32 MiB /tmp. */
        if (ioctl(g_cap_fd, VIDIOC_DQBUF, &b) != 0) {
            if (errno != EINTR && now_us - cap_err_us > G_USEC_PER_SEC) {
                cap_err_us = now_us;
                g_printerr("[ml-air-video] capture DQBUF: %s\n", strerror(errno));
            }

            continue;
        }

        capbuf = &g_cap[b.index];
        g_cap_frames++;
        last_cap_us = now_us;

        ts_us = (gint64)b.timestamp.tv_sec * G_USEC_PER_SEC + b.timestamp.tv_usec;
        if (base_us == 0) {
            base_us = ts_us;
            due_us = ts_us;
        }

        cap_fps = g_atomic_int_get(&g_cap_fps);
        period = cap_fps > 0 ? G_USEC_PER_SEC / cap_fps : 0;

        /* Rate limit against the driver's own timestamps rather than wall clock, so a frame is
         * judged by when it was captured. period is truncated down, so at a request equal to the
         * sensor rate every frame is due and nothing is dropped. */
        if (period > 0 && ts_us < due_us) {
            g_cap_skipped++;
            air_cap_qbuf(capbuf);
            continue;
        }

        if (g_atomic_int_get(&g_cap_inflight) >= g_cap_inflight_max) {
            g_cap_skipped++;
            air_cap_qbuf(capbuf);
            continue;
        }

        if (period > 0) {
            due_us += period;
            if (due_us < ts_us) {
                due_us = ts_us + period;
            }
        }

        /* Direct V4L2: the capture buffer is queued into each encoder as it stands, at the
         * tile's row offset, with no GstBuffer in between. */
        if (g_enc_direct) {
            int nq = 0;

            /* Held for a receiver: the ISP still produces every frame, so hand the buffer straight
             * back rather than queueing into encoders that are not open. */
            if (!g_atomic_int_get(&g_enc_up)) {
                g_cap_skipped++;
                air_cap_qbuf(capbuf);
                continue;
            }

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active) {
                    nq++;
                }
            }

            g_atomic_int_inc(&g_cap_inflight);
            g_atomic_int_set(&capbuf->refs, nq);

            for (int i = 0; i < AIR_NCHN; i++) {
                if (g_tile[i].active && air_enc_queue(&g_tile[i], capbuf) != 0) {
                    /* This encoder will never return the buffer, so drop its reference now. */
                    air_cap_release(capbuf);
                }
            }

            continue;
        }

        /* Copy tiles need a pool slot before anything is committed, because running out of one
         * halfway through a frame would leave the pair misaligned. Share tiles need nothing. */
        for (int i = 0; i < AIR_NCHN; i++) {
            if (!g_tile[i].active) {
                continue;
            }

            if (!g_tile[i].copy) {
                nshare++;
                continue;
            }

            pool[i] = g_async_queue_try_pop(g_tile[i].freeq);
            if (pool[i] == NULL) {
                starved = 1;
            }
        }

        if (starved) {
            for (int i = 0; i < AIR_NCHN; i++) {
                if (pool[i] != NULL) {
                    g_async_queue_push(g_tile[i].freeq, pool[i]);
                }
            }

            g_tile[0].dropped++;
            g_cap_skipped++;
            air_cap_qbuf(capbuf);
            continue;
        }

        /* Only the sharing tiles hold the capture buffer, so only they are counted. With every
         * tile copied nothing holds it and it goes straight back below. */
        if (nshare > 0) {
            g_atomic_int_inc(&g_cap_inflight);
            g_atomic_int_set(&capbuf->refs, nshare);
        }

        for (int i = 0; i < AIR_NCHN; i++) {
            GstClockTime pts = (GstClockTime)(ts_us - base_us) * GST_USECOND;

            if (!g_tile[i].active) {
                continue;
            }

            if (g_tile[i].copy) {
                int idx = GPOINTER_TO_INT(pool[i]) - 1;

                air_cap_fill(&g_tile[i], idx, capbuf);
                buf[i] = air_wrap_tile(&g_tile[i], idx, pts);
            } else {
                buf[i] = air_cap_share(&g_tile[i], capbuf, pts);
            }
        }

        if (nshare == 0) {
            air_cap_qbuf(capbuf);
        }

        air_push_frame(buf);
    }

    return NULL;
}

/** Stop the capture node. The exported memories are deliberately not freed: shares of them may
 * still be in an encoder queue, and the process is exiting. */
void air_cap_close(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (g_cap_fd < 0) {
        return;
    }

    ioctl(g_cap_fd, VIDIOC_STREAMOFF, &type);
    close(g_cap_fd);
    g_cap_fd = -1;
}
