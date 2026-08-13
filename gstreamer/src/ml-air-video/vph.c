/**
 * @file vph.c
 * @brief Video-packet-header framing for the air-unit downlink on UDP :10001.
 */
#include "vph.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief Store a 32-bit value little-endian at @p p.
 *
 * @param p destination (4 bytes).
 * @param v value to store.
 */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t vph_crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xffffffffu;

    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1u) ? (0xedb88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }

    return crc ^ 0xffffffffu;
}

size_t vph_build(uint8_t *out, size_t out_cap,
                 uint32_t chn, uint32_t is_idr, uint32_t frame_id,
                 uint32_t timestamp_ms, uint32_t resolution,
                 const uint8_t *es, uint32_t es_len)
{
    size_t total = (size_t)VPH_HDR_LEN + es_len + VPH_TAIL_LEN;

    if (out_cap < total) {
        return 0;
    }

    put_u32le(out + 0, VPH_MAGIC);
    put_u32le(out + 4, es_len);
    put_u32le(out + 8, chn);
    put_u32le(out + 12, is_idr);
    put_u32le(out + 16, frame_id);
    put_u32le(out + 20, timestamp_ms);
    put_u32le(out + 24, resolution);
    put_u32le(out + 28, VPH_TAIL_MAGIC);

    /* CRC covers the first 32 header bytes only (MagicCode through TailMagicCode). */
    put_u32le(out + 32, vph_crc32(out, 32));

    memcpy(out + VPH_HDR_LEN, es, es_len);
    put_u32le(out + VPH_HDR_LEN + es_len, VPH_TAIL_MAGIC);

    return total;
}

/* The vendor's user_data_unregistered UUID, identical on every frame and both tiles. */
static const uint8_t vph_sei_uuid[16] = {
    0xbd, 0xe9, 0x45, 0xdc, 0xb7, 0x48, 0xd9, 0xe6,
    0x20, 0xd8, 0x2c, 0x96, 0xef, 0xee, 0x23, 0xd9
};

size_t vph_first_vcl_offset(const uint8_t *es, size_t es_len)
{
    for (size_t i = 0; i + 4 < es_len; i++) {
        size_t hdr;

        if (es[i] != 0x00 || es[i + 1] != 0x00) {
            continue;
        }

        if (es[i + 2] == 0x01) {
            hdr = 3;
        } else if (es[i + 2] == 0x00 && es[i + 3] == 0x01) {
            hdr = 4;
        } else {
            continue;
        }

        if (i + hdr >= es_len) {
            break;
        }

        /* nal_unit_type is bits 6..1 of the first header byte; 0..31 are the VCL classes. */
        if (((es[i + hdr] >> 1) & 0x3f) <= 31) {
            return i;
        }

        i += hdr - 1;
    } /* for each start code */

    return es_len;
}

size_t vph_sei_build(uint8_t *out, size_t out_cap,
                     uint32_t chn, uint32_t frame_id, uint32_t pts,
                     uint32_t bitrate_kbps, uint32_t qp)
{
    char text[80];
    int text_len;
    size_t payload_len;
    size_t n = 0;

    /* snprintf returns what it would have written; the payload_size field below is a single byte,
     * so refuse anything that did not fit rather than emitting a truncated, mis-sized NAL. */
    text_len = snprintf(text, sizeof text, "ChnId %u FrameId %u PTS %x Filed 4 BR %u QP %x",
                        chn, frame_id, pts, bitrate_kbps, qp);
    if (text_len < 0 || (size_t)text_len >= sizeof text) {
        return 0;
    }

    payload_len = sizeof vph_sei_uuid + (size_t)text_len + 1;  /* the NUL is inside the payload */

    /* payload_size is emitted as one byte, so 255 and above would need 0xff continuation bytes
     * (255 itself encodes as "ff 00", not as a single 0xff). Nothing here approaches that, so
     * refuse rather than carry an untested encoding. */
    if (payload_len > 0xfe) {
        return 0;
    }

    /* Assemble the RBSP, then copy it out as EBSP. No escape is reachable with the current UUID
     * (no 00 00 pair) and a printable-ASCII body, but a changed UUID or body without it emits a
     * malformed NAL that surfaces only as a decoder fault. */
    {
        uint8_t rbsp[VPH_SEI_MAX];
        size_t r = 0;
        size_t zeros = 0;

        rbsp[r++] = 0x05;         /* payload_type 5, user_data_unregistered */
        rbsp[r++] = (uint8_t)payload_len;
        memcpy(rbsp + r, vph_sei_uuid, sizeof vph_sei_uuid);
        r += sizeof vph_sei_uuid;
        memcpy(rbsp + r, text, (size_t)text_len + 1);
        r += (size_t)text_len + 1;
        rbsp[r++] = 0x80;         /* rbsp_trailing_bits */

        /* Worst case every RBSP byte needs an escape, plus the 6-byte prefix. */
        if (out_cap < 6 + r * 2) {
            return 0;
        }

        out[n++] = 0x00;
        out[n++] = 0x00;
        out[n++] = 0x00;
        out[n++] = 0x01;
        out[n++] = 0x4e;          /* nal_unit_type 39 (PREFIX_SEI), nuh_layer_id 0 */
        out[n++] = 0x01;          /* nuh_temporal_id_plus1 */

        for (size_t i = 0; i < r; i++) {
            if (zeros >= 2 && rbsp[i] <= 0x03) {
                out[n++] = 0x03;  /* emulation_prevention_three_byte */
                zeros = 0;
            }

            out[n++] = rbsp[i];
            zeros = (rbsp[i] == 0x00) ? zeros + 1 : 0;
        } /* for each RBSP byte */
    }

    return n;
}
