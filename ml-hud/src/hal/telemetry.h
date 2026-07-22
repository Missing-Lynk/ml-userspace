/**
 * @file telemetry.h
 * @brief Reads the live goggle-local values this static binary can get directly.
 *
 * Goggle battery via the ADC, SD free space via statvfs, SoC temperature, and the incoming-video
 * bitrate (the air-unit downlink's byte rate off the SDIO netdev). Pure data, no UI. The RF
 * telemetry (link, quad battery) arrives over the OSD channel, not here.
 */
#ifndef HUD_TELEMETRY_H
#define HUD_TELEMETRY_H

typedef struct {
    int   have_battery;    /* goggle pack reading valid */
    float pack_volts;
    int   cell_count;      /* auto-detected once at plug-in (2-6S) */
    int   have_sdcard;     /* SD mount reading valid */
    float sd_free_gb;
    int   have_temp;       /* SoC temperature reading valid */
    int   temp_c;          /* SoC temperature, whole degrees Celsius */
    int   have_bitrate;    /* incoming-video bitrate valid (needs two samples over a known interval) */
    float bitrate_mbps;    /* air-unit downlink rate off the SDIO netdev, megabits/s */
} telemetry_t;

/**
 * @brief Fill @p out from the device. Missing sources leave their have_* flag at 0.
 * @param out Destination snapshot.
 */
void telemetry_read(telemetry_t *out);

/**
 * @brief True when a filesystem is actually mounted at the board's SD mount point.
 *
 * statvfs alone cannot tell an empty mount directory (which resolves to the read-only rootfs) from a
 * mounted card, so both the OSD free-space field and the record-start gate key off this explicit
 * /proc/mounts lookup instead.
 */
int sdcard_is_mounted(void);

#endif /* HUD_TELEMETRY_H */
