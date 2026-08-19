/*
 * ml-air-cam.c - SetCameraInfo (0x0C) on the air role. See ml-air-cam.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../ml-shared/mlm.h"
#include "ml-linkd.h"
#include "mp-cmd.h"
#include "ml-air-cam.h"

/* The file the ml-air-ae init script reads at boot and ml-aed re-reads on SIGHUP. Both parse it
 * the same way: absent = off, empty = 50, else the number forced to {0, 50, 60}. */
#define AIR_CAM_BANDING_DIR   "/usrdata/missinglynk"
#define AIR_CAM_BANDING_FILE  AIR_CAM_BANDING_DIR "/banding"

int air_cam_parse_banding(const uint8_t *dgram, ssize_t n, unsigned int *hz)
{
    struct mp_camera body;
    uint32_t msg_type;

    if (n < MP_CAM_BODY_OFF + (ssize_t)MP_CAM_BODY_LEN) {
        return 0;
    }

    memcpy(&msg_type, dgram, 4);
    if (msg_type != MP_SETCAMERA) {
        return 0;
    }

    memcpy(&body, dgram + MP_CAM_BODY_OFF, sizeof body);
    if (body.selector != MLM_CAM_BANDING) {
        return 0;
    }

    /* The vendor's handler forces anything but 50/60 to off rather than rejecting it. */
    *hz = (body.banding == 50 || body.banding == 60) ? body.banding : 0;

    return 1;
}

/* SIGHUP every ml-aed. By /proc scan rather than pkill, so a rootfs without procps still works
 * and nothing is exec'd from a network-driven path. */
static void air_cam_signal_aed(void)
{
    DIR *proc = opendir("/proc");
    struct dirent *de;

    if (proc == NULL) {
        return;
    }

    while ((de = readdir(proc)) != NULL) {
        char path[64], comm[32];
        char *end;
        long pid = strtol(de->d_name, &end, 10);
        int fd;
        ssize_t got;

        if (*end != '\0' || pid <= 0) {
            continue;
        }

        snprintf(path, sizeof path, "/proc/%ld/comm", pid);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }

        got = read(fd, comm, sizeof comm - 1);
        close(fd);
        if (got <= 0) {
            continue;
        }

        comm[got] = '\0';
        if (comm[got - 1] == '\n') {
            comm[got - 1] = '\0';
        }

        if (strcmp(comm, "ml-aed") == 0) {
            kill((pid_t)pid, SIGHUP);
        }
    } /* for each /proc entry */

    closedir(proc);
}

/* Persist the mode where the boot path reads it. Atomic on the same filesystem, so a power cut
 * mid-write leaves the old file rather than a truncated one. */
static int air_cam_persist(unsigned int hz)
{
    char tmp[] = AIR_CAM_BANDING_DIR "/.banding.new";
    char buf[8];
    int fd, len;

    mkdir(AIR_CAM_BANDING_DIR, 0755);

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -errno;
    }

    len = snprintf(buf, sizeof buf, "%u\n", hz);
    if (write(fd, buf, len) != len) {
        close(fd);
        unlink(tmp);

        return -EIO;
    }

    close(fd);
    if (rename(tmp, AIR_CAM_BANDING_FILE) != 0) {
        unlink(tmp);

        return -errno;
    }

    return 0;
}

void air_cam_set(const uint8_t *dgram, ssize_t n)
{
    static int applied_hz = -1;         /* nothing applied yet this run */
    unsigned int hz;

    if (!air_cam_parse_banding(dgram, n, &hz)) {
        if (g_verbose && n >= MP_CAM_BODY_OFF + 4) {
            uint32_t sel;

            memcpy(&sel, dgram + MP_CAM_BODY_OFF, 4);
            fprintf(stderr, TAG " rx SetCameraInfo sel=%u, no handler, ignored\n", sel);
        }

        return;
    }

    /* The goggle re-asserts its commanded selectors every session, so an unchanged value arrives
     * routinely; /usrdata is flash, so it is not rewritten for that. */
    if ((int)hz == applied_hz) {
        return;
    }

    if (air_cam_persist(hz) != 0) {
        fprintf(stderr, TAG " banding %u Hz: persist to %s failed: %s\n",
                hz, AIR_CAM_BANDING_FILE, strerror(errno));
    }

    air_cam_signal_aed();
    applied_hz = (int)hz;

    printf(TAG " banding %u Hz (persisted, ml-aed signalled)\n", hz);
    fflush(stdout);
}
