# MSP / FC OSD canvas format (the OSD shared memory)

The FC's Betaflight/INAV OSD arrives over **standard MSP DisplayPort**, `libvtxfc` packs it into a canvas in an OSD shared-memory region, and `ar_lowdelay` exposes it via `AR_LOWDELAY_MID_RX_GetOsdContext` (cmd `0x2c`). This doc covers only the **vendor-specific** parts; the rest is upstream Betaflight (linked, not re-derived).

**Standard Betaflight (documented upstream, don't duplicate here):**
- **Wire protocol:** MSPv1/v2 framing + command `MSP_DISPLAYPORT` = 182 (`0xb6`), sub-commands heartbeat 0 / release 1 / clearScreen 2 / writeString 3 (`<row> <col> <attr> <chars...>`) / drawScreen 4. Confirmed in `libvtxfc` `mspProcessPacket`/`processDisplayPort`. See Betaflight [`io/displayport_msp.c`](https://github.com/betaflight/betaflight/blob/master/src/main/io/displayport_msp.c) and the [MSP DisplayPort wiki](https://github.com/betaflight/betaflight/wiki).
- **Glyph codes / font:** the standard MAX7456 / Betaflight OSD font indices (see Charset below).

**Vendor-specific (our RE, the body of this doc):** the Artosyn SHM canvas packing that `GetOsdContext` returns (the `b6 03` length-chained records), the GetOsdContext call/SHM, and the fact that the **menu** (not `ar_lowdelay`) must render it. From RE of `ar_lowdelay` / `test_uidesign` / `libvtxfc` + on-device captures.

Confidence tags: **[confirmed]** (decoded cleanly + reproduces known text), **[inferred]**, **[open]**.

## GetOsdContext return [confirmed]

The vendor lib (`liblowdelay_mid`) returns this struct (from the decompile + the "SymbolId %u offset %u Size %u" log):

```c
typedef struct {
    uint32_t symbol_id;   // +0  changes each canvas update (a frame/sequence counter)
    void    *shm_ptr;     // +8  = shm_base + offset (the OSD bytes live here)
    uint16_t size;        // +16 valid canvas bytes at shm_ptr (variable; 118 in the armed sample)
} lowdelay_osd_context_t;
```

`size` is the canvas byte length, not a fixed grid; the region holds the full current sparse canvas each poll (only non-empty elements are encoded), so a reader renders fresh each time.

For a blob-free client, `GetOsdContext` is transact `0x2c` with a 12-byte buffer. The raw reply (before the vendor packs the struct above) is `{ctl_ret u32 @+0, offset u16 @+4, size u16 @+6, symbolId u32 @+8}`, and the canvas is `shm_base + offset` for `size` bytes, where `shm_base` comes from the handshake below (`libre/ipc/lowdelay_osd.c`).

## OSD shared-memory handshake [confirmed on-device]

The canvas lives in System V shared memory, not in the transact reply. `CLIENT_Init` attaches it; a blob-free client replicates that:
- transact `0x31` returns the OSD `shmid` at reply `+4` (and a media `shmid` at `+8`); `shmat(shmid, NULL, 0)` gives `shm_base`.
- transact `0x39` returns the AI-overlay `shmid` at reply `+4` (optional; only the AI OSD needs it). `GetAiOsdContext` (`0x3a`) is then `0x2c` against the AI base.
- teardown is `shmdt` per attached base.

The shmids are plain System V ids passed straight to `shmat` (not keys, not sizes). Proven blob-free in `libre/ipc/lowdelay_osd.c`, validated on-device.

## Byte format [confirmed]

The canvas is a stream of **2-byte words, each followed by a `0xff` separator**. Strip the `0xff` bytes to get the packed stream: `<10-byte header> <record> <record> ...`

- **Header** (10 bytes), e.g. `06 00 00 03 91 00 00 00 51 0a`, the last byte (`0a` here) is the LENGTH of the first record's content (the rest is not yet decoded). **[open]**
- **Records are LENGTH-CHAINED** [confirmed on-device]. The byte immediately BEFORE each `b6 03` marker is that record's content length. A record is `b6 03 <row> <col> <attr> <glyphs...> <next_len>` where the content (everything after `b6 03`) is `len` bytes and its **last byte is the length of the FOLLOWING record** (so it doubles as the next record's prefix). The displayable glyphs are content bytes `3 .. len-2` (skip row/col/attr and the trailing next_len). The final record has no trailing length.
  - `row`, `col`: cell position into the OSD grid.
  - `attr`: cell attribute (color/blink/font page). `0x00` in every sample seen. **[open]** (see "What's left").
  - **Pitfall:** the `next_len` byte is NOT a glyph. Drawing it renders a stray fragment after every element (a small mark of whatever that font index happens to be). The renderer skips the byte right before each `b6 03` marker (`msp_canvas.c`).

## Charset, standard Betaflight / MAX7456 font

The glyph codes are the standard MAX7456 / Betaflight OSD font indices: `0x20`-`0x7e` ASCII, `0x01`-`0x1f` and `0x80`+ icon glyphs (battery `0x90`-`0x9c`, timer, the volt symbol `0x06`, etc.). The numbering and the icon-to-symbol map are upstream and not re-derived here, see the [Betaflight OSD fonts](https://github.com/betaflight/betaflight-configurator/tree/master/resources/osd) and `src/main/osd/symbols.h` (`SYM_*`). Our renderer just indexes the bundled BF font PNG by code. (Note: the low `0x06`-`0x0b` bytes that looked like stray icons were the record LENGTH bytes, see Byte format above, not glyphs.)

## Worked decode of an early 121-byte sample [confirmed]

After stripping `0xff` and splitting on `b6 03`:

| row | col | attr | text |
|-----|-----|------|------|
| 15 | 2  | 0x00 | `[92] 7.91 [06]`, icon + battery voltage + volt symbol |
| 16 | 2  | 0x00 | `[9c] 03:25`, icon + flight timer |
| 15 | 43 | 0x00 | `MANUAL`, flight mode (right column) |
| 16 | 43 | 0x00 | `FAST`, rate/mode |
| 10 | 23 | 0x00 | spaces (cleared cell) |
| 8  | 23 | 0x00 | spaces |

Positions are sane for an HD DisplayPort grid (cols 0..52, rows 0..19): left-column telemetry at col 2, right-column mode flags at col 43.

## Grid dimensions [confirmed]

Standard Betaflight HD DisplayPort, **53 x 20**. The renderer uses this and the OSD lays out correctly on-device (left-column telemetry at col 2, right-column flags at col 43, warnings centred).

## Who renders it (vendor), and why our menu must too [confirmed]

RE-confirmed (objdump of both binaries):

- **`ar_lowdelay` only PRODUCES the canvas.** It receives the FC's MSP DisplayPort stream over RF, parses it with `libvtxfc` (`processDisplayPort` -> `vtx_osd_set_canvas`) into the OSD shared memory, and exposes it via `GetOsdContext` (cmd 0x2c). It does **not** composite the MSP canvas onto the display, its own VO/VENC OSD layers (`AR_LOWDELAY_RX_VO_Overlay1Enable`, `AR_LDRT_RX_VO_InsertOsdPic`, `AR_CMD_RX_ENABLE_VENC_OSD`) are fed by the FSM/AI/RC OSD queues + the system telemetry line (`FUN_0045c6c8`), not this canvas. The vtxfc glyph helpers are reachable in ar_lowdelay only from two passthroughs (`AR_LOWDELAY_TX_RC_UpdateCanvasInfo` @0x454910, `AR_LOWDELAY_GET_RC_OSD_CONTEXT` @0x454928), neither of which touches fb/VO.
- **`test_uidesign` (the Qt UI) RENDERS it.** A static function at `~0x417cb0` runs the loop: `mspUIUnpackOsdInit` -> `GetOsdContext` -> `vtx_get_osd_pack_symbol` (decode glyphs out of the canvas) -> `Capture_m::osdcontentsenduint(x, y, glyph, state)` (@0x41a530, emit each glyph to QML to paint), with `vtx_get_osd_clear` to clear. The 4th arg is the add(1)/clear(2) state, NOT the cell attribute, the `attr` byte (canvas +5) is carried opaquely and dropped before rendering.

**Implication:** our menu replaces `test_uidesign`, so while it runs `ar_lowdelay` does NOT draw the FC OSD for us, the menu must. We mirror the loop: `GetOsdContext` -> decode -> blit the glyph grid ourselves.

## Glyph font, single format: the standard HD MSP-OSD PNG

The canvas glyph codes (`0x01`-`0x1f`, `0x80`+) are the standard Betaflight/INAV MAX7456 font indices. The renderer uses ONE font format, the wtfos/Walksnail **HD font PNG** (`font_<fcid>_hd.png`: 24x36 glyphs, 256 stacked vertically, RGBA = white glyph + black outline + transparent), loaded at runtime (lodepng) and indexed by glyph code. The same loader serves the bundled default AND a user's custom font from the SD, so there is one path, not two. (We deliberately did NOT use the `.mcm` source format at runtime, that would split the default and custom-font paths.)

The bundled default is the **Betaflight default** (GPLv3): `assets/osd-fonts/betaflight.mcm` is the canonical source and `assets/osd-fonts/mcm2png.py` generates `assets/osd-fonts/font_BTFL_hd.png` from it (12x18 -> 24x36, 2x upscale). Shipped to the device as a separate file, never baked into the binary.

**Custom fonts (later):** the same PNG loader already accepts any standard `font_<fcid>_hd.png` from the SD; the only deferred part is the menu UI to pick one.

## Implementation status, DONE (validated on-device with a Betaflight FC)

The FC/MSP OSD renders over the live video, confirmed on a goggle running a Betaflight FC (DISARM, voltage, timer, mode, warnings all show correctly with the bundled BF glyph font):

- **Renderer** (canonical `libre/ipc/msp_canvas.{c,h}`, vendored verbatim into the HUD at `ml-hud/src/osd/msp_canvas.{c,h}`): parses the length-chained canvas and blits each glyph (icons + text) **1:1 at native 24x36** into a canvas, which is then scaled to the screen as ONE layer, so multi-cell elements (the AH / crosshair) tile seamlessly. Per-glyph fractional scaling split them.
- **Glyph font** (`libre/app/osd/msp_font.{c,h}`, copied to `ml-hud/src/osd/msp_font.{c,h}`): loads the HD PNG via lodepng (fetched + pinned by CMake/FetchContent), indexed by glyph code.
- **Canvas source:** two paths feed the same renderer. The libre menu polls the IPC directly (`lowdelay_client.c`: a dedicated thread runs the blocking `GetOsdContext`, gated on link-up, caching the latest canvas so the UI never blocks). The `ml-linkd` + `ml-hud` split instead delivers the identical canvas over the mlm `osd.sock` seam: `ml-linkd` drains the `:10000` telemetry and publishes each MSP frame as `MLM_T_MSP` (`ml-shared/mlm.h`), and `ml-hud`'s `osd_channel` consumes it - so `ml-hud` never touches the vendor IPC.
- **Visibility:** drawn only while the menu is closed AND `Display > FC OSD` != None; hidden behind the menu.

### What's left (minor / future)

- **`attr` byte** (the Betaflight DisplayPort write attribute, page / blink / colour). The vendor firmware carries it **opaquely**, RE-confirmed it is never bit-decoded (only an equality compare for add-vs-update in `add_osd_icon`) and is dropped before rendering, so we render monochrome page-0. It is `0x00` in every capture; non-zero only for Betaflight 4.5+ coloured/blinking warnings or page-2+ glyphs. Its bit layout is **upstream Betaflight** (`displayPortAttr_t`, `src/main/io/displayport_msp.c`), NOT recoverable from the vendor binaries (they don't interpret it). To apply it: take the bit layout from Betaflight source + a non-zero capture, then honour page/blink/colour in `msp_canvas.c`. Not blocking. **[open]**
- **Header**: only its last byte (the first record's length) is decoded; the other 9 bytes are not needed for rendering. **[open]**
- **Custom font from SD**: the PNG loader already accepts any `font_<fcid>_hd.png`; only the menu UI to select one is deferred.
