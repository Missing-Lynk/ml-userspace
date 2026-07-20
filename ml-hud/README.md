# hud

missinglynk's on-screen stack for the **open kernel** (the 6.18 slot-B system): it owns the **menu**, the **BTFL OSD** (the flight controller's Betaflight/INAV MSP DisplayPort OSD), and the **System OSD** (the goggle-drawn status bar). All three are implemented and hardware-validated; the rootfs ships the binary as `/usr/local/bin/ml-hud` under the openrc `ml-hud` service.

## States

The HUD has exactly two states (`src/hud_state.h`):

- **Menu closed** (the flying view): the System OSD and the BTFL OSD are drawn over the live video. The record button toggles DVR recording (via ml-pipeline) in this state too.
- **Menu open**: the settings menu is up; the BTFL OSD is hidden behind it (matching the vendor firmware's behaviour). The System OSD bar stays, on a solid background.

## The OSD channel (ml-linkd's mlm seams)

The BTFL OSD and the System-OSD's air-unit telemetry originate from the air unit on **UDP `:10000`** over the `sdio0` IP tunnel (video is a separate port, `:10001`, and is not in this stack). The frame format is in `re/notes/rf-telemetry-sdio0-10000.md`; the HUD's copy of the layout is `src/channel/osd_proto.h`.

The HUD does **not** bind `:10000` - **ml-linkd owns that port** (it runs the MEDIA_PARAMS handshake on it, and UDP delivers each datagram to exactly one socket). ml-linkd republishes the frames over the mlm seams (`ml-shared/mlm.h`), and the HUD consumes those: it binds `osd.sock` for the `MLM_T_MSP` records (payload = the raw `0x10` MSP canvas frame) and `telemetry.sock` (via `services/linkstate`) for the `MLM_T_STATUS` records (the raw `0x09`/`0x11` status frames), all fed into one dispatch - canvases to the BTFL renderer, status decoded to telemetry. linkstate also derives air-unit presence (gating the menu's Air Unit section) and tracks ml-pipeline's reported mode (the REC indicator). **The same seams serve the live radio and the bench replay**: `osd-replay` publishes a captured pcap onto `osd.sock`/`telemetry.sock` at the exact captured timing, a drop-in for ml-linkd, so the HUD sees byte-identical records to the real device through its production code path with no hardware in the loop.

## Rendering

The OSD is a 53×20 glyph grid. The renderer keeps that grid and, each frame, **redraws only the cells that changed** (usually just the voltage digits) and reports their rectangles, so the display backend rewrites **only those pixels** - never the whole plane, which is shared. This is what keeps CPU at ~0% (a full-frame redraw pegged a core).

The HUD only draws its **own transparent overlay** - just the opaque glyph pixels, alpha-0 everywhere else. The background it composites over (the mountain "no-signal" splash, or the live video) is put on the primary plane by the **`ml-display` startup service** (broker + splash) or the video pipeline, not by the HUD.

The overlay plane is shared by three tenants that never overlap: the System OSD owns the bottom strip (`OSD_BAR_HEIGHT`), and the BTFL OSD grid and the menu are both mapped into the region above it.

## Display backend (device)

On the open kernel the vendor `/dev/fb0` (arfb) is **vestigial** - no backing driver, it does not scan out. The panel is driven by DRM (`artosyn_vo`), and the surface that composites over live video is the **ARGB4444 DRM overlay plane** (plane 38, `--drm` to select another). The HUD gets the shared DRM-master fd from the **`ml-drmfd` broker** (`ml-shared/mlm.h`, `SETPLANE` on the overlay). The CRTC must already be modeset by another client (the video pipeline, or `ml-splash`).

## Runtime paths

Settings persist in `/usrdata/hud/settings.json` (`--settings` to override). The BTFL glyph font is the generated repo asset `assets/osd-fonts/font_BTFL_hd.png` (`make font` in userspace/); the binary searches the repo asset paths, `/usr/share/hud/fonts/` and `/usrdata/hud/fonts/`, overridable with `HUD_MSP_FONT` or `--btfl-font` (the rootfs `ml-hud` service passes its copy explicitly). Language catalogs are searched in `/usrdata/hud/lang`, `/run/ml/hud/lang`, then `/usr/local/share/hud/lang`.

## Build

```sh
make hud      # from userspace/ - cmake, aarch64 static -> ml-hud/build/{hud,osd-replay}
```

Or directly: `cmake -S ml-hud -B ml-hud/build -DCMAKE_TOOLCHAIN_FILE=ml-hud/cmake/aarch64-static.cmake && cmake --build ml-hud/build`. Needs `aarch64-linux-gnu-gcc`; the first build fetches LVGL, lodepng and cJSON (pinned) from GitHub.

## Run on the goggle

The rootfs runs the HUD as the `ml-hud` service (see `rootfs/skeleton/etc/init.d/ml-hud`); reflashing slot B is the production update path. For iterating without a reflash, stage to the device tmpfs:

```sh
tools/deploy.sh                       # build + stage to /run/ml/hud (wiped on reboot)
# then, on the goggle:
sh /run/ml/hud/goggle-preview.sh      # replay-fed preview, no air unit needed
```

`goggle-preview.sh` handles both device states: on the full boot stack it stops the `ml-hud` service and ml-linkd (osd-replay stands in for linkd on the seams - the two must not both publish) and reuses the running broker/modeset; on a bare goggle it stands up `ml-drmfd` and a background via `ml-splash` first. Env knobs: `DEVICE_IP`, `ROOT_PASS`, `DEST`, `NOSIGNAL_YUV` (deploy); `PLANE`, `BG` (preview). On the real device the background is the live video and the OSD flows off the radio through ml-linkd.

## Status

All hardware-validated on the goggle:

- **BTFL OSD**: decode + grid-diff render + DRM overlay + mlm-seam ingest + pcap replay (~0% CPU).
- **DVR OSD burn-in** (`dvr.record_osd`): while recording with the setting on, `osd/btfl_burn.c` diffs its own copy of the BTFL grid and sends each changed cell - rendered with the same loaded MSP font at the same `btfl_osd_cell_rect` rectangle - to ml-pipeline over `ctrl.sock` (`MLM_CMD_OSD_CELL`, `pipecmd_osd_cell`), which burns the opaque pixels into the recorded composite. Independent of the on-screen OSD state, so the recording keeps its OSD while the menu is open; only the BTFL OSD is ever sent, never the System OSD or the menu.
- **System OSD**: the bottom bar - goggle battery, SD card, temperature; visibility and fields follow their menu settings.
- **Menu**: LVGL sidebar/content menu over the overlay - brightness, buzzer volume, key tones, low-voltage alarm + threshold, language (en/zh, hot-switched catalogs + baked CJK fallback font), System-OSD toggles, DVR recordings list, link-gated Air Unit section; settings persisted and applied live.
- **Playback**: the recordings list shows each clip's length + size (length parsed straight from the MP4 `moov`/`mvhd`, or `mvex/mehd` for the DVR's fragmented captures; empty/aborted files are hidden). Picking a clip drives the gst pipeline (`pipecmd`, `ctrl.sock`): the menu stays up with a spinner over the row and navigation frozen until the pipeline signals the first frame, then the libre-style transport bar takes over - a scrubber (stable total from the header parse), CENTER pause/resume, LEFT/RIGHT a `-8..8x` speed ladder, and a stop icon at end-of-clip where CENTER replays. BACK returns to the list and the live stream.
