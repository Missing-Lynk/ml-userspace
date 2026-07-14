#!/bin/sh
# build-static-container.sh - the in-container half of build-static.sh (runs inside the
# alpine:3.24 aarch64 image). Not meant to be run directly: build-static.sh invokes it via docker
# with the repo mounted at /w and these env vars set:
#   GST_TAG          gstreamer mono-repo tag to build (e.g. 1.28.3)
#   GST_FULL_PLUGINS value for meson -Dgst-full-plugins ('*' = every plugin we enable below)
#   GST_FULL_LIBS    value for meson -Dgst-full-libraries (extra gst helper libs to expose)
#   REBUILD          non-empty to force a clean gst rebuild (else reuse the cached build tree)
set -eux

PREFIX=/w/gstreamer/build/gst-static
OUT=/w/gstreamer/build/static

apk add -q build-base meson ninja pkgconf git flex bison \
    glib-dev glib-static zlib-dev zlib-static pcre2-static \
    libffi-dev gettext-dev libdrm-dev libgudev-dev orc-dev orc \
    linux-headers bash xz curl

# libdrm is the only static .a we need that Alpine does not package, so build it from source into
# the prefix. libgudev is not needed (our v4l2 plugin does not link it).
LIBDRM_VER=2.4.134
if [ ! -f "$PREFIX/lib/libdrm.a" ]; then
    cd /tmp
    curl -fsSL "https://dri.freedesktop.org/libdrm/libdrm-${LIBDRM_VER}.tar.xz" | tar xJ
    cd "libdrm-${LIBDRM_VER}"
    meson setup b --prefix="$PREFIX" --buildtype=release \
        --default-library=static -Dauto_features=disabled -Dtests=false
    ninja -C b install
fi

# Persist the clone + build tree in the (gitignored) build/ dir so re-runs reconfigure
# incrementally instead of re-cloning and rebuilding all 555 targets.
SRC=/w/gstreamer/build/gst-src
if [ ! -d "$SRC/.git" ]; then
    rm -rf "$SRC"
    git clone --depth 1 --branch "$GST_TAG" \
        https://gitlab.freedesktop.org/gstreamer/gstreamer.git "$SRC"
fi
cd "$SRC"

# Skip the ~555-target gst rebuild when the aggregate lib already exists (fast link iteration).
# The curated plugin set is defined by the -D<subproject>:<plugin>=enabled flags; auto_features=
# disabled turns everything else off, so -Dgst-full-plugins='*' registers exactly the built set.
if [ -n "${REBUILD:-}" ] || [ ! -f "$PREFIX/lib/libgstreamer-full-1.0.a" ]; then
    rm -rf build   # clean meson configure; the clone (the expensive part) is preserved
    meson setup build \
        --prefix="$PREFIX" \
        --buildtype=release \
        --default-library=static \
        -Dauto_features=disabled \
        -Dgst-full=enabled \
        -Dgst-full-target-type=static_library \
        -Dgst-full-plugins="$GST_FULL_PLUGINS" \
        -Dgst-full-libraries="$GST_FULL_LIBS" \
        -Dbase=enabled -Dgood=enabled -Dbad=enabled \
        -Dugly=disabled -Ddevtools=disabled -Dges=disabled \
        -Drtsp_server=disabled -Dlibav=disabled -Dgpl=disabled \
        -Dtests=disabled -Dexamples=disabled -Dtools=disabled \
        -Dintrospection=disabled -Ddoc=disabled -Dnls=disabled \
        -Dgstreamer:check=disabled \
        -Dgstreamer:gst_debug=false \
        -Dgst-plugins-base:app=enabled \
        -Dgst-plugins-base:videoconvertscale=enabled \
        -Dgst-plugins-base:videorate=enabled \
        -Dgst-plugins-base:typefind=enabled \
        -Dgst-plugins-good:v4l2=enabled \
        -Dgst-plugins-good:isomp4=enabled \
        -Dgst-plugins-bad:videoparsers=enabled \
        -Dgst-plugins-bad:kms=enabled
    ninja -C build
    ninja -C build install
else
    echo "=== gst-full already built ($PREFIX); skipping rebuild (REBUILD=1 to force) ==="
fi

# The per-plugin .pc land in meson-private/ (not installed), but gstreamer-full-1.0.pc Requires
# them, so install them into the prefix for pkg-config --static to resolve the whole closure.
for p in gstcoreelements gstapp gsttypefindfunctions gstvideoconvertscale gstvideorate \
         gstisomp4 gstvideo4linux2 gstvideoparsersbad gstkms; do
    cp -f "build/meson-private/$p.pc" "$PREFIX/lib/pkgconfig/" 2>/dev/null || true
done

echo "=== linking standalone ml-pipeline against gstreamer-full-1.0 ==="
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
pkg-config --exists gstreamer-full-1.0 && echo "have gstreamer-full-1.0" || echo "NO gstreamer-full-1.0 .pc"
# gst static libs are mutually circular -> --start-group/--end-group. --undefined (in the full .pc
# Libs.private) pulls the static plugin registration.
gcc -O2 -Wall -static -o "$OUT/ml-pipeline.debug" \
    $(pkg-config --cflags gstreamer-full-1.0) \
    /w/gstreamer/src/ml-pipeline/*.c \
    -Wl,--start-group $(pkg-config --libs --static gstreamer-full-1.0) -Wl,--end-group \
    -lpthread || { echo "LINK FAILED - see errors above"; exit 1; }

# Ship the stripped binary; keep the unstripped copy for symbolizing crashes.
cp "$OUT/ml-pipeline.debug" "$OUT/ml-pipeline"
strip --strip-all "$OUT/ml-pipeline"
echo "=== result (standalone, fully static) ==="
ls -la "$OUT/ml-pipeline" "$OUT/ml-pipeline.debug"
file "$OUT/ml-pipeline"
