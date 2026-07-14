#!/bin/sh
# gst-display-up.sh - load the open display (DRM) + wave5 codec module stacks from the
# squashfs prefix, in dependency order (load-order.txt is written by build-prefix.sh).
# Idempotent: already-loaded modules are skipped. Verifies /dev/dri/card0 + /dev/video*.
set -e
D="$(dirname "$(readlink -f "$0")")/../modules"

while read -r ko; do
    name="$(basename "$ko" .ko | tr - _)"
    if grep -q "^$name " /proc/modules; then
        echo "  [=] $name already loaded"
    elif [ -f "/tmp/$ko" ]; then
        # /tmp override: deploy.sh stages freshly built modules (e.g. artosyn_vo) here so a
        # driver fix reaches the device without rebuilding/re-pushing the whole squashfs.
        echo "  [+] insmod /tmp/$ko (override)"
        insmod "/tmp/$ko"
    else
        echo "  [+] insmod $ko"
        insmod "$D/$ko"
    fi
done < "$D/load-order.txt"

sleep 1
echo "--- result ---"
ls -l /dev/dri/ /dev/video* 2>&1 || true
dmesg | tail -15 || true
