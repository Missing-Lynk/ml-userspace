# VDEC API (AR_MPI_VDEC_*) and the decode-to-display resolution path

Reference for the Artosyn goggle video decoder (`libmpi_vdec.so`) and how a decoded frame reaches the display. Reconstructed statically from `out/P1_GND/rootfs/usr/lib/libmpi_vdec.so`, `libldrt_pipeline.so`, `libmpi_scaler.so`, `libhal_scaler.so`, and the caller `firmware/bin/ar_lowdelay` (fw 1.0.44, Proxima-9311). The MPP API mirrors HiSilicon MPP (`HI_MPI_VDEC_*`); layout was verified against the binary, not assumed. Facts are proven-from-bytes unless marked inferred or `TODO(unverified)`.

## Headline: does VDEC upscale? No. The ar_scaler hardware does.

VDEC decodes to the coded (native) resolution of the incoming H.264/H.265 stream. If the air unit sends 720p, VDEC produces a 720p frame; the decoder even tracks resolution changes mid-stream (`VdChn[%d] resolution sequence_changed, width(%d->%d) height(%d->%d)`). The `u32PicWidth`/`u32PicHeight` in `VDEC_CHN_ATTR` are the MAX (buffer-allocation) dimensions, not the output size, and there is no scaler call anywhere inside `libmpi_vdec.so`.

The block that turns 720p into the panel resolution is the ar_scaler hardware (`/dev/arscaler`), reached through `AR_MPI_SCALER_CropResize`. It is invoked once per frame from `libldrt_pipeline`'s `AR_LDRT_RX_FusionProcess`, which builds a source descriptor from the decoded frame and a fixed-resolution destination, resizes into the RX server frame ring, and publishes it. The MIPI-DSI panel is 1080P, so the scaler output buffer (the buffer the display controller reads) holds an already-scaled 1080p frame. The display controller does NOT scale a native-res frame at scanout; it scans out the pre-scaled fusion output.

RX datapath (proven from the per-thread call graphs in `libldrt_pipeline.so`):

- `AR_LDRT_RX_VdecFrameReceiveThread`: `select(vdec_fd)` -> `AR_MPI_VDEC_GetFrame` (coded-res frame) -> `AR_MPI_VDEC_QueryStatus` -> enqueue. No scaler, no VO here.
- `AR_LDRT_RX_FusionThread`: dequeue -> `AR_LDRT_RX_FusionProcess` [ `AR_MPI_SCALER_CropResize` -> `AR_RT_RX_FusionSendToVo` (publishes into the server ring via `FusionServerSendBuffer`) ] -> `AR_MPI_VDEC_ReleaseFrame`. This is the mandatory scale stage.
- `AR_LDRT_RX_VoSendThread`: `AR_LDRT_PIPELINE_GetFrameByClientRx` -> `AR_LDRT_RX_PushFrameToDisplay` -> `AR_MPI_VO_SendFrame`. The VO display thread consumes the same post-scale server ring as a client.

Because both the VO display path and the RTSP re-encoder (`ar_lowdelay`'s `AR_LDRT_PIPELINE_CreateClientRx -> GetFrameByClientRx -> AR_MPI_VENC_SendFrame(chn8)`, see `rtsp-reverse-engineering.md`) pull from that one post-scale ring, the RTSP feed is 1080p regardless of link mode. That constant-1080p observation is the empirical confirmation that the ring (and thus the DC buffer) holds a scaled, fixed-resolution frame.

Note: `ar_lowdelay` links `libmpi_vpss.so` and `libmpi_scaler.so` but references no VPSS or SCALER symbol in its own dynamic-symbol table, and `libldrt_pipeline.so` imports no VPSS symbol at all. So VPSS is not in the goggle RX datapath; scaling is ar_scaler only, reached indirectly through the pipeline. This differs from a stock HiSi decode->VPSS(resize)->VO chain.

Digital zoom (`AR_LDRT_RX_ZoomWithScaler` / `AR_LDRT_RX_ZoomRatio`) rides the same `CropResize` pass (`FusionProcess` computes an even-aligned crop rectangle and a scale ratio); at zoom 1.0 it is a pure resize to the display buffer.

TODO(unverified): the exact 1920x1080 destination constant is loaded from the fusion/server attr struct (`AR_LDRT_RX_ServerCreate` + `UAV_COMMON_AllocScaleBuffer`), not an inline immediate, so it was not isolated to a single instruction; 1080p is taken from the RTSP capture plus the `1080P` / `mipi_dsi_lcd1` panel strings in `ar_lowdelay`. An on-device confirmation would be to feed a known-720p link and read `AR_MPI_VDEC_GetFrame`'s returned width/height (expect 1280x720) while the RTSP/VO output stays 1920x1080.

## Call list (AR_MPI_VDEC_*)

All are thin MPI wrappers over the single `_IOWR('A',15,24)` give-cmd envelope on `/dev/vdec%d`. Channel index range is 0..0x27 (40 channels).

- `AR_MPI_VDEC_Init` / `AR_MPI_VDEC_DeInit` - module init/teardown (channel count).
- `AR_MPI_VDEC_CreateChn(VdChn, VDEC_CHN_ATTR*)` - create a decode channel; give-cmd create_inst (attr 0x58 bytes).
- `AR_MPI_VDEC_DestroyChn(VdChn)` - destroy channel.
- `AR_MPI_VDEC_GetChnAttr` / `AR_MPI_VDEC_SetChnAttr(VdChn, VDEC_CHN_ATTR*)` - get/set attr (0x58).
- `AR_MPI_VDEC_SetChnParam` / `AR_MPI_VDEC_GetChnParam` - per-channel decode params (0x34).
- `AR_MPI_VDEC_SetProtocolParam` / `AR_MPI_VDEC_GetProtocolParam` - codec protocol params.
- `AR_MPI_VDEC_StartRecvStream` / `AR_MPI_VDEC_StopRecvStream(VdChn)` - start/stop decoding.
- `AR_MPI_VDEC_QueryStatus(VdChn, VDEC_CHN_STATUS*)` - status (0x70): counters (`in/rel/left/ready/skip/dis/err`).
- `AR_MPI_VDEC_GetFd(VdChn)` / `AR_MPI_VDEC_CloseFd(VdChn)` - the `select()`-able channel fd.
- `AR_MPI_VDEC_ResetChn(VdChn)` - reset channel.
- `AR_MPI_VDEC_GetDecInfo` - decoded-stream info (parsed SPS/PPS dims).
- `AR_MPI_VDEC_SendStream(VdChn, VDEC_STREAM*, milliSec)` - feed a coded-bitstream chunk (give-cmd send_stream 0x30).
- `AR_MPI_VDEC_GetFrame(VdChn, VIDEO_FRAME_INFO*, milliSec)` - dequeue one decoded frame (give-cmd get_frame 0x120). Blocks / `select`-able.
- `AR_MPI_VDEC_ReleaseFrame(VdChn, VIDEO_FRAME_INFO*)` - return a frame to the pool (0x120).
- `AR_MPI_VDEC_GetUserData` / `AR_MPI_VDEC_ReleaseUserData` - SEI / user-data side channel (0x30).
- `AR_MPI_VDEC_SetUserPic` / `AR_MPI_VDEC_EnableUserPic` / `AR_MPI_VDEC_DisableUserPic` - static "no-signal" picture.
- `AR_MPI_VDEC_SetDisplayMode` / `AR_MPI_VDEC_GetDisplayMode` - preview vs playback order.
- `AR_MPI_VDEC_SetRotation` / `AR_MPI_VDEC_GetRotation` - output rotation.
- `AR_MPI_VDEC_AttachVbPool(VdChn, VB_POOL)` / `AR_MPI_VDEC_DetachVbPool(VdChn)` - bind frame-buffer pool (attach 0x08).
- `AR_MPI_VDEC_SetModParam` / `AR_MPI_VDEC_GetModParam` - module params (`H26xClock(%dMHz %dMHz) JpegClock(%dMHz) VBSource=%d, MiniBuf=%d, Parallel=%d`).

(`libmpi_vdec.so` also exports a full `h264_*` / `h265_*` / `sei_*` bitstream parser used to sniff coded dims and SEI before handing to the hardware VPU.)

## VDEC_CHN_ATTR (88 bytes = 0x58; == the create_inst / set_attr / get_attr give-cmd size)

Field names are taken verbatim from the CreateChn log format string and the offsets from the load/store order in `AR_MPI_VDEC_CreateChn`.

| off | type | field | notes |
|----|------|-------|-------|
| +0  | u32 | enType | PAYLOAD_TYPE: JPEG=26 (0x1a), H264=96 (0x60), H265=265 (0x109), MJPEG=1002 (0x3ea). Any other -> error 0x80058403. |
| +4  | u32 | enVBSource | VB pool source mode. |
| +8  | u32 | enMode | VIDEO_MODE (stream vs frame decode). |
| +12 | u32 | u32PicWidth | MAX decode width (buffer allocation cap), not the output width. |
| +16 | u32 | u32PicHeight | MAX decode height, not the output height. |
| +20 | u32 | u32StreamBufSize | bitstream ring size; validated to be > 0x8000 (32 KiB) else "streamSize too small". |
| +24 | u32 | u32FrameBufSize | frame-buffer size (fbSize). |
| +28 | u32 | u32FrameBufCnt | frame-buffer count (fbCnt). |
| +32 | u32 | u32RefFrameNum | reference-frame count (refNum). |
| +36 | u32 | bTemporalMvpEnable | temporal MVP enable. |
| +40 | u32 | u32TmvBufSize | temporal-MV buffer size. |
| +44 | u32 | u32CmdQueueDepth | command-queue depth. |
| +48..+87 | - | reserved / codec union | `TODO(unverified)`: 40 bytes not read by CreateChn (JPEG-vs-video attr union expected). |

## VIDEO_FRAME_INFO returned by GetFrame (0x120-byte give-cmd payload)

The MPI-level struct the caller passes is larger (writes reach +444); the give-cmd copies a 0x120 = 288-byte subset. Field names come from the GetFrame log `VdChn[%d] stride = %d, width = %d, height = %d, Y(%#llx, %u) U(%#llx, %u) V(%#llx, %u)`. Proven offsets into the caller struct:

| off | type | field | notes |
|----|------|-------|-------|
| +4   | u32 | u32Width | decoded (coded) width. This is the key resolution evidence: it equals the stream size, not a configured size. |
| +8   | u32 | u32Height | decoded (coded) height. |
| +12  | u32 | videoFormat/field | set to 4 on the H264/H265 paths; `TODO(unverified)` (VIDEO_FIELD or compress-mode). |
| +16  | u32 | enPixelFormat | see enum below. |
| +48/+52/+56 | u32 | chroma/plane dims | luma-h and luma-h/2 pair (420) or luma-h (non-420). |
| +136 | u64 | phys (aux) | |
| +144 | u64 | u64PhyAddr[0] | Y plane physical address. |
| +152 | u64 | u64PhyAddr[1] | U plane = Y + u32Stride[0]. |
| +160 | u64 | u64PhyAddr[2] | V plane = U + u32Stride[1]. |
| +356 | u32 | u32Stride[0] | luma stride. |
| +360 | u32 | u32Stride[1] | chroma stride. |
| +364 | u32 | u32Stride[2] | chroma stride. |

Frame buffers are MMZ physical addresses; the CPU reads them via `ar_hal_sys_mmap_cache` (seen at 0xa90c), never a codec-register mmap.

## Output PIXEL_FORMAT

`enPixelFormat` (VIDEO_FRAME_INFO +16) is set per decode mode in GetFrame:

- 0x16 (22): chroma height = luma/2 -> YUV420 semi-planar family.
- 0x17 (23): chroma height = luma/2 -> YUV420 semi-planar family.
- 0x18 (24): chroma height = luma -> non-420 (422-family) variant.
- 0x3c (60): fallback path.

For H.264/H.265 decode the output is YUV420 semi-planar (the NV12/NV21 family): a 3-plane descriptor with U = Y + stride and V = U + stride, where the packed semi-planar case keeps chroma in the U plane. `TODO(unverified)`: which of 0x16/0x17 is NV12 vs NV21, and the exact meaning of 0x18/0x3c, are not isolated from the bytes; confirm on-device by reading a decoded frame's `enPixelFormat` and dumping the Y/UV planes.

## Give-cmd size reconciliation

The VDEC give-cmd table (create_inst 0x58, set/get attr 0x58, set/get param 0x34, send_stream 0x30, get_frame/release_frame 0x120, query_status 0x70) is consistent with the struct sizes here: `VDEC_CHN_ATTR` = 0x58 = 88 bytes, and the GetFrame/ReleaseFrame payload = 0x120 = 288 bytes.
