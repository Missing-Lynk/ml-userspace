/**
 * @file recordings.h
 * @brief Lists the DVR recordings on the SD card by a plain directory read.
 */
#ifndef HUD_RECORDINGS_H
#define HUD_RECORDINGS_H

typedef struct {
    char name[64];   /* file name, e.g. "Video008.mp4" */
    long size_mb;    /* size in MB */
} recording_t;

/**
 * @brief List the .mp4 recordings on the SD card, sorted newest-name first.
 * @param out Destination array.
 * @param max Capacity of @p out.
 * @return The number of entries written.
 */
int recordings_list(recording_t *out, int max);

#include <stddef.h>

/**
 * @brief Build the absolute path of recording @p name (on the SD card) into @p out.
 */
void recordings_path(const char *name, char *out, size_t outsz);

/**
 * @brief Clip length in milliseconds, read from the MP4 `moov`/`mvhd` header (no decode, no gst).
 *
 * Walks the top-level atom tree by following size headers - `mdat` is skipped by its length, never
 * read - so cost is a handful of seeks regardless of file size. The result is cached per (path,
 * size, mtime), so repeated calls while rendering the list are free.
 *
 * @return Duration in ms, or 0 if the header could not be parsed.
 */
unsigned recordings_duration_ms(const char *path);

#endif /* HUD_RECORDINGS_H */
