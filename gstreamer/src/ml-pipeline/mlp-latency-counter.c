/* ml-pipeline latency counter (MLM_CMD_LATENCY_COUNTER): burn a free-running millisecond value
 * into every composite, so it reaches the panel and the recording identically.
 *
 * Point the air unit's camera at the goggle's panel and each recorded frame then carries two
 * copies of the counter: the one burned in here, and an older nested one that travelled camera,
 * ISP, encode, RF and decode. Their difference is the whole end-to-end latency, with no
 * correction term and no clock shared between the two devices.
 *
 * It is drawn into the composite rather than onto the HUD's overlay plane because the encoder and
 * the display consume the same pool buffer: pixels written here cannot skew between what the panel
 * shows and what the file holds, and a skew there is indistinguishable from latency.
 *
 * 7-segment bars, not glyphs: the digits have to survive the camera, the air-side encoder and the
 * RF link, where a thin antialiased glyph smears into an unreadable blob.
 */
#include "ml-pipeline.h"

#define LATENCY_DIGITS      5

/* Wraps every 100 s. Latency is tens of ms, so a wrap is never ambiguous, and a decimal rollover
 * is easier to subtract by eye than a truncated one.
 */
#define LATENCY_MODULO      100000

/* Digit geometry in luma pixels. Every value is even so the box maps cleanly onto the I420 chroma
 * planes, and the box sits at the top of the frame: those rows scan out first, so their
 * assembly-to-photon delay is the smallest and the most stable.
 */
#define LATENCY_DIGIT_H     96
#define LATENCY_DIGIT_W     52
#define LATENCY_STROKE      16
#define LATENCY_GAP         20
#define LATENCY_PAD         16
#define LATENCY_TOP         24

#define LATENCY_BOX_W       (LATENCY_DIGITS * LATENCY_DIGIT_W + (LATENCY_DIGITS - 1) * LATENCY_GAP + 2 * LATENCY_PAD)
#define LATENCY_BOX_H       (LATENCY_DIGIT_H + 2 * LATENCY_PAD)
#define LATENCY_BOX_X       ((((COMP_W - LATENCY_BOX_W) / 2) / 2) * 2)

/* BT.601 limited range, the composite's own encoding: luma black and white levels, neutral
 * chroma. Both box and ink are achromatic, so the chroma planes are a flat fill and only the luma
 * plane carries the digits.
 */
#define LATENCY_Y_BOX       16
#define LATENCY_Y_INK       235
#define LATENCY_C_NEUTRAL   128

/* Segment bits in the order a, b, c, d, e, f, g: top, top-right, bottom-right, bottom,
 * bottom-left, top-left, middle.
 */
#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40

static const guint8 g_digit_segments[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_F | SEG_G | SEG_B | SEG_C,
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};

/* Fill a rectangle in one plane. Opaque writes only, one memset per row: the composite is a
 * write-combine mapping, so a blend would read back over an uncached path.
 */
static void plane_fill(guint8 *plane, int stride, int x, int y, int w, int h, guint8 val)
{
    for (int row = 0; row < h; row++) {
        memset(plane + (gsize) (y + row) * stride + x, val, (gsize) w);
    }
}

/* One 7-segment digit, top-left corner at (@p x, @p y) in the luma plane. */
static void draw_digit(guint8 *luma, int x, int y, int digit)
{
    int middle = (LATENCY_DIGIT_H - LATENCY_STROKE) / 2;
    guint8 segments = g_digit_segments[digit];

    if (segments & SEG_A) {
        plane_fill(luma, COMP_LSTRIDE, x, y, LATENCY_DIGIT_W, LATENCY_STROKE, LATENCY_Y_INK);
    }

    if (segments & SEG_G) {
        plane_fill(luma, COMP_LSTRIDE, x, y + middle, LATENCY_DIGIT_W, LATENCY_STROKE, LATENCY_Y_INK);
    }

    if (segments & SEG_D) {
        plane_fill(luma, COMP_LSTRIDE, x, y + LATENCY_DIGIT_H - LATENCY_STROKE, LATENCY_DIGIT_W, LATENCY_STROKE,
                   LATENCY_Y_INK);
    }

    if (segments & SEG_F) {
        plane_fill(luma, COMP_LSTRIDE, x, y, LATENCY_STROKE, middle + LATENCY_STROKE, LATENCY_Y_INK);
    }

    if (segments & SEG_B) {
        plane_fill(luma, COMP_LSTRIDE, x + LATENCY_DIGIT_W - LATENCY_STROKE, y, LATENCY_STROKE,
                   middle + LATENCY_STROKE, LATENCY_Y_INK);
    }

    if (segments & SEG_E) {
        plane_fill(luma, COMP_LSTRIDE, x, y + middle, LATENCY_STROKE, LATENCY_DIGIT_H - middle, LATENCY_Y_INK);
    }

    if (segments & SEG_C) {
        plane_fill(luma, COMP_LSTRIDE, x + LATENCY_DIGIT_W - LATENCY_STROKE, y + middle, LATENCY_STROKE,
                   LATENCY_DIGIT_H - middle, LATENCY_Y_INK);
    }
}

/* The counter's own value: monotonic milliseconds, sampled at composite assembly. Both numbers a
 * capture carries come from this one clock on this one device, which is why the reading needs no
 * clock discipline between the goggle and the air unit.
 */
static guint32 latency_counter_value(void)
{
    return (guint32) ((g_get_monotonic_time() / 1000) % LATENCY_MODULO);
}

void latency_counter_apply(struct ctx *c, guint8 *map)
{
    if (!c->latency_counter_on) {
        return;
    }

    guint32 value = latency_counter_value();

    plane_fill(map, COMP_LSTRIDE, LATENCY_BOX_X, LATENCY_TOP, LATENCY_BOX_W, LATENCY_BOX_H, LATENCY_Y_BOX);
    plane_fill(map + COMP_UOFF, COMP_CSTRIDE, LATENCY_BOX_X / 2, LATENCY_TOP / 2, LATENCY_BOX_W / 2,
               LATENCY_BOX_H / 2, LATENCY_C_NEUTRAL);
    plane_fill(map + COMP_VOFF, COMP_CSTRIDE, LATENCY_BOX_X / 2, LATENCY_TOP / 2, LATENCY_BOX_W / 2,
               LATENCY_BOX_H / 2, LATENCY_C_NEUTRAL);

    for (int index = LATENCY_DIGITS - 1; index >= 0; index--) {
        int x = LATENCY_BOX_X + LATENCY_PAD + index * (LATENCY_DIGIT_W + LATENCY_GAP);

        draw_digit(map, x, LATENCY_TOP + LATENCY_PAD, (int) (value % 10));
        value /= 10;
    }
}
