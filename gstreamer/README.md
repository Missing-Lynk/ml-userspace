# gstreamer/ - GStreamer on the open slot-B goggle, from the SD card

The development packaging from `plans/gst-hud-architecture.md`, hardware-validated 2026-07-05: full Alpine v3.24 aarch64 GStreamer 1.28.3 packages plus the display/codec kernel modules and runtime scripts, packed into one squashfs image on the (exFAT, so symlink-free) SD card, loop-mounted on the goggle at `/mnt/gst`.

**Two build tracks** (`plans/gst-static-rootfs.md`): this SD squashfs is the **development** track - the full dynamic GStreamer with debug logging, fast to iterate against (swap a plugin, re-run, no relink). The **production** track is a single standalone static binary that ships in the rootfs with no SD card at all (see "Production build" below). Same `ml-pipeline` source; different packaging.

**Validated results (BetaFPV VR04, open kernel #37):**
- `videotestsrc` -> `kmssink`: works, BGRx and I420 both render correctly. CPU-painted 1080p tops out ~15-30 fps (videotestsrc painting + ~30-45 ms/frame copies into uncached/display memory); that is a CPU limit, not a pipeline defect.
- H.264/H.265 file -> `v4l2h264dec`/`v4l2h265dec` (wave5) -> `kmssink`: **1080p60 at a measured 60.00 fps, 0 drops, true zero-copy** (decoder dma-bufs PRIME-imported straight to the display plane). Requires `capture-io-mode=dmabuf` + `skip-vsync=true`.
- The two-process pipeline/HUD split (`plans/done/gst-two-process-hud.md`): `ml-pipeline` decoding on the primary plane at 59.97 fps / 0 drops WHILE `ml-hud` renders a live per-frame counter on the ARGB4444 overlay plane, joined only by a datagram socket and a shared DRM fd. Kill/restart of either side leaves the other undisturbed (a cleanly SIGTERMed `ml-pipeline` does NOT wedge the VPU - only unclean kills do). Confirmed on-panel 2026-07-05: live video + counter, cold boot.
- **Bad-boot gotcha (seen once, 2026-07-05):** one boot came up with the display dead end-to-end - no fbdev console, black panel, `bl_power` stuck at 4 - while decode/DRM stats all looked perfect (SetPlane latches at 60 Hz into a display nobody sees). Decode fps is NOT proof of visible output. Sanity check after every boot: is the console visible on the panel? If not, power-cycle before debugging anything else. (The first-gst-run plugin-registry rebuild - ~19 s scanning 238 plugins - is FIXED: `gst-env.sh` now persists the registry on the SD card instead of tmpfs, so it survives reboots; startup is <1 s warm. Pre-build it at deploy time so no user ever waits.)

## Default operating mode (the target runtime)

The default runtime is **composite + DMA-blit + OSD**, hardware-measured 2026-07-08: **62 fps, evict=0, blit=2750d/0c (0 CPU blits)**, video + OSD both live, CPU mostly idle. The three requirements (max framerate + framebuffer/OSD + low system load) all come from this one configuration:

- **`ml-pipeline ML_COMPOSE=1`** - decode the two RF tiles, composite into one 1080p frame on the DRM primary plane, drive the display.
- **`ml_dmablit.ko` loaded** (`/dev/ml-dmablit`) - the two-tile compose runs on the **DMA engine, off-CPU**. REQUIRED for 60 fps in composite mode; without it the CPU-blit fallback caps ~40 fps and eats a core.
- **`ml-hud`** on the ARGB overlay plane - the OSD/menu, blended at scanout only.
- Composite pool in **CMA** (`ML_HEAP=default_cma_region`, default), **full depth** - do NOT apply the `ML_COMP_MAX` / `dec_cap_bufs` caps: they were a DVR-memory-budget workaround and starve the compositor to ~40 fps with `evict>0` (signature: low fps + idle CPU + high evict = buffer starvation, not blit cost). See memory `composite-60fps-needs-dmablit`.

Composite mode (vs direct tile scanout) is deliberate, not a fallback: it is the only mode that leaves a plane free for the OSD overlay AND produces the assembled 1080p frame the DVR encodes. Recording tees off the composite; because the OSD is a scanout-only overlay plane, the recorded video has **no OSD burned in** (matching the vendor). The SD card mounts at `/mnt/sdcard` (`ml-sdcard` init); recordings and the menu's Playback list both live there.

**DVR records native 1080p60**: 60 fps composite display + a playable 1920x1080@60 H.264 MP4 simultaneously, `drop=0 evict=0`. `ML_DVR_RES=720 ML_DVR_FPS=30` selects a smaller recording if ever needed. Recording robustness: `push_au` bounds each input appsrc at 4 MiB with drop-until-IRAP resync (appsrc `block=false` never fails a push; an unbounded input queue OOMs).

## Use

```
gstreamer/scripts/build-prefix.sh   # host: build rootfs/build/gst-sd/gst-sd.sqsh (~150 MB)
gstreamer/scripts/deploy.sh         # host: bootstrap SD modules over SSH, push image, loop-mount
# then on the goggle (or via ssh):
/mnt/gst/missinglynk/bin/gst-display-up.sh      # load DRM display + wave5 codec module stacks
/mnt/gst/missinglynk/bin/splash-up.sh           # boot splash (vendor mountain) on the primary (optional)
/mnt/gst/missinglynk/bin/gst-rf-up.sh           # default runtime: RF tiles -> composite + OSD -> display
# to play a file instead of RF (decode->display validation): ml-pipeline <file.h264|file.h265>
```

## Production build: standalone static binary (rootfs track)

For the shipping image there is no SD card and no `/mnt/gst`: `ml-pipeline` is a single fully-static binary with GStreamer linked in. `build-static.sh` (host, docker) does it; `build-static-container.sh` is the in-container half (vars passed via `-e`).

```
gstreamer/scripts/build-static.sh          # -> gstreamer/build/static/ml-pipeline (~6.6 MB, static-pie, zero deps)
REBUILD=1 gstreamer/scripts/build-static.sh   # force a clean gst rebuild (after changing the plugin set)
```

How it is slim: the SD squashfs is 156 MB not from plugin code (all 238 plugins = 27 MB) but from the stock package set's 370 MB transitive lib closure (Mesa/LLVM via the GL stack, flite, x265/aom/vpx, GTK) - none of which our pipeline touches. The static build compiles **only the 9 plugins we use** (`coreelements`, `app`, `videoconvertscale`, `videorate`, `typefindfunctions`, `video4linux2`, `isomp4`, `videoparsersbad`, `kms`) into `libgstreamer-full-1.0` via meson's `gst-full`, registered at `gst_init` - so **no plugin directory and no registry scan** (retires the ~19 s cold-scan entirely). Built `--buildtype=release -Dgstreamer:gst_debug=false` and stripped. `libdrm` is the only static `.a` Alpine does not package, so it is built from source into the prefix.

`rootfs/build.sh` stages this binary -> `/usr/local/bin/ml-pipeline` (plus `ml-linkd`; `ml-drmfd`/`ml-hud`/modules/codec-fw are already there), launched by `/usr/local/bin/ml-video-up.sh` (no `gst-env`, no registry). Flash the **`slim`** rootfs flavor for production (`FLAVOR=slim rootfs/build.sh`; the `dev` flavor's debug tools overflow the 45 MiB partition). See `plans/gst-static-rootfs.md`.

## The two-process pipeline/HUD split (`src/`)

`src/` holds the implementation of `plans/done/gst-two-process-hud.md` (built by `src/build.sh` in an aarch64 Alpine 3.24 container, staged into the squashfs by `build-prefix.sh`):

- **`ml-drmfd`**: opens `/dev/dri/card0` once and hands the fd to every client via `SCM_RIGHTS` over `/run/missinglynk/drm.sock`. All clients share one open file description = all are DRM master on their own plane (DRM leases can't share a CRTC; see the plan). Clients must use blocking legacy `SetPlane` only - no DRM events on the shared fd. The broker anchors master; restart it only when its clients are down.
- **`ml-pipeline`**: file (later: RF UDP) -> parse -> wave5 V4L2 decode -> `kmssink fd=N plane-id=33` zero-copy, plus one `MLM_T_FRAMESTATS` datagram per decoded frame from a decoder pad probe (the future SEI tap's emission point). Never blocks on a consumer (`MSG_DONTWAIT`, drop on error). Clean NULL teardown on SIGINT/SIGTERM so the VPU survives restarts.
- **`ml-hud`**: framecount HUD on overlay plane 38 (ARGB4444 dumb buffer, straight alpha, stb_truetype text) consuming the telemetry socket; the seed of the libre menu's DRM overlay backend. Binds `/run/missinglynk/telemetry.sock` (consumer binds; one consumer at a time).
- **`ml-mdump`**: reference consumer, prints records; stands in for the HUD when debugging the seam.
- **`ml-splash`**: one-shot boot splash - paints the vendor mountain (`nosignal.yuv`, raw I420 1920x1080, staged into `missinglynk/share/` from your own dump, git-ignored) on the DRM primary via the broker fd and exits; the frame persists on the broker's fd and later video/OSD planes cover it, exactly stock's no-signal behavior. Raw DRM ioctls, fully static - it also ships ON the rootfs together with `ml-drmfd` and runs at boot via the `ml-display` OpenRC service (`rootfs/`), which makes the boot-started broker the session anchor. Manual runner: `runtime/splash-up.sh` (skips an already-running broker).
- **`ml-shared/mlm.h`** (top level, moved from `src/`): the cross-component wire contract (record framing, socket paths, fd passing) - also used by the top-level `ml-linkd/` RF link daemon, which `build-prefix.sh` builds and stages into the squashfs bin dir alongside these tools.

`deploy.sh` is re-runnable (checksum-skips an unchanged image) and handles a virgin boot: it pushes and loads `artosyn_gpio`/`dw_mci-artosyn` over SSH first (the SD controller is modular and the rootfs ships no /lib/modules - the image with the module is ON the card, hence the SSH bootstrap; the mmc nodes themselves are in the boot DTB).

## RF mode: two-tile decode + display (`ml-pipeline rf`, 60 fps end-to-end validated 2026-07-07)

`ml-pipeline rf` binds UDP `:10001`, deframes the 36 B `video_packet_header`, demuxes the two H.265 tiles (c0 1920x560 top / c1 1920x552 bottom, 32-row overlap), decodes each with its own wave5 instance (zero-copy dmabuf capture), pairs decoded tiles by PTS (= FrameId/60), and displays through a custom DRM sink (kmssink replaced: its skip-vsync release freed buffers mid-scanout). Sustained 60 fps, evict=0, validated with looped synthetic and real-capture replays AND live over RF (2026-07-07: the full gated chain - `rf-bringup` -> `ml-drmfd` -> `ml-pipeline rf` -> `ml-linkd` - joined the air unit's session at FrameId 0 and held 60 fps plane-scanout on the panel). There are TWO display modes:

- **Plane scanout (default) - the low-latency mode.** Both tiles scan out DIRECTLY on the DC's video0/video1 overlay planes (one atomic commit per pair; both banks latch on the same 0x1518 bit3 shadow bracket, so the row-528 seam is tear-free by hardware). No composite buffer exists: zero blits, ~2.5 ms less display latency per frame, ~7.9% total CPU, 47 MB reserved heap free, ~370 MB/s less DDR traffic. The cost: the DC has exactly 2 overlay banks, so NOTHING can blend over the video - no HUD, no OSD (fb0 writes still land, they are just hidden under the opaque tiles). This is the mode for minimum glass-to-glass latency.
- **Composite (`ML_COMPOSE=1`) - the HUD-capable mode, and the vendor's own shape.** Both tiles are DMA-blitted (ml_dmablit.ko, dw-axi-dmac) into one 1920x1080 I420 composite (16-buffer reserved-heap pool) flipped on the primary; overlay bank 1 stays free for the HUD plane (ARGB4444, per-pixel source-over in hardware - exactly how stock composites its fb0 OSD over video). Costs vs plane mode: +~3.4% total CPU, +47 MB heap, +~2.5 ms latency, DMA engine busy. Still a solid 60 fps.

Mode choice is per pipeline start; the natural product shape is composite while HUD/menu matters, plane scanout when latency wins. Switching modes = stop the pipeline, WAIT for full process exit, start the new one - overlapping teardown with a fresh start hard-hung the SoC once (2026-07-07; two guards now exist: plane-mode shutdown latches the planes off before decoder teardown, and the wave5 close-path VPU safety reset fires only for the last live instance - but do not tempt it).

Architecture facts (each one paid for in bricked runs - do not regress them):
- **Never flush a live wave5 decoder** (permanent `result not ready: 0x800` stall) and never feed through a session boundary (PTS-to-content desync). A FrameId regression (= new air session) is absorbed by PTS EPOCH CONTINUATION (the epoch advances so decoder PTS stays monotonic; no teardown, no re-staggered skew); the full decode rebuild survives in `rf_do_rebuild` as the fallback for real parameter changes.
- **Pairing = vendor fusion semantics, skew-tolerant**: PTS-keyed slot table; a frame displays only when BOTH halves land; oldest incomplete evicted when full; never a half-stale or half-black frame on the panel.
- **Retire-after-scanout, one flip late**: the sink frees a displayed buffer only at its GRANDPARENT's page-flip event - artosyn_vo arms the event on the software vblank counter while the DC latches at the hardware frame edge, so events can complete one frame early.
- **Zero-copy needs the GstVideoMeta allocation probe**: without advertising `GST_VIDEO_META_API_TYPE` on the appsink allocation query, v4l2videodec silently normalize-copies the non-16-aligned tile (552 rows) to system memory. With the probe, wave5 meta reports ALIGNED-height plane offsets (chroma at 1920x560 even for the 552 tile) and scans correctly; caps-height packing applies only to meta-less buffers.
- **Lockstep flow control at the rx side**: tile-0 AUs are held whenever dec0 runs ahead beyond the adaptive skew/inflight gate (derived from the backing pool, hardcoded bounds silently broke every time the pool moved); tile-1 always flows; startup staggers tile-1's first AUs.
- **appsrc leaky-type MUST be 0**: dropping queued compressed AUs breaks the H.265 reference chain and paints accumulating garbage until the next IDR.

Testbed (no RF hardware needed): `glue/capture/make-synth-session.py` renders a testsrc2 clip with a huge frame counter ACROSS the tile seam, tiles + encodes it FPV-shape, and wraps bit-exact RF datagrams into a `.rfdump`; `glue/capture/rf-replay.py --dump` does the same from a real pcap. The static `glue/capture/ml-rf-replay.c` plays a `.rfdump` ON the goggle to `127.0.0.1:10001` (`ip link set lo up` first - slot B boots with lo down; host->goggle UDP over the USB gadget wedges it). `--loop` wraps FrameId and exercises the session-restart path every pass.

## Playback: a recording preempts the live stream (`mlp-playback.c`)

Selecting a clip in the HUD's Playback list sends `MLM_CMD_PLAY <path>` on `ctrl.sock`; the pipeline preempts the live RF feed and plays the file, returning to live on stop or end. Because only one wave5 decode graph may be live, PLAY tears the RF graph down to NULL and re-inits the display sink for single-stream scanout (`qtdemux ! h264parse ! v4l2h264dec` for the DVR's H.264/MP4, a direct parser for a raw elementary stream), then decodes the file and scans each frame out on the primary CRTC via the same custom DRM sink. The graph swap parks the CRTC on a persistent idle FB first, so tearing a graph down never leaves the DC fetching a freed framebuffer (that powers the panel off - the swap wedge). The playback appsink needs the same GstVideoMeta allocation probe as RF mode, or the decoder normalize-copies to system memory and the zero-copy scanout drops every frame.

Transport, all over `ctrl.sock` (`MLM_CMD_PAUSE/RESUME/STOP/SPEED`):
- **Speed** is a single rate seek (no periodic seeking), with trickmode key-units above 1x so wave5 only emits keyframes and never falls behind at 4x/8x. Forward (2/4/8x) is validated; **reverse is unproven on wave5** (reverse trickmode) - the fallback is periodic backward seeks. A live wave5 flush-seek is a known stall risk, so `MLM_CMD_SEEK` (position seek, `playback_seek`) exists but is exercised only by the `ml-play` CLI, not the HUD.
- **End of clip** holds the last frame (the graph *pauses*, it is not torn down, so the last sample stays pinned and lit) and publishes `MLM_STATE_F_ENDED`; the HUD then replays or exits.
- **First-frame signal**: the first decoded frame submitted to the display sets `MLM_STATE_F_RENDERING`, which the HUD waits on to drop its loading spinner and reveal the transport bar - so the ~1 s decoder warm-up hides behind the menu instead of showing the idle splash.

State (mode + paused/ended/rendering flags + position/duration) is published to the HUD as `MLM_T_STATE` at ~5 Hz for the scrubber. Duration for the scrubber is the HUD's own MP4-header parse, not the pipeline's live duration (which grows for fragmented recordings).

## Platform facts baked into the scripts (details in the script comments)

- `kmssink` needs `driver-name=artosyn-vo plane-id=33`: its auto-probe knows neither our driver name nor that plane 0 is the scanout primary (its auto-pick grabs the ARGB4444 overlay and fails caps negotiation).
- I420 through generic DRM clients needs the `artosyn_vo` 64-px dumb-pitch fix (in `kernel/overlay/.../artosyn_vo.c`): the DC hardcodes the chroma stride to width/2 while clients derive it as luma-pitch/2. Same fix makes wave5 decoder buffers (1920/960 strides) import 1:1.
- The wave5 decoder needs the bitstream-sizeimage clamp + allocation instrumentation (patches `kernel/patches/0009-wave5-vpu-dec.patch`, `0006-wave5-vdi.patch`): GStreamer's 17.7 MiB worst-case bitstream request otherwise explodes into >100 MiB of codec-pool demand (power-of-2 rounding per allocation) and ENOMEMs.
- The VPU only initializes on a cold power-on: a wedged/killed decode instance or a wave5.ko swap needs a power cycle, not rmmod/insmod (fails -16).
- Keep `kmssink` as the sink when benchmarking: only it negotiates `memory:DMABuf` caps; with `fakesink` GStreamer silently falls back to copy-at-threshold out of uncached memory (~21 fps) and you measure the wrong thing.

Test clips live at `/mnt/sdcard/missinglynk/` (`test-1080p60-ref1.h264` = testsrc2, `ref=1` low-DPB, the FPV-realistic shape; `test-1080p60.h265` likewise). Regenerate with `./scripts/make-test-clips.sh` (host, needs ffmpeg; outputs to `rootfs/build/gst-sd/`), then push to the SD card.
