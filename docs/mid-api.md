# MID API (liblowdelay_mid), full surface + System-OSD telemetry sources

The goggle UI talks to `ar_lowdelay` over an IPC wrapped by `liblowdelay_mid.so`. The lib exports **64**
`AR_LOWDELAY_MID_*` functions; stock `test_uidesign` imports 54. This doc catalogues the whole surface and,
critically, maps where each **System-OSD** field actually comes from.

Source: static RE of `firmware/bin/ar_lowdelay` + `build-qcap/liblowdelay_mid.so`; live values from
`libre/tools/midsurvey.c`. Status tags: **[live]** exercised on-device, **[RE]** handler decoded (args
known, not yet exercised), **[doc]** name/purpose only.

## Dispatch table
cmd-id-indexed table at `.rodata 0x4c1cb0`, 16-byte `{handler_ptr, name_str}` entries, dispatched at
`0x4559a8` via `*(0x4c1bc0 + cmd*16 + 0xf0)`. cmd ids below are relative to that base.

## System-OSD telemetry, where each field comes from (the key finding)
The OSD string (VA `0x4c3c30`) is `Signal:%u CH:%u FlightTime:%u SBat:%.2f GBat:%.2f Bitrate:%uMbps
Distance:%um STYMode:%u`, built at `0x45c6c8` from ONE aggregated context struct (`ctx` = arg0 of the
encoder OSD callback). **That ctx is not returned by any single getter**, but most fields ARE
separately reachable:

| OSD field | ctx off | Reachable via | Status |
|---|---|---|---|
| Channel | +270 u8 | **SYS_GetInfo +0x3E (ChnIdx)**, NOT GET_Tranmit_Param (that's TX power) | [RE] |
| Bitrate (Mbps) | +228 kbps | **SYS_GetInfo +0x14 (ShowKbps)**, /1000 = Mbps | [RE] |
| Signal | +268 | **SYS_GetInfo +0x3C (SignalStrength)** | [RE] |
| Connection (linked?) |, | **GetDebugInfo** ret==OK, content-checked | [live] |
| GBat (goggle batt) | +216 f32 | **local ADC** (telemetry.c) | [live] already shown |
| SBat (quad batt) | +212 f32 | **SYS_GetInfo +0x04 (TxVoltage, f32)** | [RE] |
| Distance | +266 u16 | **SYS_GetInfo +0x3A (u16)** | [RE] |
| Quad temp |, | **SYS_GetInfo +0x00 (TxTempture, u16)** | [RE] |
| STYMode (work mode) | +292 u16 | **RX_GetFastMode** (separate call) | [RE] |
| FlightTime | +272 u16 | NONE, an app timer, not IPC |, |

**Reachable: channel, bitrate, signal, quad/SBat voltage, distance, quad temp (all SYS_GetInfo), connection
(GetDebugInfo), work-mode (RX_GetFastMode).** Only FlightTime is not IPC-reachable (an app timer). The
authoritative SYS_GetInfo layout is below.

## Key getter reply layouts [RE]

### SYS_GetInfo (cmd 31) OUT struct, AUTHORITATIVE (RE of the STOCK UI `test_uidesign`)
The stock UI reads this struct field-by-field; THESE are what the OSD actually shows. Field names are from
the UI's Qt signal-arg table (.rodata 0xddd959+) + the firmware debug-printf strings.

| off | type | field | OSD use |
|---|---|---|---|
| +0x00 | u16 | TxTempture | air/sky temp |
| +0x02 | u16 | RxTempture | goggle temp |
| **+0x04** | **f32** | **TxVoltage** | **quad / SBat voltage** (volts, no scaling), stock getter `getskyvol` |
| +0x08 | f32 | RxVoltage | link-RX rail (NOT the goggle pack) |
| **+0x14** | **u32** | **ShowKbps** | **bitrate**, the DISPLAYED one; /1000 = Mbps (4 idle, 11-16 armed) |
| +0x18 / +0x1C / +0x20 / +0x24 | u32 | TargetKbps / RealKbps / CurPeakKbps / TransmitPhyKbps | |
| +0x2E | u16 | DelayMs | latency |
| **+0x3A** | **u16** | **Distance** | **distance (metres)**, getter `getdistance` |
| **+0x3C** | **u16** | **SignalStrength** | **signal** |
| **+0x3E** | **u8** | **ChnIdx** | **channel**, absolute index; displayed `number = ChnIdx - 1` (ChnIdx 1 = CH 16), see channel-numbering.md (getter `getwirelesschn`) |
| +0x3F | s8 | CurMcs | MCS (signed) |
| +0x48 / +0x50 | u64/u16 | SdCardCapacity / Availability | |
| +0x54 | u16 | StandbyModeStatus | |
| +0x58 / +0x5C | f32 | MinSnr / MaxSnr | |

So the WHOLE System-OSD bar (channel, bitrate, SBat voltage, distance, signal, temps, MCS) is in SYS_GetInfo.
STYMode is a separate `RX_GetFastMode` call (stock getter `getspeedmode`); GBat (goggle pack) = local ADC,
not this struct; FlightTime = an app timer, not IPC.

### GET_Tranmit_Param (cmd 35, handler 0x4593c0), TX POWER + bandwidth (NOT channel)
From the SDK wrapper (`liblowdelay_mid` 0x5a6c): `reply[4]` is a power-LEVEL index, and the wrapper remaps
it via a switch into **`out[0]` = TX power in mW**: `{idx 5->3, 14->25, 20->100, 23->200}`. On-device
`out[0]=0xc8=200` = **200 mW** (matches the menu Power setting). `out[1]` = `reply[5]` = bandwidth
(`0x14` = 20 MHz). So this is the **Power getter** (for the Power menu item), NOT the channel. The channel
is in `SYS_GetInfo` (ChnIdx).

### GetHnbqDeviceInfo (cmd 59, handler 0x4591b8), 208-byte reply
- `reply+4/+6/+8` u16 device-id triplet · `reply+12` f32 · `reply+0x10` & `+0x50` two strings.
- Air-unit IDENTITY (vendor/product/hw ids + strings), NOT quad battery / distance. f32@+12 + the strings
  TODO-validate on hardware.

### GetFlightDynamicWorkMode (cmd 67=0x43, handler 0x456ce8), 8-byte reply
- `reply+4` = a single dynamic-mode BOOLEAN (u8), gated by an elapsed-time compare. NOT the multi-valued
  STYMode (which is ctx+292 only).

### Active channel
`GET_Tranmit_Param reply[4]` (`session[176][104]`), set by cmd 3 WIRELESS_SELECT_CHANNEL (handler
0x45cc08), config default 26. A session-struct field, not a bare global.

## Full catalogue of previously-undocumented functions (cmd; handler; behavior)
`S` = session config block `*(ctx+176)`, persisted via `AR_CFG_MID_Save`. All [RE] (static).

- **GetAperture** (56; 0x4579c0): reply+4 = aperture u8 (S+56).
- **SetAperture** (55; 0x456c28): arg byte -> S+56, saved.
- **GetZoomStatus** (54; 0x455420): reply+4 = mode u8 (S+37), reply+8 = zoom factor f32 (S+40).
- **SetZoomStatus** (53; 0x45b718): float@+8 -> SetScaleMode, S+37/40, saved.
- **GetRoiStatus** (47; 0x455508): reply+4 = ROI flag u8 (S+67).
- **SetRoiStatus** (46; 0x457ea0): arg u8 -> SetROI + S+67, saved.
- **GetDisplayScaleStatus** (52; 0x455498): reply+4 = mode u32 (S+36).
- **SetDisplayScaleStatus** (51; 0x4581f0): arg u32 -> SetScaleMode + S+36, saved.
- **GetUIButtonEn** (66; 0x455cd8): query group by id (req+4); reply+8 = enabled u8 (S+184/188).
- **UIButtonEn** (65; 0x456808): id (req+4) + on/off (req+8) -> S+184/188/113, saved.
- **GetDebugMode** (38; 0x457098): reply+4 = mode u8.
- **SetDebugMode** (39; 0x456aa0): arg u8 @ req+4.
- **RX_MavlinkLogToggle** (63; 0x455058): int -> S+92/93/94, saved.
- **RX_PULL_Usrlog** (68; 0x4569e8): arg u32 @ req+0 -> UPLOAD_USRLOG; reply+4 = result.
- **GET_LED_STATUS** (28; 0x454fe8): reply+4 = LED state u8 (S+98).
- **SYS_Buzzer_GetStatus** (30; 0x455018): reply+4/+5/+6 = S+92/+94/+93 = En / Volume / VolAlert.
  CONFIRMS our SetStatus packing (byte0=En, byte1=Volume, byte2=VolAlert), and lets us READ buzzer state.
- **SYS_GetUpgradeStatus** (34; 0x4551e8): reply+4 = {u64+u32} stage+percent.
- **SYS_Upgrade** (33; 0x455230): req+4 param, req+8 action (1/2); reply+0 = result.
- **REC_RemoveMediaFiles** (50; 0x45bb00): req+4 id, req+8 handle; deletes from media JSON DB.
- **REC_SetAttr / REC_SetTimeLimit** (14; 0x459ee8, cmd 0xe): the DVR attribute struct (read via
  REC_GetAttr 0xb). Field map, validated by diffing REC_GetAttr against the stock UI:
  `u32 @0x00` = **Loop** (0/1); `u32 @0x08` = **max segment length in seconds** (`0x7FFFFFFF` =
  unlimited, else minutes x 60: 5/10/15/30 min -> 300/600/900/1800); `@0x18` = resolution (1920x1080);
  `@0x1c` = fps (0x3c = 60). The vendor sets Loop + segment length via **REC_SetAttr** (full 40-byte
  struct, read-modify-write). The `REC_SetTimeLimit` MID wrapper's short payload writes NOTHING readable
  in practice (the attr is unchanged), do not use it. Wired in recorder.c.
- **SetHnbqDeviceInfo** (62; 0x4553c0): two u32 set values; daemon near-no-op acknowledger.
- **GETHnbqZoomInfo** (64; 0x456808): MISNAMED, actually a SETTER (selector 1-6 @ req+4 + on/off @ req+8
  -> S+{68,93,113,184,187,188}, saved); reply only status.

## Getter safety (probed one-per-process with a pidof + latency check, `libre/tools/midsurvey.c`)

**SAFE (fast, ar_lowdelay stable):** GetFastMode, GET_Tranmit_Param, SYS_GetInfo, GetFlightDynamicWorkMode,
SYS_GetVersion, GetDebugInfo, GetHnbqDeviceInfo, PANEL_GetBrightness, GET_LED_STATUS, SYS_Buzzer_GetStatus,
CAMERA_GetSetting, GetAperture, GetZoomStatus, GetRoiStatus, GetDisplayScaleStatus, SYS_GetUpgradeStatus,
REC_Status, REC_GetAttr, GetUIButtonEn, GetScanResult.

**PROBLEMATIC:**
- **GetDebugMode**, blocks ~33s and KILLS ar_lowdelay (server-killer; pid lingers then dies). Do not call.
- **GetOsdContext**, blocks the CLIENT (waits on a queued OSD msg); server survives, `killall` the client to recover.

**Baseline values (no link):** GET_Tranmit_Param `[0]=0xc8` (TX power, 200 mW) `[1]=0x14` (20 MHz);
SYS_Buzzer_GetStatus `01 0a` (En=1, Vol=10); GET_LED_STATUS `01`; PANEL_GetBrightness `01 06`;
GetZoomStatus zoom=1.0; GetHnbqDeviceInfo firmware `1.0.44.rel`; REC_GetAttr `80 07 38 04` = 1920x1080;
SYS_GetUpgradeStatus `04`; GetDebugInfo `-1` (correct, no link); SYS_GetInfo populated (float ~7.34 @ +8).

## Caveats
All [RE]/[doc] entries are static-only, validate on hardware (use `libre/tools/midsurvey.c`). Shared
bytes S+92/93/94 are touched by BOTH MavlinkLogToggle and Buzzer_GetStatus; GetHnbqDeviceInfo f32@+12 +
strings need confirmation. **Never blind-fire setters**, `SetFastMode` out-of-range crashed
`ar_lowdelay` and the watchdog rebooted the goggle. RE valid args first.
