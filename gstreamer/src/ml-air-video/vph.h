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

/** Upper bound on a built SEI NAL, for sizing a caller's scratch buffer. Sized for the worst case
 * the builder will accept: a 0xfe payload plus prefix, with an emulation-prevention byte after
 * every second byte. The real NAL is 76-77 bytes. */
#define VPH_SEI_MAX     512

/**
 * @brief Offset of the first VCL NAL in an HEVC access unit.
 *
 * A prefix SEI has to sit immediately before the first slice: ahead of the parameter sets it would
 * shift the access-unit head, and a vendor receiver byte-checks that head (see
 * userspace/docs/rf-video-downlink.md). On an IDR access unit this lands after VPS/SPS/PPS, on a
 * P access unit at offset 0, which is exactly the vendor's own layout.
 *
 * @param es     HEVC access unit.
 * @param es_len length of @p es.
 * @return byte offset of the start code introducing the first VCL NAL (nal_unit_type <= 31), or
 *         @p es_len if the access unit contains none.
 */
size_t vph_first_vcl_offset(const uint8_t *es, size_t es_len);

/**
 * @brief Build the vendor's per-access-unit PREFIX_SEI (user_data_unregistered).
 *
 * A vendor goggle calls AR_MPI_VDEC_GetUserData on every decoded frame and tears its whole receive
 * pipeline down when the access unit carried none, so this NAL is mandatory rather than
 * diagnostic. Layout and the constant UUID are recovered from a vendor capture; the field format is
 * "ChnId %d FrameId %d PTS %x Filed %d BR %d QP %x" with a NUL terminator ("Filed" is the vendor's
 * spelling). Whether the receiver reads any field or only requires the call to succeed is not
 * established, so the format is matched rather than approximated.
 *
 * @param out     destination, at least VPH_SEI_MAX bytes.
 * @param out_cap capacity of @p out.
 * @param chn     ChnId.
 * @param frame_id FrameId.
 * @param pts     PTS, emitted in hex.
 * @param bitrate_kbps BR, emitted in decimal.
 * @param qp      QP, emitted in hex.
 * @return bytes written, or 0 if @p out_cap is too small.
 */
size_t vph_sei_build(uint8_t *out, size_t out_cap,
                     uint32_t chn, uint32_t frame_id, uint32_t pts,
                     uint32_t bitrate_kbps, uint32_t qp);

#endif /* ML_AIR_VPH_H */
