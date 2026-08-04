# Air video encoder throughput: reference results

Encoder and RF ceiling of the air unit's two-tile H.265 path, measured with no camera in the loop. Runner: `glue/camera/au-enc-bench.sh`, which drives `ml-air-video` in benchmark mode: a ring of frames rendered once into dma-heap buffers at startup, then resubmitted. No CPU pass over pixel data during the run.

Geometry: 1920x1080 split into tile 0 (560 rows) and tile 1 (552 rows), 32-row overlap. Two independent wave5 H.265 instances, GOP 65535, ring 8. Figures are per tile with both tiles running concurrently.

## Results

4 Mbit/s per tile CBR, rate control enabled, encode only (no transmit).

### MODE=sustain, 60 fps, 15 s per tier

All four tiers, one instance pair, one boot. Frames are per tile against 900 nominal; the shortfall is the inter-tier ring rewrite, which falls inside the measurement window.

| tier | frames t0 / t1 | fps held | bitrate/tile | drops |
|---|---|---|---|---|
| static | 896 / 896 | 60 | 0.02 Mbit/s | 0 |
| bars | 862 / 862 | 60 | 5.9 to 7.3 Mbit/s | 0 |
| detail | 864 / 864 | 60 | 3.3 to 5.0 Mbit/s | 0 |
| noise | 895 / 894 | 60 | 37.0 Mbit/s | 1 |

`bars` at 60 fps agrees with two earlier independent runs on different boots (5.4 to 7.6 Mbit/s).

### MODE=max

Separate boots, earlier module build.

| tier | max fps | bitrate/tile |
|---|---|---|
| static | 126 | 0.05 Mbit/s |
| bars | 114, 115 | 13.3 to 15.2 Mbit/s |
| detail | not measured | |
| noise | not measured | |

`static` and `bars` max are from one instance pair, so directly comparable.

Decoded verification: dumped elementary streams decode to 476 frames at 1920x560 and 1920x552.

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
- `noise` full-entropy Y, Cb, Cr. Worst-case **survival**: the pass criterion is that the encoder keeps producing frames, not that it produces them quickly. Currently fails. Runs last, because a wedge ends the run.

`detail` does not replace `noise`. A rate cannot be measured on incompressible content, and a survival defect cannot be caught by content chosen to avoid it. Both stay in the default set.

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

`noise` now completes 15 s at 60 fps with **zero** occurrences in dmesg. The overflow does not happen, so the recovery path in `0010` never executes and remains unexercised on hardware.

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
- `TX=1` with the goggle up, so RF is never separated from encode.

## Discarded runs

| run | reason |
|---|---|
| static, bars, noise at RC off | rate control off; bitrates carry no information |
| second `bars` sustain | duplicate, used only as reproduction |
| two `noise` attempts | wedged, no rate produced |
| single-tile diagnostic | third encoder instance of the boot |

## Gaps, in order

1. `TX=1` with the goggle up. All figures so far are encode-only.
2. Raise the harness ceiling above 126 fps so the encoder's own limit becomes reachable: deeper ring, and check whether the single feeder thread or the per-frame appsrc push is the constraint. `MODE=max ONLY=0 TIERS=static` separates per-frame process overhead from total frames per second and costs one boot.
3. Redesign `detail` so it is actually harder than `bars`, or drop it and accept `bars` as the rate reference.
4. Confirm the `VLC_BUF_FULL` attribution by forcing a small coded buffer, which also exercises the recovery path.
5. 10 Mbit/s per tile, both modes.
