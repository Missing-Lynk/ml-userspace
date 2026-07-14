# Decode to display pipeline and the second-process harness safety contract

How `ar_lowdelay` builds the goggle (RX) decode to display media graph, where the resize and encode taps sit, and the exact conditions under which a second process can safely exercise the encoder or decoder without wedging the running vendor stack. All findings are from static analysis of the local binaries (`out/P1_GND/rootfs/usr/lib/*.so`, `firmware/bin/ar_lowdelay`) plus live captures from the running stock stack. Toolchain: `aarch64-linux-gnu-{objdump,nm,readelf}`. Inferences are marked; nothing here was validated on device.

## 1. The concrete RX decode to display graph

The goggle media path is a set of manually pumped stages, not a bound MPP graph. `libldrt_pipeline.so` imports zero `AR_MPI_SYS_Bind`/`AR_MPI_SYS_UnBind` and zero `AR_MPI_VPSS_*` symbols (verified with `nm -D`), so there is no `SYS_Bind` datapath and no VPSS group in the RX display chain. Every stage is driven by a dedicated pump thread that pulls a frame from the previous stage and pushes it to the next.

Stage graph (goggle, RX):

RF link (`bb_socket` over `sdio0`) delivers the H.265 elementary stream plus the FC OSD in SEI. `ar_lowdelay` feeds it to a VDEC channel with `AR_MPI_VDEC_SendStream`. A VB pool is bound to the decoder with `AR_MPI_VDEC_AttachVbPool` (the decoder writes decoded frames into VB blocks, this is a pool attach, not a `SYS_Bind`). `AR_LDRT_RX_VdecFrameReceiveThread` / `AR_LDRT_RX_VdecReceiveFrameThread*` pull decoded frames with `AR_MPI_VDEC_GetFrame` and release them with `AR_MPI_VDEC_ReleaseFrame`.

Decoded frames then fan out to two independent consumers, both manual pumps:

Display consumer. The Fusion stage (`AR_LDRT_RX_FusionThread` -> `AR_LDRT_RX_FusionProcess` -> `AR_LDRT_RX_FusionDmaTransfer` / `AR_LDRT_RX_ZoomRatio` -> `AR_RT_RX_FusionSendToVo`) does the crop/resize/zoom via `AR_MPI_SCALER_CropResize` (the `/dev/arscaler` engine at phys `0x08840000`), then `AR_LDRT_RX_PushFrameToDisplay` / `AR_LDRT_RX_VoSendThread` hands the frame to the video layer with `AR_MPI_VO_SendFrame`. The VO device scans the layer out to the DSI panel (`AR_MPI_VO_Dsi_*`). The OSD/menu pixels are a separate `fb0` plane composited in hardware by the display controller, not part of this VO video layer (see `runtime-architecture.md`).

Encode consumers (the taps). `AR_LDRT_PIPELINE_CreateClientRx` registers a broadcast subscriber on the decoded-frame stream; a pump thread then loops `AR_LDRT_PIPELINE_GetFrameByClientRx` -> `AR_MPI_VENC_SendFrame` -> `AR_LDRT_PIPELINE_ReleaseFrameRx`. `venc8` is the RTSP encoder (H.265, re-encode, `rtsp-reverse-engineering.md`), `venc4` is the DVR encoder (H.264). These read the same decoded frames the display path uses; they do not sit inline with VO.

So the datapath modules are VDEC, ar_scaler (via the Fusion stage), VO for display, and VENC for the RTSP/DVR taps. Binding model: none of it is `AR_MPI_SYS_Bind`, all stages are pumped by `libldrt_pipeline` threads.

### 1.1 VPSS, VO, and scaler roles (the wiring question)

VPSS: not in the RX display datapath. `libldrt_pipeline.so` imports no `AR_MPI_VPSS_*`. A VPSS group is nonetheless created at MPP init, because `AR_MPI_SYS_Init` calls `ar_hal_vpss_create` unconditionally (disassembly of `libmpi_sys.so` `AR_MPI_SYS_Init` @ `0x1d90`), but the goggle RX chain never sends frames into it. VPSS is used by the capture side (VI -> VPSS on the air/TX unit) and is scaffolding on the goggle. TODO(unverified): whether any non-pipeline caller in `ar_lowdelay` routes a frame through VPSS on the goggle; the pipeline does not.

ar_scaler (`/dev/arscaler`, `0x08840000`): IS in the live RX datapath. The Fusion stage calls `AR_MPI_SCALER_CropResize` to resize/zoom the decoded frame before display. This is the goggle's decode-to-panel resize stage and complements the VDEC agent's upscaling analysis: the decoded picture is scaled to the panel rect by ar_scaler, not by VPSS. `libmpi_vpss.so` and `libldrt_pipeline.so` are the two `AR_MPI_SCALER_CropResize` callers.

VO layer: the VO video layer is configured with `AR_MPI_VO_SetVideoLayerAttr` / `AR_MPI_VO_EnableVideoLayer` / `AR_MPI_VO_SetChnAttr` / `AR_MPI_VO_SetLowdelayAttr`, and frames arrive by `AR_MPI_VO_SendFrame`. Whether VO applies an additional scale (layer image-size vs display rect) or composites 1:1 after the scaler has already resized is not settled statically. Given ar_scaler is explicitly used for the resize, the likely division is scaler-does-resize, VO-composites-at-rect. TODO(unverified): compare the VO layer image size against the display rect on device to confirm VO is 1:1.

### 1.2 AR_LDRT_PIPELINE client API (reconstructed)

Signatures reconstructed from `libldrt_pipeline.so` bodies plus the `ar_lowdelay` RTSP pump caller at `0x43d950` (the pump that feeds venc8). The frame object is 456 bytes (`0x1c8`, the value `ar_lowdelay` `memset`s before each `GetFrameByClientRx`).

```
// Register a broadcast subscriber on the decoded-frame stream. The stock venc8
// caller sets up no argument before the call, so the client id/name argument is
// not load-bearing in that path. Returns an opaque client handle, NULL on error
// (requires the RX pipeline to be inited: it checks g_rx_ctx[+24] first).
void *AR_LDRT_PIPELINE_CreateClientRx(void);   // TODO(unverified): whether other callers pass a name/id

// Pull one decoded frame for this client. frame_out points to a 456-byte frame
// object. timeout is in milliseconds (stock pump uses 50). Returns 0 on success,
// -1 on error, -2 on empty/timeout (queue pop timeout).
int AR_LDRT_PIPELINE_GetFrameByClientRx(void *client, void *frame_out, int timeout_ms);

// Return a frame previously obtained from GetFrameByClientRx.
int AR_LDRT_PIPELINE_ReleaseFrameRx(void *client, void *frame);

// Tear down a client created by CreateClientRx.
int AR_LDRT_PIPELINE_DestroyClientRx(void *client);
```

Internals confirmed from disassembly: `GetFrameByClientRx` (`0x1a358`) treats `client` as a struct whose first int is an index into a global client table (`g_rx_ctx` at `[x19,#3232]`), fetches the per-client queue, and calls `ar_queue_pop_timeout(queue, frame_out, timeout)`. `ReleaseFrameRx` (`0x1a6d8`) matches the frame back to that client's queue. There is a matching Tx side for the air unit (`AR_LDRT_TX_PushFrameToDisplay`, `AR_LDRT_TX_PIPELINE_*`, `AR_LDRT_TX_VencSendFrameThread`), not used on the goggle.

The stock venc8 pump (`ar_lowdelay` `0x43d950`) is exactly:

```
handle = AR_LDRT_PIPELINE_CreateClientRx();          // stored in per-venc-chn slot [+560]
prctl(PR_SET_NAME, ...);
loop:
  memset(&frame, 0, 0x1c8);
  if (AR_LDRT_PIPELINE_GetFrameByClientRx(handle, &frame, 50) == 0)
      AR_MPI_VENC_SendFrame(chn /*=[base+184], 8 for RTSP*/, &frame + 8, 100);
  // ...error/stop handling...
AR_LDRT_PIPELINE_DestroyClientRx(handle);
```

`VENC_SendFrame` receives the frame body at `&frame + 8` (the 8-byte prefix is an index/header the client layer uses), channel from the per-channel slot, 100 ms timeout.

## 2. Harness safety contract

### 2.1 MPP runs in LIBRARY mode, monolithic, not service mode

Proven, not service mode. On this firmware there is no running `mpp_service.app` daemon and clients do NOT attach over RPC.

Evidence:
- `ar_sys.ko` is loaded with `mpp_service=lib` (`etc/init.d/start_nodev_cmd.sh`), and the `mpp_service.app &` launcher (`etc/init.d/start_app.sh`) is commented out of `start.sh` and is not referenced anywhere in the product boot tree (`usr/usrdata/product/`). The product actually boots `usr/usrdata/product/start.sh` -> `ar_product` -> `run.sh` -> `ar_lowdelay`.
- Live `ps` shows only `servicemanager`, `auto_sync -gnd`, and `ar_lowdelay -m 2 -t 0`. No `mpp_service.app`.
- Live findings show `ar_lowdelay` (pid 312) itself opens `/dev/mem`, programs the media hardware, and serves the 16 major-14 CUSE nodes (`vo`, `ge2d_server`, `ar_vgs`, `ar_overlay`, `vctrl/{h26x,jpeg}`, `mpp_que`, `ai`, `ao`, `acodec`, `ar_region`, `cam_server`). It is both the hardware owner and the in-process CUSE/rpc_fs server.

So `libmpp_service.so` is linked INTO `ar_lowdelay` and runs the codec/ISP/VO/scaler in-process (the "service" is a library, hence `mpp_service=lib`). The multi-client RPC path exists in the code but is dormant: `ar_hal_sys_open`/`ioctl`/`close` (in `librpc_proxy.so`) branch on `ar_hal_sys_mpp_service_is_lib`, and only take the `rpc_proxy_*` (forward-to-daemon) branch when is_lib is 0. Here `ar_hal_sys_mpp_service_is_lib` queries the kernel (`ioctl(/dev/ar_sys, _IOR('p',29,4))`) which returns "lib", so every process, ours included, takes the DIRECT glibc `ioctl` branch and never forwards to a server.

Consequence: the intended "second client attaches to a running MPP service over RPC" model is NOT available on this stock firmware. There is no daemon to attach to, and the client libs will not RPC-forward while `mpp_service=lib`.

### 2.2 What SYS_Init and VB_Init do, and why a second-process init is the wedge

`AR_MPI_SYS_Init` (`libmpi_sys.so` @ `0x1d90`) does, in order:

```
ar_hal_sys_init();                       // per-process: open /dev/ar_sys, /dev/mmz_userdev, ...
ar_set_mpp_service_master_app(1);        // UNCONDITIONALLY claims THIS process as MPP master
if (ar_hal_sys_mpp_service_is_lib())     // true on this firmware
    mpp_service_init();                  // in-process service bring-up (see below)
ar_log_init();
ar_hal_sys_bind_init();                  // binder
ar_hal_vpss_create();                    // creates a VPSS group
ar_hal_ai_init(); ar_hal_ao_init();      // audio in/out
```

`mpp_service_init` (`libmpp_service.so` @ `0x130208`) is guarded only by a PER-PROCESS `.bss` flag (`[x19,#2912]`), not a system-wide lock, so a second process runs the full `ar_plat_init` -> `init_app_start_boot` bring-up: `rpc_fs_register_dev_with_mask` (register the CUSE nodes `vo`/`ge2d_server`/`vctrl`/...), `ar_display_init`, `ar_hal_vcodec_ctrl_dev_init`, audio, clocks, DMA, `/dev/mem` register mapping. Doing this from a second process while `ar_lowdelay` already owns those CUSE names and hardware is the wedge: duplicate CUSE registration, a second `/dev/mem` register programmer, re-init of shared clocks/display. The matching `mpp_service_deinit`/`AR_MPI_SYS_Exit` tears down display, codec, audio, and the CUSE server, so calling Exit from our process would tear down state `ar_lowdelay` depends on.

`AR_MPI_VB_Init` / `AR_MPI_VB_SetConfig` (`libmpi_vb.so`, thin thunks to `ar_hal_vb_init` / `ar_hal_vb_set_config`) configure and initialize the GLOBAL VB pool layout. `ar_lowdelay` already created the common pools (live `/proc/media-mem`: `VbPool` 18 MiB, `h26x-COMMON` 3 MiB, `UserPool` 3 MiB, `fb_mmz` 13 MiB). A second `VB_SetConfig`+`VB_Init` reconfigures/reinitializes those shared pools out from under `ar_lowdelay`. Do not call them.

### 2.3 The safe attach verdict

There is no safe way for an independent second process to open a VENC/VDEC channel against the RUNNING stock `ar_lowdelay` in this lib-mode deployment. Any path that reaches the codec needs the MPP session (`SYS_Init` -> `mpp_service_init`, plus VB pools and the register mmap), and that session is owned in-process by `ar_lowdelay`; there is no RPC front door while `mpp_service=lib`. A second `SYS_Init` claims master, re-runs the in-process service bring-up, and collides. This matches the deployment: nothing else on the goggle drives MPP hardware. `libre-menu` and `servicemanager` use binder only (live findings), and `test_uidesign` uses `fb0` + binder, none of them open a VENC/VDEC/VO channel.

The two safe ways to exercise the encoder/decoder are therefore:

Option A, in-process to `ar_lowdelay` (the proven pattern). Feed a fresh VENC channel or tap decoded frames from inside `ar_lowdelay` using the pipeline client API (section 1.2), exactly as the RTSP venc8 patch does. This shares the existing session and never re-inits MPP. It is a hook/patch, not a separate process.

Option B, sole-owner second process (only when the vendor stack is NOT running). Stop `ar_lowdelay` cleanly first (never SIGTERM it, see the memory note; use the product's own stop path or run the harness on a slot where `ar_lowdelay` was never started), then the harness is the sole master: it may call `SYS_Init` once, own the hardware, and create channels freely. This is the natural fit for the open-Alpine slot where the vendor stack is absent.

Read-only shared-kernel probes (MMZ query, VB pool stats, `ar_sys` PTS reads) are safe from any process at any time because they do not touch the MPP session; see the probe order below.

### 2.4 Free channel indices and VB pool sharing

Known-used channels on the running goggle: VENC 8 (RTSP, H.265) and VENC 4 (DVR, H.264); one live VDEC decode channel (config-driven, in `/usrdata/lowdelay/lowdelay_cfg/`, not in this dump). TODO(unverified): the exact live VDEC channel number; the single downlink stream is conventionally VDEC channel 0, so treat 0 as used.

Free indices for a sole-owner harness (Option B): VENC accepts channel indices up to 39 (`chn <= 0x27` in `ar_hal_venc_dev_open`), so pick a fresh VENC index outside {4, 8} (for example 0-3, 5-7, 9+). For VDEC pick an index other than the live decode channel (avoid 0). These indices only matter when the harness owns the session; against the running stock stack, no index is safe (section 2.3).

VB pools for a new channel: a new codec channel draws frame/stream buffers from a VB common pool or its own pool. Prefer `AR_MPI_VB_CreatePool` (create a NEW private pool for the harness channel) over `AR_MPI_VB_SetConfig`/`AR_MPI_VB_Init` (which reconfigure the GLOBAL pool layout and are unsafe while anything else is running). VDEC attaches its pool with `AR_MPI_VDEC_AttachVbPool`; VENC pulls from the input frame's pool.

The concrete buffer-allocation and feed-struct ABI (MMZ/VB alloc signatures, VDEC_STREAM_S, the VENC SendFrame descriptor, and the GetStream pack layout, with proven vs inferred offsets) is in `mpp-buffers.md`.

### 2.5 Recommended minimal-risk probe order

Guard between every step with a `pidof ar_lowdelay` check plus a feed-liveness check (confirm the goggle video is still live), matching the careful on-device probe style in `mid-api.md`. Never SIGTERM `ar_lowdelay`.

1. Read-only, session-free, safe against the running stock stack. Query MMZ (`/proc/media-mem` or the `mmz_userdev` `'m',24` query), VB pool stats (`/dev/ar_vb` read fops, or the `AR_MPI_VB_GetConfig` getter), and `ar_sys` PTS reads. These never touch the MPP session. Use them to confirm the tooling and the ABI without risk.

2. Decide the mode. If `ar_lowdelay` is running, STOP here for any channel work: a second-process VENC/VDEC is unsafe (section 2.3). Either switch to Option A (in-process hook) or move to a slot/state where `ar_lowdelay` is not running for Option B.

3. Sole-owner only (Option B, `ar_lowdelay` not running). Call `AR_MPI_SYS_Init` exactly once. Then create ONE fresh VENC channel (index outside {4,8}) with its own `AR_MPI_VB_CreatePool`, feed it a synthetic frame (a solid-color VB block), and confirm `AR_MPI_VENC_GetStream` returns bytes. Re-check `pidof`/liveness after.

4. Sole-owner only. Create ONE fresh VDEC channel (index != live), attach a private VB pool, `AR_MPI_VDEC_SendStream` a small H.265/H.264 file, and pull frames with `AR_MPI_VDEC_GetFrame`. Re-check liveness after.

5. Tear down in reverse (`DestroyChn` -> `DestroyPool` -> `SYS_Exit`) only in the sole-owner case. Do not call `SYS_Exit` if anything else uses MPP.

Destructive-op caution: some `AR_LOWDELAY_MID_*` IPCs and some setters return OK without acting or crash `ar_lowdelay` on bad input (see the memory notes on blind-probe RF setters, IPC length, and GetDebugMode). Pull valid enum values and exact lengths from the vendor wrappers before issuing any setter; keep the probe read-only first.
