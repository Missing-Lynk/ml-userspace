/**
 * @file osd_proto.h
 * @brief Wire layout of the air-unit -> goggle telemetry/OSD stream on `sdio0` UDP `:10000`.
 *
 * Reverse-engineered in re/notes/rf-telemetry-sdio0-10000.md. Every `:10000` datagram is one frame
 * with a common 20-byte little-endian header; the OSD family carries an MSP DisplayPort canvas as its
 * payload, the status families carry small binary structs.
 *
 *   off  size  field
 *   0    u32   msg_type      0x09 version / 0x10 OSD / 0x11 periodic (+ 0x00..0x03 at association)
 *   4    u32   (always 0)
 *   8    u32   timestamp     ~1 MHz counter, us since boot (steady state)
 *   12   u32   (always 0)
 *   16   u32   payload_len   bytes of payload that follow
 *   20   ...   payload[payload_len]
 */
#ifndef HUD_OSD_PROTO_H
#define HUD_OSD_PROTO_H

#include <stdint.h>

enum {
    OSD10K_PORT       = 10000,
    OSD10K_HEADER_LEN = 20,
};

/** @brief The frame's message type (header u32 at offset 0). */
enum osd10k_msg {
    OSD10K_MSG_VERSION  = 0x09,   /* 128-byte payload: hw/fw strings, voltage, link metrics */
    OSD10K_MSG_OSD      = 0x10,   /* variable payload: MSP DisplayPort canvas */
    OSD10K_MSG_PERIODIC = 0x11,   /* 6-byte payload: voltage only */
};

/* Header field offsets (little-endian u32). */
enum {
    OSD10K_OFF_TYPE        = 0,
    OSD10K_OFF_TS          = 8,
    OSD10K_OFF_PAYLOAD_LEN = 16,
};

/* type 0x11 (periodic) payload: */
enum {
    OSD10K_PERIODIC_OFF_VOLTAGE_MV = 4,     /* u16 */
};

/* type 0x09 (version) payload: */
enum {
    OSD10K_VERSION_OFF_HW         = 0,      /* char[16] */
    OSD10K_VERSION_OFF_FW         = 32,     /* char[16] */
    OSD10K_VERSION_OFF_VOLTAGE_MV = 96,     /* u16 */
    OSD10K_VERSION_OFF_TEMP_C     = 98,     /* u16, air-unit temperature in deg C */
    OSD10K_VERSION_OFF_LINK_B     = 116,    /* u8,  candidate SNR / link-quality */
};

/** @brief The common frame header (type + timestamp + payload length). */
typedef struct {
    uint32_t type;          /* enum osd10k_msg */
    uint32_t ts_us;         /* ~1 MHz counter */
    uint32_t payload_len;
} osd_header_t;

/** @brief Decoded type 0x11 (periodic) frame. */
typedef struct {
    uint16_t voltage_mV;
} osd_periodic_t;

/** @brief Decoded type 0x09 (version/info) frame. */
typedef struct {
    char     hw[17];        /* NUL-terminated */
    char     fw[17];        /* NUL-terminated */
    uint16_t voltage_mV;
    uint16_t air_temp_c;    /* @98: air-unit temperature in deg C */
    uint8_t  link_b;        /* @116: candidate SNR / link-quality */
} osd_version_t;

#endif /* HUD_OSD_PROTO_H */
