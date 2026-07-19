# VENC video-encoder userspace API

Reference for the Artosyn Proxima-9311 MPP video-encoder API (`AR_MPI_VENC_*` in `libmpi_venc.so`), the encoder channel-attribute / rate-control / GOP structures, the accepted SendFrame input pixel formats, and the concrete encoder configuration `ar_lowdelay` programs for the RTSP (venc8, H.265) and DVR (venc4, H.264) channels. Reconstructed statically from `firmware/bin/libmpi_venc.so` (fw 1.0.44.rel) and `firmware/bin/ar_lowdelay` with objdump/readelf/nm. Nothing here is validated on hardware. Field offsets marked "proven" are read directly from the binary (the print-attr function and/or ar_lowdelay's struct fill); everything else is `TODO(unverified)`. The API mirrors HiSilicon MPP (`HI_MPI_VENC_*`); the HiSi SDK was used only as a naming hint and every layout below is verified against the bytes.

## Open V4L2 wave5 encoder - hardware-validated capabilities

Everything below this section is the vendor MPP blob API. THIS section is the open path we actually build DVR on: the mainline Chips&Media wave5 V4L2 M2M encoder (`kernel/patches/0010-wave5-vpu-enc.patch`), same `wave5.ko` as the decoder, exposed as `/dev/video1` (`v4l2h264enc` / `v4l2h265enc`). Measured on the device (BetaFPV VR04, open kernel 6.18.36).

### Formats

**Output codecs** (the compressed CAPTURE side, `SRC` pad):

| Codec | GStreamer element | V4L2 pixfmt | Status |
|-------|-------------------|-------------|--------|
| H.264 | `v4l2h264enc` | `V4L2_PIX_FMT_H264` | enumerates; validated end-to-end (playable MP4). DVR default (matches vendor DVR + `scripts/dvr-frame.sh`) |
| H.265 / HEVC | `v4l2h265enc` | `V4L2_PIX_FMT_HEVC` | enumerates (same codec used by the vendor RTSP path) |

JPEG/MJPEG are CODA9, a different IP - not exposed by this wave5 driver.

**Input pixel formats** (the raw OUTPUT side; from `gst-inspect-1.0 v4l2h264enc` SINK caps + `wave5-vpu-enc.c` `enc_fmt_list`). Two feed paths accept the same set:
- system memory (`video/x-raw`) - CPU-filled, copied into the encoder buffer;
- DMABuf import (`video/x-raw(memory:DMABuf)`, `format=DMA_DRM`) - zero-copy, what the DVR uses (the composite dma-buf).

| Format (FourCC) | Chroma | Layout | Notes |
|-----------------|--------|--------|-------|
| `I420` / `YU12` | 4:2:0 | planar Y,U,V | the composite's format - fed to the DVR encoder directly |
| `NV12` | 4:2:0 | semiplanar Y,UV | |
| `NV21` | 4:2:0 | semiplanar Y,VU | |
| `Y42B` / `YU16` | 4:2:2 | planar | |
| `NV16` | 4:2:2 | semiplanar Y,UV | |
| `NV61` | 4:2:2 | semiplanar Y,VU | |

Each also has a multiplanar `*M` variant (`YUV420M`, `NV12M`, ...) in the driver's format list. 4:2:0 and 4:2:2 only; no 10-bit, no 4:4:4.

**Resolution / frame rate.** Caps advertise `width/height [1, 32768]` and `framerate [0/1, ...]` (generic V4L2 ranges, not the silicon ceiling). Validated at 1920x1080 and 1280x720. The encoder does NOT scale or change frame rate itself (pure encoder); both are done upstream. The recording format is selected from the HUD (`dvr.resolution`, latched into `ml-pipeline` via `MLM_CMD_DVR_RES` and applied at the next recording start); `ML_DVR_RES` / `ML_DVR_FPS` override it for bench work, `ML_DVR_NO_HWSCALE=1` forces the CPU scale path.

| Profile | Downscale | Frame rate | Zero-copy? | Notes |
|---------|-----------|-----------|------------|-------|
| 1080p60 | none | 60 | yes | composite dma-buf straight to encoder; the low-overhead path |
| 1080p30 | none | 30 | yes (kept frames) | `videorate` drops to 30; kept frames pass through zero-copy |
| 720p60 | `ar_scaler` 3-plane dmabuf batch | 60 | yes | ~6 ms/frame on a record-side worker thread; the scaled pool dst (a `g_rec_fmts` row's geometry) is `dmabuf-import`ed into the encoder |
| 720p30 | `ar_scaler` 3-plane dmabuf batch | 30 | yes | rate halved by a PTS gate BEFORE the scale (half the scaler work); lightest encoder load |

With `/dev/arscaler` absent (module not loaded) a menu-selected 720p records native 1080p instead (logged): the CPU `videoscale` alternative silently wedges the encoder on this hardware (PIC_RUN entered, zero bytes written, no error) and is reachable only through the explicit env levers. The HW path stays dma-buf end to end (composite in, scaled pool buffer out, `dmabuf-import` into the encoder); its dst pool is allocated at pipeline startup because steady-state CmaFree (~0.3 MiB) cannot back a record-time allocation. Wiring details: `plans/done/hw-downscaled-dvr-scaler.md`, `mlp-record.c` (`rec_hw_init` / `rec_scale_one`).
- **Throughput - the encoder is NOT the limit; the CPU feed is.** The vendor DVR records 1080p60 H.264 on this same Wave521C (`recording.md` `REC_GetAttr` = 1920x1080 @ fps 60), so the silicon does 1080p60. A file-fed gst test measured only ~38-42 fps at 1080p, but that is a HARNESS artifact: system-memory frames are memcpy'd into the encoder's V4L2 buffer every frame (~180 MB/s at 60 fps) on a **2-core** SoC. Measured 42 fps at only ~131% of 200% CPU with NV12 too - a serialized-copy bottleneck, not compute saturation and not the codec. The vendor avoids it by feeding a zero-copy VB pool; the DVR must likewise feed the encoder the **composite dma-buf zero-copy** (`v4l2h264enc output-io-mode=dmabuf-import`, the display's own buffer), no CPU copy. True zero-copy 1080p60 is expected but not yet measured on the open path (blocked by the CMA item below). 720p file-fed already hit ~79 fps. The load-reduction levers (720p downscale, 30 fps) remain useful margin, especially concurrent with two decoders, but 1080p60 is not ruled out. The v4l2 encoder does not scale - downscale happens before it (CPU `videoscale` or the `ar_scaler` HW path); framerate halving = feed every Nth frame.
- **Memory - the encoder's CMA appetite (two over-allocations, both now addressed).** The encoder's v4l2 vb2 pools come from **CMA** (`CmaTotal 64 MiB`, carved from only 148 MiB System RAM; the wave5 decoder's frames use the separate 108 MiB `no-map` mmz pool at `0x29400000`, which vb2 MMAP buffers cannot). Two over-allocations blew CMA:
  1. the raw SOURCE (input) pool - ~60 MiB standalone (`CmaFree 3.6 MB`). FIXED by feeding the encoder via **DMABuf import** (`output-io-mode=dmabuf-import`): it imports the composite dma-buf and allocates no source pool. Validated (the record bin started and pushed 128 composites, 0 dropped).
  2. the coded (CAPTURE) pool - GStreamer negotiates a worst-case ~17 MiB coded `sizeimage`; stacked on the two tile decoders' capture pools it still exhausted CMA (`CmaFree 576 kB`) and wedged the VPU (`0x800`, 0-byte file). FIXED by clamping the coded `sizeimage` to 1 MiB in `wave5-vpu-enc.c` (`wave5_vpu_enc_try_fmt_cap` + `s_fmt_out`), mirroring the decoder's `wave5-vpu-dec.c` clamp. Validation pending (needs a power cycle to coldplug the clamped wave5.ko).
  A `CONFIG_CMA_SIZE_MBYTES` bump cannot substitute - only 148 MiB total RAM - so the import (not allocating the big pool) is the real fix.
- **Framebuffer/OSD onto the recording:** not an encoder capability - the encoder encodes whatever buffer it is handed. Burning the FB/OSD into the file is a pre-encode composite (blend the OSD over the frame before `v4l2h264enc`). The hardware overlay blend used for the live panel happens at scanout and never touches the encoded buffer, so the record path must do its own blend.

## Layering and transport

`libmpi_venc.so` (`AR_MPI_VENC_*`, the public API) validates arguments, prints the attr, then calls into `libhal_venc.so` (`ar_hal_venc_*`), which issues the give-cmd ioctl on `/dev/venc%d`. Each MPI call maps to one give-cmd `cmd_code` (create_dev, create_inst, set_attr, send_frame, get_stream, set_rc_sparam, etc.); the sizes in the give-cmd table reconcile with the struct sizes here (ChnAttr 0x170, SendFrame 0x98, GetStream 0xe0, sparam 0x438, dparam 0x1a0, vui 0x1a7c, jpeg/mjpeg 0xc8, roi 0x20). See `runtime-architecture.md` for the libmpi -> libhal -> mpp_service split.

## Channels

Channel index range 0..0x27 (40 channels, 0..39). Guarded in CreateChn/SetChnAttr/DestroyChn/SendFrame with `cmp chn,#0x27; b.hi` and asserted by the string `VeChn >= 0 && VeChn < 40`. Known assignments (from `rtsp-reverse-engineering.md` and ar_lowdelay): venc8 = RTSP re-encode channel (H.265), venc4 = DVR recording channel (H.264). All other indices are free; ar_lowdelay does not create them.

## AR_MPI_VENC_* call list

Lifecycle and device:
- `AR_MPI_VENC_Init` / `AR_MPI_VENC_Exit`: module init/teardown (give-cmd module_init 0xf00a / module_exit 0xf00b).
- `AR_MPI_VENC_CreateChn` / `AR_MPI_VENC_DestroyChn`: create/destroy an encoder channel from a VENC_CHN_ATTR_S (create_dev + create_inst). CreateChn also seeds a default VUI.
- `AR_MPI_VENC_ResetChn`: reset a channel (reset_dev 0xf00c).
- `AR_MPI_VENC_Suspend` / `AR_MPI_VENC_Resume`: suspend/resume encoding (suspend 0xf008).
- `AR_MPI_VENC_SetModParam` / `AR_MPI_VENC_GetModParam`: module-wide parameters (set_mod_param 0xc003 / 0xc004, 0x38 bytes).
- `AR_MPI_VENC_GetFd` / `AR_MPI_VENC_CloseFd`: expose/close the channel fd (for select on decoded readiness; VENC itself blocks in GetStream).

Attributes and frame flow:
- `AR_MPI_VENC_SetChnAttr` / `AR_MPI_VENC_GetChnAttr`: set/get the full VENC_CHN_ATTR_S (set_attr 0x000b / get_attr 0x8001, 0x170 bytes).
- `AR_MPI_VENC_SetChnParam` / `AR_MPI_VENC_GetChnParam`: per-channel runtime param (mod_param family; 0x38 bytes).
- `AR_MPI_VENC_StartRecvFrame` / `AR_MPI_VENC_StopRecvFrame`: arm/stop frame reception; StartRecvFrame takes `{s32RecvPicNum}` (-1 = unlimited).
- `AR_MPI_VENC_SendFrame` / `AR_MPI_VENC_SendFrameEx`: submit one input VIDEO_FRAME to encode (send_frame 0x000c, 0x98=152 bytes / send_frame_ext 0x000d, 0x108 bytes).
- `AR_MPI_VENC_GetStream` / `AR_MPI_VENC_ReleaseStream`: pull/return an encoded bitstream pack (get_stream 0x8019 / release_stream 0x801a, 0xe0=224 bytes). GetStream blocks on the codec completion IRQ.
- `AR_MPI_VENC_QueryStatus`: channel status (query_status 0x8018, 0x1c bytes).
- `AR_MPI_VENC_RequestIDR` / `AR_MPI_VENC_EnableIDR`: force / enable an IDR frame (enable_idr 0x0002).
- `AR_MPI_VENC_GetStreamBufInfo`: stream buffer descriptor (get_streambuf_info 0x8023).
- `AR_MPI_VENC_AttachVbPool` / `AR_MPI_VENC_DetachVbPool`: attach/detach a VB pool as the frame source.

Rate control:
- `AR_MPI_VENC_SetRcParam` / `AR_MPI_VENC_GetRcParam`: the RC "static" param (set_sparam 0x000e / get_sparam 0x800c, 0x438=1080 bytes): per-mode min/max I/P/B QP, min/max Iprop, QP-map enable, first-frame QP, HVS QP, CU/MB-level RC.
- `AR_MPI_VENC_SetRefParam` / `AR_MPI_VENC_GetRefParam`: reference/long-term-ref control (dparam family, 0x1a0 bytes).
- `AR_MPI_VENC_SetFrameLostStrategy` / `AR_MPI_VENC_GetFrameLostStrategy`: frame-drop policy under bitrate overrun.
- `AR_MPI_VENC_SetIntraRefresh` / `AR_MPI_VENC_GetIntraRefresh`: intra-refresh (gradual I).
- `AR_MPI_VENC_SetSSERegion` / `AR_MPI_VENC_GetSSERegion`: SSE (quality-measure) region.
- `AR_MPI_VENC_SetDeBreathEffect` / `AR_MPI_VENC_GetDeBreathEffect`: de-breathing.
- `AR_MPI_VENC_SetCuPrediction` / `AR_MPI_VENC_GetCuPrediction`: CU prediction weights.

ROI and rotation:
- `AR_MPI_VENC_SetRoiAttr` / `AR_MPI_VENC_GetRoiAttr` / `AR_MPI_VENC_SetRoiAttrEx` / `AR_MPI_VENC_GetRoiAttrEx`: ROI rectangles (set_roi 0x0007 / get_roi 0x8004, 0x20 bytes).
- `AR_MPI_VENC_SetRoiBgFrameRate` / `AR_MPI_VENC_GetRoiBgFrameRate`: ROI background frame rate.
- `AR_MPI_VENC_SetChnRotationParam` / `AR_MPI_VENC_GetChnRotationParam`: rotation (rotation 0x0019 / 0x8013, 0x08 bytes).
- `AR_MPI_VENC_SetChnMirroParam` / `AR_MPI_VENC_GetChnMirrorParam`: mirror (mirror 0x001a / 0x8014, 0x08 bytes).

H.264 tools:
- `AR_MPI_VENC_SetH264SliceSplit` / `AR_MPI_VENC_GetH264SliceSplit` (slice_split 0x0011 / 0x800a, 0x18).
- `AR_MPI_VENC_SetH264IntraPred` / `AR_MPI_VENC_GetH264IntraPred` (0x0012 / 0x800b, 0x04).
- `AR_MPI_VENC_SetH264Trans` / `AR_MPI_VENC_GetH264Trans`: transform/quant tables.
- `AR_MPI_VENC_SetH264Entropy` / `AR_MPI_VENC_GetH264Entropy` (CABAC/CAVLC; 0x0008 / 0x8005, 0x04).
- `AR_MPI_VENC_SetH264Dblk` / `AR_MPI_VENC_GetH264Dblk`: deblocking.
- `AR_MPI_VENC_SetH264Vui` / `AR_MPI_VENC_GetH264Vui`: H.264 VUI (vui 0x001b / 0x8015, 0x1a7c).

H.265 tools:
- `AR_MPI_VENC_SetH265SliceSplit` / `AR_MPI_VENC_GetH265SliceSplit` (0x18).
- `AR_MPI_VENC_SetH265PredUnit` / `AR_MPI_VENC_GetH265PredUnit`.
- `AR_MPI_VENC_SetH265Trans` / `AR_MPI_VENC_GetH265Trans`.
- `AR_MPI_VENC_SetH265Entropy` / `AR_MPI_VENC_GetH265Entropy`.
- `AR_MPI_VENC_SetH265Dblk` / `AR_MPI_VENC_GetH265Dblk` (deblock 0x0009 / 0x8006, 0x10).
- `AR_MPI_VENC_SetH265Sao` / `AR_MPI_VENC_GetH265Sao`: SAO.
- `AR_MPI_VENC_SetH265Vui` / `AR_MPI_VENC_GetH265Vui`: H.265 VUI (vui 0x001b / 0x8015, 0x1a7c).

JPEG / MJPEG:
- `AR_MPI_VENC_SetJpegParam` / `AR_MPI_VENC_GetJpegParam` (jpeg 0x001c / 0x8016, 0xc8).
- `AR_MPI_VENC_SetMjpegParam` / `AR_MPI_VENC_GetMjpegParam` (mjpeg 0x001d / 0x8017, 0xc8).
- `AR_MPI_VENC_SetJpegEncodeMode` / `AR_MPI_VENC_GetJpegEncodeMode`.

User data:
- `AR_MPI_VENC_InsertUserData`: insert an SEI/user-data NAL (insert_userData 0x8021, 0x20).

## VENC_CHN_ATTR_S (368 bytes, 0x170)

Three sub-structs: `stVencAttr` (encoder attr, 0x00..0x4c), `stRcAttr` (rate control, 0x4c..0x68), `stGopAttr` (GOP, 0x68..0x170). Size 0x170 is proven from the CreateChn `memset(0x170)` and the give-cmd create_inst/set_attr size. ar_lowdelay zeroes only the first 0x150 (336) bytes of its stack copy; the trailing 32 bytes fall inside `stGopAttr`'s reserved tail.

### stVencAttr (encoder attr), offsets 0x00..0x4c, all proven

| off | field | type | notes |
|-----|-------|------|-------|
| 0x00 | enType | u32 (PAYLOAD_TYPE_E) | H264=96, H265=265, JPEG=26, MJPEG=1002. Invalid -> 0x8008a403. |
| 0x04 | enPixelFormat | u32 | vendor label; ar_lowdelay sets 23 on the encode-input channel. Value semantics TODO(unverified). |
| 0x08 | u32MaxPicWidth | u32 | |
| 0x0c | u32MaxPicHeight | u32 | |
| 0x10 | u32BufSize | u32 | ar_lowdelay sets align16(w)*align16(h)*3/2. |
| 0x14 | u32Profile | u32 | H.264 1..6, MJPEG 7..9, H.265 10..15 accepted per codec (CreateChn); 0 = base. |
| 0x18 | bByFrame | s32 | 1 = one stream pack per frame. |
| 0x1c | u32PicWidth | u32 | |
| 0x20 | u32PicHeight | u32 | |
| 0x24 | union stAttrH264e / stAttrH265e / stAttrJpege | 0x28 bytes | selected by enType; ends at 0x4c. |

Codec union members (names from the vendor print strings; H.264e and H.265e share a layout). Offsets inside the union are `TODO(unverified)` beyond that the two byte fields sit at the end (0x48/0x49, proven by `ldrb [x19,#0x48]`): bRcnRefShareBuf, u32CmdQueueDepth, s32SubFrameSyncEnable, s32SubFrameSyncSrc, s32Cframe50Enable, s32Cframe50LosslessEnable, s32Cframe50Tx16Y, s32Cframe50Tx16C, s32Cframe50_422, u8KeyFrameSizeMultiplier (0x48), u8NonKeyFrameSizeMultiplier (0x49). ar_lowdelay's H.264 path sets CmdQueueDepth=3, KeyFrameSizeMultiplier=2, NonKeyFrameSizeMultiplier=1. Jpege union members: bSupportDCF, stMPFCfg.u8LargeThumbNailNum, ..., enReceiveMode (offsets TODO(unverified)).

### stRcAttr (rate control), offsets 0x4c..0x68

`enRcMode` at 0x4c (proven: CreateChn range-checks it and the print string reads it). The mode union starts at 0x50; the CBR member layout is proven from ar_lowdelay's fill, VBR/FixQp/MJPEG member layouts are from the vendor print strings (field order proven, exact per-field offset within the union member `TODO(unverified)` except CBR):

CBR (stH264Cbr / stH265Cbr), proven offsets:

| off | field |
|-----|-------|
| 0x50 | u32Gop |
| 0x54 | u32StatTime |
| 0x58 | u32SrcFrameRate |
| 0x5c | fr32DstFrameRate |
| 0x60 | u32BitRate (kbps) |

VBR (stH264Vbr / stH265Vbr): u32Gop, u32StatTime, u32SrcFrameRate, fr32DstFrameRate, u32MaxBitRate, u32MinBitRate (6 u32, fills 0x50..0x68; this widest member sets stRcAttr = 0x1c). FixQp (stH264FixQp / stH265FixQp): u32Gop, u32SrcFrameRate, fr32DstFrameRate, u32IQp, u32PQp, u32BQp. MJPEG: stMjpegCbr {u32StatTime, u32SrcFrameRate, fr32DstFrameRate, u32BitRate} (no Gop), stMjpegVbr {..MaxBitRate}, stMjpegFixQp {u32SrcFrameRate, fr32DstFrameRate, u32Qfactor}.

### VENC_RC_MODE_E (enRcMode values)

```
H264CBR=1   H264VBR=2   H264AVBR=3   H264QVBR=4   H264CVBR=5   H264FIXQP=6
MJPEGCBR=7  MJPEGVBR=8  MJPEGFIXQP=9
H265CBR=10  H265VBR=11  H265AVBR=12  H265QVBR=13  H265CVBR=14  H265FIXQP=15
```
Proven: the per-codec valid ranges (CreateChn) and the CBR=1/10, VBR=2/11, CVBR=5/14 pairings (ar_lowdelay shared handlers), MJPEG 7/8/9, and FIXQP as each range endpoint. `TODO(unverified)`: the AVBR-vs-QVBR order at 3/4 and 12/13 (those modes are not exercised by ar_lowdelay).

### stGopAttr (GOP), offset 0x68

`enGopMode` at 0x68; the mode union starts at 0x6c. ar_lowdelay leaves enGopMode = 0 (NormalP) and writes stNormalP.s32IPQpDelta = -2 at 0x6c. Union members (vendor print strings): stNormalP {s32IPQpDelta}; stDualP {u32SPInterval, s32SPQpDelta, s32IPQpDelta}; stSmartP / stAdvSmartP {u32BgInterval, s32BgQpDelta, s32ViQpDelta}; stBipredB {u32BFrmNum, s32BQpDelta, s32IPQpDelta}. The struct spans 0x68..0x170; the region past the small union is reserved. enGopMode numeric values (NormalP/DualP/SmartP/AdvSmartP/BipredB) other than NormalP=0 are `TODO(unverified)`.

### rcParam (SetRcParam, 0x438 = 1080 bytes)

Separate from stRcAttr; carries per-mode QP clamps. Field names from the vendor print strings (offsets `TODO(unverified)`): s32CuOrMbLevelRcEnable, s32HvsQPEnable, s32HvsQpScale, s32HvsMaxDeltaQp, s32FirstFrameStartQp, and per-mode stParamH264Cbr/H264Vbr/H265Cbr/H265Vbr {u32MinIQp, u32MaxIQp, u32MinPQp, u32MaxPQp, u32MinBQp, u32MaxBQp, u32MinIprop, u32MaxIprop, bQpMapEn}, stParamH264Fixqp/H265Fixqp {u32IQp, u32PQp, u32BQp}.

## SendFrame input pixel formats (VIDEO_FRAME enPixelFormat)

`AR_MPI_VENC_SendFrame` accepts these enPixelFormat values (proven from the switch at 0x8738); anything else logs `unknown pixel format mode: %d set I420 as default` and falls back to I420. The value is decoded into two internal fields:

| enPixelFormat | internal flag_a | internal fmt_code |
|---------------|-----------------|-------------------|
| 22 (0x16) | 0 | 1  |
| 23 (0x17) | 0 | 0  |
| 25 (0x19) | 1 | 1  |
| 26 (0x1a) | 1 | 0  |
| 29 (0x1d) | 1 | 1  |
| 31 (0x1f) | 0 | 23 |

The pairings (22/23, 25/26) plus the flag_a bit are consistent with semiplanar 4:2:0 in two chroma orders (NV12 / NV21) and planar-vs-tiled variants, but the exact PIXEL_FORMAT_E names are `TODO(unverified)`. Helper routines: `pixel_format_convert`, `hal_format_to_mpi_format`, `do calc subsample`.

## Concrete encoder configuration

### venc8, RTSP re-encode (H.265), proven from ar_lowdelay 0x43f198

`AR_LOWDELAY_RtspVencInit(chn=8, enType=265/H.265, rcMode=10/H265CBR, width, height, bitrate=5000 kbps, gop=0, framerate)`, then `StartRecvFrame(8, RecvPicNum=-1)`. StatTime = 30. GOP mode NormalP (IPQpDelta = -2). H.265 VUI enabled (SetH265Vui with two fields set to 1 and 6). A VENC mod-param is set for H.265 (a field = 16 and a packed 400/600 constant). Width/height/framerate come from the live decoded source at runtime (per `rtsp-reverse-engineering.md`: 1080p ~60 fps). GOP is passed as 0 (encoder default interval). SrcFrameRate = DstFrameRate = framerate.

### venc4, DVR recording (H.264)

Not configured through `AR_MPI_VENC_*` in ar_lowdelay; the DVR path drives `AR_MediaRecorder_*` (libmedia), which owns the venc4 channel. Codec is H.264 (per `rtsp-reverse-engineering.md`). Log line `AR_MediaRecorder_Create %uKbps@H.%u mode = %u success.` implies configurable bitrate/codec/mode. Exact venc4 bitrate/GOP/resolution are `TODO(unverified)`: they must be reversed from the MediaRecorder layer, not libmpi_venc.

## Reconstructed header

`libre/sdk/include/ar_mpi_venc.h` declares the `AR_MPI_VENC_*` prototypes and the reconstructed structs/enums, with `static_assert`/`offsetof` guards on every proven size and offset. Unproven members are declared and commented `TODO(unverified)`.
