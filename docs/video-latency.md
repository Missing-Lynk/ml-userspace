# Video latency: measurement and stage breakdown

How the RF video pipeline's latency is measured, what the numbers mean, and where the time goes. Current figures are from a 5-minute bench run (run-0104, 18249 frames, 2026-07-20) with the immediate-flip-submission path and the HUD re-assert suppression in place.

## Measurement

`ml-pipeline` stamps every frame at each pipeline stage with goggle-local `CLOCK_MONOTONIC` (`mlp-latstats.c`, enabled by `ML_LATSTATS=1`, flag file `/usrdata/missinglynk/latstats`). Frames are keyed by PTS/FrameId across stages, so percentiles are true per-frame distributions. A 1 Hz summary line goes to the log; `ML_LATRAW=1` additionally prints one line per flip with the absolute stage timestamps (short captures only).

The stage marks:

| mark | where | meaning |
|---|---|---|
| `rx` | `rf_rx` | first UDP datagram of the FrameId on :10001 |
| `dec0`/`dec1` | appsink new-sample | tile decoded out of wave5 |
| `pair` | `slot_push` | both tiles complete, frame assembled |
| `issue` | `disp_try_submit` | flip ioctl entered |
| `submit` | `disp_try_submit` | flip ioctl returned |
| `flip` | DRM flip event | frame latched for scanout at the vsync edge |

All marks are one clock on one device: no goggle/air clock synchronization is involved, so `rx -> flip` is absolute wall time for the goggle segment, not a relative estimate.

Boundaries of the measurement:

- Air-side time (camera exposure, ISP, encode, RF queueing) is upstream of `rx` and not covered. External glass-to-glass measurement (high-fps camera filming a stimulus and the panel) is the only way to include it.
- `rx` is the FIRST datagram of the frame, so the arrival of the remaining datagrams is inside the measurement.
- The flip event is the scanout latch. The panel then scans top to bottom over one refresh period, and the LCD adds its response time; both are constants common to any stack on this hardware.

`glue/dev/ab-report.py` aggregates the summary lines (plus `stats.csv` / `threads.csv` from ml-logd) into the M1-M6 metrics and compares two run sets; see `plans/video-latency-and-load.md` for the metric definitions.

## Stage breakdown (run-0104 medians)

| stage | p50 | nature |
|---|---|---|
| rx -> tile 0 decoded | 4.5 ms | tile 0 datagram tail + wave5 decode |
| tile 0 -> tile 1 decoded (pair) | 8.0 ms | tile 1 datagram arrival + decode |
| pair -> flip submitted | 1.1 ms | DVR tap, slot push, flip ioctl (ioctl itself 0.11 ms) |
| submit -> vsync latch | 10.2 ms | wait for the next refresh edge |
| rx -> flip total | 25.1 ms | p99 35.3 ms |

The two dominant blocks:

- **Tile arrival (12.5 ms rx->pair).** Mostly transmission spread, not compute: a frame is 33-50 KB at 16-24 Mbps and the air unit paces its datagrams over a large fraction of the frame period, tile 0 first, tile 1 trailing. Goggle-side compute is small (tile 0 is decoded 4.5 ms after its first byte, including its own datagram tail). This block is set by the air unit's send pacing.
- **Vsync quantization (10.2 ms submit->flip).** The assembled frame completes at a random phase of the refresh cycle and waits for the next latch; the median wait is about half a period. This is the target of pixel-clock pacing (below).

## Refresh vs source rate

Measured from the flip-event and pair timestamps of the same run:

- Panel refresh: 62.079 Hz. The VO's fixed timings scan an effective 2091 x 1144 total; at the stock 148.5 MHz pixel clock that is 62.08 Hz, not 60.
- Source rate: ~60.33 Hz effective at the pairing point.
- The 1.75 Hz beat forces a repeated frame per beat period: ~105 repeats/min predicted, 135/min measured (the rest is RF jitter). This is the entire residual cadence defect after the flip-path fixes.

The pixel clock is tunable at runtime: `artosyn_vo` exposes `pclk_hz` in sysfs, which slews the CGU pixel-PLL leaf (clamped 141.1-155 MHz, ~0.049 %/step, one frame per step). `mlp-pace.c` (ML_PACE, flag file `/usrdata/missinglynk/pace` whose contents seed the matched rate in Hz) closes the loop: a PI servo on the 5th percentile of the per-flip submit-to-latch wait, with a deadband (the P action itself slews phase, so correcting inside the noise floor sustains a limit cycle) and an asymmetric error clamp (post-wrap seconds read a large wait and would bias the integrator above the matched rate, sustaining a sawtooth of one repeated frame per ~2 s).

The source itself bounds what pacing can achieve: over 27k paced flips the air unit's frame cadence wanders 59.68-60.59 Hz (up to ~1.2 Hz between half-second windows) with frame-arrival jitter of p90 7.2 ms and p99 19.4 ms around the local trend. Frames arriving more than one display period late repeat unconditionally, a floor of roughly 30 repeats/min that no display-side controller can remove. The servo therefore buys bounded latency (~19-30 ms rx2flip instead of a slow 24-40 ms drift) and judder at that floor. Buffering one extra vsync before presentation would absorb the jitter at +16 ms latency (a smoothness-vs-latency mode choice); removing the wobble at its origin needs control of the air-unit encoder.

## History

| configuration | rx2flip p50 | p99 | completed fps | repeats/min |
|---|---|---|---|---|
| field-test baseline (run-0098) | 47.5 ms | 67.4 ms | 39.7 | 1280 |
| + immediate submit + HUD re-assert suppression (run-0104) | 25.1 ms | 35.3 ms | 59.5 | 135 |

The baseline's extra ~22 ms and the 40 fps cap came from the HUD's per-present legacy SETPLANE: on an atomic DRM driver it becomes a blocking commit on the shared CRTC, and DRM's `stall_checks()` then holds the next video flip until that commit's vblank cleanup, one stolen vsync on ~54% of flips. The HUD now suppresses its re-assert while `MLM_STATE_F_VIDEO_LIVE` is fresh (video commits latch the overlay's pixels anyway); its self-refresh returns automatically when video stops.
