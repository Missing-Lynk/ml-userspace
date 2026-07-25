/**
 * @file vph.h
 * @brief Video-packet-header framing for the air-unit downlink on UDP :10001.
 *
 * The air unit wraps each encoded tile access unit in the vendor's 36-byte little-endian
 * video_packet_header, followed by the HEVC payload and a 4-byte tail magic. The goggle's
 * ml-pipeline receiver (mlp-rf.c) validates the magic, the CRC-32 over the first 32 header
 * bytes, and the tail magic, then demuxes the two tiles by ChnIndex. This module builds that
 * frame. It has no GStreamer or platform dependency, so the framing is host-testable.
 */
#ifndef ML_AIR_VPH_H
#define ML_AIR_VPH_H

#include <stddef.h>
#include <stdint.h>

/** MagicCode at header offset 0. */
#define VPH_MAGIC       0x12345678u
/** TailMagicCode at header offset 28, and the standalone 4-byte trailer after the payload. */
#define VPH_TAIL_MAGIC  0x87654321u
/** Fixed header length in bytes. */
#define VPH_HDR_LEN     36
/** Trailer length in bytes (a repeat of the tail magic). */
#define VPH_TAIL_LEN    4
/** Per-access-unit wire overhead: header + trailer. */
#define VPH_OVERHEAD    (VPH_HDR_LEN + VPH_TAIL_LEN)

/**
 * @brief Build the Resolution field: high 16 bits = width, low 16 bits = height.
 *
 * Real captures carry the composite frame size (1920x1080) on both channels, not the tile size.
 *
 * @param width  composite width in pixels.
 * @param height composite height in pixels.
 * @return the packed Resolution word.
 */
static inline uint32_t vph_resolution(uint16_t width, uint16_t height)
{
    return ((uint32_t)width << 16) | (uint32_t)height;
}

/**
 * @brief CRC-32 (zlib polynomial 0xedb88320, init/xorout 0xffffffff) over @p n bytes at @p data.
 *
 * Bit-identical to the goggle's crc32_buf and to Python zlib.crc32.
 *
 * @param data start of the buffer.
 * @param n    byte count.
 * @return the CRC-32 value.
 */
uint32_t vph_crc32(const uint8_t *data, size_t n);

/**
 * @brief Serialize one tile access unit into @p out.
 *
 * Writes the 36-byte little-endian header (with the CRC-32 over its first 32 bytes), the
 * @p es_len payload bytes, and the 4-byte tail magic. The caller sends the whole buffer as one
 * datagram; the sdio0 link IP-fragments anything over its MTU and the receiver reassembles it.
 *
 * @param out          destination buffer.
 * @param out_cap      capacity of @p out in bytes.
 * @param chn          ChnIndex (0 = top tile, 1 = bottom tile).
 * @param is_idr       isIdrStream (1 on the session-start keyframe, 0 otherwise).
 * @param frame_id     FrameId, shared across the two tiles of one source frame.
 * @param timestamp_ms TimeStap in milliseconds.
 * @param resolution   Resolution word (see vph_resolution).
 * @param es           HEVC elementary-stream access unit.
 * @param es_len       length of @p es in bytes.
 * @return the total byte count written, or 0 if @p out_cap is too small.
 */
size_t vph_build(uint8_t *out, size_t out_cap,
                 uint32_t chn, uint32_t is_idr, uint32_t frame_id,
                 uint32_t timestamp_ms, uint32_t resolution,
                 const uint8_t *es, uint32_t es_len);

#endif /* ML_AIR_VPH_H */
