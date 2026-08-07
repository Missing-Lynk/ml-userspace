/**
 * @file vph.c
 * @brief Video-packet-header framing for the air-unit downlink on UDP :10001.
 */
#include "vph.h"

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
