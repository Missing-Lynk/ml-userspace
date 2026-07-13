# ml-ledd

Status RGB LED indicator daemon. Owns the goggle's WS2812 status LED chain (three populated pixels, driven in unison) and renders a pattern (off / solid / breathe / blink) on it. Static binary, no external dependencies, single C file.

It is a command sink, not a link-state mirror: it binds `led.sock` and any producer sends it an `MLM_T_LED` record. The last command wins. This keeps LED policy out of the producers and lets the LED be reused for other indications (recording, battery, errors) without reworking the transport. ml-linkd is the first producer: breathe red until the RF link carries video, solid green once it does.

## The LED

One WS2812-style chain (three populated pixels, all driven the same colour) on the DesignWare SPI master, `/dev/spidev*.0`, CS0 (`dw_spi_mmio`; see `kernel/docs/status-led.md`). No hardware brightness ramp, so ml-ledd renders the animation itself by repainting the LED on a ~33 Hz tick. Each WS2812 data bit is one SPI byte at 6.25 MHz (`0x80` = "0", `0xFC` = "1"); one pixel is 24 bits GRB, the data is 3 pixels = 72 bytes, followed by trailing low bytes for the reset/latch. The bus id is dynamically assigned (`/dev/spidev0.0` on the open kernel, not a fixed index), so the node is globbed, not hard-coded (`SPIDEV=/dev/spidevN.0` overrides).

The "0" symbol is `0x80` (one high SPI bit), **not** the vendor's `0xC0`: on the 3-pixel chain the longer `0xC0` high pulse is misread as a "1" and the error compounds down the chain (all-red renders red/pink/orange). `0x80` is clean on all three pixels across R/G/B/white.

## IPC (contract: `../ml-shared/mlm.h`, datagram AF_UNIX under `/run/missinglynk`)

| Socket | Direction | Records |
|---|---|---|
| `led.sock` | producer -> ml-ledd (ml-ledd binds) | `MLM_T_LED`: `mode` (off/solid/breathe/blink), `r`/`g`/`b`, `period_ms` |

Producers send `MSG_DONTWAIT` and drop on error; ml-ledd never blocks a producer. Because commands are edges with no snapshot, a producer should re-assert its desired pattern periodically so a late-started or restarted ml-ledd reconverges (ml-linkd re-asserts ~1 Hz).

## Behavior

- Default at startup: breathe red, `2000 ms`. The LED is meaningful before any command arrives.
- `solid` writes once and idles (a static pattern only re-drives the bus on change); `breathe`/`blink` repaint every tick. Breathe uses a triangle envelope eased by a squared curve, integer math only (no libm).
- On a clean SIGTERM/SIGINT the LED is set off, so a stopped service is not misread as a live state.

## Boot placement

ml-ledd starts in the **boot** runlevel (`etc/init.d/ml-ledd`, `after devfs`), earlier than every other `ml-*` daemon (which sit in `default`). Its only dependencies are `/run` (mounted in sysinit) and `/dev/spidev*` (built-in SPI, present once devtmpfs mounts), both satisfied at the top of the boot runlevel, so breathe-red is one of the first signs of life. The binary is baked into the rootfs at `/usr/local/bin/ml-ledd` (staged by `rootfs/build.sh`), not the SD-card gst squashfs, which mounts later.

## Build

Built from the userspace repo root by the top-level `Makefile`:

```
make ledd   # static aarch64 musl binary (docker Alpine 3.24) -> build/ml-ledd
make        # everything (daemons, gstreamer, hud)
```

## Usage

```
ml-ledd
```

Foreground process. SIGINT/SIGTERM turn the LED off and exit cleanly. Owns `led.sock` and the status-LED spidev; one instance.
