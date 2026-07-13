/** @file pipecmd.c @brief See pipecmd.h. */
#include "pipecmd.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../../ml-shared/mlm.h"

/* Cached unbound DGRAM socket; opened lazily on first send. sendto() addresses ctrl.sock each time,
 * so nothing needs reconnecting when the pipeline restarts.
 */
static int g_fd = -1;

static void send_cmd(uint32_t cmd, uint32_t arg)
{
    if (g_fd < 0) {
        g_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (g_fd < 0) {
            perror("pipecmd: socket");
            return;
        }
    }

    struct {
        struct mlm_hdr h;
        struct mlm_cmd cmd;
    } __attribute__((packed)) rec = {
        .h = { .magic = MLM_MAGIC, .type = MLM_T_CMD, .flags = 0 },
        .cmd = { .cmd = cmd, .arg = arg },
    };

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, MLM_CTRL_SOCK, sizeof addr.sun_path - 1);
    /* Best-effort: pipeline down (ENOENT/ECONNREFUSED) just drops the command. */
    sendto(g_fd, &rec, sizeof rec, MSG_DONTWAIT, (struct sockaddr *) &addr, sizeof addr);
}

void pipecmd_record_toggle(void)
{
    send_cmd(MLM_CMD_REC_TOGGLE, 0);
}
