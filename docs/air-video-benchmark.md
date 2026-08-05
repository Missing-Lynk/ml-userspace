# Air video encoder throughput: reference results

Encoder and RF ceiling of the air unit's two-tile H.265 path, measured with no camera in the loop. Runner: `glue/camera/au-enc-bench.sh`, which drives `ml-air-video` in benchmark mode: a ring of frames rendered once into dma-heap buffers at startup, then resubmitted. No CPU pass over pixel data during the run.

Geometry: 1920x1080 split into tile 0 (560 rows) and tile 1 (552 rows), 32-row overlap. Two independent wave5 H.265 instances, GOP 65535, ring 8. Figures are per tile with both tiles running concurrently.

## Results

4 Mbit/s per tile CBR, rate control enabled, encode only (no transmit).

> **Every table below the visual-validation section predates the QP and VBV defaults.** They were taken at the driver's own `min_qp` 8 and `vbv_size` 1000 ms, which let rate control drive QP to the floor and permitted access units far above the datagram limit. The runner now defaults to `MINQP=25` and derives `VBV` from `BITRATE`, so achieved bitrates and peak access-unit sizes will differ. Re-baseline before comparing anything new against these rows.

### MODE=sustain, 60 fps, 15 s per tier, vendor-parity encoder configuration

`BITRATE=5000000`, `MINQP=25`, `IQP=32`, `MBRC=1`, derived `VBV=78` ms, `GOP=0`. All four tiers, one instance pair, one boot. Frames are per tile against 900 nominal; the shortfall is the inter-tier ring rewrite, which falls inside the measurement window.

| tier | frames t0 / t1 | fps held | bitrate/tile | largest AU | fits 65467 B? |
|---|---|---|---|---|---|
| static | 896 / 896 | 60 | 0.06 Mbit/s | 422 / 450 B | yes |
| bars | 863 / 863 | 60 | 0.30 Mbit/s | 803 / 764 B | yes |
| detail | 865 / 865 | 60 | 5.01 Mbit/s | 44,572 / 44,334 B | **yes** |
| noise | 899 / 898 | 60 | 37.0 Mbit/s | 77,487 / 76,622 B | no, 1.18x over |

Zero drops, zero lost, zero `VLC_BUF_FULL`. All four tiers held 60 fps.

**CU-level rate control was the missing parity setting.** Same tiers, same harness, `MBRC` the only difference:

| tier | peak with MBRC=0 | peak with MBRC=1 | reduction |
|---|---|---|---|
| detail | 276,948 B | 44,572 B | 6.2x |
| noise | 656,422 B | 77,487 B | 8.5x |

`detail` now fits one datagram with 32% headroom, where it was 4.2x over. It also lands on the 5 Mbit/s target exactly, and its mean access unit of 10,437 B sits close to the vendor's measured 11,154 B, so the stream now resembles vendor output in character as well as rate.

Peak-to-mean on `detail` is 4.3x against the vendor's 1.7x, so shaping is much better but not at parity. The remaining gap is most likely the vendor's `SetRcParam` override, whose field offsets are still untyped.

`noise` stays 1.18x over the ceiling and is not fixable by rate control: entropy coding expands incompressible data and no configuration bounds it. Diagnostic tier, not a target.

**The QP floor was the cause of the old overshoot.** At the driver's `min_qp` default of 8, easy content was pinned at the floor and could not spend fewer bits. Raising it to 25 cut `bars` from 5.9 to 7.3 Mbit/s down to 0.30, a factor of 20, while leaving `detail` unaffected because it was already inside the control range. That is what the long-unexplained "1.6x CBR overshoot" was.

### Vendor reference: measured access-unit sizes

From `archive/out/rf-capture/assoc-arm-sdio0.pcap`, parsed with `archive/libre/tools/ml-rf-udp/ml-rf-udp.py --frames`. 2017 video packets, both channels, 1920x1080 split into the same two tiles.

| | bytes |
|---|---|
| IDR (n=2) | 18,919 max |
| P-frames (n=2015) | 11,154 mean, 18,174 max |
| p50 / p99 / p999 | 12,029 / 16,798 / 18,040 |
| maximum | 18,919 |
| over 65,467 | **0** |

The vendor **does not split access units**. One AU per datagram with IP fragmentation underneath, the same design we use. It never approaches the ceiling: its worst packet is 28% of the limit.

Its distribution is also remarkably flat. The IDR is 1.7x the mean P-frame. For comparison, our `detail` tier's peak is 34x its mean. Real camera content is far more compressible than the synthetic tiers, and the vendor's intra frames are tightly bounded.

**So the ceiling is not a protocol problem.** It is a question of matching the vendor's per-frame budget, and the synthetic tiers' peaks do not predict camera behaviour.

### Vendor encoder configuration, from the blob

`AR_LOWDELAY_RtspVencInit(8, H265, H265CBR, w, h, 5000 kbps, gop=0, framerate)`, channel attr `enRcMode=10`, `u32Gop=0`, `u32StatTime=30`, `bByFrame=1`, GOP mode NormalP with `s32IPQpDelta = -2`, followed by a `GetRcParam` / `SetRcParam` override.

Two things this rules out, both checked in the decompile:

- **Not intra refresh.** `AR_MPI_VENC_SetIntraRefresh` is exported and implemented, but the H.265 RTSP path never calls it. Mainline's `HEVC_REFRESH_TYPE` only offers IDR anyway, so it is not a parity control here.
- **Not slice splitting.** `AR_MPI_VENC_SetH265SliceSplit` exists but venc8 does not use it, and it takes CTU rows rather than a byte cap.

Note that `video_gop_size` has a range of 0 to 2047, so the `65535` used in earlier runs could never have been accepted. The driver default of 0 applied instead, which happens to match the vendor.

### The rate-control parameters, settled

Every field the vendor's `GetRcParam` / mutate / `SetRcParam` sequence touches is now typed with an offset and a value, and so is every default the `Get` returns. The struct layout, the vendor values, and where each one was recovered from are in `venc-api.md` (`rcParam`); this section is the parity comparison against the open driver and what follows from it.

| field | vendor | driver default | control |
|---|---:|---|---|
| `s32CuOrMbLevelRcEnable` | 1 | 0 | `h264_mb_level_rate_control` (aggregate) |
| `s32HvsQPEnable` | 1 | 0 | same aggregate |
| `s32HvsQpScale` | **4** | 2, hardcoded | none yet: open gap |
| `s32HvsMaxDeltaQp` | **4** | 10, hardcoded | none yet: open gap |
| `s32FirstFrameStartQp` | -1 | -1 when RC is on | none needed |
| `u32Min/MaxIprop` | 50 / 100 | n/a | none, and none needed: see below |
| `u32Max{B,P,I}Qp` | 51 | 51 | `hevc_maximum_qp_value` |
| `u32Min{B,P,I}Qp` | 0 | **8** | `hevc_minimum_qp_value` |
| `bQpMapEn` | 0 | 0 | none needed |

Three consequences.

**CU-level rate control is confirmed parity**, not an improvement. It was enabled on inference and cut peak access-unit size 6.2x on `detail`; the vendor default turns out to be 1. The aggregate control is coarser than the vendor's three separate fields, but every field it sets matches.

**HVS QP scale and max delta are a real parity gap, still open.** The driver hardcodes 2 and 10 in `wave5_set_enc_openparam`; the vendor uses 4 and 4. Because there is one correct value per field on this hardware, the fix belongs in the driver rather than in a new control. Not yet written, so no measurement above reflects it.

**Iprop is retired as the explanation for the vendor's flat peak-to-mean.** The service stores and returns the pair but nothing on this path consumes it (call chain in `venc-api.md`); the host-side VBR helper reads a different pair of slots. It is public API state the service preserves, not a rate-allocation input. Whatever flattens the vendor's peak, it is not this.

**`MINQP` is now 0**, the vendor value, where the recorded runs above used 25. The driver's default of 8 is what let rate control pin flat content at the QP floor and overshoot the target 1.6x on `bars`; raising it shapes the stream better than the vendor does, which makes it an improvement rather than parity. The case for a floor above 0 lives in `plans/beyond-vendor-backlog.md`. Every table above was taken at `MINQP=25` and is not comparable to a run at the new default.

### VBV does not bound the peak on hard content

Derived `VBV=98` ms at 4 Mbit/s permits 49,000 B per access unit. Measured peaks were 277 KB on `detail` and 656 KB on `noise`, 5.6x and 13x that budget.

So VBV shapes the average and works on compressible content, but it is **not** a per-frame cap. Combined with the absence of any exposed hard per-frame byte limit, there is currently no way to guarantee an access unit fits one datagram.

Consequence for the synthetic tiers: at 4 Mbit/s and 60 fps, only `static` and `bars` are transmittable; `detail` and `noise` peak above the ceiling and would lose frames.

This does **not** predict the camera path. The vendor's measured distribution above tops out at 28% of the ceiling on real content, so `detail` is a synthetic worst case rather than a preview of what the sensor produces. The peak that matters is almost certainly the single IDR, which is the one frame a 65535 GOP never repeats and the one the vendor keeps to 1.7x its mean P-frame.

`oversize` reads 0 in encode-only runs because nothing is transmitted; `largest AU` is the diagnostic to read there.

### MODE=sustain with TX=1, 60 fps, 15 s per tier

Transmitting to the goggle on 10.0.0.1:10001 over `sdio0`. Separate boot from the encode-only table.

| tier | frames t0 / t1 | fps held | tx vs done | bitrate/tile | drops |
|---|---|---|---|---|---|
| static | 896 / 896 | 60 | equal | 0.02 Mbit/s | 0 |
| detail | 858 / 858 | 60 | equal | 3.9 to 4.1 Mbit/s | 0 |

`sdio0` over the whole run: 14,361,959 B in 7042 packets, 0 errors, 0 dropped. Consistent with `detail` at 7.9 Mbit/s total for 15 s.

`tx` counts a successful `sendto`, not receipt. Goggle-side decode and display are **not** verified by this run: the goggle was not reachable from the host, and the air unit carries no ssh client.

`bars` and `noise` are not run with `TX=1`. At 60 fps they produce 12.6 and 74 Mbit/s total against a link that carries about 8, so they measure link discard rather than path throughput.

### Visual validation, scroll tier with TX=1

`TIERS=scroll SECS=300 FPS=15 BITRATE=1000000 GOP=30`, derived `VBV=392` ms. Confirmed on the goggle panel: bars scrolling smoothly, no visible seam between the tiles.

| tier | fps | tx vs done | oversize | txerr | largest AU | bitrate/tile |
|---|---|---|---|---|---|---|
| scroll | 13 of 15 | equal | 0 | 0 | 422 / 450 B | 0.01 Mbit/s |

13 fps against 15 requested is the per-frame render cost of the tier. The tiny access units are expected: a pure translation with no dither leaves no residual after motion estimation. **This run validates the transport and the picture, not the encoder under load.**

### MODE=max

Separate boots, earlier module build.

| tier | max fps | bitrate/tile |
|---|---|---|
| static | 126 | 0.05 Mbit/s |
| bars | 114, 115 | 13.3 to 15.2 Mbit/s |
| detail | not measured | |
| noise | not measured | |

`static` and `bars` max are from one instance pair, so directly comparable.

Decoded verification: dumped elementary streams decode to 476 frames at 1920x560 and 1920x552. This counts frames and geometry only. **No run in this document has had a decoded picture inspected**, so nothing here is evidence that the ring produces a correct image; the figures are throughput figures alone.

### detail is not the worst-case rate tier

`detail` costs less than `bars` (3.3 to 5.0 against 5.9 to 7.3) despite denser high-frequency content and no global motion vector. The tier does not do what it was built to do.

`detail` also lands on the 4 Mbit/s target while `bars` overshoots by about 1.6x. Inference, not measured: `bars` is cheap enough that minimum QP binds and rate control cannot spend fewer bits, while `detail` sits inside the control range. If that holds, the "1.6x CBR overshoot" recorded below is a min-QP floor, not a calibration error, and it appears only on content easier than the target rate.

### The max figures are harness-bound, not encoder-bound

`static` leaves the encoder near-idle (all-skip, 0.05 Mbit/s) and still tops out at 126 fps, only 10% above `bars` at 114. The benchmark path itself, the feeder thread through appsrc, v4l2 and appsink with a ring of 8, therefore caps near 126.

Consequences:

- The encoder's own ceiling is **unmeasured**. It is at least 114 and the harness cannot show how much higher.
- `bars` costs roughly 10% of the available headroom relative to idle content.
- The 1.9x margin over 60 fps is a property of the whole benchmark path, which is the useful figure for "can this path carry 60 fps". It is not an encoder capability figure and must not be quoted as one.

## Content tiers

- `static` every ring slot identical. All-skip after the first IRAP.
- `bars` colour bars scrolled per frame plus a per-frame luma dither. The dither prevents every block resolving to a motion vector with no residual.
- `detail` a 3x3-box-blurred noise texture, sampled with a different horizontal velocity per band across 8 bands, alternating direction. Dense high-frequency structure with no global motion vector, still compressible. Chroma carries the same structure at a quarter excursion. Worst-case **rate**.
- `noise` full-entropy Y, Cb, Cr. Worst-case **survival**: the pass criterion is that the encoder keeps producing frames, not that it produces them quickly. Runs last, because a wedge ends the run.

- `scroll` bars rendered every frame with the phase taken from the shared frame counter. The only tier meant to be looked at. Renders per frame, so it caps near 17 fps and measures nothing.

`detail` does not replace `noise`. A rate cannot be measured on incompressible content, and a survival defect cannot be caught by content chosen to avoid it. Both stay in the default set.

### The ring tiers are not watchable, by construction

`static`, `bars`, `detail` and `noise` pre-render `pool_n` images once and resubmit them. The feeder takes whichever buffer the encoder released first, so the displayed phase hops among those `pool_n` values in completion order rather than advancing, and the two tiles hold unrelated phases so the seam between them does not line up. On a panel that reads as corruption. It is not: it is what a load generator looks like.

Judge the ring tiers by counters only. Use `scroll` to confirm the picture, or the live `videotestsrc` path via `glue/camera/au-video-tx.sh`.

## Modes

- `MODE=sustain` paces at `FPS`. Measures whether the rate holds.
- `MODE=max` resubmits as slots return. Measures the ceiling. `FPS` still sets the rate declared to the encoder.

## Counters

- `pushed/s` frames handed to the encoder.
- `done/s` encoder output. Counted separately from transmit, so it is valid with no goggle present.
- `dropped/s` frames skipped because no ring slot was free. Attributed to the first tile in the pair that lacks a buffer, so a stalled tile shows the drops on its partner.
- Bitrate is counted on encoder output, valid whether or not the run transmits.

## Constraints on measurement

**One `(bitrate, fps, mode)` point per boot.** Bitrate and frame rate are fixed at encoder instance open; the wave5 firmware has `OPT_CHANGE_PARAM` but the driver never issues it. Only the first encoder instance pair of a boot is usable: later instances watchdog or emit nothing while in `PIC_RUN`. Content tiers are free within a run.

**Rate control is off by default.** `V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE` defaults to 0 in wave5. Without it, `video_bitrate` and `video_bitrate_mode` have no effect: `open_param->rc_enable` stays clear. Any bitrate far above target was taken with rate control off and is not a bitrate measurement.

**wave5 cannot be hot-swapped.** Probing loads firmware into the VPU; a second probe returns `-16` from `vpu_init_with_bitcode` and leaves the codec unbound for the boot. The module must be in the rootfs before boot. The runner md5-checks it and refuses to run otherwise.

**CBR overshoot is 1.6x.** Rate control allocates constant bits per frame from the declared frame rate; achieved bitrate tracks the actual rate. Measured: 4 Mbit/s target held 6.5 at 60 fps; 115 fps against a declared 60 held 12 (4 x 115/60 x 1.6 = 12.3). In `MODE=max` the bitrate column is therefore not a link-budget figure.

## Datagram limit: one access unit per UDP datagram

The `:10001` protocol puts one access unit in one datagram. `AIR_TX_MAX` is 65507, the maximum UDP payload, so with `VPH_HDR_LEN` 36 and `VPH_TAIL_LEN` 4 the largest carriable access unit is **65467 B**. `vph_build` returns 0 above that (`vph.c:42`).

Bytes per access unit scale inversely with frame rate at a fixed bitrate, so the limit binds at low frame rates, not high ones:

| fps | 4 Mbit/s per tile | headroom to 65467 B |
|---|---|---|
| 60 | about 8.3 KB average | wide |
| 30 | about 16.7 KB average | adequate |
| 15 | about 33 KB average | peaks cross it |

Measured at 15 fps: tile 1 sustained 4.2 to 5.4 Mbit/s and lost about 20% of its frames, `done` 15/s against `tx` 12/s, while tile 0 at 3.9 to 4.0 Mbit/s lost none. Both `sdio0` netdevs reported 0 errors and 0 dropped, and UDP `SndbufErrors` was 0, because the frames never reached the socket.

This loss was **silent** before `tx_oversize` and `tx_error` were added: `dropped` counts ring-slot starvation only, so a discarded frame left every counter on both ends reading healthy. The only visible symptom was `tx` trailing `done`, which is easy to attribute to the link.

Consequences for measurement:

- A `TX=1` run below 30 fps is measuring the datagram limit, not the path.
- `tx` equal to `done` at 60 fps means frames fit, not that the link is lossless.
- Judge a transmit run by `oversize` and `txerr` being zero, then by `tx` against `done`.

### VBV sets the per-frame ceiling

`V4L2_CID_MPEG_VIDEO_VBV_SIZE` is a window in **milliseconds** (driver default 1000, range 10 to 3000). The bytes it permits in one access unit are `vbv_ms * bitrate / 8000`.

That is the lever, and the arithmetic matches the hardware exactly. At the driver default of 1000 ms and a 1 Mbit/s target it permits 125,000 B; measured `maxau` was 125,985 and 127,413.

To keep an access unit inside 65467 B, `vbv_ms` must be under `65467 * 8000 / bitrate`:

| bitrate/tile | max vbv | runner default (3/4 margin) | permits |
|---|---|---|---|
| 1 Mbit/s | 524 ms | 392 ms | 49,000 B |
| 4 Mbit/s | 131 ms | 98 ms | 49,000 B |
| 10 Mbit/s | 52 ms | 39 ms | 48,750 B |

`au-enc-bench.sh` derives `VBV` from `BITRATE` for this reason. A fixed value that is safe at one bitrate is silently unsafe at another.

`hevc_min_qp` (driver default 8) is the secondary lever: it stops rate control spending the whole window on easy content. It is not a byte cap. **No hard per-frame byte cap is exposed by this driver**; `pic_stream_buffer_size` is CAPTURE capacity, not a rate-control target, and overrunning it is the `VLC_BUF_FULL` failure instead.

`V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` is **not** exposed, and the per-picture path writes `W5_CMD_ENC_PIC_PIC_PARAM` as 0 every frame, so there is no on-demand IDR. Recovery from a lost keyframe depends on `GOP` or the refresh controls. With `GOP 65535` there is one IDR for the whole stream, so losing it to `oversize` leaves the decoder permanently unsynchronised. Transmit runs should use a short GOP.

## Bitrate scale

The vendor air unit takes no configured bitrate. `AR_8030_TX_GetBitRate` derives it from live RF MCS as `throughput * Ar803xThroutputRate * cfg`, capped at `ArMaxBitRate`, returning 8000 kbit/s when throughput reads zero. From captured `cfg_transmedium.json`: ratio 0.7, cap 20000 kbit/s. `bitrate_q` from the goggle menu is stored and never read.

| point | per tile | total |
|---|---|---|
| vendor default | 3.93 Mbit/s (HW-observed) | 8 Mbit/s |
| realistic link | varies | link throughput x 0.7 |
| `ArMaxBitRate` | 10 Mbit/s | 20 Mbit/s |

Runner default is 4 Mbit/s per tile.

## VLC_BUF_FULL: no longer reproduced, recovery still untested

```
wave5_vpu_enc_finish_encode: vpu_enc_get_output_info fail: -5 reason: 0x10000
```

`0x10000` is `WAVE5_SYSERR_VLC_BUF_FULL`. Historically one occurrence ended the run: no further `ENC_PIC` was attempted and every ring slot stayed held.

`noise` now completes 15 s at 60 fps with **zero** occurrences in dmesg. The overflow does not happen, so the recovery path in `0280` never executes and remains unexercised on hardware.

Attribution, not yet isolated: every wedging run predates the fix to the **second** `sizeimage` clamp in `s_fmt_out`, which is the value the CAPTURE pool allocates from. Those runs therefore had a 1 MiB coded buffer, not the 1.54 MiB (`w * h * 3 / 2` at 1920x560) the first clamp site suggested. Capacity is the likely explanation.

Capacity is not a general fix. Entropy coding expands incompressible data, so no buffer size bounds it; 1.54 MiB is merely enough for this content at the QP rate control settles on (37 Mbit/s per tile, about 77 KB per frame average). A harder peak frame can still overflow. The recovery requirement stands: an overflow must cost one frame, not the instance.

Flight-relevant, because one usable instance pair per boot means a wedge removes the encoder until power cycle.

To exercise the recovery deliberately, the coded buffer has to be forced small enough to overflow on `noise`.

## Not measured

- The encoder's own ceiling. The harness caps near 126 fps.
- `detail` and `noise` in `MODE=max`.
- A worst-case **rate** tier that actually exceeds `bars`.
- The `VLC_BUF_FULL` recovery path, which no run has entered.
- 10 Mbit/s per tile (`ArMaxBitRate`).
- Goggle-side receipt, decode and display. Every `TX=1` figure is an air-unit send counter.
- Link latency on the video path. Ping over `sdio0` measured 46 ms RTT, which is not necessarily the video path's queue.

## Discarded runs

| run | reason |
|---|---|
| static, bars, noise at RC off | rate control off; bitrates carry no information |
| second `bars` sustain | duplicate, used only as reproduction |
| two `noise` attempts | wedged, no rate produced |
| single-tile diagnostic | third encoder instance of the boot |

## Gaps, in order

1. Goggle-side verification: confirm the panel decodes and displays the TX run. Needs the goggle on the host USB bridge.
2. Raise the harness ceiling above 126 fps so the encoder's own limit becomes reachable: deeper ring, and check whether the single feeder thread or the per-frame appsrc push is the constraint. `MODE=max ONLY=0 TIERS=static` separates per-frame process overhead from total frames per second and costs one boot.
3. Redesign `detail` so it is actually harder than `bars`, or drop it and accept `bars` as the rate reference.
4. Confirm the `VLC_BUF_FULL` attribution by forcing a small coded buffer, which also exercises the recovery path.
5. 10 Mbit/s per tile, both modes.
