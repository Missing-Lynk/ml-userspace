/** @file msp_canvas.c @brief Implementation; see msp_canvas.h.
 *  VERBATIM COPY of libre/ipc/msp_canvas.c (libre stays untouched).
 */
#include "msp_canvas.h"

#include <stddef.h>

/* The canvas is Betaflight MSP DisplayPort glyph data in Artosyn's packing (see
 * docs/reference/msp-osd-format.md): 0xff-separated bytes that, once stripped, form records of a
 * 2-byte marker, a 3-byte header (row, col, attr), then the row's glyph bytes.
 */
#define CANVAS_SEPARATOR          0xff   /* stripped before parsing */
#define MSP_DISPLAYPORT_CMD       0xb6   /* MSP command 182 */
#define DISPLAYPORT_WRITE_STRING  0x03   /* DisplayPort sub-command 3 */
#define RECORD_MARKER_LEN         2      /* CMD + sub-command */
#define RECORD_HEADER_LEN         3      /* row, col, attr */

/** @brief Whether an MSP DisplayPort write-string marker (b6 03) begins at packed[pos]. */
static int is_record_marker(const unsigned char *packed, int pos, int n)
{
    return pos + 1 < n
        && packed[pos] == MSP_DISPLAYPORT_CMD
        && packed[pos + 1] == DISPLAYPORT_WRITE_STRING;
}

void msp_canvas_parse(const unsigned char *data, int len, const msp_canvas_sink_t *sink, void *ctx)
{
    unsigned char packed[MSP_CANVAS_MAX];
    int n = 0;
    for (int i = 0; i < len && n < (int) sizeof(packed); i++) {
        if (data[i] != CANVAS_SEPARATOR) {
            packed[n++] = data[i];
        }
    }

    int i = 0;
    while (i + 1 < n) {
        if (!is_record_marker(packed, i, n)) {
            i++;
            continue;
        }

        i += RECORD_MARKER_LEN;
        if (i + RECORD_HEADER_LEN > n) {
            break;
        }

        int row = packed[i++];
        int col = packed[i++];
        int attr = packed[i++];
        if (sink->on_record != NULL) {
            sink->on_record(ctx, row, col, attr);
        }

        int c = col;
        while (i < n) {
            if (is_record_marker(packed, i, n)) {
                break;
            }

            /* The byte immediately before the next record's marker is that record's length
             * (structural, not a glyph). Skip it so it does not render as a stray fragment.
             */
            if (is_record_marker(packed, i + 1, n)) {
                i++;
                break;
            }

            unsigned char glyph = packed[i++];
            if (sink->on_glyph != NULL) {
                sink->on_glyph(ctx, row, c, attr, glyph);
            }
            c++;
        }
    }
}
