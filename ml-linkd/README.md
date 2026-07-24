# ml-linkd

RF link daemon for the AR8030 link. One binary, two roles selected by `--role` (default `rx`): the goggle (RX) side brings the link up, keeps it up, and starts the transmitter's video stream when a consumer is ready; the air unit (`--role air`) side transmits telemetry to the goggle. Static binary, no external dependencies, single C file.

## Air (TX) role (`--role air`)

On the air unit ml-linkd owns no bb-socket control plane - association is autonomous from the `artosyn_sdio` insmod config, so the air role never opens `/dev/artosyn_sdio`. It speaks only UDP on `sdio0`:

- Reads the two SoC sensors over IIO: battery voltage from the SAR ADC (`artosyn-adc`, channel 1, `in_voltage1_input` x the board divider) and the junction temperature from the SoC sensor (`temperature`, `in_temp_scale`). Both are resolved by IIO device name and retried until the modules have coldplugged.
- Transmits the vendor's `:10000` status frames to the goggle (10.0.0.1): `0x11` periodic (voltage, ~6 Hz) and `0x09` version/info (hw/fw strings + voltage + temperature, ~1 Hz).
- Answers the goggle's `:20001` identity probe (mirrors the 520-byte type-0 datagram back with `byte[0]=0x01`).

The goggle's RX role republishes the received `0x09`/`0x11` frames on `telemetry.sock` as `MLM_T_STATUS`, so the HUD shows the air unit's voltage and temperature.

## Behavior

### Link FSM (`/dev/artosyn_sdio`)

| State | Action | Next |
|---|---|---|
| `WAIT_DEV` | open the device node, retry every 1 s | `ASSOC` |
| `ASSOC` | send the fixed association frame sequence, 20 ms spacing | `SETTLE` |
| `SETTLE` | ~2.5 s of port `0x73` link poll (~6 Hz) + `ff02` heartbeat only | `OPEN` |
| `OPEN` | two SET config frames (select channel index 5, RX power 23 dBm), once; then TX power 23 dBm + power auto-adjust | `STEADY` |
| `STEADY` | port `0x0c` ~24 Hz, port `0x73` ~6 Hz, `ff02` ~3.4 Hz, forever | - |

Port `0x0c` polling must not start before `OPEN`; sending it during association wedges the TX unit. The chip's ASCII log (channel `0x05`) is printed to stdout as `[chip]` lines under `-v` (off by default, since a disconnected link floods it fast enough to fill the log tmpfs).

### UDP (`sdio0`, RX = 10.0.0.1, TX = 10.0.0.100)

- `:20001` - 3-way hello: 520 B type-0 at ~3 Hz; on the TX unit's type-1 identity, reply with the same packet, `byte[0]=0x02`, `byte[5]=0x00`. Hello stops once done.
- `:10000` - params handshake: 24 B type-1 request every 2 s (timestamp at offset 8); on the TX unit's type-2 reply, send the 24 B type-3 ACK. Video on `:10001` starts only after the ACK.
- `:10000` - telemetry RX (u32 LE message type at offset 0): `0x10` = MSP DisplayPort, `0x09`/`0x11` = binary status. Republished over the IPC sockets below.

### Consumer-ready gate

The type-1 request is held until a consumer declares READY on `link.sock`. The TX unit emits a single IDR at FrameId 0 immediately after the ACK; the gate guarantees the video consumer is already bound on `:10001` when it arrives.

- READY = `MLM_T_READY` heartbeat; consumer liveness window 6 s.
- The type-1 poll stops when a heartbeat with `frames_seen=1` arrives after the ACK; if none arrives within 6 s of an ACK, polling resumes (a lost type-3 leaves video off with no TX-side retry).
- `--no-gate` disables the gate.

### TX loss

More than 5 s without `:10000` RX in `STEADY` = TX unit lost: handshake state resets, the bb-socket cadence keeps running (the chip re-associates autonomously). When `:10000` RX resumes, the 3-way and the gated params handshake rerun.

## IPC (contract: `../ml-shared/mlm.h`, datagram AF_UNIX under `/run/missinglynk`)

| Socket | Direction | Records |
|---|---|---|
| `link.sock` | consumer -> ml-linkd (ml-linkd binds) | `MLM_T_READY` heartbeat, `frames_seen` flag |
| `telemetry.sock` | ml-linkd -> consumer | `MLM_T_STATUS` (raw `0x09`/`0x11` frames), `MLM_T_LINK` (states: associated, params_acked, tx_lost, session_restart) |
| `osd.sock` | ml-linkd -> consumer | `MLM_T_MSP` (raw `0x10` frames) |

Sends are `MSG_DONTWAIT`, dropped on error; a missing or slow consumer never blocks the link.

## Requirements

- The RF stack must already be up: `artosyn_sdio` loaded with firmware, `sdio0` configured as 10.0.0.1. ml-linkd loads no modules.
- One instance; it owns `/dev/artosyn_sdio`, UDP `:10000`, `:20001`, and `link.sock`. The video port `:10001` is not touched.

## Build

Built from the userspace repo root by the top-level `Makefile`:

```
make linkd  # static aarch64 musl binary (docker Alpine 3.24) -> build/ml-linkd
make        # everything (daemons, gstreamer, hud)
```

## Usage

```
ml-linkd [-d /dev/artosyn_sdio] [--role air|rx] [--no-gate] [-v]
```

`--role air` runs the air-unit telemetry transmitter (see the Air role section); the default `rx` runs the goggle side documented below.

Foreground process. SIGINT/SIGTERM stop it cleanly (cadence stops, device closed, `link.sock` unlinked). `-v` logs every transmitted frame.

## Known limitations

- The gate's `frames_seen` confirmation is only as good as the consumer's report; a consumer reporting a cumulative counter can confirm from a previous session after a restart, stopping the type-1 poll early.
- Binding a new TX unit is not implemented; the daemon only associates with an already-bound peer.
