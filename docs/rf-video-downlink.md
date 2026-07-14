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
- One PREFIX_SEI (NAL 39) per frame with ASCII `ChnId / FrameId / PTS / BR / QP`.
- Deframer/decode tooling: `libre/tools/ml-rf-udp/` (README has the byte-level layout).

## The video-start chain (all steps required, in order)

1. **Associate:** the 22 verbatim association frames (20 ms spacing), then let the chip reach CONNECT on its own (LOCK -> CONNECT is chip PHY firmware).
2. **Vendor poll cadence, settle first:** ~2.5 s of `port 73` (~6 Hz) + `ff02` heartbeat (~3.4 Hz) only. Do NOT fire `port 0c` during association; premature or too-fast polling (the old 150 Hz loop) wedges the air silent in ~11 s.
3. **Socket-open config, ONCE:** two `ch02` (SET) frames are the proven minimum - `ch02 p06 {02,05}` (SelectChnIndex, channel 5) then `ch02 p08 {08,17}` (SET_POWER, RX chain, 23 dBm). The original vendor capture also interleaved 5 `ch01` (GET) read-backs and a `ch02 p15` 136-byte "set remote info" frame; the GETs configure nothing and the 136-byte frame is a vendor uninitialized-stack bug, so all six are dropped with no effect on video (cold-boot validated, driven by `ml-linkd`). There is no `ch04` open; that model was wrong.
4. **Runtime TX power:** `TX_SET_POWER` (bb_ioctl cmd `0x02000008`, payload `{0x00, dBm}`, 23 dBm) + `SET_POWER_AUTO` (`0x02000009`, `{1}`). Without it the goggle transmits at chip default power, SNR is too low, and the link never reaches `type:8` (the high-bandwidth video profile the chip derives from config + SNR; there is no "set type 8" command). Chip log `cur type:8 req type:8` = good.
5. **Steady cadence:** `port 0c` ~24 Hz, `port 73` ~6 Hz, `ff02` ~3.4 Hz, forever.
6. **`:20001` identity handshake (3-way):** goggle sends 520 B type-0 probes (~3 Hz); air answers type-1 (its identity, ONCE); goggle must echo the air's type-1 back with exactly `byte[0]=0x02, byte[5]=0x00` (type-2 ACK). Without the ACK the air retransmits type-1 forever and never advances. The vendor stops the type-0 probe after this completes.
7. **`:10000` media-params handshake (the actual video trigger):** goggle sends `msg_type=1` MEDIA_PARAMS_REQUEST (24 B, ts@8, len=0) every ~2.0 s; air answers `msg_type=2` MEDIA_PARAMS (24 B header + 72 B body: codec/res/fps); goggle sends `msg_type=3` ACK (24 B, ts@8). The air begins VideoSend on `:10001` right after the type-3 ACK. The type-3 ACK was the final missing piece of the whole investigation.

## UDP reliability model (wire-measured on slot A)

- `type=1` request: re-sent every ~2.0 s until answered, then stops. The retry loop is the only reliability mechanism and also how the goggle polls out the air's own readiness gate (the air ignores requests until its VPU/VIN are up).
- `type=2` reply and `type=3` ACK: fire-once, never retransmitted. A lost type-3 wedges video with no recovery short of a session restart, so a robust client should keep re-eliciting type-2 (via type-1) until it actually sees `:10001` frames.
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
