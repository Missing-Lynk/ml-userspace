#!/bin/sh
# gst-env.sh - source this on the goggle to use the SD-card GStreamer prefix.
# The prefix is the loop-mounted squashfs at /mnt/gst (see gstreamer/deploy.sh).
# Registry/cache go to /tmp (the prefix is read-only).
GSTP="${GSTP:-/mnt/gst}"
export PATH="$GSTP/usr/bin:$PATH"
export LD_LIBRARY_PATH="$GSTP/usr/lib:$GSTP/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GST_PLUGIN_SYSTEM_PATH_1_0="$GSTP/usr/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER_1_0="$GSTP/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
# Persist the plugin registry so the full cold scan of the 238-plugin prefix (~19 s: dlopen +
# feature-extract every .so) runs ONCE and is reused across reboots, instead of on every boot
# (tmpfs /tmp is wiped each reboot, so the scan was paying 19 s of startup latency every time).
# Pick the first writable persistent SD dir; fall back to /tmp if none is writable (still correct,
# just not persisted). The registry stores /mnt/gst plugin paths + mtimes, stable across boots.
GST_REG=/tmp/gst-registry.bin
for _d in /tmp/sdcard/missinglynk /mnt/sdcard/missinglynk; do
    if [ -d "$_d" ] && ( : >"$_d/.gstreg" ) 2>/dev/null; then rm -f "$_d/.gstreg"; GST_REG="$_d/gst-registry.bin"; break; fi
done
export GST_REGISTRY_1_0="$GST_REG"
export XDG_CACHE_HOME=/tmp/.cache

# kmssink needs both: our DRM driver name is not on its auto-probe list, and its plane
# auto-pick lands on the ARGB4444 overlay (no GST format) instead of the primary.
# Plane ids are deterministic for this driver; override via KMS_PLANE_ID if they move.
export KMSSINK="kmssink driver-name=artosyn-vo plane-id=${KMS_PLANE_ID:-33}"
