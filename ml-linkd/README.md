# ml-linkd

RF link daemon for the AR8030 link. One binary, two roles selected by `--role` (default `rx`): the goggle (RX) side brings the link up, keeps it up, and starts the transmitter's video stream when a consumer is ready; the air unit (`--role air`) side transmits telemetry to the goggle. Static binary, no external dependencies.

## Air (TX) role (`--role air`)

Association is autonomous from the `artosyn_sdio` insmod config, so in the steady state the air role speaks only UDP on `sdio0`. Three paths open `/dev/artosyn_sdio`: the rate governor and the TX power control, both off unless asked for, and the pair window, only while it is open. The fd is shared and reference-counted, because two opens of that node in one process wedge the RF chip.

- Reads the two SoC sensors over IIO: battery voltage from the SAR ADC (`artosyn-adc`, channel 1, `in_voltage1_input` x the board divider) and the junction temperature from the SoC sensor (`temperature`, `in_temp_scale`). Both are resolved by IIO device name and retried until the modules have coldplugged.
- Transmits the `:10000` status frames to the goggle (10.0.0.1): `0x11` periodic (voltage, ~6 Hz) and `0x09` version/info (hw/fw strings + voltage + temperature, ~1 Hz).
- Answers the goggle's `:20001` identity probe (mirrors the 520-byte type-0 datagram back with `byte[0]=0x01`).
- Answers the goggle's `:10000` type-1 MEDIA_PARAMS_REQUEST with a type-2 reply. The goggle gates its solid-green LED, its IDR request and its video-stall watch on this reply. The reply is the 24-byte header alone.
- Answers the goggle's `:10000` type-3 MEDIA_IDR_REQUEST by forcing a keyframe through `ml-air-video`'s control socket. The stream carries one IDR at FrameId 0 and P-frames after, so this is how a receiver that joined later gets a decodable entry point. At most one forced keyframe per 500 ms.

- Reads the TX power and standby-arm the goggle commands in `:10000` SetTranParm (`0x0d`) and SetLdCfg (`0x0a`), and answers its StbAck (`0x1b`). Off unless `--power-adapt` or `--power-probe` is passed; without either, all three are drained and discarded.
- Watches the bind button and runs a 30 s pair window on a press held 2 s or less. The window blinks the red indicator, holds the green power LED off, and binds to whichever peer answers.

The goggle's RX role republishes the received `0x09`/`0x11` frames on `telemetry.sock` as `MLM_T_STATUS`, so the HUD shows the air unit's voltage and temperature.

### Pair window (DEV role)

A press held at most 2 s runs the pair sequence; a longer hold does nothing. The window is 30 s.

1. Refuse if a link is up. `GET_MCS` throughput reads 0 with no peer associated and the link rate once associated, so link state is read from the chip. A chip that does not answer within 300 ms also refuses.
2. Save the current `ap_mac`, taken from `"ap_mac"` in `/usrdata/missinglynk/bb_config_air.json` when present, otherwise `/lib/firmware/bb_config_air.json`.
3. Write `ap_mac` = `ffffffff`. On failure the window does not open and nothing is undone, since pair mode was never entered.
4. Enter pair mode on slot 0. On failure the saved `ap_mac` is written back and the window does not open.
5. Poll `GET_PAIR` every 20 ms. Byte 0 of the reply is the candidate bitmask; the lowest set bit becomes the slot, and the peer MAC follows at `1 + slot*4` in wire order. Hits accumulate and are not reset by a zero read.
6. On the 6th hit, exit pair mode on the discovered slot, then write `ap_mac` = the peer MAC on the following tick, byte order unchanged.
7. On expiry, exit pair mode and write the saved `ap_mac` back, retried for up to `AIR_BIND_RESTORE_TRIES` ticks.
8. After a committed peer, `ml-rf-persist --air` writes it to the config.

A commit the chip rejects is not persisted: the sequence moves to step 7 instead, so a persisted binding is one the chip holds.

The window is a state machine across ticks, not a blocking call, because `ml_msp_service()` drains the FC UART every tick and a 30 s block overruns that tty at 115200 with MSP DisplayPort active. The tick shortens to 20 ms while the window is open. Periodic transmits are gated on elapsed milliseconds, so the tick change does not move their rates.

The chip re-reads its config at insmod and there is no host apply command, so a committed peer survives a power cycle only through the persisted file: `ml-rf-persist --air` sets `baseband.basic.dev.ap_mac` in a `/usrdata/missinglynk` copy, minified, and `ml-air-link` exports `ML_RF_FW_PATH=/usrdata/missinglynk` so `ml-rf-bringup` puts that directory on the kernel firmware search path ahead of `/lib/firmware`.

### Bind button

The button is a `gpio-keys-polled` device at a 50 ms `poll-interval`, since `artosyn_gpio` registers no irqchip. The kernel emits an evdev event on a level change, so every measured hold carries a 50 ms quantisation.

The device is resolved by matching `EVIOCGNAME` against `ml-bind-button` across `/dev/input/event*`, retried every 5 s until it appears. Events are drained non-blocking once per tick, up to `AIR_BIND_EV_BURST_MAX`; the kernel queues anything arriving between ticks. Hold time is the difference of two `ev.time` stamps. Those are `CLOCK_REALTIME` while the window deadline is `CLOCK_MONOTONIC`; the two are never mixed, and a negative hold is rejected.

The blink is the kernel `timer` trigger at `delay_on` = `delay_off` = 40 ms. `delay_on`/`delay_off` exist only once the trigger is selected, so the trigger is written first. The green LED is set to 0 for the duration of the window and restored to its sampled brightness after: the two lines share one series resistor, so lighting green starves the red. LED writes fail silently, the nodes being absent until `artosyn_gpio` binds.

### Service loop

Both roles run a fixed-tick loop: each service routine runs in turn, every fd is checked non-blocking, and the tick ends in `usleep()`. The air role ticks at 50 ms, and 20 ms while a pair window is open. Periodic transmits are gated on elapsed milliseconds rather than tick counts. Every drain is bounded per tick, so no socket or input queue can hold the loop.

### Encoder rate governor (`--rate-adapt`, off by default)

Polls `GET_MCS` on the bb socket every 200 ms and derives an encoder target from the reply: `(throughput_kbps / 100) * 100 * 0.7`, capped at 20000 kbps, and 8000 kbps when throughput reads zero. The integer divide happens before the multiply, so the input is quantised to 100 kbps steps. The result is a total across both tiles; it is halved for the per-tile control and pushed to `ml-air-video` on `/run/missinglynk/air-video.sock`.

It recomputes on MCS transitions only, not on throughput drift, and the response is asymmetric: a drop in MCS is applied at once, a rise only after the higher MCS has held for a second. Frame rate and min QP are not touched.

`--rate-probe` runs the same poll and logs the raw reply, the decode and the target it would send, without touching the encoder. The reply is 8 bytes: MCS index at +0 biased by 2, link throughput as a u32 LE in kbps at +4. Measured values are `19 00 00 00 00 00 00 00` unassociated (MCS 23, 0 kbps) and `0c 00 00 00 be 51 00 00` associated (MCS 10, 20926 kbps), so throughput is the field that distinguishes the two states and the idle MCS reading is the higher one.

### TX power and standby (`--power-adapt`, off by default)

Two independent gates: **radiated power follows the FC arm state, frame rate follows the goggle's standby arm.** Arming means flying, so it takes both to full.

| FC | armed | standby | frame rate | power |
|---|---|---|---|---|
| absent | - | no | 60 | goggle commands |
| absent | - | yes | 15 | goggle commands |
| present | no | no | 60 | **minimum (5 dBm)** |
| present | no | yes | 15 | **minimum (5 dBm)** |
| present | yes | either | 60 | goggle commands |

**An absent FC is "arm unknown", not "disarmed".** `arm_flag` reads 0 with no FC attached, which is indistinguishable from a real disarm, so the arm gate applies only while `ml_msp_fc_present()` holds (MSP seen within `ML_MSP_FRESH_MS`). Otherwise a bench unit with no FC would sit at the minimum forever and look like a fault.

Arming also cancels a commanded standby in the reported work mode, so the goggle is never shown a standby the air is not honouring. Standby itself no longer moves power: a disarmed aircraft is already at the minimum, and an armed one is flying.

Both `:10000` messages carrying the two fields are read, and only those two fields: SetTranParm (`0x0d`, body\[0\] dBm, body\[8\] standby arm) arrives on a ~2 s cadence and is the live lever, SetLdCfg (`0x0a`, struct offsets 0x68 and 0x70) arrives once per association and is the durable one. The remaining 190 bytes of SetLdCfg are undecoded vendor state and are not read.

A value outside the chip's `pwr_range` of \[5, 23\] dBm is rejected rather than clamped, and leaves both the previous commanded value and the standby bit alone. A power that has not changed is not re-written, so the 2 s command cadence does not become a 2 s write cadence on the bb socket. Honouring a commanded power and letting the chip adjust it are mutually exclusive, so the first write turns the chip's self-adjust off and it stays off.

The air reports its work mode in a `0x12` on every change and re-reports an unacked standby entry every 500 ms; **the frame rate does not drop until the goggle's `0x1b` ack arrives**, so the receiver always knows before the stream changes under it. Leaving standby does not wait for anything. Transmit duty, the third vendor standby lever, is driven by the AP and is not touched here.

The frame-rate half is pushed on `ml-air-video`'s control socket as **`capfps`, not `fps`**, and the difference is the point. `capfps` moves the capture feeder alone and leaves the encoders' declared frame rate where it is; their rate control budgets bits per picture from that declared rate, so feeding fewer pictures makes the emitted bitrate fall in proportion. `fps` would move both, keeping bits-per-picture consistent and holding the bitrate at the configured bits per second — measured flat at ~1015 kB/s across 60 and 15 fps. So standby is a bitrate drop as well as a frame-rate drop, and the airtime saving comes from the feeder half rather than the power half.

After 5 s of `:10000` silence the commanded state is discarded and the radio is handed back to the chip's closed loop at full frame rate, so a goggle that disappears mid-standby cannot leave the air pinned at the minimum.

`--power-probe` polls the read-back and logs the target it would apply, writing nothing to the radio and sending nothing on the wire. `GET_POWER` is polled at 1 Hz in both modes; its 2-byte reply is read as the `{dir, dBm}` pair `SET_POWER` writes, a decode transcribed from the SET payload shape rather than captured, which is what the probe's raw hexdump is for.

The startup power is not set here. It comes from `baseband.basic.power` (`auto_init` / `manu_init`) in `bb_config_air.json` at insmod and governs the pre-association window, which is the window that decides whether the link comes up at all.

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
- `:10000` - params handshake: 24 B type-1 request every 2 s (timestamp at offset 8); on the TX unit's type-2 reply, send the 24 B type-3 MEDIA_IDR_REQUEST. Video on `:10001` starts after the type-3, which the TX unit answers with a keyframe (`mp-cmd.h`, `mp_idr_request`). Sent while video is unconfirmed; stops once the consumer reports composed frames.
- `:10000` - telemetry RX (u32 LE message type at offset 0): `0x10` = MSP DisplayPort, `0x09`/`0x11` = binary status. Republished over the IPC sockets below.

### Consumer-ready gate

The type-1 request is held until a consumer declares READY on `link.sock`. The TX unit emits a single IDR at FrameId 0 and P-frames after, so the gate guarantees the video consumer is already bound on `:10001` when it arrives.

- READY = `MLM_T_READY` heartbeat; consumer liveness window 6 s.
- `frames_seen` is decode-level (the pipeline's composed-frame count), so it separates "datagrams arriving" from "picture on screen". While it is 0 the type-3 IDR request keeps going out, repairing a late join or a lost keyframe without a session restart.
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
ml-linkd [-d /dev/artosyn_sdio] [--role air|rx] [--no-gate] [--rate-adapt|--rate-probe]
         [--power-adapt|--power-probe] [-v]
```

`--role air` runs the air-unit telemetry transmitter (see the Air role section); the default `rx` runs the goggle side documented below. `--rate-adapt` / `--rate-probe` and `--power-adapt` / `--power-probe` apply to the air role only.

Foreground process. SIGINT/SIGTERM stop it cleanly (cadence stops, device closed, `link.sock` unlinked). `-v` logs every transmitted frame.

## Known limitations

- The gate's `frames_seen` confirmation is only as good as the consumer's report; a consumer reporting a cumulative counter can confirm from a previous session after a restart, stopping the type-1 poll early.
- If `/lib/firmware/bb_config_air.json` cannot be read, a pair window still opens but has no `ap_mac` to restore, so a window that finds no peer leaves the unit broadcast-bound until a power cycle. It logs this at window open.
