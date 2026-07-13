/*
 * ml-rf-bringup - one-shot AR8030 RF link bring-up.
 *
 * Runs the full ground-side sequence: release the AR8030 from reset, re-probe the SDIO host so the
 * now-awake chip enumerates, download the baseband firmware into the chip by loading artosyn_sdio,
 * then bring sdio0 up as 10.0.0.1. On success the RF link can associate and video flows.
 *
 * Idempotent: exits 0 if artosyn_sdio is already loaded (a warm reload of that driver hangs the
 * device, so it must never happen). Any failed phase logs the reason and exits non-zero; the caller
 * (ml-video) treats that as "no video this boot" and leaves SSH/serial up.
 *
 * Usage: ml-rf-bringup <fw_name> <cfg_name>   (both required; the caller names the baseband blobs).
 * The blobs live in /lib/firmware (baked into the rootfs); the driver pulls them from there at load
 * time. ML_RF_FW_PATH overrides that search path for the RAM-boot dev flow (blobs in /run), and
 * ML_RF_KO overrides the artosyn_sdio.ko path.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#include <linux/gpio.h>

#define PROG            "ml-rf-bringup"
#define GPIO_CHIP_LABEL "ar-gpio1"   /* the AR8030 reset line sits on this artosyn_gpio bank */
#define GPIO_RESET_LINE 0            /* line 0 = GPIO23 = AR8030 reset */
#define RF_MODULE       "artosyn_sdio"
#define RF_HOST         "1b00000.mmc"
#define RF_DRIVER       "/sys/bus/platform/drivers/dwmmc_artosyn"
#define SDIO_IFACE      "sdio0"
#define SDIO_ADDR       "10.0.0.1"
#define SDIO_NETMASK    "255.255.255.0"
#define GPIOCHIP_FMT    "/dev/gpiochip%d"
#define SDIO_BUS        "/sys/bus/sdio/devices"
#define SYS_NET         "/sys/class/net/"
#define FW_PATH_ATTR    "/sys/module/firmware_class/parameters/path"
#define WAIT_TRIES      50           /* x 200ms = 10s per wait phase */
#define WAIT_STEP_US    200000

static int finit_module(int fd, const char *params, int flags)
{
    return (int) syscall(SYS_finit_module, fd, params, flags);
}

/* Write a NUL-terminated string to a sysfs attribute. Returns 0 on success, -1 on error. */
static int write_attr(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t written = write(fd, value, strlen(value));
    close(fd);

    return (written < 0) ? -1 : 0;
}

/* True if the named module is present in /proc/modules (matched at the start of a line). */
static int module_loaded(const char *name)
{
    FILE *modules = fopen("/proc/modules", "r");
    if (modules == NULL) {
        return 0;
    }

    char line[256];
    size_t name_len = strlen(name);
    int found = 0;
    while (fgets(line, sizeof(line), modules) != NULL) {
        if (strncmp(line, name, name_len) == 0 && line[name_len] == ' ') {
            found = 1;
            break;
        }
    }

    fclose(modules);
    return found;
}

/* Pulse the AR8030 reset line low then high via the GPIO chardev v2 ABI (no sysfs GPIO on this
 * kernel). Returns 0 on success.
 */
static int release_reset(void)
{
    int chip_fd = -1;
    char path[32];
    for (int chip_num = 0; chip_num < 16; chip_num++) {
        snprintf(path, sizeof(path), GPIOCHIP_FMT, chip_num);
        int candidate = open(path, O_RDWR);
        if (candidate < 0) {
            continue;
        }

        struct gpiochip_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(candidate, GPIO_GET_CHIPINFO_IOCTL, &info) == 0
            && strcmp(info.label, GPIO_CHIP_LABEL) == 0) {
            chip_fd = candidate;
            break;
        }

        close(candidate);
    }

    if (chip_fd < 0) {
        fprintf(stderr, PROG ": gpiochip '%s' not found\n", GPIO_CHIP_LABEL);
        return -1;
    }

    struct gpio_v2_line_request request;
    memset(&request, 0, sizeof(request));
    request.offsets[0] = GPIO_RESET_LINE;
    request.num_lines = 1;
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;

    strncpy(request.consumer, "ar8030_reset", sizeof(request.consumer) - 1);
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request) < 0) {
        perror(PROG ": GPIO_V2_GET_LINE_IOCTL");
        close(chip_fd);
        return -1;
    }

    struct gpio_v2_line_values values;
    values.mask = 1;

    /* assert reset (low) */
    values.bits = 0;
    ioctl(request.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
    usleep(5000);

    /* release reset (high) */
    values.bits = 1;
    ioctl(request.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
    usleep(5000);

    close(request.fd);
    close(chip_fd);
    fprintf(stderr, PROG ": AR8030 reset released (%s line %d)\n",
            GPIO_CHIP_LABEL, GPIO_RESET_LINE);

    return 0;
}

/* Re-probe the RF SDIO host so the chip, now out of reset, enumerates. The coldplug already bound
 * dw_mci-artosyn to both mmc hosts at boot; unbind/bind just the RF host to rescan it. Best-effort:
 * if the driver sysfs is not there we carry on and let the wait below decide.
 */
static void reprobe_host(void)
{
    char path[64];

    snprintf(path, sizeof(path), "%s/unbind", RF_DRIVER);
    write_attr(path, RF_HOST);

    snprintf(path, sizeof(path), "%s/bind", RF_DRIVER);
    write_attr(path, RF_HOST);
}

/* Poll the SDIO bus for the AR8030 (device id 0x8030, ROM mode). Returns 0 once seen. */
static int wait_ar8030(void)
{
    for (int attempt = 0; attempt < WAIT_TRIES; attempt++) {
        DIR *dir = opendir(SDIO_BUS);
        if (dir != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') {
                    continue;
                }

                char path[300];
                snprintf(path, sizeof(path), SDIO_BUS "/%s/device", entry->d_name);
                int fd = open(path, O_RDONLY);
                if (fd < 0) {
                    continue;
                }

                char device_id[16] = { 0 };
                ssize_t n_read = read(fd, device_id, sizeof(device_id) - 1);
                close(fd);
                if (n_read > 0 && strstr(device_id, "8030") != NULL) {
                    closedir(dir);
                    return 0;
                }
            }

            closedir(dir);
        }

        usleep(WAIT_STEP_US);
    }

    return -1;
}

/* Load artosyn_sdio with the firmware/config params, which downloads the baseband into the chip.
 * Deps (ar_dtbo_sdio, artosyn_gpio, dw_mci-artosyn) are already up from the coldplug, so a direct
 * finit_module of the one .ko is enough. Returns 0 on success.
 */
static int load_rf_driver(const char *fw, const char *cfg)
{
    const char *fw_path = getenv("ML_RF_FW_PATH");
    const char *ko = getenv("ML_RF_KO");
    char ko_buf[128];
    char params[256];

    /* Point the kernel firmware loader at an override dir (dev RAM-boot); default /lib/firmware
     * already covers the production rootfs.
     */
    if (fw_path != NULL) {
        write_attr(FW_PATH_ATTR, fw_path);
    }

    if (ko == NULL) {
        struct utsname uts;
        if (uname(&uts) != 0) {
            perror(PROG ": uname");
            return -1;
        }

        snprintf(ko_buf, sizeof(ko_buf), "/lib/modules/%s/kernel/%s.ko", uts.release, RF_MODULE);
        ko = ko_buf;
    }

    int fd = open(ko, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, PROG ": open %s: %s\n", ko, strerror(errno));
        return -1;
    }

    snprintf(params, sizeof(params), "fw_name=%s cfg_name=%s", fw, cfg);
    int rc = finit_module(fd, params, 0);
    close(fd);
    if (rc != 0) {
        fprintf(stderr, PROG ": load %s (%s): %s\n", RF_MODULE, params, strerror(errno));
        return -1;
    }

    fprintf(stderr, PROG ": %s loaded (%s)\n", RF_MODULE, params);
    return 0;
}

/* Wait for the sdio0 netdev the driver creates once the firmware is up, then bring it up and set
 * 10.0.0.1/24. The address add fires the inetaddr notifier the driver's RX path needs.
 */
static int configure_sdio0(void)
{
    int attempt = 0;
    while (attempt < WAIT_TRIES && access(SYS_NET SDIO_IFACE, F_OK) != 0) {
        usleep(WAIT_STEP_US);
        attempt++;
    }

    if (access(SYS_NET SDIO_IFACE, F_OK) != 0) {
        fprintf(stderr, PROG ": %s never appeared\n", SDIO_IFACE);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror(PROG ": socket");
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, SDIO_IFACE, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        perror(PROG ": SIOCGIFFLAGS");
        close(sock);
        return -1;
    }

    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        perror(PROG ": SIOCSIFFLAGS");
        close(sock);
        return -1;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *) &ifr.ifr_addr;
    memset(&ifr.ifr_addr, 0, sizeof(ifr.ifr_addr));
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, SDIO_ADDR, &addr->sin_addr);
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        perror(PROG ": SIOCSIFADDR");
        close(sock);
        return -1;
    }

    memset(&ifr.ifr_addr, 0, sizeof(ifr.ifr_addr));
    addr = (struct sockaddr_in *) &ifr.ifr_netmask;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, SDIO_NETMASK, &addr->sin_addr);
    ioctl(sock, SIOCSIFNETMASK, &ifr);

    close(sock);
    fprintf(stderr, PROG ": %s up, %s/24\n", SDIO_IFACE, SDIO_ADDR);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <fw_name> <cfg_name>\n", PROG);
        return 2;
    }

    const char *fw = argv[1];
    const char *cfg = argv[2];

    if (module_loaded(RF_MODULE)) {
        fprintf(stderr, PROG ": %s already loaded; nothing to do\n", RF_MODULE);
        return 0;
    }

    if (release_reset() != 0) {
        return 1;
    }

    reprobe_host();

    if (wait_ar8030() != 0) {
        fprintf(stderr, PROG ": AR8030 never enumerated on SDIO\n");
        return 1;
    }

    if (load_rf_driver(fw, cfg) != 0) {
        return 1;
    }

    if (configure_sdio0() != 0) {
        return 1;
    }

    fprintf(stderr, PROG ": RF link ready\n");
    return 0;
}
