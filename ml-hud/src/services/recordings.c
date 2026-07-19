/** @file recordings.c @brief Implementation; see recordings.h */
#include "recordings.h"
#include "board.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Below this a recording cannot hold even a single compressed 1080p keyframe, so it is an empty or
 * aborted capture (0-byte, or a header-only stub) with nothing to play - hidden from the list. */
#define MIN_PLAYABLE_BYTES (64 * 1024)

static bool has_mp4_suffix(const char *name)
{
    size_t length = strlen(name);
    if (length < 4) {
        return 0;
    }

    return strcasecmp(name + length - 4, ".mp4") == 0;
}

static int compare_name_desc(const void *a, const void *b)
{
    const recording_t *ra = (const recording_t *) a;
    const recording_t *rb = (const recording_t *) b;

    return strcmp(rb->name, ra->name);
}

int recordings_list(recording_t *out, int max)
{
    DIR *dir = opendir(board_current()->sdcard_mount);
    if (dir == NULL) {
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while (count < max && (entry = readdir(dir)) != NULL) {
        if (!has_mp4_suffix(entry->d_name)) {
            continue;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", board_current()->sdcard_mount, entry->d_name);
        struct stat info;
        if (stat(path, &info) != 0 || info.st_size < MIN_PLAYABLE_BYTES) {
            continue;   /* empty or aborted recording (no decodable frame): not worth listing */
        }

        snprintf(out[count].name, sizeof(out[count].name), "%s", entry->d_name);
        out[count].size_mb = (long) (info.st_size / (1024 * 1024));
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(out[0]), compare_name_desc);
    return count;
}

void recordings_path(const char *name, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", board_current()->sdcard_mount, name);
}

static uint32_t read_be32(const unsigned char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static uint64_t read_be64(const unsigned char *p)
{
    return ((uint64_t) read_be32(p) << 32) | read_be32(p + 4);
}

/* Read a positioned mvhd body into `timescale` + `duration`. The timescale and duration follow the
 * create/modify times, whose width depends on the version (v0: 32-bit, v1: 64-bit).
 */
static void read_mvhd(FILE *f, uint32_t *timescale, uint64_t *duration)
{
    unsigned char b[32];
    if (fread(b, 1, sizeof b, f) != sizeof b) {
        return;
    }

    if (b[0] == 1) {
        *timescale = read_be32(b + 4 + 8 + 8);
        *duration = read_be64(b + 4 + 8 + 8 + 4);
    } else {
        *timescale = read_be32(b + 4 + 4 + 4);
        *duration = read_be32(b + 4 + 4 + 4 + 4);
    }
}

/* Walk an mvex body of `mvex_size` bytes for its mehd child and read the fragment_duration into
 * `frag_dur`. mehd carries the total duration of a fragmented (streaming) MP4, whose mvhd duration is
 * 0 because the samples live in the moof fragments rather than the movie header.
 */
static void read_mvex_frag_dur(FILE *f, uint64_t mvex_size, uint64_t *frag_dur)
{
    uint64_t remaining = mvex_size;
    while (remaining >= 8) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) {
            return;
        }

        uint64_t size = read_be32(hdr);
        if (size < 8 || size > remaining) {
            return;
        }

        if (memcmp(hdr + 4, "mehd", 4) == 0) {
            unsigned char verflags[4];
            unsigned char dur[8];
            if (fread(verflags, 1, 4, f) != 4) {
                return;
            }

            int wide = (verflags[0] == 1);   /* v1: 64-bit fragment_duration, v0: 32-bit */
            if (fread(dur, 1, wide ? 8 : 4, f) != (wide ? 8u : 4u)) {
                return;
            }

            *frag_dur = wide ? read_be64(dur) : read_be32(dur);
            return;
        }

        if (fseeko(f, (off_t) (size - 8), SEEK_CUR) != 0) {
            return;
        }

        remaining -= size;
    }
}

/* Read the clip duration from an already-positioned moov body of `moov_size` bytes. Prefers the mvhd
 * duration; falls back to the mvex/mehd fragment_duration for fragmented recordings (mvhd duration 0).
 * Returns duration in ms, or 0 if neither is present.
 */
static unsigned parse_moov(FILE *f, uint64_t moov_size)
{
    uint64_t remaining = moov_size;
    uint32_t timescale = 0;
    uint64_t mvhd_dur = 0;
    uint64_t frag_dur = 0;

    while (remaining >= 8) {
        unsigned char hdr[8];
        if (fread(hdr, 1, 8, f) != 8) {
            break;
        }

        uint64_t size = read_be32(hdr);
        if (size < 8 || size > remaining) {
            break;   /* malformed or truncated */
        }

        off_t body_start = ftello(f);
        if (memcmp(hdr + 4, "mvhd", 4) == 0) {
            read_mvhd(f, &timescale, &mvhd_dur);
        } else if (memcmp(hdr + 4, "mvex", 4) == 0) {
            read_mvex_frag_dur(f, size - 8, &frag_dur);
        }

        /* Re-anchor to the atom boundary regardless of how far the child reader advanced. */
        if (fseeko(f, body_start + (off_t) (size - 8), SEEK_SET) != 0) {
            break;
        }

        remaining -= size;
    }

    if (timescale == 0) {
        return 0;
    }

    uint64_t duration = mvhd_dur ? mvhd_dur : frag_dur;
    uint64_t ms = duration * 1000ULL / timescale;

    /* An empty/aborted recording can carry an uninitialised mehd fragment_duration (a huge garbage
     * value). Reject an implausible length (> 24 h) so the row falls back to the file size. */
    if (ms > 24ULL * 3600 * 1000) {
        return 0;
    }

    return (unsigned) ms;
}

/* One-entry-per-clip cache, keyed by identity (size + mtime), so re-rendering the list is free and a
 * replaced file (same name) is re-parsed. Small and linear - the recordings list is short. */
#define DUR_CACHE_MAX 64
static struct dur_cache {
    char name[64];
    off_t size;
    time_t mtime;
    unsigned ms;
} g_dur_cache[DUR_CACHE_MAX];
static int g_dur_cache_n;

static unsigned dur_cache_get(const struct stat *info, const char *name, int *found)
{
    for (int i = 0; i < g_dur_cache_n; i++) {
        if (g_dur_cache[i].size == info->st_size && g_dur_cache[i].mtime == info->st_mtime
            && strcmp(g_dur_cache[i].name, name) == 0) {
            *found = 1;
            return g_dur_cache[i].ms;
        }
    }

    *found = 0;
    return 0;
}

static void dur_cache_put(const struct stat *info, const char *name, unsigned ms)
{
    struct dur_cache *slot = (g_dur_cache_n < DUR_CACHE_MAX)
                             ? &g_dur_cache[g_dur_cache_n++]
                             : &g_dur_cache[0];   /* full: recycle slot 0 (rare - far fewer clips than the cap) */
    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->size = info->st_size;
    slot->mtime = info->st_mtime;
    slot->ms = ms;
}

unsigned recordings_duration_ms(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        return 0;
    }

    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    int found = 0;
    unsigned cached = dur_cache_get(&info, name, &found);

    if (found) {
        return cached;
    }

    unsigned ms = 0;
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        /* Walk top-level atoms, following each size header; mdat is skipped, never read. */
        for (;;) {
            unsigned char hdr[16];
            if (fread(hdr, 1, 8, f) != 8) {
                break;
            }

            uint64_t size = read_be32(hdr);
            int header_len = 8;
            if (size == 1) {   /* 64-bit extended size in the 8 bytes after the type */
                if (fread(hdr + 8, 1, 8, f) != 8) {
                    break;
                }
                size = read_be64(hdr + 8);
                header_len = 16;
            }

            if (size < (uint64_t) header_len) {
                break;   /* malformed */
            }

            if (memcmp(hdr + 4, "moov", 4) == 0) {
                ms = parse_moov(f, size - header_len);
                break;
            }

            if (fseeko(f, (off_t) (size - header_len), SEEK_CUR) != 0) {
                break;   /* skip this atom's body (e.g. the large mdat) */
            }
        }

        fclose(f);
    }

    dur_cache_put(&info, name, ms);
    return ms;
}
