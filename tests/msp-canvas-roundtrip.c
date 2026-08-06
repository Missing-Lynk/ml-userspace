/**
 * @file msp-canvas-roundtrip.c
 * @brief Host test: our canvas encoder must reproduce the vendor's bytes exactly.
 *
 * The air unit does not forward the FC's MSP DisplayPort frames, it re-encodes them into Artosyn's
 * canvas format (ml-linkd/ml-msp.c emit_canvas). A vendor goggle renders only what libvtxfc would
 * have produced, so any structural difference in the 9-byte header, the length chaining or the 0xff
 * interleave breaks it. Testing our encoder against our own decoder cannot catch that: both were
 * written from the same reading of the format, so a shared misreading passes.
 *
 * This closes the loop against the vendor instead. For every 0x10 frame in a captured VENDOR
 * session:
 *
 *   1. decode it with the goggle-side decoder (ml-hud msp_canvas_parse) into row/col/attr/glyphs,
 *   2. synthesise the MSP v1 DisplayPort frames an FC would have sent to produce that,
 *   3. feed them through the real air-side parser and encoder (ml_msp_service -> emit_canvas),
 *   4. require the emitted canvas to equal the vendor's bytes.
 *
 * The canvas sequence is seeded from the vendor frame because it is our own counter, not the FC's.
 *
 * Limitation worth stating: step 2 rebuilds the MSP input from DECODED records, so a field our
 * decoder drops is absent from both sides and cannot fail here. It pins the encoder's byte layout
 * against the vendor, not our reading of every field.
 *
 * Usage: msp-canvas-roundtrip [CAPTURE.pcap]
 */
#include "../ml-linkd/ml-msp.h"
#include "../ml-hud/src/osd/msp_canvas.h"

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PCAP  "ml-hud/tools/btfl-osd-10000.pcap"

#define PCAP_GHDR_LEN 24
#define PCAP_RHDR_LEN 16
#define SLL_HDR_LEN   16
#define SLL_OUTGOING  4       /* sll packet_type for a frame WE sent, so not air -> goggle */

#define OSD10K_PORT       10000
#define OSD10K_HEADER_LEN 20
#define OSD10K_MSG_OSD    0x10

#define MSP_DISPLAYPORT      0xb6
#define MSP_DP_WRITE_STRING  0x03
#define MSP_DP_DRAW_SCREEN   0x04

#define MAX_CANVAS   4096
#define MAX_RECORDS  256
#define MAX_TEXT     128

struct record {
    int row;
    int col;
    int attr;
    unsigned char text[MAX_TEXT];
    int text_len;
};

struct recset {
    struct record r[MAX_RECORDS];
    int n;
    int overflow;
};

struct captured {
    uint8_t buf[MAX_CANVAS];
    size_t len;
    int calls;
};

static unsigned be16(const unsigned char *p)
{
    return (unsigned)((p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t le32(const unsigned char *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* Strip the 0xff interleave, recovering the plain header+records the packing was applied to. */
static size_t unpack(uint8_t *dst, size_t dstsz, const uint8_t *src, size_t len)
{
    size_t d = 0;

    for (size_t i = 0; i < len && d < dstsz; i++) {
        if (src[i] != 0xff) {
            dst[d++] = src[i];
        }
    }

    return d;
}

static void on_record(void *ctx, int row, int col, int attr)
{
    struct recset *rs = ctx;

    if (rs->n >= MAX_RECORDS) {
        rs->overflow = 1;
        return;
    }

    rs->r[rs->n].row = row;
    rs->r[rs->n].col = col;
    rs->r[rs->n].attr = attr;
    rs->r[rs->n].text_len = 0;
    rs->n++;
}

static void on_glyph(void *ctx, int row, int col, int attr, unsigned char glyph)
{
    struct recset *rs = ctx;
    struct record *r;

    (void)row;
    (void)col;
    (void)attr;

    if (rs->n == 0) {
        return;
    }

    r = &rs->r[rs->n - 1];
    if (r->text_len >= MAX_TEXT) {
        rs->overflow = 1;
        return;
    }

    r->text[r->text_len++] = glyph;
}

/* One MSP v1 reply frame: $ M > <len> <cmd> <payload> <xor over len,cmd,payload>. */
static size_t msp_frame(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t ck = (uint8_t)(len ^ cmd);

    out[0] = '$';
    out[1] = 'M';
    out[2] = '>';
    out[3] = len;
    out[4] = cmd;
    memcpy(out + 5, payload, len);

    for (uint8_t i = 0; i < len; i++) {
        ck ^= payload[i];
    }

    out[5 + len] = ck;

    return (size_t)len + 6;
}

static void capture_canvas(const uint8_t *canvas, size_t len, void *ctx)
{
    struct captured *c = ctx;

    c->calls++;
    if (len > sizeof c->buf) {
        len = sizeof c->buf;
    }

    memcpy(c->buf, canvas, len);
    c->len = len;
}

/*
 * Drive the real air-side parser and encoder with the MSP frames an FC would have sent for @rs, and
 * return the canvas it emits. Returns 0 on success.
 */
static int reencode(const struct recset *rs, uint32_t seed_seq, struct captured *out)
{
    struct ml_msp msp;
    uint8_t stream[8192];
    size_t s = 0;
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return -1;
    }

    /* ml_msp_service reads until read() stops returning data, so the fd must not block. */
    if (fcntl(sv[0], F_SETFL, O_NONBLOCK) != 0) {
        perror("fcntl");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    for (int i = 0; i < rs->n; i++) {
        uint8_t payload[4 + MAX_TEXT];
        uint8_t len = (uint8_t)(4 + rs->r[i].text_len);

        payload[0] = MSP_DP_WRITE_STRING;
        payload[1] = (uint8_t)rs->r[i].row;
        payload[2] = (uint8_t)rs->r[i].col;
        payload[3] = (uint8_t)rs->r[i].attr;
        memcpy(payload + 4, rs->r[i].text, (size_t)rs->r[i].text_len);

        if (s + len + 6 > sizeof stream) {
            fprintf(stderr, "  stream buffer too small\n");
            close(sv[0]);
            close(sv[1]);
            return -1;
        }

        s += msp_frame(stream + s, MSP_DISPLAYPORT, payload, len);
    }

    {
        uint8_t draw = MSP_DP_DRAW_SCREEN;
        s += msp_frame(stream + s, MSP_DISPLAYPORT, &draw, 1);
    }

    memset(&msp, 0, sizeof msp);
    ml_msp_init(&msp, capture_canvas, out);
    msp.fd = sv[0];

    /* Suppress the polls: this test drives the receive path only, and the peer is a socketpair. */
    msp.next_status_ms = LONG_MAX;
    msp.next_battery_ms = LONG_MAX;

    /* emit_canvas pre-increments, so seed one below the value the vendor frame carries. */
    msp.canvas_seq = seed_seq - 1;

    if (write(sv[1], stream, s) != (ssize_t)s) {
        perror("write");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    ml_msp_service(&msp, 0);

    close(sv[0]);
    close(sv[1]);

    return 0;
}

static void dump_diff(const uint8_t *want, size_t want_len, const uint8_t *got, size_t got_len,
                      const char *label)
{
    size_t n = want_len < got_len ? want_len : got_len;
    size_t at = n;

    for (size_t i = 0; i < n; i++) {
        if (want[i] != got[i]) {
            at = i;
            break;
        }
    }

    printf("      %s: vendor %zu bytes, ours %zu bytes", label, want_len, got_len);
    if (at < n) {
        printf(", first difference at offset %zu (vendor 0x%02x, ours 0x%02x)", at, want[at], got[at]);
    } else if (want_len != got_len) {
        printf(", identical up to the shorter length");
    }

    printf("\n");
}

static int check_canvas(const uint8_t *vendor, size_t vendor_len, int index)
{
    struct recset rs;
    struct captured got;
    msp_canvas_sink_t sink = { on_record, on_glyph };
    uint8_t vendor_plain[MAX_CANVAS];
    uint8_t ours_plain[MAX_CANVAS];
    size_t vendor_plain_len;
    size_t ours_plain_len;
    uint32_t seq;

    memset(&rs, 0, sizeof rs);
    memset(&got, 0, sizeof got);

    vendor_plain_len = unpack(vendor_plain, sizeof vendor_plain, vendor, vendor_len);
    if (vendor_plain_len < 9) {
        printf("  frame %d: SKIP, unpacked to %zu bytes, no room for the 9-byte header\n",
               index, vendor_plain_len);
        return 0;
    }

    seq = be32(vendor_plain + 1);

    msp_canvas_parse(vendor, (int)vendor_len, &sink, &rs);
    if (rs.n == 0) {
        printf("  frame %d: SKIP, decoded to no records\n", index);
        return 0;
    }

    if (rs.overflow) {
        printf("  frame %d: SKIP, decoded records exceed this test's limits\n", index);
        return 0;
    }

    if (reencode(&rs, seq, &got) != 0) {
        return -1;
    }

    if (got.calls != 1) {
        printf("  frame %d: FAIL, encoder produced %d canvases, expected 1\n", index, got.calls);
        return -1;
    }

    if (got.len == vendor_len && memcmp(got.buf, vendor, vendor_len) == 0) {
        return 0;
    }

    printf("  frame %d: FAIL, %d records, vendor header count=%u seq=%u len=%u\n",
           index, rs.n, vendor_plain[0], seq, be32(vendor_plain + 5));

    dump_diff(vendor, vendor_len, got.buf, got.len, "packed  ");

    ours_plain_len = unpack(ours_plain, sizeof ours_plain, got.buf, got.len);
    dump_diff(vendor_plain, vendor_plain_len, ours_plain, ours_plain_len, "unpacked");

    if (ours_plain_len >= 9) {
        printf("      header: vendor count=%u seq=%u len=%u | ours count=%u seq=%u len=%u\n",
               vendor_plain[0], be32(vendor_plain + 1), be32(vendor_plain + 5),
               ours_plain[0], be32(ours_plain + 1), be32(ours_plain + 5));
    }

    return -1;
}

/* Locate the incoming :10000 UDP payload inside one SLL record, or NULL. */
static const unsigned char *sll_osd_payload(const unsigned char *rec, int incl, int *out_len)
{
    const unsigned char *ip;
    const unsigned char *udp;
    int ihl;
    int plen;
    int avail;

    if (incl < SLL_HDR_LEN + 20) {
        return NULL;
    }

    if (be16(rec) == SLL_OUTGOING) {
        return NULL;
    }

    ip = rec + SLL_HDR_LEN;
    if ((ip[0] >> 4) != 4 || ip[9] != 17) {
        return NULL;
    }

    ihl = (ip[0] & 0x0f) * 4;
    if (incl < SLL_HDR_LEN + ihl + 8) {
        return NULL;
    }

    udp = ip + ihl;
    if ((int)be16(udp) != OSD10K_PORT && (int)be16(udp + 2) != OSD10K_PORT) {
        return NULL;
    }

    plen = (int)be16(udp + 4) - 8;
    avail = incl - (SLL_HDR_LEN + ihl + 8);
    if (plen < 0 || plen > avail) {
        plen = avail;
    }

    *out_len = plen;

    return udp + 8;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : DEFAULT_PCAP;
    unsigned char gh[PCAP_GHDR_LEN];
    unsigned char rh[PCAP_RHDR_LEN];
    FILE *f;
    int seen = 0;
    int failed = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    if (fread(gh, 1, PCAP_GHDR_LEN, f) != PCAP_GHDR_LEN) {
        fprintf(stderr, "%s: short pcap global header\n", path);
        fclose(f);
        return 2;
    }

    printf("msp-canvas-roundtrip: %s\n", path);

    while (fread(rh, 1, PCAP_RHDR_LEN, f) == PCAP_RHDR_LEN) {
        unsigned char rec[65536];
        const unsigned char *payload;
        uint32_t incl = le32(rh + 8);
        int plen = 0;

        if (incl == 0 || incl > sizeof rec) {
            break;
        }

        if (fread(rec, 1, incl, f) != incl) {
            break;
        }

        payload = sll_osd_payload(rec, (int)incl, &plen);
        if (payload == NULL || plen < OSD10K_HEADER_LEN) {
            continue;
        }

        if (le32(payload) != OSD10K_MSG_OSD) {
            continue;
        }

        {
            uint32_t canvas_len = le32(payload + 16);
            const unsigned char *canvas = payload + OSD10K_HEADER_LEN;

            if (canvas_len == 0 || canvas_len > (uint32_t)(plen - OSD10K_HEADER_LEN) ||
                canvas_len > MAX_CANVAS) {
                continue;
            }

            seen++;
            if (check_canvas(canvas, canvas_len, seen) != 0) {
                failed++;
            }
        }
    }

    fclose(f);

    printf("%d canvas frames checked, %d mismatched\n", seen, failed);

    if (seen == 0) {
        printf("NO CANVASES FOUND: the capture carried no 0x10 frames, nothing was proven\n");
        return 2;
    }

    return failed == 0 ? 0 : 1;
}
