#!/bin/sh
# splash-up.sh - one-shot boot splash: bring the display up and put the vendor
# mountain (nosignal.yuv, I420 on the DRM primary) on the panel. Idempotent.
# The image persists after exit (it lives on the ml-drmfd broker fd); later
# video/OSD planes simply cover it. plans/rf-loose-ends.md item 3.
set -e
HERE="$(dirname "$(readlink -f "$0")")"

# display driver chain (rootfs ships the modules; modprobe resolves deps)
modprobe artosyn_pwm 2>/dev/null || true
# panel reset GPIO is on a10a000.gpio (sdio dtbo overlay); without it the panel/DSI/VO
# probes defer forever and /dev/dri/card0 never appears.
modprobe artosyn_gpio 2>/dev/null || true
modprobe panel-qy45043a0
modprobe artosyn_dsi
modprobe artosyn_vo
[ -e /dev/dri/card0 ] || { echo "splash-up: /dev/dri/card0 missing" >&2; exit 1; }

# the session DRM-master broker (also what ml-pipeline/ml-hud attach to later)
if ! pgrep -x ml-drmfd >/dev/null 2>&1; then
    "$HERE/ml-drmfd" &
    i=0
    while [ ! -S /run/missinglynk/drm.sock ] && [ $i -lt 50 ]; do
        sleep 0.1; i=$((i + 1))
    done
fi

"$HERE/ml-splash" "$@"

echo 0 > /sys/class/backlight/backlight/bl_power 2>/dev/null || true
echo "splash-up: done"
