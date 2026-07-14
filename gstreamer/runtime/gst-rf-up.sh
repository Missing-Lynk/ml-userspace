#!/bin/sh
# gst-rf-up.sh - start the DEFAULT-MODE video pipeline: receive + decode the two RF tiles, composite
# them (off-CPU via the DMA blit engine) into one 1080p frame on the DRM primary plane, with the
# OSD/HUD on the overlay. This is the target runtime documented in gstreamer/README.md "Default
# operating mode": ~60 fps, DMA compositing, low CPU. Recording is additive and toggled separately.
#
# The prerequisites are set up at boot: the DRM broker (ml-display init), the OSD (ml-hud init), and
# the DMA blit engine (ml_dmablit via modules-load.d). This script just launches the video onto that
# stack. Source arg: `rf` (the RF UDP downlink fed by ml-linkd) is the default; for the simulated
# port run ml-rf-replay against the same socket.
set -e
GSTP="${GSTP:-/mnt/gst}"
. "$GSTP/missinglynk/bin/gst-env.sh"

# Warm plugin registry (gst-env.sh persists it on the SD; a fresh SD may still cold-scan once ~19s).
[ -e /run/missinglynk/drm.sock ] || { echo "FATAL: no DRM broker (/run/missinglynk/drm.sock). Is ml-display up?"; exit 1; }
[ -e /dev/ml-dmablit ] || echo "WARN: /dev/ml-dmablit absent -> CPU-blit fallback (~40 fps). Is ml_dmablit loaded (modules-load.d)?"
# dec_cap_bufs must stay 0: any cap below the stream's fbc_buf_count (9) makes wave5 spin on
# display-buffer-full (~25-40 fps, both cores pegged). See rootfs modprobe.d/ml.conf.
if [ -e /sys/module/wave5/parameters/dec_cap_bufs ]; then
    cap="$(cat /sys/module/wave5/parameters/dec_cap_bufs)"
    [ "$cap" = "0" ] || echo "WARN: wave5 dec_cap_bufs=$cap - caps starve the decoder; expected 0"
fi

# Composite mode is the default use case: it frees a plane for the OSD overlay and produces the frame
# the DVR encodes. Composite pool in CMA at full depth (NO ML_COMP_MAX - that cap starves the pipeline
# to ~40 fps). ML_HEAP defaults to the CMA heap in the binary. Recording adds a tee at runtime.
exec env ML_COMPOSE=1 "$GSTP/missinglynk/bin/ml-pipeline" "${1:-rf}"
