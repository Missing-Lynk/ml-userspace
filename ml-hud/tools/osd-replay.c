/**
 * @file osd-replay.c
 * @brief Replay the OSD/telemetry channel from an ml-sniff pcap - a bench drop-in for ml-linkd.
 *
 * Reads an ml-sniff SLL/cooked pcap (DLT 113), extracts the INCOMING (air -> goggle) UDP `:10000`
 * datagrams, and re-publishes each frame with the original inter-frame timing over the SAME mlm
 * seams ml-linkd uses (ml-shared/mlm.h): MSP canvases (0x10) as MLM_T_MSP on osd.sock, status frames
 * (0x09/0x11) as MLM_T_STATUS on telemetry.sock - MSG_DONTWAIT, dropped when no consumer is bound,
 * exactly like ml-linkd. The HUD therefore sees byte-identical records to the live radio through its
 * production code path, with no hardware in the loop.
 *
 * stdlib + sockets only, no dependencies. See re/notes/rf-telemetry-sdio0-10000.md for the format and
 * ml-hud/tools/btfl-osd-10000.pcap for the distilled corpus.
 *
 * Usage:
 *   osd-replay CAPTURE.pcap [--speed F] [--loop] [--no-timing] [--max N]
 */
#include "../src/channel/osd_proto.h"

#include "../../ml-shared/mlm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PCAP_GHDR_LEN 24
#define PCAP_RHDR_LEN 16
#define SLL_HDR_LEN   16
#define SLL_OUTGOING  4       /* sll packet_type for a frame WE sent (skip: not air -> goggle) */

/* pcap byte order, decided from the global-header magic. */
static int g_swap;

static uint32_t rd32(const unsigned char *p)
{
    uint32_t v = (uint32_t) p[0] | ((uint32_t) p[1] << 8)
               | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
    if (g_swap) {
        v = __builtin_bswap32(v);
    }

    return v;
}

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t) ((p[0] << 8) | p[1]);
}

/* One :10000 datagram to replay: its payload and capture timestamp (microseconds). */
typedef struct {
    unsigned char *data;
    int            len;
    uint64_t       ts_us;
} pkt_t;

/*
 * Locate the incoming :10000 UDP payload inside one SLL record. Returns a pointer into @rec (with
 * *out_len set) or NULL if the record is not an incoming IPv4/UDP :10000 datagram.
 */
static const unsigned char *sll_osd_payload(const unsigned char *rec, int incl, int *out_len)
{
    if (incl < SLL_HDR_LEN + 20) {
        return NULL;
    }

    if (be16(rec) == SLL_OUTGOING) {
        return NULL;                                 /* not air -> goggle */
    }

    const unsigned char *ip = rec + SLL_HDR_LEN;
    if ((ip[0] >> 4) != 4) {
        return NULL;                                 /* IPv4 only */
    }

    int ihl = (ip[0] & 0x0f) * 4;
    if (ip[9] != 17) {
        return NULL;                                 /* UDP */
    }

    if (incl < SLL_HDR_LEN + ihl + 8) {
        return NULL;
    }

    const unsigned char *udp = ip + ihl;
    if (be16(udp) != OSD10K_PORT && be16(udp + 2) != OSD10K_PORT) {
        return NULL;
    }

    int plen = be16(udp + 4) - 8;                    /* UDP total length - header */
    int avail = incl - (SLL_HDR_LEN + ihl + 8);
    if (plen < 0 || plen > avail) {
        plen = avail;                                /* trust the frame, not the length field */
    }

    if (plen <= 0) {
        return NULL;
    }

    *out_len = plen;
    return udp + 8;
}

/* Read every incoming :10000 datagram from @path into a freshly allocated array; returns the count
 * (0 on none/error) and stores the array in *out. */
static int load_pcap(const char *path, pkt_t **out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "osd-replay: cannot open %s\n", path);
        return 0;
    }

    unsigned char gh[PCAP_GHDR_LEN];
    if (fread(gh, 1, PCAP_GHDR_LEN, f) != PCAP_GHDR_LEN) {
        fprintf(stderr, "osd-replay: short pcap header\n");
        fclose(f);
        return 0;
    }

    uint32_t magic = (uint32_t) gh[0] | ((uint32_t) gh[1] << 8)
                   | ((uint32_t) gh[2] << 16) | ((uint32_t) gh[3] << 24);
    if (magic == 0xa1b2c3d4) {
        g_swap = 0;
    } else if (magic == 0xd4c3b2a1) {
        g_swap = 1;
    } else {
        fprintf(stderr, "osd-replay: not a pcap (magic %08x)\n", magic);
        fclose(f);
        return 0;
    }

    pkt_t *pkts = NULL;
    int n = 0, cap = 0;
    unsigned char rh[PCAP_RHDR_LEN];
    while (fread(rh, 1, PCAP_RHDR_LEN, f) == PCAP_RHDR_LEN) {
        uint32_t ts_sec = rd32(rh);
        uint32_t ts_usec = rd32(rh + 4);
        uint32_t incl = rd32(rh + 8);
        unsigned char *rec = malloc(incl);
        if (rec == NULL || fread(rec, 1, incl, f) != incl) {
            free(rec);
            break;
        }

        int plen = 0;
        const unsigned char *payload = sll_osd_payload(rec, (int) incl, &plen);
        if (payload != NULL) {
            if (n == cap) {
                cap = cap ? cap * 2 : 256;
                pkts = realloc(pkts, (size_t) cap * sizeof(*pkts));
            }
            pkts[n].data = malloc((size_t) plen);
            memcpy(pkts[n].data, payload, (size_t) plen);
            pkts[n].len = plen;
            pkts[n].ts_us = (uint64_t) ts_sec * 1000000ULL + ts_usec;
            n++;
        }
        free(rec);
    }

    fclose(f);
    *out = pkts;

    return n;
}

static void sleep_us(uint64_t us)
{
    struct timespec ts = { .tv_sec = (time_t) (us / 1000000ULL),
                           .tv_nsec = (long) ((us % 1000000ULL) * 1000ULL) };
    nanosleep(&ts, NULL);
}

/* Publish one raw :10000 frame over the mlm seam @path as record @type, ml-linkd style:
 * MSG_DONTWAIT to the consumer-bound socket, dropped on any error (a missing or slow consumer never
 * stalls the replay timing). Returns the sendto result. */
static ssize_t mlm_pub(int fd, const char *path, uint16_t type, const unsigned char *frame, int len)
{
    unsigned char buf[sizeof(struct mlm_hdr) + 2048];
    if (len < 0 || (size_t) len > sizeof(buf) - sizeof(struct mlm_hdr)) {
        return -1;
    }

    struct mlm_hdr h = { .magic = MLM_MAGIC, .type = type, .flags = 0 };
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), frame, (size_t) len);

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);

    return sendto(fd, buf, sizeof(h) + (size_t) len, MSG_DONTWAIT,
                  (struct sockaddr *) &a, sizeof(a));
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s CAPTURE.pcap [--speed F] [--loop] [--no-timing] [--max N]\n", prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    double speed = 1.0;
    int loop = 0, no_timing = 0;
    long maxn = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && path == NULL) {
            path = argv[i];
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            speed = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--loop")) {
            loop = 1;
        } else if (!strcmp(argv[i], "--no-timing")) {
            no_timing = 1;
        } else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            maxn = atol(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (path == NULL) {
        usage(argv[0]);
        return 2;
    }

    if (speed <= 0.0) {
        speed = 1.0;
    }

    pkt_t *pkts = NULL;
    int n = load_pcap(path, &pkts);
    if (n == 0) {
        fprintf(stderr, "osd-replay: no incoming :10000 datagrams in %s\n", path);
        free(pkts);
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "osd-replay: socket failed\n");
        return 1;
    }

    fprintf(stderr, "osd-replay: %d frame(s) -> %s + %s, speed %.2fx%s%s\n",
            n, MLM_OSD_SOCK, MLM_TELEMETRY_SOCK, speed,
            no_timing ? " (no timing)" : "", loop ? ", looping" : "");

    /* Route each frame the way ml-linkd does: MSP canvases to osd.sock, status to telemetry.sock.
     * `sent` counts frames that reached a consumer; `played` drives --max and keeps the timing
     * honest even while nothing is bound yet.
     */
    long sent = 0, played = 0;
    do {
        for (int i = 0; i < n && (maxn == 0 || played < maxn); i++) {
            if (!no_timing && i > 0) {
                uint64_t dt = pkts[i].ts_us > pkts[i - 1].ts_us ? pkts[i].ts_us - pkts[i - 1].ts_us : 0;
                sleep_us((uint64_t) ((double) dt / speed));
            }

            if (pkts[i].len < 4) {
                continue;
            }

            uint32_t type = (uint32_t) pkts[i].data[0] | ((uint32_t) pkts[i].data[1] << 8)
                          | ((uint32_t) pkts[i].data[2] << 16) | ((uint32_t) pkts[i].data[3] << 24);
            ssize_t w;
            if (type == OSD10K_MSG_OSD) {
                w = mlm_pub(fd, MLM_OSD_SOCK, MLM_T_MSP, pkts[i].data, pkts[i].len);
            } else if (type == OSD10K_MSG_VERSION || type == OSD10K_MSG_PERIODIC) {
                w = mlm_pub(fd, MLM_TELEMETRY_SOCK, MLM_T_STATUS, pkts[i].data, pkts[i].len);
            } else {
                continue;   /* association-time chatter (types 0x00..0x03); ml-linkd drops it too */
            }

            played++;
            if (w > 0) {
                sent++;
            }
        }
    } while (loop && (maxn == 0 || played < maxn));

    close(fd);
    fprintf(stderr, "osd-replay: %ld frame(s) played, %ld delivered\n", played, sent);

    for (int i = 0; i < n; i++) {
        free(pkts[i].data);
    }
    free(pkts);

    return 0;
}
