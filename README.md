# MissingLynk userspace (the on-device open programs)

The open user-space stack for Artosyn Proxima-9311 + AR8030 devices: the video pipeline, the HUD/menu, the RF link and status-LED daemons, and the wire contract they share. These run on the open Alpine slot-B rootfs against the open kernel, with no vendor user-space. Device-neutral: the same SoC/RF pair spans goggle, VRx, VTx, and air-unit products; hardware validation happens on a BetaFPV VR04 HD goggle, but nothing here is goggle-specific except where noted.

## Components

| Component | What it is |
|---|---|
| `gstreamer/` | The video pipeline: RF-feed decode-to-display (zero-copy DRM/KMS at 1080p60), the two-process pipeline/HUD split, and DVR recording on the wave5 encoder. Two packaging tracks (SD squashfs for development, a static binary for the rootfs); see `gstreamer/README.md`. |
| `hud/` | The LVGL menu and OSD stack (Betaflight OSD, system OSD, settings menu), drawn on a DRM overlay plane. Static aarch64, third-party deps via CMake FetchContent. |
| `ml-linkd/` | The RF link daemon: AR8030 association and steady cadence, the READY-gated `:10000`/`:20001` handshakes, telemetry published over the mlm seams. |
| `ml-ledd/` | The status-LED daemon: renders off/solid/breathe/blink on the WS2812 chain; a command sink any producer can drive. |
| `ml-shared/` | `mlm.h`, the MissingLynk Messaging (MLM) wire contract every component includes. |
| `assets/` | The boot splash and the OSD font, rendered by the user-space binaries and staged into the rootfs. |
| `docs/reference/` | The component internals and the vendor interfaces they consume: codec APIs (`vdec`/`venc`/`mpp-buffers`), the decode-display pipeline, RF video downlink / modes / channels, the MSP OSD format, and the RF-reached air unit. |

Every program includes `ml-shared/mlm.h`, so they live in one repo: the wire contract stays a normal in-tree include instead of a cross-repo version-skew problem.

## Build

One top-level `Makefile` drives all of it (cross-builds need docker with arm64 emulation via qemu binfmt):

```sh
make            # everything: daemons, gstreamer, hud
make daemons    # ml-linkd + ml-ledd (static musl aarch64) -> build/
make gst        # the gstreamer pipeline/HUD binaries
make hud        # the LVGL HUD binary
make clean      # remove build/
```

The static daemons land in `build/`; gstreamer and hud own their own build trees.

## Building from a wrapper checkout

These programs are consumed via the [MissingLynk wrapper](https://github.com/Missing-Lynk/MissingLynk), which mounts this repo at `userspace/` alongside its sibling components. The build reaches out of the repo for a few things (the `kernel/` container pin, the `rootfs/build/` output area, the vendor `firmware/bin` blobs the gst stack stages), so build from a `--recurse-submodules` wrapper clone rather than this repo alone.

## Support

This is unpaid nights-and-weekends work: reverse engineering, bricked-and-recovered hardware, and serial-console archaeology. Everything here is free and open, but if it saved you time or got video flowing off your goggles, you can [buy me a coffee](https://buymeacoffee.com/stylesuxx) - it genuinely helps keep work like this going.

## License

GPL-3.0 (see [`LICENSE`](LICENSE)): the on-device stack a vendor would ship in a product, so copyleft and anti-tivoization keep derived firmware open and devices reflashable. The proprietary vendor firmware and binaries are not covered by this license and are not distributed here.
