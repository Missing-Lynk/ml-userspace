#include "ml-pipeline.h"

/* CRC-32, zlib polynomial 0xedb88320; matches the vendor packet header. */
static guint32 crc32_tab[256];

void crc32_init(void)
{
    for (guint32 i = 0; i < 256; i++) {
        guint32 c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_tab[i] = c;
    }
}

guint32 crc32_buf(const guint8 *p, int n)
{
    guint32 c = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        c = crc32_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }

    return c ^ 0xffffffffu;
}

/* Does this Annex-B access unit contain a session-start NAL (VPS 32, or IDR 19/20)? */
gboolean au_has_idr(const guint8 *es, int n)
{
    int i = 0;
    while (i + 4 < n) {
        if (es[i] == 0 && es[i + 1] == 0 &&
            (es[i + 2] == 1 || (es[i + 2] == 0 && es[i + 3] == 1))) {
            int body = (es[i + 2] == 1) ? i + 3 : i + 4;
            if (body < n) {
                int nal_type = (es[body] >> 1) & 0x3f;
                if (nal_type == 32 || nal_type == 19 || nal_type == 20) {
                    return TRUE;
                }
            }
            i = body;
        } else {
            i++;
        }
    }

    return FALSE;
}

gboolean map_tile(GstSample *s, GstBuffer *buf, GstMapInfo *m, struct tileview *t)
{
    if (!gst_buffer_map(buf, m, GST_MAP_READ)) {
        return FALSE;
    }

    /* wave5 buffers sometimes carry a GstVideoMeta with zero strides/offsets (seen on the
     * 1920x552 tile); trust the meta only if it looks filled in, else derive from caps
     */
    GstVideoMeta *vm = gst_buffer_get_video_meta(buf);
    if (vm && vm->stride[0] > 0) {
        t->w = vm->width;
        t->h = vm->height;
        t->nv12 = (vm->format == GST_VIDEO_FORMAT_NV12);
        t->y = m->data + vm->offset[0];
        t->ys = vm->stride[0];
        t->u = m->data + vm->offset[1];
        t->us = vm->stride[1];
        if (!t->nv12) {
            t->v = m->data + vm->offset[2];
            t->vs = vm->stride[2];
        }
    } else {
        GstVideoInfo vi;
        GstCaps *caps = gst_sample_get_caps(s);
        if (gst_video_info_from_caps(&vi, caps) && GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0) > 0) {
            t->w = GST_VIDEO_INFO_WIDTH(&vi);
            t->h = GST_VIDEO_INFO_HEIGHT(&vi);
            t->nv12 = (GST_VIDEO_INFO_FORMAT(&vi) == GST_VIDEO_FORMAT_NV12);
            t->y = m->data + GST_VIDEO_INFO_PLANE_OFFSET(&vi, 0);
            t->ys = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0);
            t->u = m->data + GST_VIDEO_INFO_PLANE_OFFSET(&vi, 1);
            t->us = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 1);

            if (!t->nv12) {
                t->v = m->data + GST_VIDEO_INFO_PLANE_OFFSET(&vi, 2);
                t->vs = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 2);
            }
        } else {
            /* DMA_DRM caps carry no plane layout (format=DMA_DRM parses with zero strides),
             * and the wave5 552-height tile can arrive meta-less on top of that: the plane
             * offsets then use the 16-ALIGNED height, not the caps height. wave5 dmabufs are
             * plain stride=width YU12/NV12, so recover the aligned height from the actual map
             * size and lay the planes out from that.
             */
            GstStructure *st = gst_caps_get_structure(caps, 0);
            gint w = 0;
            gint h = 0;
            const gchar *df = gst_structure_get_string(st, "drm-format");
            gst_structure_get_int(st, "width", &w);
            gst_structure_get_int(st, "height", &h);
            if (w <= 0 || h <= 0) {
                gst_buffer_unmap(buf, m);
                return FALSE;
            }

            /* Plane offsets pack at the tile's OWN caps height, NOT the 16-aligned allocation
             * height. Proven by dumping the raw wave5 buffer and rendering it (scratchpad
             * t1_caps552 vs t1_aligned560): tile 1 (552 rows) has chroma at stride*552 = 1059840
             * (the std of the data jumps from luma ~56 to chroma ~79 exactly at row 552), and the
             * caps-height render is clean while the aligned-560 render shows colour bands. Tile 0
             * differs only because its caps height (560) already equals the alignment. The tail of
             * the allocation past chroma is padding.
             */
            t->w = w;
            t->h = h;
            t->nv12 = (df && strcmp(df, "NV12") == 0);
            t->y = m->data;
            t->ys = w;
            t->u = m->data + (gsize)w * h;
            if (t->nv12) {
                t->us = w;
            } else {
                t->us = w / 2;
                t->v = t->u + (gsize)(w / 2) * (h / 2);
                t->vs = w / 2;
            }
        }
    }

    /* Capture the source dmabuf fd + plane byte offsets for the DMA blit path.
     * Only usable when the whole tile is ONE dmabuf memory (wave5's packed YU12 case); with a
     * multi-memory or non-dmabuf (MMAP) buffer, leave fd = -1 and the caller CPU-blits. The
     * plane offsets parallel the y/u/v CPU pointers, adjusted by the memory's own base offset
     * so they are absolute within the fd's mmap.
     */
    t->fd = -1;
    t->yoff = (gsize)(t->y - m->data);
    t->uoff = (gsize)(t->u - m->data);
    t->voff = t->nv12 ? 0 : (gsize)(t->v - m->data);

    if (gst_buffer_n_memory(buf) == 1) {
        GstMemory *mem = gst_buffer_peek_memory(buf, 0);
        if (gst_is_dmabuf_memory(mem)) {
            t->fd = gst_dmabuf_memory_get_fd(mem);
            t->yoff += mem->offset;
            t->uoff += mem->offset;
            if (!t->nv12) {
                t->voff += mem->offset;
            }
        }
    }

    return TRUE;
}

/* Emit one MLM_T_FRAMESTATS telemetry datagram (non-blocking) and stamp the fps clock. */
void emit_framestats(struct ctx *c, GstClockTime pts)
{
    struct { struct mlm_hdr h; struct mlm_framestats fs; } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_FRAMESTATS, .flags = 0 },
        .fs = { .frame_id = ++c->frame_id, .pts_ns = pts },
    };
    gint64 now = g_get_monotonic_time();

    if (c->t_first == 0) {
        c->t_first = now;
    }

    c->t_last = now;
    if (sendto(c->tsock, &rec, sizeof rec, MSG_DONTWAIT,
               (struct sockaddr *)&c->taddr, sizeof c->taddr) < 0) {
        c->dropped++;
    }
}
