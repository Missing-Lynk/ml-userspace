# RF modes (Race / Freestyle / Long Range)

How the goggle's three RF "modes" work, what each one sets, and what is still unconfirmed. Source: the `ar_lowdelay` / `liblowdelay_mid` / `test_uidesign` decompile, the baseband config files in the device rootfs (`out/P1_GND/rootfs/usr/usrdata/ar813x/`), and on-device readings.

Each fact is tagged: **[confirmed]** (direct evidence or observed on-device), **[inferred]** (consistent with the evidence but not proven), **[open]** (still to be determined).

## Summary

The UI offers three modes: **Race**, **Freestyle**, **Long Range**. Channel-wise they collapse to two behaviours: **Race uses 16 channels, the other two use 3.** The mode is a goggle-side setting that is also transmitted to the air unit.

## Channel sets per mode [confirmed]

The valid-channel bitmap is selected by mode and lives in two rootfs files. The bitmap bits index the baseband channel table.

| File | Bitmap | Bits set | Channels |
|------|--------|----------|----------|
| `chan_fast_valid_bmp.json` (race) | `0x0007FFF8` | 3..18 | 16 |
| `chan_valid_bmp.json` (non-race)  | `0x00000007` | 0..2  | 3 |

Consequences:
- The race set is bits 3..18: the 16 channels seen in `GetScanResult` (raceband R1..R8 = 5658/5695/5732/5769/5806/5843/5880/5917 MHz, plus 8 low-band 5620..5340 MHz).
- The non-race set is bits 0..2: **three different channels (table indices 0,1,2)** that are NOT part of the race set, so their frequencies are not in any race-mode scan we have captured. **[open]** capture a scan while in a non-race mode to read the three normal-mode frequencies.

## How a mode is selected

- **Marker file** `/usrdata/lowdelay/fastmode`: present = race, absent = non-race. `FastModeEn` becomes 3 when present, 0 when absent. **[confirmed]**
- **`/usrdata/lowdelay/modechange`**: touching it triggers a channel re-scan. **[inferred]**
- **IPC**: `AR_LOWDELAY_MID_RX_SetFastMode` (cmd `0x3c`, one byte) sets it; `AR_LOWDELAY_MID_RX_GetFastMode` (cmd `0x3d`) reads it back. On-device, `GetFastMode` returned `1` while the marker file was present (race), and the file was absent in a non-race mode. **[confirmed]**
- Under the hood `SetFastMode` reaches `AR_AR8030_RX_SetMcsMode`, which issues `bb_ioctl(..., 0x200000c, ...)` with the MCS mode in the high byte of a u16. **[inferred]**
- `AR_LOWDELAY_MID_RX_SetChnMode` and `AR_LOWDELAY_MID_SET_Tranmit_Param` (bitrate/power, cmd 0x24) are the other relevant setters. **[confirmed they exist]**

### Out-of-range values crash the firmware [confirmed, the hard way]

`SetFastMode` does no input validation. Sending an out-of-range byte during probing crashed `ar_lowdelay` and the watchdog rebooted the goggle (which reverts our runtime bind-mount back to stock, and the marker file persisted as absent = normal). Only a small set of byte values is valid. **Do not brute-force this on a live unit.** The safe values still need to be read out of the UI logic or probed very carefully with a timeout and an immediate restore.

## What each mode sets

| Mode | Channels | fastmode file | MCS / tuning | Use case |
|------|----------|---------------|--------------|----------|
| Race | 16 (`0x0007FFF8`) | present | highest rate, low latency | racing **[channels confirmed; rest inferred]** |
| Freestyle | 3 (`0x00000007`) | absent | balanced rate | general flying **[inferred]** |
| Long Range | 3 (`0x00000007`) | absent | lower/robust MCS, lower rate | distance **[inferred]** |

## Work mode (Race / Freestyle / Long Range), how it actually works

Confirmed by RE of both the goggle and the air-unit `ar_lowdelay` binaries:

- **The goggle does NOT set the work mode.** It only queries it (`GetFlightDynamicWorkMode`, cmd 0x43) and displays it (the OSD "STYMode" field, read straight from the air unit's link-status struct at +0x124). `SetChnMode` is a dead stub (logs "only manual support at now", returns 0xffffffff). **[confirmed]**
- **The air unit owns and applies the mode**, from a low-delay config blob sent goggle->air (`SetLdCfg`) plus `SET_Tranmit_Param` (cmd 0x24). **[confirmed]**
- **Band (fastmode) and work mode are INDEPENDENT.** Selecting a work mode does not touch the fastmode file; the earlier "race -> fastmode=1" was the stock UI setting both separately, not a firmware link. **[confirmed]**

What the work mode carries (the RF levers, the "RF power" suspicion is right):
- **TX power**, LD-config offset 0x68, applied via `AR_AR8030_TX_SET_POWER` (bb_ioctl 0x2000008) when the manual-power flag (sysinfo +0xa8) is set. **[confirmed]**
- **MCS / modulation**, `SetMcsMode` (bb_ioctl 0x200000c); default from JSON. **[confirmed]**
- **Bitrate**, `SET_Tranmit_Param` stores a 9-byte block (first byte a remapped bitrate/MCS-level code). **[confirmed]**
- **Bandwidth**, a 5 MHz baseband profile (`bb_config_gnd_5m.json`, the natural Long Range profile) exists but is present-yet-unused at boot: the boot script hardcodes the 10 MHz `bb_config_gnd.json` and nothing calls `SET_BANDWIDTH` at runtime. **[confirmed]**

**The exact per-mode value table (Race/Freestyle/LR -> {power, MCS, bitrate, bandwidth}) is NOT in these binaries**, it lives in the stock UI/menu app, which is present only as PLT thunks. **[open]** Recover it by decompiling that app, or by capturing what the stock ships over `SET_Tranmit_Param`/`SetLdCfg` per mode.

The remaining unknowns (per-mode `SetFastMode` values and out-of-range set, the three normal-mode channel frequencies, the per-mode MCS/TX-power/bitrate table, and the safe set sequence) need the quad powered and linked.

## Implications for our menu

- The Channel page already shows 16 (race) vs 3 (normal) from the `fastmode` file, but the **normal-mode frequencies in the no-link fallback are placeholders** (we reuse R1..R3) until item 2 above is captured.
- Wiring a Normal/Race switch into our menu is blocked on items 1 and 4: we must know the valid values and the exact safe sequence first, since a wrong value reboots the unit.
