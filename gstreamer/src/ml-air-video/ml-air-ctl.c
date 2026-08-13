// SPDX-License-Identifier: MIT
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PROG "ml-air-ctl"

static const char *env_or(const char *name, const char *dflt)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : dflt;
}

int main(int argc, char **argv)
{
    const char *path = env_or("ML_AIR_CTRL", "/run/missinglynk/air-video.sock");
    struct sockaddr_un addr;
    char cmd[128];
    char reply[256];
    ssize_t n;
    int fd;
    int arg = 1;

    if (argc > 1 && argv[1][0] == '/') {
        path = argv[1];
        arg = 2;
    }

    /* keyframe and session-reset take no argument of their own. */
    int noarg = argc - arg >= 1 && (strcmp(argv[arg], "keyframe") == 0 ||
                                    strcmp(argv[arg], "session-reset") == 0);

    if (
        argc - arg < 1 ||
        (argc - arg < 2 && !noarg) ||
        (strcmp(argv[arg], "bitrate") != 0 && strcmp(argv[arg], "fps") != 0 &&
        strcmp(argv[arg], "capfps") != 0 &&
        strcmp(argv[arg], "rate") != 0 && !noarg)
    ) {
        fprintf(stderr,
                "usage: " PROG " [socket] bitrate <bps-per-tile> [vbv-ms]\n"
                "       " PROG " [socket] fps <fps>\n"
                "       " PROG " [socket] capfps <fps>   (feeder only, rate control untouched)\n"
                "       " PROG " [socket] rate <bps-per-tile> <fps> [vbv-ms]\n"
                "       " PROG " [socket] keyframe\n"
                "       " PROG " [socket] session-reset   (what ml-linkd sends a new receiver)\n");
        return 2;
    }

    if (noarg) {
        snprintf(cmd, sizeof cmd, "%s\n", argv[arg]);
    } else if (strcmp(argv[arg], "rate") == 0 && argc - arg >= 4) {
        snprintf(cmd, sizeof cmd, "rate %s %s %s\n", argv[arg + 1], argv[arg + 2],
                 argv[arg + 3]);
    } else if (strcmp(argv[arg], "rate") == 0 && argc - arg >= 3) {
        snprintf(cmd, sizeof cmd, "rate %s %s\n", argv[arg + 1], argv[arg + 2]);
    } else if (strcmp(argv[arg], "bitrate") == 0 && argc - arg >= 3) {
        snprintf(cmd, sizeof cmd, "bitrate %s %s\n", argv[arg + 1], argv[arg + 2]);
    } else {
        snprintf(cmd, sizeof cmd, "%s %s\n", argv[arg], argv[arg + 1]);
    }

    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "socket path too long: %s\n", path);
        return 2;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "connect %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, cmd, strlen(cmd)) != (ssize_t)strlen(cmd)) {
        perror("write");
        close(fd);
        return 1;
    }

    n = read(fd, reply, sizeof reply - 1);
    if (n < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    reply[n] = '\0';
    fputs(reply, stdout);
    close(fd);

    return strncmp(reply, "ok ", 3) == 0 ? 0 : 1;
}
