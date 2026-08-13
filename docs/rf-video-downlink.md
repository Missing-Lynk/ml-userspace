# RF video downlink, the working protocol (reference)

How the goggle makes the air unit stream H.265 video to it over the AR8030 link, as solved and HW-confirmed on the fully open slot-B stack (open kernel + open `artosyn_sdio` + `libre/tools/ml-rf-video`). This is the durable protocol record. The reference implementation is `libre/tools/ml-rf-video/ml-rf-video.c`; the productionized daemon is **`ml-linkd/`** (top level; spec in `ml-linkd/README.md`), which drives this whole chain plus a consumer-READY gate (the params handshake is held until the video consumer is listening, so the one-and-only FrameId-0 IDR is never missed). The full gated chain incl. live decode-to-panel is HW-validated.

## Transport

- Video = plain UDP `10.0.0.100:49958 -> 10.0.0.1:10001` over the `sdio0` IP tunnel (driver `0xCC` link type -> `netif_rx`). ~1.5 MB/s sustained when streaming.
- Telemetry/OSD + the media-params handshake = UDP `:10000` (both directions). A separate `:20001` identity handshake also exists (below).
- Control plane (association, polls, chip setters) = 19-23 byte bb-socket frames written to `/dev/artosyn_sdio` (channel topology: `datasheets/ar8030-rf-link.md`).

## Video framing (`:10001`)

- One datagram = one access unit. 36-byte LE header: MagicCode `0x12345678`, StreamLen, ChnIndex, isIdrStream, FrameId, TimeStap (milliseconds), Resolution, TailMagic `0x87654321`, CRC-32 over the first 32 B; then StreamLen bytes of HEVC and a 4 B tail magic.
- Two independent H.265 bitstreams interleaved by ChnIndex: tile 0 = top 1920x560, tile 1 = bottom 1920x552, with a 32-row overlap (tile 1 rows 0..31 duplicate tile 0 rows 528..559; the vendor cross-fades the seam). Tile split is mode-dependent (1/2/3/4-tile pattern table); VR04 1080p = the 560/552 2-tile shape.
- IDR policy: exactly one IDR per tile at FrameId 0 (session start), then P-frames only, no periodic IDR. A consumer that misses session start cannot decode; `isIdrStream` = 1 on the FrameId-0 packets only.
- One PREFIX_SEI (NAL 39) per access unit, carrying `user_data_unregistered`. **MANDATORY. A vendor goggle tears its receive pipeline down without it** - see "The per-frame SEI is required" below. An earlier revision of this file called it diagnostic-only and said a vendor goggle does not care; that was wrong, and it cost several hardware sessions.

### The IDR access unit head is byte-checked, and an AUD breaks it

**A vendor receiver does not parse the start of an IDR, it compares twelve bytes against hard-coded values.** `AR_LDRT_RX_VDEC_RecvStreamCheck` in `libldrt_pipeline.so` (disassembly `0x1fa9c..0x1fb10`; reached through the `AR_LDRT_RX_Init` PLT thunk out of `ar_lowdelay`) tests, in order:

| offset | required | meaning |
|---|---|---|
| 0..3 | `00 00 00 01` | 4-byte start code; a 3-byte one fails |
| 4 | `0x40` | NAL 32 = VPS. **The first NAL of the AU must be the VPS** |
| 5 | `0x01` | NAL header byte 2 |
| 39..42 | `00 00 00 01` | second start code, so **the VPS must be exactly 35 bytes** |
| 43 | `0x42` | NAL 33 = SPS |
| 44 | `0x01` | |

On any mismatch it logs `Parser IDR Stream Error %x %x ...` (twelve bytes), calls `AR_LDRT_RX_VdecSendAttrReset`, and drops the frame. The receiver then never accepts an IDR, and `" P Stream Receive Upto %llu , Need Request IDR !!! "` makes it re-request one every ~2 s forever - which on the air side looks like a healthy stream of `:10000` type-3 requests and a black panel.

The check is positional, so anything that shifts the head fails it. The one that bit us is the **access-unit delimiter**: wave5 registers `V4L2_CID_MPEG_VIDEO_AU_DELIMITER` with a default of **1** (`wave5-vpu-enc.c`), so an encoder that never sets the control prepends a 7-byte AUD (`00 00 00 01 46 01 10`) to every access unit and fails five of the twelve. `ml-air-video` now sets it to 0. The vendor encoder emits no AUD.

Our own goggle imposes none of this and decodes an AUD-prefixed stream perfectly, so the defect is invisible on our own link. Verify a dump against the real check with `glue/capture/check-idr-head.py FILE.h265` rather than by decoding it.

### The per-frame SEI is required, and its absence stops the receiver dead

`AR_LDRT_RX_VdecFrameReceiveThread` calls `AR_MPI_VDEC_GetUserData` for every decoded frame. When
the access unit carried no user data the call fails, and the receiver's response is not to shrug but
to **tear down the whole receive pipeline**. Captured from a vendor goggle running against our air
unit, with every other defect in this file already fixed:

```
Rx Chn [1] Recv IDR Frame[1] Magic[0x12345678] Id [1], Len [6512] Resolution 1920x1080.
Rx Chn [0] Recv IDR Frame[1] Magic[0x12345678] Id [2], Len [7153] Resolution 1920x1080.
Vdec Chn 0 IDR Receive, Frame Id 2
current message id [AR_FSM_RX_MESSAGE_IDR_RECEIVED]
AR_LOWDELAY_RX_TRANSMEDIUM_ReceiveStart Begain With Audio
get user data on vdec channel 0 failed, s32Ret=-2147122156, stUserData.bValid=0
AR_MPI_VDEC_GetUserData Failed. Vdec Chn [0], ret=0xffffffff reset pipeline.
PipeLine Rst Event -> PipeLine Rst Begain -> StopRx -> PipeLine Rst End
```

Both tiles' IDRs were accepted. The pipeline stopped anyway, exactly once, and never restarted: the
rest of that four-minute boot contains nothing but the 60 s time-sync heartbeat. That is the black
panel, the goggle falling silent on the `:10000` control plane, and the reason a wedged goggle does
not recover when the air unit reboots - its receive pipeline is stopped, and nothing retries it.

**Placement matters.** The SEI must come after the parameter sets on an IDR access unit, or it
shifts the head and fails the twelve-byte check above. The vendor's own layout, from
`archive/out/rf-capture/assoc-arm.c0.h265`:

```
IDR AU:  VPS(39 B) -> SPS(51 B) -> PPS(12 B) -> PREFIX_SEI(76 B) -> IDR_W_RADL
P AU:    PREFIX_SEI(77 B) -> TRAIL_R
```

The general rule that produces both: insert the SEI immediately before the first VCL NAL
(`nal_unit_type <= 31`).

**Exact bytes**, one vendor P-access-unit SEI in full:

```
00 00 00 01   start code
4e 01         NAL header: nal_unit_type 39 (PREFIX_SEI), nuh_layer_id 0, nuh_temporal_id_plus1 1
05            payload_type = 5 (user_data_unregistered)
44            payload_size = 68 (16 UUID + 52 text incl. NUL)
bd e9 45 dc b7 48 d9 e6 20 d8 2c 96 ef ee 23 d9      UUID, constant across every frame and tile
"ChnId 0 FrameId 1 PTS 43b19de Filed 4 BR 3926 QP 25\0"
80            rbsp_trailing_bits
```

Text format: `ChnId %d FrameId %d PTS %x Filed %d BR %d QP %x`, NUL-terminated. `PTS` and `QP` are
hex, `BR` is decimal kbps, `Filed` is the vendor's spelling and was 4 in every frame sampled. The
NAL length varies (76/77 B observed) purely because the numbers vary in width, so `payload_size`
must be computed rather than fixed.

Whether the receiver parses any of these fields or merely requires the call to succeed is **not
established**; only that a missing SEI is fatal. Match the format anyway.

### TimeStap is compared against the receiver's own clock, so it must restart per session

A vendor receiver runs a constant-frame-rate stage (`arCfrHandleThread`) that checks each frame's PTS against its current time and discards anything from the future. A receiver that power-cycles restarts its clock at zero, so a transmitter that keeps counting from the previous session is rejected frame by frame:

```
arCfrHandleThread: u32FrameId 78183, incorrect PTS 219450000, current time 188069720
Current Gnd-Frame Send PTS[..] < Sky-Frame PTS[..], maybe timecalibration issue
```

Measured as a constant 31.38 s skew, with **every decode counter healthy** - `Recv IDR Frame` 2, `invalid frame` 0, `PipeLine Rst` 0, `First Frame Dispaly` 2 - because the frames decode and are discarded after the decoder. Counters cannot see this failure; only the presentation-stage log can.

`ml-air-video` therefore rebases `TimeStap` to 0 when a receiver establishes a session, triggered by the `:10000` type-1 params request (`air_tx_session_reset`). Two things that look like the right trigger are not: type-3 IDR requests repeat whenever the goggle is unhappy and never converge, and the `:10000` silence window fires on *healthy* links, because a vendor goggle stops sending `:10000` entirely once video is up.

`FrameId` is left running across sessions. The vendor restarts it, and a receiver logs `Stream Not Continue, CurStreamId [n], LastStreamId [0]` when it does not - **but it warns and accepts**: the next lines are `Vdec Chn N IDR Receive` and `AR_FSM_RX_MESSAGE_IDR_RECEIVED`, measured over three consecutive re-associations that all displayed. Restarting it is not free: the receiver pairs the two tiles by `FrameId`, and our per-tile counters have no shared frame identity to restart against (see `plans/vendor-goggle-interop.md`).

The reset is a **request**, not a mutation: `air_tx_session_reset` sets a per-tile flag from the control socket and the transmit path adopts it on its next access unit, so every field involved has one writing thread. This is not defensive - writing the state directly from the control thread landed between the two tiles' emits and broke tile pairing outright, twice over two attempts. The race is real and visible in the logs: one session rebased its tiles a frame apart (1339 and 1340), the next rebased both at 2723, and both displayed.

### Declare Level 4.0, and check what actually goes out

The vendor's VPS carries `general_level_idc = 120` (Level 4.0). Unset, ours goes out as **10**, which is not a defined HEVC level at all - the valid codes are 30/60/63/90/93/120 - and even read as Level 1 it permits a maximum luma picture size of 36864 samples against the 1,075,200 of a 1920x560 tile. A receiver that provisions decode or display buffers from the declared level therefore sizes for QCIF. `ml-air-video` now sets `V4L2_CID_MPEG_VIDEO_HEVC_LEVEL` to `LEVEL_4`, which wave5 maps to `40 * 3 = 120`.

Do not trust the control-to-wire mapping without measuring it: the control's registered default is `LEVEL_1`, which should emit 30, and the observed value was 10. Parse the emitted VPS from a live capture (`glue/capture/vph-sniff.c`) rather than assuming.

### Inspecting the live downlink without spending a bring-up

`glue/capture/vph-sniff.c` reads the air's own outgoing `:10001` packets via `AF_PACKET`/`ETH_P_ALL` on `sdio0` while `ml-air-camera` keeps running, and reports the access-unit size distribution, the Resolution field's constancy, and the head of every IDR. Every field it needs is in the 36-byte header, which rides in the first IP fragment, so the 4096-byte MTU needs no reassembly. Force a fresh IDR into the window with `ml-air-ctl keyframe`.

Prefer it to `ML_AIR_DUMP`, which has to be the boot's **first** camera bring-up to write any DRAM at all (`plans/done/au-b-pipeline-dead-20260802.md`), and therefore costs a whole boot.
- Deframer/decode tooling: `libre/tools/ml-rf-udp/` (README has the byte-level layout).

## The video-start chain (all steps required, in order)

1. **Associate:** the 22 verbatim association frames (20 ms spacing), then let the chip reach CONNECT on its own (LOCK -> CONNECT is chip PHY firmware).
2. **Vendor poll cadence, settle first:** ~2.5 s of `port 73` (~6 Hz) + `ff02` heartbeat (~3.4 Hz) only. Do NOT fire `port 0c` during association; premature or too-fast polling (the old 150 Hz loop) wedges the air silent in ~11 s.
3. **Socket-open config, ONCE:** two `ch02` (SET) frames are the proven minimum - `ch02 p06 {02,05}` (SelectChnIndex, channel 5) then `ch02 p08 {08,17}` (SET_POWER, RX chain, 23 dBm). The original vendor capture also interleaved 5 `ch01` (GET) read-backs and a `ch02 p15` 136-byte "set remote info" frame; the GETs configure nothing and the 136-byte frame is a vendor uninitialized-stack bug, so all six are dropped with no effect on video (cold-boot validated, driven by `ml-linkd`). There is no `ch04` open; that model was wrong.
4. **Runtime TX power:** `TX_SET_POWER` (bb_ioctl cmd `0x02000008`, payload `{0x00, dBm}`, 23 dBm) + `SET_POWER_AUTO` (`0x02000009`, `{1}`). Without it the goggle transmits at chip default power, SNR is too low, and the link never reaches `type:8` (the high-bandwidth video profile the chip derives from config + SNR; there is no "set type 8" command). Chip log `cur type:8 req type:8` = good.
5. **Steady cadence:** `port 0c` ~24 Hz, `port 73` ~6 Hz, `ff02` ~3.4 Hz, forever.
6. **`:20001` identity handshake (3-way):** goggle sends 520 B type-0 probes (~3 Hz); air answers type-1 (its identity, ONCE); goggle must echo the air's type-1 back with exactly `byte[0]=0x02, byte[5]=0x00` (type-2 ACK). Without the ACK the air retransmits type-1 forever and never advances. The vendor stops the type-0 probe after this completes.
7. **`:10000` media-params handshake (the actual video trigger):** goggle sends `msg_type=1` MEDIA_PARAMS_REQUEST (24 B, ts@8, len=0) every ~2.0 s; air answers `msg_type=2` MEDIA_PARAMS (24 B header + 72 B body: codec/res/fps); goggle sends `msg_type=3` MEDIA_IDR_REQUEST (24 B, ts@8). The air begins VideoSend on `:10001` right after the type-3.

   **The 72-byte body is mandatory, and its length is checked.** The receiver's dispatch arm
   (`AR_FSM_RX_CommonMessageProcessThread`, `archive/re/ghidra/out/ar_lowdelay-full.txt:21814`)
   compares the declared length at offset 16 against `0x48` before anything else and drops the
   datagram with `receive params size[%d] error , really size[72]!!` on a mismatch - so a
   header-only reply never raises PARAMS_RECEIVED and video never starts. It then copies the body
   into `g_stRxParams` and pulls the frame rate out of it for
   `AR_LOWDELAY_RX_SYSCTRL_SetCurInputFps`.

   Field map, offsets absolute into the datagram, recovered from the consumer's stores (`:21921`)
   and its readers in `AR_FSM_RX_RealTimeInit` (`:18752`). `mp_params_reply` in
   `ml-linkd/mp-cmd.h` writes exactly these:

   Values below are what a real vendor air unit sends, read off the wire
   (`archive/out/rf-capture/assoc-arm-sdio0.pcap`, the single type-2 in it, 104 bytes total). Where
   the decompile and the capture disagreed, the capture won - all three disagreements are noted.

   | offset | type | vendor value | meaning |
   |---|---|---|---|
   | 16 | u32 | `0x48` | body length, must be `0x48` |
   | 20 | u32 | **1** | pipeline flag. **MANDATORY.** See below - zero costs you the second decoder channel |
   | 24 | u32 | **1** | pipeline value, read only when the flag is 1 |
   | 28 | u32 | 1 | source ready. **Non-zero is mandatory:** `RealTimeInit` refuses with `RealTime Pipeline Init Error, Tx Sns Is Not Ready...` while it is 0, so a correctly sized body of zeros fails one rung later rather than working. |
   | 32 | u32 | **0** | an earlier revision said 1, from the vendor's format-change path (`:15479`). That is a transient TX-side HDMI state which `AR_FSM_TX_ProcessParamsRequest` clears before replying; the wire has 0, and nothing on the receiver reads this field. |
   | 36 | u32 | 0 | csc mode (`ar_lowdelay-full.txt:47558`) |
   | 40 | u32 | 0 | is-interlaced (same log format string). 0 is correct for a progressive camera source |
   | 44 | f32 | 59.99988 | frame rate, high precision. 0.0 is tolerated - the receiver substitutes its own configured rate (`:18882`). An exact 60.0 is fine |
   | 48 | u32 | 1920 | width. `0x780` = 1920, `0x500` = 1280, `0x2d0` = 720 |
   | 52 | u32 | 1080 | height. The 48/52 pair is decoded into `720P60` and friends at `:23182` |
   | 56 | u32 | 60 | frame rate, whole. Drives `SetCurInputFps` |
   | 88 | f32 | 1.0 | |
   | 92..103 | | 0 | a 12-byte tail past the declared body. An earlier revision said there was none, because the decompiled assembly ends at the body. The receiver does not read it; matching the vendor costs nothing |

### Offset 20 decides how many decoder channels exist

The flag at offset 20 is not a hint. `AR_LDRT_RX_VDEC_Enable` in `libldrt_pipeline.so` (`@0x38a68`)
seeds a channel count of 1 and replaces it with the tile pattern's count **only** when the flag
reads 1:

```
38ac0:  mov  w1, #0x1          ; count = 1
38ac4:  ldr  w0, [x20]         ; arg[0] = the flag at body offset 20
38acc:  cmp  w0, w1
38ad0:  b.ne 38adc             ; not 1 -> count stays 1
38ad4:  ldrh w0, [x25, #40]    ; 1 -> count = pattern channel count
```

That count bounds the `AR_MPI_VDEC_CreateChn` loop (`@0x38c14`, bound reloaded `@0x38ce0`), so a
zero flag creates decoder channel 0 and nothing else. The demux still feeds channel 1, whose
per-channel bitstream-buffer size is therefore 0, and `AR_MPI_VDEC_SendStream` rejects every frame
with `invalid frame, bs_size = 0` and `0x80058403` - the VDEC illegal-parameter code, not a verdict
on the bitstream. Two consecutive rejections reset the receive pipeline, which then never restarts.

Symptom to recognise: the goggle accepts both tiles' IDRs, logs `First Frame Dispaly` for neither or
only one, and dies with `bs_size = 0` on channel 1 only while channel 0 never errors.

   Our own goggle ignores all of it - `ml-rx-udp.c` dispatches on `msg_type` and takes geometry from
   SetLdCfg - so the body matters only for vendor receivers, and a defect here is invisible on our
   own link. The producer side is `AR_FSM_TX_ProcessParamsRequest` (`:15581`), which copies the 72
   bytes straight out of `g_stTxParams` and the globals `DAT_00510f20..0x510f58`; that block is
   filled by `AR_FSM_TX_SetSnsParams` from the VIN config. Reading those globals is how the field
   map was first built, but the values in the table above come from the capture, which is the
   authority whenever the two disagree.

   **Untested against vendor software.** Every offset above is read off the RE; `make check` asserts
   our bytes match that reading, not that the reading is right.

   `msg_type=3` is the on-demand keyframe request. The goggle emits it from `AR_FSM_RX_ProcessIdrRequest @0x42de70`, with the same header shape `AR_FSM_RX_ProcessParamsRequest @0x42d8c0` uses for type 1. The air routes it to TX FSM message 8, `AR_FSM_TX_ProcessIdrRequest @0x428938`, which runs `PIPELINE_Start` and, while already streaming, `AR_LDRT_TX_PIPELINE_IdrEnable`. Video begins because the request is answered with an IDR, so the same message also repairs a mid-flight join or a desynced decoder.

## UDP reliability model (wire-measured on slot A)

- `type=1` request: re-sent every ~2.0 s until answered, then stops. The retry loop is the only reliability mechanism and also how the goggle polls out the air's own readiness gate (the air ignores requests until its VPU/VIN are up).
- `type=2` reply and `type=3` IDR request: fire-once, never retransmitted. A lost type-3 leaves video off, so a client re-elicits type-2 (via type-1) and re-requests until it sees decoded `:10001` frames. `ml-linkd` sends one type-3 per type-2 reply while video is unconfirmed and stops once the consumer reports composed frames; the air honours at most one forced keyframe per 500 ms, bounding the cost of a stuck requester.
- Telemetry (`0x09/0x10/0x11`) and video: pure fire-and-forget streams.

## Driver facts (open `artosyn_sdio`)

The SDIO transport and netdev bring-up (host node, reset GPIO, clock glue, firmware upload, `sdio0` framing) are in `../../kernel/docs/artosyn-sdio.md`; the video-RX-specific facts stay here.

- RX is complete and drops nothing; hardware SDIO IRQ works, no polling pump needed. When video is absent the air is not sending it.
- `0xCC` frames are IP-prefix-compressed; the driver rebuilds the address from the sdio0 IP (inetaddr notifier fix).
- **RX stream-sync:** compressed-IP headers can split across SDIO reads and a transfer can carry multiple trailer-delimited runs. The original walker discarded split headers and resynced on any `0x45` byte, which inside HEVC payload fabricates ghost frames with random dst octets (shows up as `InAddrErrors` + huge bogus totlens). The driver now carries split headers across reads, plausibility-checks headers (proto whitelist, totlen <= 4352 = the 4096 `sdio0` MTU + 256 headroom), back-parses multi-run transfers from their 4-byte trailers, and counts junk skips in `rx_frame_errors`. Note the link MTU IS 4096: most video fragments are 3584-4095 B, so never assume 1500.
- No TX send-window gate: the vendor's in-flight gate is drained by `0xEE` flow ACKs, which the chip emits autonomously only when the uplink queue has pending bytes; it gates the uplink only and is off the video (downlink) critical path. The gate defaults off (`unacked_max=0`); the vendor threshold is 40960 if a sustained bulk uplink ever needs it.
- Never warm-reload `artosyn_sdio` (hangs the device); test driver changes via a fresh slot-B RAM boot only.

## Gotchas

- The air unit's log lines (MCS/bitrate, `/tmp/usrlog`) persist across reboots; use the air's `sdio0` TX byte counter as ground truth for "is it streaming" (~1 MB/s = yes).
- A low air-unit battery browns out and mimics link/driver failures; control the battery before concluding anything.
- The air unit overheats on the bench; power it down between runs.
