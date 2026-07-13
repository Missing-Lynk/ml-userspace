#!/bin/sh
# goggle-preview.sh - RUN THIS ON THE GOGGLE. Test harness to see the staged HUD's BTFL OSD on the
# DRM ARGB4444 overlay plane, fed by the pcap replay standing in for ml-linkd - no air unit needed.
#
# Works on both device states:
#   - the full rootfs boot stack: the ml-hud service is stopped (it would hold osd.sock) and
#     ml-linkd is stopped (osd-replay publishes on its seams; they must not both send). ml-display
#     has already anchored the DRM broker and modeset the CRTC, so nothing else is stood up.
#   - a bare dev goggle: the broker (ml-drmfd) and a background/modeset (ml-splash + nosignal.yuv,
#     staged by deploy.sh) are started here first, since no display service has.
#
# Then: the staged hud binds the mlm OSD seams (osd.sock; linkstate owns telemetry.sock) and
# osd-replay publishes the captured :10000 frames onto them at the captured timing; the HUD draws
# the BTFL OSD onto the overlay plane, composited over whatever the primary plane shows.
#
# Runs from the HUD staging dir (deploy.sh puts everything there together): hud, osd-replay,
# font_BTFL_hd.png, btfl-osd-10000.pcap, and nosignal.yuv on a bare goggle.
# Env: BG=<I420 image>  PLANE=<overlay plane id, default 38>
ML="$(cd "$(dirname "$0")" && pwd)"
BG="${BG:-$ML/nosignal.yuv}"
PLANE="${PLANE:-38}"
DRM_SOCK=/run/missinglynk/drm.sock

# busybox pkill -x is unreliable on this device: kill by scanning ps. The [b]racket keeps the
# pattern from matching the grep itself. SIGTERM only (never -9 for the HUD: it must free its DRM
# buffer, a -9 leaks it into the broker's shared drm_file).
kill_by() {
    ps w | grep "$1" | awk '{print $1}' | while read -r pid; do
        kill "$pid" 2>/dev/null
    done
}

# Stop everything that would fight over the OSD seams or the overlay plane; everything below is
# nohup'd + detached from stdin so it survives the ssh session closing.
# - the boot HUD via its service when openrc manages it (keeps rc status coherent), else by scan
# - ml-linkd: osd-replay stands in for it on the mlm seams, the two must not both publish
# - leftovers of a previous preview run out of this staging dir (targeted patterns: a bare
#   "$ML/" would match this very script's own ps line and kill it)
[ -e /etc/init.d/ml-hud ] && rc-service ml-hud stop >/dev/null 2>&1
kill_by "[m]l-hud"
kill_by "[m]l-linkd"
kill_by "[/]${ML#/}/hud"
kill_by "[/]${ML#/}/osd-replay"
sleep 0.5

# Bare goggle only: no broker socket means no display service ran, so anchor the DRM master and
# paint/modeset the primary plane ourselves. On the boot stack the socket exists and this is skipped
# (repainting would clobber the live pipeline's output).
if [ ! -S "$DRM_SOCK" ]; then
    DRMFD="$(command -v ml-drmfd)" || DRMFD=""
    if [ -z "$DRMFD" ]; then
        echo "goggle-preview: no $DRM_SOCK and no ml-drmfd binary; cannot anchor DRM" >&2
        exit 1
    fi
    nohup "$DRMFD" >/tmp/ml-drmfd.log 2>&1 </dev/null &
    sleep 1

    SPLASH="$(command -v ml-splash)" || SPLASH=""
    if [ -n "$SPLASH" ] && [ -f "$BG" ]; then
        "$SPLASH" "$BG" >/tmp/splash.log 2>&1 || echo "goggle-preview: splash failed (see /tmp/splash.log)" >&2
        sleep 1
    else
        echo "goggle-preview: no ml-splash or no $BG; overlay may not scan out without a modeset" >&2
    fi
fi

# The HUD first (it binds osd.sock + telemetry.sock; records sent before that are dropped, like
# with ml-linkd), then the replay onto the mlm seams at the exact captured speed.
nohup "$ML/hud" --drm "$PLANE" --idle-ms 0 --btfl-font "$ML/font_BTFL_hd.png" >"$ML/hud.log" 2>&1 </dev/null &
sleep 1

nohup "$ML/osd-replay" "$ML/btfl-osd-10000.pcap" --loop >"$ML/replay.log" 2>&1 </dev/null &
sleep 2

echo "=== alive? ==="
ps w | grep -q "[m]l-drmfd"      && echo "ml-drmfd:   RUNNING" || echo "ml-drmfd:   dead"
ps w | grep -q "[/]${ML#/}/osd-replay" && echo "osd-replay: RUNNING" || echo "osd-replay: dead"
ps w | grep -q "[/]${ML#/}/hud"  && echo "hud:        RUNNING" || echo "hud:        dead"
echo "=== hud.log ==="; cat "$ML/hud.log"
echo "=== replay.log ==="; cat "$ML/replay.log"
