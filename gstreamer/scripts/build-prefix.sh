#!/usr/bin/env bash
# build-prefix.sh - build the SD-card GStreamer development prefix for the open slot-B goggle.
#
# Implements the "Development: full packages on the SD card" strategy from
# plans/gst-hud-architecture.md: apk-install the stock Alpine aarch64 GStreamer packages
# (gstreamer + base/good/bad plugins + tools) into a host-side prefix, add the display/codec
# kernel modules and runtime helper scripts, and pack the whole thing into ONE squashfs image.
# The SD card is exFAT (no symlinks, no unix perms), so the prefix cannot live on it as a
# plain tree; the goggle instead loop-mounts the image (CONFIG_SQUASHFS=y + BLK_DEV_LOOP=y):
#
#   host  : gstreamer/build-prefix.sh            -> rootfs/build/gst-sd/gst-sd.sqsh
#   deploy: gstreamer/deploy.sh            -> streams the image to the SD over SSH
#   device: /mnt/sdcard/missinglynk/gst-sd.sqsh loop-mounted at /mnt/gst by gst-mount.sh
#
# Reuses the rootfs build's verified apk.static + Alpine signing keys (run rootfs/build.sh
# once first if they are missing). Same Alpine pin as the rootfs (v3.24), so the prefix's
# musl/libs match the device. Packages install with --no-scripts (aarch64 post-install
# scripts can't run on the build host; none of them matter for a LD_LIBRARY_PATH prefix).
#
# Re-runnable: apk downloads are cached by apk itself in the prefix; the squashfs is
# rebuilt from scratch each run.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

ALPINE_CDN="https://dl-cdn.alpinelinux.org/alpine"
ALPINE_BRANCH="v3.24"   # keep in lockstep with rootfs/build.sh
ARCH=aarch64

ROOTFS_WORK="$REPO/rootfs/build/work"
APK="$ROOTFS_WORK/sbin/apk.static"
KEYS="$ROOTFS_WORK/keys"
[ -x "$APK" ] && [ -d "$KEYS" ] || {
  echo "ERROR: $APK / $KEYS missing - run rootfs/build.sh once to stage apk.static + keys" >&2
  exit 1
}

OUT="$REPO/rootfs/build/gst-sd"     # gitignored (rootfs/build is)
PREFIX="$OUT/prefix"
IMG="$OUT/gst-sd.sqsh"
mkdir -p "$OUT"

# Kernel modules: the display (DRM) + codec (wave5) stacks are =m in the flashed config and
# the slim rootfs ships no /lib/modules, so the image carries them. Built tree = the same
# one that produced the running Image (kernel/scripts/pin.env BUILD_DIR convention).
source "$REPO/kernel/scripts/pin.env"
KBASE="${BUILD_DIR:-/media/home-ext/_kernel-m1/repro-${KERNEL_VERSION}}"
# insmod order matters; gst-display-up.sh loads them in exactly this order.
# artosyn_pwm first: the panel's pwm-backlight supplier defers the whole DSI/VO chain
# until the PWM provider exists.
MODULES=(
  ml-modules/artosyn_pwm.ko
  linux/drivers/gpu/drm/drm_panel_orientation_quirks.ko
  linux/drivers/gpu/drm/drm.ko
  linux/drivers/gpu/drm/drm_kms_helper.ko
  linux/drivers/gpu/drm/clients/drm_client_lib.ko
  linux/drivers/gpu/drm/drm_dma_helper.ko
  linux/drivers/gpu/drm/bridge/synopsys/dw-mipi-dsi.ko
  linux/drivers/gpu/drm/artosyn/panel-qy45043a0.ko
  linux/drivers/gpu/drm/artosyn/artosyn_dsi.ko
  linux/drivers/gpu/drm/artosyn/artosyn_vo.ko
  linux/drivers/media/common/videobuf2/videobuf2-common.ko
  linux/drivers/media/common/videobuf2/videobuf2-v4l2.ko
  linux/drivers/media/common/videobuf2/videobuf2-memops.ko
  linux/drivers/media/common/videobuf2/videobuf2-dma-contig.ko
  linux/drivers/media/v4l2-core/v4l2-mem2mem.ko
  linux/drivers/media/platform/chips-media/wave5/wave5.ko
)
for m in "${MODULES[@]}"; do
  [ -f "$KBASE/$m" ] || { echo "ERROR: missing $KBASE/$m - build the kernel tree first" >&2; exit 1; }
done

echo "[*] apk install into $PREFIX (Alpine $ALPINE_BRANCH $ARCH) ..."
mkdir -p "$PREFIX"
fakeroot -- "$APK" \
  --arch "$ARCH" --root "$PREFIX" --keys-dir "$KEYS" \
  -X "$ALPINE_CDN/$ALPINE_BRANCH/main" -X "$ALPINE_CDN/$ALPINE_BRANCH/community" \
  --initdb --no-scripts --usermode --no-cache --no-interactive \
  add musl gstreamer gstreamer-tools gst-plugins-base gst-plugins-good gst-plugins-bad \
      font-dejavu

echo "[*] staging kernel modules ..."
mkdir -p "$PREFIX/missinglynk/modules"
for m in "${MODULES[@]}"; do cp "$KBASE/$m" "$PREFIX/missinglynk/modules/"; done
# Keep the load order machine-readable for the runtime script.
printf '%s\n' "${MODULES[@]}" | sed 's|.*/||' > "$PREFIX/missinglynk/modules/load-order.txt"

echo "[*] building + staging the pipeline/HUD binaries (gstreamer/src) ..."
"$REPO/gstreamer/src/build.sh"

# ml-linkd is a static, gst-free daemon and ships in the rootfs (rootfs/build.sh stages it into
# /usr/local/bin/ml-linkd), not here - the squashfs is only the gst development stack.

echo "[*] staging runtime scripts + binaries ..."
mkdir -p "$PREFIX/missinglynk/bin"
cp "$REPO/gstreamer/runtime/"* "$PREFIX/missinglynk/bin/"
cp "$REPO/gstreamer/build/bin/"ml-* "$PREFIX/missinglynk/bin/"
chmod +x "$PREFIX/missinglynk/bin/"*

# splash asset: I420 1920x1080 raw; painted on the DRM primary by ml-splash (splash-up.sh).
mkdir -p "$PREFIX/missinglynk/share"
cp "$REPO/assets/splash/splash.yuv" "$PREFIX/missinglynk/share/nosignal.yuv"

echo "[*] mksquashfs -> $IMG ..."
rm -f "$IMG"
# gzip: the only decompressor guaranteed in the device config (SQUASHFS=y default).
fakeroot -- mksquashfs "$PREFIX" "$IMG" -comp gzip -noappend -no-xattrs -quiet

du -sh "$PREFIX" "$IMG"
echo "[+] done. Deploy with gstreamer/deploy.sh"
