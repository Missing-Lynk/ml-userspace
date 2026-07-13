/** @file tone.c @brief See tone.h. */
#include "tone.h"
#include "buzzer.h"

#define TONE_BEEP_MS 70

static int      g_beeping;
static uint32_t g_beep_off_ms;

void tone_beep(uint32_t now_ms)
{
    if (buzzer_volume() <= 0) {
        return;
    }

    buzzer_enable(1);
    g_beeping = 1;
    g_beep_off_ms = now_ms + TONE_BEEP_MS;
}

void tone_tick(uint32_t now_ms)
{
    if (g_beeping && (int32_t) (now_ms - g_beep_off_ms) >= 0) {
        buzzer_enable(0);
        g_beeping = 0;
    }
}
