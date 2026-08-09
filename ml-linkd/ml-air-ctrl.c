/*
 * ml-air-ctrl.c - one-shot requests to ml-air-video's control socket.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ml-air-ctrl.h"

/* Send one line to ml-air-video's control socket. Returns 0 when it answered "ok". A fresh connect
 * per command: the socket is a one-shot request/reply and ml-air-video may not be up yet. */
int air_ctrl_send(const char *cmd)
{
    const char *path = getenv("ML_AIR_CTRL");
    struct timeval tv = { .tv_sec = 0, .tv_usec = AIR_CTRL_TIMEOUT_MS * 1000 };
    struct sockaddr_un addr;
    char reply[128];
    size_t len = strlen(cmd);
    ssize_t n;
    int fd;

    if (path == NULL || path[0] == '\0') {
        path = AIR_CTRL_SOCK;
    }

    if (strlen(path) >= sizeof addr.sun_path) {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }

    if (write(fd, cmd, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }

    n = read(fd, reply, sizeof reply - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }

    reply[n] = '\0';

    return strncmp(reply, "ok ", 3) == 0 ? 0 : -1;
}