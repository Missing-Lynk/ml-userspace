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
 * planes.
 *
 * Sized for the copy that comes BACK, not for the panel. The filmed copy is as small as the panel
 * is in the camera's frame, so a digit that is comfortable to read here arrives a fraction of the
 * size, and the whole measurement rests on reading it. Nothing else is on the panel in this mode,
 * so the space costs nothing.
 */
#define LATENCY_DIGIT_H     160
#define LATENCY_DIGIT_W     88
#define LATENCY_STROKE      26
#define LATENCY_GAP         32
#define LATENCY_PAD         26

/* The row the box starts on. Every row costs COMP_H rows of frame time in scanout, so the counter
 * reads about 0.0155 ms later per row at 60 Hz, and that delay is inside the measurement: the
 * filmed copy is these rows as the panel emitted them. At 180 the box carries ~2.8 ms of scanout
 * that row 0 would not, which is the price of putting it where the camera can hold it in frame.
 */
#define LATENCY_TOP         180

/* A saturated red frame around the box. It reads red on the panel but NOT in a recording: the air
 * unit meters for the dark goggle body around the panel, so the panel clips and the border comes
 * back white. What it contributes is a closed high-contrast rectangle, which is what the reader's
 * pattern search locks onto.
 */
#define LATENCY_BORDER      12

/* Tracking markers: a solid bar inside each end of the digit row, overhanging it top and bottom so
 * nothing else in the box has their shape. A blown-out digit blooms its gaps shut and turns into an
 * anonymous blob, while a solid bar has no gaps to lose, so the pair holds the box's position,
 * scale and tilt when the digits alone no longer would. They are part of the pattern the reader
 * correlates, so nothing has to detect them separately.
 */
#define LATENCY_MARK_W      52
#define LATENCY_MARK_OVER   12
#define LATENCY_MARK_H      (LATENCY_DIGIT_H + 2 * LATENCY_MARK_OVER)

/* Kept wider than the digit gap so the markers' bloom cannot bridge into the first and last digit,
 * which would cost exactly the digit the fine reading depends on.
 */
#define LATENCY_MARK_GAP    64

/* The sweep bar: a bar under the digits that fills left to right once per LATENCY_SWEEP_MS and
 * resets, carrying the time the digits cannot.
 *
 * The digits fail in a way that cannot be detected from the image. The panel advances the counter
 * every frame, a camera exposure that spans two of those frames records both values superimposed,
 * and the union of two 7-segment glyphs is almost always a legible WRONG digit: 8 absorbs 9, 0
 * absorbs 7. The reading looks clean and is off by a frame.
 *
 * Superposition takes the brighter pixel, so two bars superimpose to the LONGER one, and its right
 * edge is the later of the two times. The worst case is a bounded error of one frame with a known
 * sign, instead of an undetectable one. The exception is a straddle across the reset, where the
 * later frame's bar is the shorter one; the reader drops the samples near either end for that.
 *
 * The track spans the box between the markers, which is 800 px for 100 ms: 8 px per millisecond
 * here, and about 2 px per millisecond by the time the panel has been filmed at a distance.
 */
#define LATENCY_SWEEP_MS    100
#define LATENCY_TRACK_H     44
#define LATENCY_TRACK_GAP   26

#define LATENCY_DIGITS_W    (LATENCY_DIGITS * LATENCY_DIGIT_W + (LATENCY_DIGITS - 1) * LATENCY_GAP)
#define LATENCY_BOX_W       (LATENCY_DIGITS_W + 2 * (LATENCY_PAD + LATENCY_MARK_W + LATENCY_MARK_GAP))
#define LATENCY_BOX_H       (2 * LATENCY_PAD + LATENCY_DIGIT_H + LATENCY_TRACK_GAP + LATENCY_TRACK_H)
#define LATENCY_BOX_X       ((((COMP_W - LATENCY_BOX_W) / 2) / 2) * 2)

#define LATENCY_DIGITS_X    (LATENCY_BOX_X + LATENCY_PAD + LATENCY_MARK_W + LATENCY_MARK_GAP)
#define LATENCY_MARK_Y      (LATENCY_TOP + LATENCY_PAD - LATENCY_MARK_OVER)

#define LATENCY_TRACK_X     (LATENCY_BOX_X + LATENCY_PAD)
#define LATENCY_TRACK_W     (LATENCY_BOX_W - 2 * LATENCY_PAD)
#define LATENCY_TRACK_Y     (LATENCY_TOP + LATENCY_PAD + LATENCY_DIGIT_H + LATENCY_TRACK_GAP)

#define LATENCY_FRAME_W     (LATENCY_BOX_W + 2 * LATENCY_BORDER)
#define LATENCY_FRAME_H     (LATENCY_BOX_H + 2 * LATENCY_BORDER)
#define LATENCY_FRAME_X     (LATENCY_BOX_X - LATENCY_BORDER)
#define LATENCY_FRAME_Y     (LATENCY_TOP - LATENCY_BORDER)

/* BT.601 limited range, the composite's own encoding: neutral chroma for the box and the ink, and
 * full-saturation red for the border.
 *
 * The ink is deliberately NOT the white level. The panel clips in a recording because it is far
 * brighter than the dark goggle body the air unit's autoexposure meters for, and a clipped digit
 * blooms its gaps shut. Dropping the ink about a stop below white buys that headroom back without
 * dimming the video around it, which the panel backlight control cannot do. Against the box's black
 * it is still a contrast of over a hundred levels, and the reader correlates zero-mean, so the
 * absolute level costs it nothing.
 */
#define LATENCY_Y_BOX       16
#define LATENCY_Y_INK       140
#define LATENCY_C_NEUTRAL   128
#define LATENCY_Y_RED       81
#define LATENCY_U_RED       90
#define LATENCY_V_RED       240

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

    /* Red frame, then the black box inside it: two rectangle fills per plane rather than four
     * strips, since the box overwrites the middle of the frame anyway.
     */
    plane_fill(map, COMP_LSTRIDE, LATENCY_FRAME_X, LATENCY_FRAME_Y, LATENCY_FRAME_W,
               LATENCY_FRAME_H, LATENCY_Y_RED);
    plane_fill(map + COMP_UOFF, COMP_CSTRIDE, LATENCY_FRAME_X / 2, LATENCY_FRAME_Y / 2,
               LATENCY_FRAME_W / 2, LATENCY_FRAME_H / 2, LATENCY_U_RED);
    plane_fill(map + COMP_VOFF, COMP_CSTRIDE, LATENCY_FRAME_X / 2, LATENCY_FRAME_Y / 2,
               LATENCY_FRAME_W / 2, LATENCY_FRAME_H / 2, LATENCY_V_RED);

    plane_fill(map, COMP_LSTRIDE, LATENCY_BOX_X, LATENCY_TOP, LATENCY_BOX_W, LATENCY_BOX_H, LATENCY_Y_BOX);
    plane_fill(map + COMP_UOFF, COMP_CSTRIDE, LATENCY_BOX_X / 2, LATENCY_TOP / 2, LATENCY_BOX_W / 2,
               LATENCY_BOX_H / 2, LATENCY_C_NEUTRAL);
    plane_fill(map + COMP_VOFF, COMP_CSTRIDE, LATENCY_BOX_X / 2, LATENCY_TOP / 2, LATENCY_BOX_W / 2,
               LATENCY_BOX_H / 2, LATENCY_C_NEUTRAL);

    plane_fill(map, COMP_LSTRIDE, LATENCY_BOX_X + LATENCY_PAD, LATENCY_MARK_Y,
               LATENCY_MARK_W, LATENCY_MARK_H, LATENCY_Y_INK);
    plane_fill(map, COMP_LSTRIDE, LATENCY_BOX_X + LATENCY_BOX_W - LATENCY_PAD - LATENCY_MARK_W,
               LATENCY_MARK_Y, LATENCY_MARK_W, LATENCY_MARK_H, LATENCY_Y_INK);

    /* Drawn before the digits consume @p value, and rounded down: the bar's right edge is the time
     * it has reached, so rounding up would place the edge ahead of the value the digits show.
     */
    int filled = (int) ((guint64) (value % LATENCY_SWEEP_MS) * LATENCY_TRACK_W / LATENCY_SWEEP_MS);

    if (filled > 0) {
        plane_fill(map, COMP_LSTRIDE, LATENCY_TRACK_X, LATENCY_TRACK_Y, filled, LATENCY_TRACK_H,
                   LATENCY_Y_INK);
    }

    for (int index = LATENCY_DIGITS - 1; index >= 0; index--) {
        int x = LATENCY_DIGITS_X + index * (LATENCY_DIGIT_W + LATENCY_GAP);

        draw_digit(map, x, LATENCY_TOP + LATENCY_PAD, (int) (value % 10));
        value /= 10;
    }
}
