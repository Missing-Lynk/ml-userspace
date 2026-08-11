# MissingLynk userspace (the on-device open programs)

The open user-space stack for Artosyn Proxima-9311 + AR8030 devices: media pipeline, HUD/menu, RF/session control, hardware daemons, and the wire contract they share. These programs run on the open Alpine slot-B rootfs against the open kernel, with no vendor user-space.

The code is device-neutral where the SoC/RF pair is shared across goggle, VRx, VTx, and air-unit products. Hardware validation currently happens on BetaFPV VR04 HD goggles and matching air units.

## Components

| Component | What it is |
|---|---|
| `gstreamer/` | Media pipeline for receiver display/DVR and transmitter encode. Two packaging tracks: SD-card development builds and static rootfs-shipped binaries; see `gstreamer/README.md`. |
| `ml-hud/` | The LVGL menu and OSD stack (Betaflight OSD, system OSD, settings menu), drawn on a DRM overlay plane. Static aarch64, third-party deps via CMake FetchContent. |
| `ml-linkd/` | RF/session daemon for receiver and air roles: AR8030 cadence, media handshakes, telemetry, OSD transport, RF commands, standby and power policy. |
| `ml-*` tools | Small daemons and command-line helpers for RF bring-up, LEDs, MSP testing, and runtime control. Each tool directory carries its own local notes where needed. |
| `ml-shared/` | `mlm.h`, the MissingLynk Messaging (MLM) wire contract every component includes. |
| `assets/` | The boot splash and the OSD font, rendered by the user-space binaries and staged into the rootfs. |
| `docs/` | The component internals and the vendor interfaces they consume: codec APIs (`vdec`/`venc`/`mpp-buffers`), the decode-display pipeline, RF video downlink / modes / channels, the MSP OSD format, and the RF-reached air unit. |

Every program includes `ml-shared/mlm.h`, so they live in one repo: the wire contract stays a normal in-tree include instead of a cross-repo version-skew problem.

## Build

One top-level `Makefile` drives all of it (cross-builds need docker with arm64 emulation via qemu binfmt):

```sh
make            # everything: static daemons/tools, media pipeline, hud, font assets
make daemons    # static musl aarch64 daemons/tools -> build/
make gst        # SD-card development media binaries
make gst-static # static rootfs-shipped media binaries
make hud        # the LVGL HUD binary
make check      # host-side tests, no device and no docker
make clean      # remove build/
```

The static daemons/tools land in `build/`; gstreamer and hud own their own build trees.

## Building from a wrapper checkout

These programs are consumed via the [MissingLynk wrapper](https://github.com/Missing-Lynk/MissingLynk), which mounts this repo at `userspace/` alongside its sibling components. The build reaches out of the repo for a few things (the `kernel/` container pin, the `rootfs/build/` output area, the vendor `firmware/bin` blobs the gst stack stages), so build from a `--recurse-submodules` wrapper clone rather than this repo alone.

## Support

Everything here is free and open. The work behind it is unpaid nights and weekends: reverse engineering, bricked and recovered hardware, and a lot of time on a serial console. If it saved you some of your own, you can [buy me a coffee](https://buymeacoffee.com/stylesuxx).

Not bought the hardware yet? The [project README](https://github.com/Missing-Lynk/MissingLynk#support-this-project) has affiliate links that support the work at no extra cost to you.
