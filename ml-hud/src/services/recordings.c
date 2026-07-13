/** @file recordings.c @brief Implementation; see recordings.h */
#include "recordings.h"
#include "board.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int has_mp4_suffix(const char *name)
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
        long size_mb = 0;
        if (stat(path, &info) == 0) {
            size_mb = (long) (info.st_size / (1024 * 1024));
        }

        snprintf(out[count].name, sizeof(out[count].name), "%s", entry->d_name);
        out[count].size_mb = size_mb;
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(out[0]), compare_name_desc);
    return count;
}
