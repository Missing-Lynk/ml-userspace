/* ml-pipeline pmsg breadcrumbs (ML_PMSG=1): a per-frame trail in persistent DRAM.
 *
 * The hard freeze stops the goggle with a frame on the panel, no ping and nothing on the
 * console, so the last thing ml-pipeline logged through stderr never reaches the flash. The
 * kernel's ramoops region (ramoops@29100000 in the board DTS) is memory that survives the
 * watchdog reset, and /dev/pmsg0 is its userspace door: a write lands in DRAM immediately and
 * reappears after the reset as /sys/fs/pstore/pmsg-ramoops-0.
 *
 * One character per pipeline phase, so the 128 KiB ring holds roughly 6000 entries, about twenty
 * seconds at 60 fps. The ring wraps, so what remains is always the most recent history, which
 * is the part the freeze is in:
 *
 *   B / b   dmablit ioctl entered / returned   (the AXI DMA engine)
 *   I / S   flip commit entered / returned     (DRM atomic or page-flip)
 *   E       flip event received                (the frame is latched)
 *
 * A trail ending in B, I or S names the call that never came back, which is the one question
 * the console log cannot answer. Every phase is display-path-hot, so with ML_PMSG unset each
 * mark returns on the first branch and the device node is never opened.
 */
#include "ml-pipeline.h"

#include <fcntl.h>
#include <unistd.h>

/* The kernel writes each write() into the ring as one record, so one syscall per phase keeps
 * entries whole even when the ring wraps mid-frame.
 */
void pmsg_mark(struct ctx *c, char phase, GstClockTime pts)
{
    if (c->pmsg_fd < 0) {
        return;
    }

    char line[48];
    int n = snprintf(line, sizeof line, "%c %llu %lld\n", phase,
                     (unsigned long long)(pts / GST_MSECOND),
                     (long long)g_get_monotonic_time());

    if (n > 0) {
        /* Best effort by design: a full or absent ring must never disturb the display path,
         * and there is no recovery a dropped breadcrumb would benefit from. */
        (void)!write(c->pmsg_fd, line, (size_t)n);
    }
}

void pmsg_init(struct ctx *c)
{
    c->pmsg_fd = -1;
    if (!getenv("ML_PMSG")) {
        return;
    }

    c->pmsg_fd = open("/dev/pmsg0", O_WRONLY | O_CLOEXEC);
    if (c->pmsg_fd < 0) {
        fprintf(stderr, "ml-pipeline: ML_PMSG set but /dev/pmsg0 is absent; "
                        "breadcrumbs need a kernel built with EXTRA_FRAGMENTS=debug-freeze\n");
        return;
    }

    /* One line per generation, so a harvested ring shows where this run's trail begins. */
    dprintf(c->pmsg_fd, "--- ml-pipeline pid %d\n", (int)getpid());
    fprintf(stderr, "ml-pipeline: pmsg breadcrumbs on (/dev/pmsg0)\n");
}
