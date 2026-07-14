#!/usr/bin/env bash
# build-static.sh - PRODUCTION track (plans/gst-static-rootfs.md): build a slim, standalone
# ml-pipeline with GStreamer statically linked via the gst-full mechanism. Only the curated plugin
# set is compiled in and registered at gst_init - no plugin dir, no registry scan. This is the
# hand-compiled shipping binary that lands in the rootfs. (The SD squashfs, build-prefix.sh, remains
# the experimentation track - untouched.)
#
# Cross-builds in the same Alpine 3.24 aarch64 container pin as the rootfs, so musl + versions match.
# The actual build steps live in build-static-container.sh (this is just the docker wrapper).
# Output: gstreamer/build/static/ml-pipeline (self-contained). Set REBUILD=1 to force a clean gst
# rebuild after changing the plugin set; otherwise the cached build tree is reused.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

GST_TAG="${GST_TAG:-1.28.3}"   # match Alpine v3.24's gstreamer
# Curated set is defined by the -D<subproject>:<plugin>=enabled flags in build-static-container.sh;
# '*' then registers exactly that built set (avoids fragile plugin-name matching in meson).
GST_FULL_PLUGINS="*"
# gst helper libraries whose APIs ml-pipeline links (gst_app_*, GstVideo*, dmabuf allocator).
# gstreamer-1.0 is prepended by meson automatically; list only the extras.
GST_FULL_LIBS="gstreamer-app-1.0;gstreamer-video-1.0;gstreamer-allocators-1.0"

mkdir -p "$REPO/gstreamer/build/static"

docker run --rm --platform linux/arm64 \
  -v "$REPO":/w -w /w \
  -e GST_TAG="$GST_TAG" \
  -e GST_FULL_PLUGINS="$GST_FULL_PLUGINS" \
  -e GST_FULL_LIBS="$GST_FULL_LIBS" \
  -e REBUILD="${REBUILD:-}" \
  alpine:3.24 sh /w/gstreamer/scripts/build-static-container.sh

echo "[i] done; standalone binary at gstreamer/build/static/ml-pipeline"
