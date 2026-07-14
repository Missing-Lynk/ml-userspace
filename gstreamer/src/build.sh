#!/usr/bin/env bash
# build.sh - cross-build the pipeline/HUD binaries
# in an aarch64 Alpine 3.24 container (same pin as the SD prefix and the slot-B
# rootfs, so musl + gstreamer versions match the runtime exactly).
#
#   ml-drmfd    static, no deps          - DRM master-fd broker
#   ml-mdump    static, no deps          - metadata-seam dump tool / reference consumer
#   ml-rec      static, no deps          - send a record command to ml-pipeline's ctrl.sock
#   ml-hud      static, stb_truetype -lm - framecount HUD on the overlay plane
#   ml-pipeline dynamic, gstreamer-1.0   - decode->display + telemetry producer
#                                          (runs against the /mnt/gst prefix libs)
#
# Output: gstreamer/build/bin/ (gitignored). build-prefix.sh stages these into
# the squashfs at /mnt/gst/missinglynk/bin/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/../build/bin"
mkdir -p "$OUT"

docker run --rm --platform linux/arm64 \
  -v "$REPO":/w -w /w/gstreamer/src \
  alpine:3.24 sh -euc '
    apk add -q build-base pkgconf gstreamer-dev gst-plugins-base-dev libdrm-dev linux-headers
    O=/w/gstreamer/build/bin
    gcc -O2 -Wall -static -o "$O/ml-drmfd"  ml-drmfd/ml-drmfd.c
    gcc -O2 -Wall -static -o "$O/ml-mdump"  ml-mdump/ml-mdump.c
    gcc -O2 -Wall -static -o "$O/ml-rec"    ml-rec/ml-rec.c
    gcc -O2 -Wall -static -o "$O/ml-hud"    ml-hud/ml-hud.c -I. -lm
    gcc -O2 -Wall -o "$O/ml-pipeline" ml-pipeline/*.c \
        $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0 libdrm) -lpthread
    gcc -O2 -Wall -static -o "$O/ml-splash" ml-splash/ml-splash.c
    ls -la "$O"
  '
echo "[+] built into $OUT"
