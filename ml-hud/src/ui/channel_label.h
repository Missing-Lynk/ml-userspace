/**
 * @file channel_label.h
 * @brief Format an RF channel label shared by the System OSD and the channel grid.
 */
#ifndef HUD_UI_CHANNEL_LABEL_H
#define HUD_UI_CHANNEL_LABEL_H

#include <stdio.h>

/**
 * @brief Write a channel label into @p buf: "CH<idx>", with the raceband name appended for the 8
 *  raceband channels. @p idx is the raw channel table index (the value passed to a channel select).
 *  Table indices 3..10 are raceband R1..R8. e.g. CH0, CH3 (R1), CH16.
 */
static inline void channel_label(char *buf, unsigned n, int idx)
{
    if (idx >= 3 && idx <= 10) {
        snprintf(buf, n, "CH%d (R%d)", idx, idx - 2);
    } else {
        snprintf(buf, n, "CH%d", idx);
    }
}

#endif /* HUD_UI_CHANNEL_LABEL_H */
