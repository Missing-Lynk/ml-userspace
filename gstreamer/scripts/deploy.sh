#!/usr/bin/env bash
# deploy.sh - deploy the GStreamer SD prefix to the slot-B goggle.
#
# Mounts the exFAT SD card on the device, streams the squashfs image (and the H.264 test
# clip if present) into /mnt/sdcard/missinglynk/ over SSH (no scp/sftp on the device),
# then loop-mounts the image at /mnt/gst. Re-runnable; remounts a stale loop mount.
#
# Env: DEVICE_IP (192.168.3.100), ROOT_PASS (libre).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$REPO/rootfs/build/gst-sd"
IMG="$OUT/gst-sd.sqsh"
CLIP="$OUT/test-1080p60.h264"

DEVICE_IP="${DEVICE_IP:-192.168.3.100}"
PASS="${ROOT_PASS:-libre}"

[ -f "$IMG" ] || { echo "ERROR: $IMG missing - run build-prefix.sh first" >&2; exit 1; }
command -v sshpass >/dev/null || { echo "ERROR: sshpass not installed" >&2; exit 1; }

sshg() { sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o LogLevel=ERROR -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@"$DEVICE_IP" "$@"; }

sshg true || { echo "ERROR: cannot SSH root@$DEVICE_IP" >&2; exit 1; }

# The SD controller itself is modular and the slim rootfs ships no /lib/modules, so the
# card is unreachable on a fresh boot until these load (chicken-and-egg with the
# squashfs ON the card - they must come over SSH). Idempotent; the mmc nodes are in the
# boot DTB. The card enumerates as mmcblk0 or mmcblk1 depending on whether the RF/SDIO
# side is up, so probe both.
echo "[*] bootstrapping SD-controller modules ..."
source "$REPO/kernel/scripts/pin.env"
KBASE="${BUILD_DIR:-/media/home-ext/_kernel-m1/repro-${KERNEL_VERSION}}"
for ko in artosyn_gpio dw_mci-artosyn; do
  sshg "cat > /tmp/$ko.ko" < "$KBASE/ml-modules/$ko.ko"
done

# Always stage the CURRENT display driver too: gst-display-up.sh prefers /tmp/<mod>.ko over
# the squashfs copy, so a rebuilt artosyn_vo reaches the device without re-pushing the whole
# image (the 156 MB image push only happens when the image content itself changes).
sshg "cat > /tmp/artosyn_vo.ko" < "$KBASE/ml-modules/rootfs/lib/modules/${KERNEL_VERSION}/kernel/artosyn_vo.ko"
# Same /tmp-override path for the wave5 codec: gst-display-up.sh prefers /tmp/wave5.ko, so a
# rebuilt decoder (e.g. the multi-instance RESULT_NOT_READY retry fix) reaches the device
# without re-pushing the squashfs. Only takes effect on a FRESH boot (wave5 will not warm-reload).
sshg "cat > /tmp/wave5.ko" < "$KBASE/ml-modules/rootfs/lib/modules/${KERNEL_VERSION}/kernel/wave5.ko"
sshg 'for ko in artosyn_gpio dw_mci-artosyn; do
        name="$(echo "$ko" | tr - _)"
        grep -q "^$name " /proc/modules || insmod /tmp/$ko.ko
      done'

echo "[*] mounting SD card ..."
sshg 'for i in 1 2 3 4 5 6 7 8; do [ -b /dev/mmcblk0 ] || [ -b /dev/mmcblk1 ] && break; sleep 2; done
      SD=/dev/mmcblk1; [ -b /dev/mmcblk1 ] || SD=/dev/mmcblk0
      mkdir -p /mnt/sdcard && { grep -q " /mnt/sdcard " /proc/mounts || mount -t exfat "$SD" /mnt/sdcard; } && mkdir -p /mnt/sdcard/missinglynk'

remote_sum() { sshg "sha256sum '$1' 2>/dev/null | cut -d' ' -f1" || true; }
push() {  # push <local> <remote> - skip when checksums already match
  local lsum rsum
  lsum="$(sha256sum "$1" | cut -d' ' -f1)"
  rsum="$(remote_sum "$2")"
  if [ "$lsum" = "$rsum" ]; then echo "  [=] $2 up to date"; return; fi
  echo "  [+] $1 -> $2 ($(du -h "$1" | cut -f1)) ..."
  sshg "cat > '$2.part' && mv '$2.part' '$2'" < "$1"
  rsum="$(remote_sum "$2")"
  [ "$lsum" = "$rsum" ] || { echo "ERROR: checksum mismatch after push of $2" >&2; exit 1; }
}

push "$IMG" /mnt/sdcard/missinglynk/gst-sd.sqsh
[ -f "$CLIP" ] && push "$CLIP" /mnt/sdcard/missinglynk/test-1080p60.h264

echo "[*] loop-mounting squashfs at /mnt/gst ..."
sshg 'mkdir -p /mnt/gst; grep -q " /mnt/gst " /proc/mounts && umount /mnt/gst; mount -o loop -t squashfs /mnt/sdcard/missinglynk/gst-sd.sqsh /mnt/gst && ls /mnt/gst'

echo "[+] deployed. On the goggle:"
echo "      /mnt/gst/missinglynk/bin/gst-display-up.sh     # load DRM + wave5 modules"
echo "      /mnt/gst/missinglynk/bin/splash-up.sh          # boot splash on the primary (optional)"
echo "      /mnt/gst/missinglynk/bin/gst-rf-up.sh          # default runtime: RF tiles -> composite+OSD"
