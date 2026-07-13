/**
 * @file msp_canvas.h
 * @brief One parser for the FC/MSP OSD canvas wire format, shared by the menu renderer and the OSD
 *        tools so the format has a single implementation.
 *
 * The canvas is Betaflight MSP DisplayPort glyph data in Artosyn's length-chained packing: 0xff
 * separators, then `b6 03 <row> <col> <attr> <glyphs...> <next_len>` records. See
 * docs/reference/msp-osd-format.md.
 *
 * VERBATIM COPY of libre/ipc/msp_canvas.h so hud/ owns its decoder and libre stays untouched.
 */
#ifndef MSP_CANVAS_H
#define MSP_CANVAS_H

/** @brief The largest raw canvas this parser accepts, in bytes; longer input is truncated. */
#define MSP_CANVAS_MAX 4096

/** @brief Event callbacks for msp_canvas_parse(); either field may be NULL to ignore that event. */
typedef struct {
    /** Called at each record header (b6 03 row col attr). */
    void (*on_record)(void *ctx, int row, int col, int attr);

    /** Called for each glyph, at its running (row, col). */
    void (*on_glyph)(void *ctx, int row, int col, int attr, unsigned char glyph);
} msp_canvas_sink_t;

/**
 * @brief Decode a raw OSD canvas, emitting each record header and glyph through @p sink.
 *
 * Strips the 0xff separators, then walks the b6-03 length-chained records. The byte just before the
 * next record's marker is that record's length (structural), so it is skipped, not emitted.
 * @param data Raw canvas bytes (from GetOsdContext or the OSD shared memory).
 * @param len  Byte count (clamped to MSP_CANVAS_MAX).
 * @param sink Event callbacks.
 * @param ctx  Opaque pointer forwarded to the callbacks.
 */
void msp_canvas_parse(const unsigned char *data, int len, const msp_canvas_sink_t *sink, void *ctx);

#endif /* MSP_CANVAS_H */
