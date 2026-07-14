#!/usr/bin/env bash
# Regenerate the 1080p60 test clips used to validate the decode->display
# pipeline (gstreamer/README.md). Output goes next to the squashfs in
# rootfs/build/gst-sd/ (gitignored); push to the goggle's SD card at
# /mnt/sdcard/missinglynk/ and play with `ml-pipeline <file>` (file mode).
#
# Encoding shape mirrors the real FPV feed: 1080p60, 8 Mbps, no B-frames,
# 1 reference frame (low DPB -> fewest wave5 CAPTURE buffers; the plain-ref
# h264 clip is kept as a contrast case), keyframe every 60 frames, annex-b
# elementary stream (no container - h264parse/h265parse take it directly).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../rootfs/build/gst-sd}"
mkdir -p "$OUT"

SRC="testsrc2=size=1920x1080:rate=60"
DUR=30

# H.264, default refs (DPB-heavier contrast case)
ffmpeg -y -loglevel error -f lavfi -i "$SRC" -t "$DUR" \
    -c:v libx264 -profile:v main -pix_fmt yuv420p -g 60 -bf 0 -b:v 8M \
    -bsf:v h264_mp4toannexb -f h264 "$OUT/test-1080p60.h264"

# H.264, ref=1: the FPV-realistic shape and the primary validation clip
ffmpeg -y -loglevel error -f lavfi -i "$SRC" -t "$DUR" \
    -c:v libx264 -profile:v main -pix_fmt yuv420p -g 60 -bf 0 \
    -refs 1 -x264-params ref=1 -b:v 8M \
    -bsf:v h264_mp4toannexb -f h264 "$OUT/test-1080p60-ref1.h264"

# H.265: the codec the RF feed actually uses (venc8 is H.265)
ffmpeg -y -loglevel error -f lavfi -i "$SRC" -t "$DUR" \
    -c:v libx265 -preset fast -x265-params "keyint=60:bframes=0:ref=1" \
    -pix_fmt yuv420p -b:v 8M -f hevc "$OUT/test-1080p60.h265"

ls -la "$OUT"/test-1080p60*
