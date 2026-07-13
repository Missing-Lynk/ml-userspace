#!/usr/bin/env bash
# deploy.sh - build the HUD (aarch64, static) and stage it + its runtime assets onto the goggle.
#
# Host-side. Everything the HUD needs lands together in one staging dir on the device tmpfs (default
# /run/ml/hud; wiped on reboot, so re-run after each boot). Then, on the goggle:
#
#     sh /run/ml/hud/goggle-preview.sh
#
# Works from a standalone userspace checkout: the wrapper repo's glue/lib/ssh-opts.sh is used when
# present, otherwise the equivalent options are inlined; the nosignal.yuv background (a rootfs build
# artifact) is staged only if found - the preview needs it only on a bare goggle with no display
# service to paint the primary plane.
#
# Env: DEVICE_IP (default 192.168.3.100), ROOT_PASS (default libre), DEST (default /run/ml/hud),
#      NOSIGNAL_YUV (background image; default: the rootfs build's copy, skipped if absent).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HUD="$(cd "$HERE/.." && pwd)"          # hud/
ROOT="$(cd "$HUD/.." && pwd)"          # userspace/
REPO="$(cd "$ROOT/.." && pwd)"         # wrapper repo root (glue/, rootfs/), if this is one
DEVICE_IP="${DEVICE_IP:-192.168.3.100}"
ROOT_PASS="${ROOT_PASS:-libre}"
DEST="${DEST:-/run/ml/hud}"

# Shared SSH options from the wrapper repo when available; otherwise the same no-host-key-
# persistence set inline (slot hops change the device key, so never record it).
if [ -f "$REPO/glue/lib/ssh-opts.sh" ]; then
    . "$REPO/glue/lib/ssh-opts.sh"
    SSHOPTS=("${SSH_OPTS_LIBRE[@]}")
else
    SSHOPTS=(
        -o StrictHostKeyChecking=no
        -o LogLevel=ERROR
        -o UserKnownHostsFile=/dev/null
        -o ConnectTimeout=8
    )
fi
sshv() { sshpass -p "$ROOT_PASS" ssh "${SSHOPTS[@]}" root@"$DEVICE_IP" "$@"; }
# gzip on the wire (the CDC-ECM gadget is slow and large cat-over-ssh has crashed it before).
push_gz() { gzip -c "$1" | sshv "gunzip > '$2' && chmod ${3:-644} '$2'"; }

command -v sshpass >/dev/null || { echo "deploy: sshpass required" >&2; exit 1; }

FONT="$ROOT/assets/osd-fonts/font_BTFL_hd.png"
PCAP="$HUD/tools/btfl-osd-10000.pcap"
BG="${NOSIGNAL_YUV:-$REPO/rootfs/build/work/overlay/usr/local/share/nosignal.yuv}"
[ -f "$FONT" ] || { echo "deploy: missing $FONT (generate with 'make font' in userspace/)" >&2; exit 1; }
[ -f "$PCAP" ] || { echo "deploy: missing $PCAP" >&2; exit 1; }

echo "[*] building HUD (aarch64, static) ..."
cmake -S "$HUD" -B "$HUD/build" -DCMAKE_TOOLCHAIN_FILE="$HUD/cmake/aarch64-static.cmake" >/dev/null
cmake --build "$HUD/build" -j"$(nproc)"

echo "[*] stopping any staged HUD/replay on the device, resetting $DEST ..."
# busybox pkill -x is unreliable here. Kill by scanning ps for processes running out of $DEST; the
# [/] bracket keeps the pattern from matching this very command's own shell. SIGTERM (not -9) so the
# HUD runs its cleanup and frees its DRM buffer - a -9 would leak it into ml-drmfd's shared file.
# Wait briefly, then -9 anything that ignored the term. Then wipe the staging dir clean.
sshv "ps w | grep '[/]${DEST#/}/' | awk '{print \$1}' | while read pid; do kill \"\$pid\" 2>/dev/null; done
      sleep 1
      ps w | grep '[/]${DEST#/}/' | awk '{print \$1}' | while read pid; do kill -9 \"\$pid\" 2>/dev/null; done
      rm -rf '$DEST'
      mkdir -p '$DEST'"

echo "[*] staging to $DEVICE_IP:$DEST ..."
push_gz "$HUD/build/hud"                "$DEST/hud" 755
push_gz "$HUD/build/osd-replay"         "$DEST/osd-replay" 755
push_gz "$FONT"                         "$DEST/font_BTFL_hd.png"
push_gz "$PCAP"                         "$DEST/btfl-osd-10000.pcap"
push_gz "$HUD/tools/goggle-preview.sh"  "$DEST/goggle-preview.sh" 755

if [ -f "$BG" ]; then
    push_gz "$BG" "$DEST/nosignal.yuv"
else
    echo "[*] note: $BG absent; skipping the splash background (only needed on a bare goggle" \
         "with no display service - set NOSIGNAL_YUV to stage one)"
fi

# language catalogs (menu i18n) -> $DEST/lang, one of the dirs the menu searches
sshv "mkdir -p '$DEST/lang'"
push_gz "$HUD/lang/en.lang"             "$DEST/lang/en.lang"
push_gz "$HUD/lang/zh.lang"             "$DEST/lang/zh.lang"

echo "[*] staged:"
sshv "ls -la '$DEST'"
echo "[*] done. On the goggle:  sh $DEST/goggle-preview.sh"
