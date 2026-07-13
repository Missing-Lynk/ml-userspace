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

#endif /* HUD_RECORDINGS_H */
