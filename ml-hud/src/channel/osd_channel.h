/**
 * @file osd_channel.h
 * @brief The HUD's ingest of the OSD/telemetry frames, off ml-linkd's mlm seams.
 *
 * ml-linkd owns UDP `:10000` (it runs the MEDIA_PARAMS handshake on it), so the HUD never binds the
 * port. It consumes ml-linkd's republish instead: osd_channel_open binds osd.sock and receives the
 * MLM_T_MSP records (payload = the raw `:10000` MSP DisplayPort frame); the 0x09/0x11 status frames
 * arrive as MLM_T_STATUS on telemetry.sock, which services/linkstate owns and feeds back through
 * osd_channel_dispatch. On the bench, tools/osd-replay publishes the pcap corpus over the same
 * seams, standing in for ml-linkd.
 *
 * Dispatch routes each raw `:10000` frame by type to the matching callback: the OSD canvas (0x10)
 * to on_osd, the version (0x09) and periodic (0x11) status frames to their handlers.
 */
#ifndef HUD_OSD_CHANNEL_H
#define HUD_OSD_CHANNEL_H

#include "osd_proto.h"

typedef struct {
    /** Raw OSD canvas payload of a type-0x10 frame (ready for btfl_osd). */
    void (*on_osd)(void *ctx, const unsigned char *canvas, int len);

    /** Decoded type-0x09 version frame. */
    void (*on_version)(void *ctx, const osd_header_t *header, const osd_version_t *v);

    /** Decoded type-0x11 periodic frame. */
    void (*on_periodic)(void *ctx, const osd_header_t *header, const osd_periodic_t *p);
} osd_channel_cb_t;

/**
 * @brief Open and bind the mlm OSD seam (osd.sock; MLM_T_MSP records from ml-linkd or osd-replay).
 * @return socket fd on success, -1 on error.
 */
int osd_channel_open(void);

/**
 * @brief Receive at most one record (up to @p timeout_ms), unwrap its mlm_hdr, and dispatch the raw
 *        frame via @p cb. Non-MLM_T_MSP/MLM_T_STATUS records are consumed and ignored.
 * @return 1 if a record arrived, 0 on timeout, -1 on error.
 */
int osd_channel_poll(int fd, const osd_channel_cb_t *cb, void *ctx, int timeout_ms);

/**
 * @brief Dispatch one raw `:10000` frame (header + payload) to the callbacks. Used by poll and by
 *        services/linkstate for the MLM_T_STATUS records it drains off telemetry.sock.
 */
void osd_channel_dispatch(const unsigned char *frame, int len, const osd_channel_cb_t *cb, void *ctx);

void osd_channel_close(int fd);

/**
 * @brief Decode the common frame header from a raw `:10000` frame.
 * @return 0 on success, -1 if the frame is shorter than the header.
 */
int osd_decode_header(const unsigned char *frame, int len, osd_header_t *out);

/** @brief Decode a type-0x09 version payload. @return 0 on success, -1 if too short. */
int osd_decode_version(const unsigned char *payload, int payload_len, osd_version_t *out);

/** @brief Decode a type-0x11 periodic payload. @return 0 on success, -1 if too short. */
int osd_decode_periodic(const unsigned char *payload, int payload_len, osd_periodic_t *out);

#endif /* HUD_OSD_CHANNEL_H */
