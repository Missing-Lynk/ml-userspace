# RF channel numbering

The same physical channel has several numbers. This doc states how they map and what the menu displays.

## The numbers, for one example channel (R8, 5917 MHz)

| name | value | what it is |
|---|---|---|
| **scan `number`** | 8 | The scan record's number field (record +8). Runs 1..15 across the channels, then **0 for the 16th**. |
| **ChnIdx** | 9 | What `SYS_GetInfo +0x3E` reports for the active channel. `ChnIdx = number + 1`. |
| **displayed "CH N"** | 8 | What the menu shows everywhere = the `number` itself (with `0` -> "CH 16"). |
| **raceband** | R8 | Name for this *frequency* (R1..R8 = 5658, 5695, …, 5917 MHz). Only the 8 raceband freqs have one. |
| frequency | 5917 MHz | The centre frequency. |

## The mapping: `ChnIdx = number + 1`

`SelectChn(number)` tunes to `ChnIdx = number + 1`, across all 16 channels:

| SelectChn(`number`) | freq | ChnIdx |
|---|---|---|
| 1 | 5658 (R1) | 2 |
| 5 | 5806 (R5) | 6 |
| 8 | 5917 (R8) | 9 |
| 15 | 5380 | 16 |
| 0 | 5340 | 1 |

Reading `ChnIdx` with `seltest` requires `test_uidesign` to be stopped first: a concurrent scan retunes the RX and corrupts the reading.

## What the menu displays + selects

- **Display ("CH N")**: the firmware `number` directly, with `0` -> 16, via `channel_display_from_number()` (`menu_config.h`). On a tile: its `number`. On the OSD: `number = ChnIdx - 1`, via `channel_display_number(chnidx)`. Both screens use the same scheme, so they agree. Result: R1=CH1 … R8=CH8, then the low band CH9…CH16.
- **Raceband in brackets** on each tile (e.g. "CH 8 (R8)") plus the frequency.
- **Active tile** (green border + label): the tile whose `number == ChnIdx - 1`.
- **Selection**: `select_channel()` sends the scan record's `number` verbatim (a single byte at offset 0; the wrapper places it at request +4). The firmware tunes to `ChnIdx = number + 1`.

## Scan + select orchestration

Reliable channel switching requires all of:

1. **Scan one-shot, never continuously.** `GetScanResult` retunes the RX through all channels, so a continuous loop races and reverts selects. The menu scans on screen-entry and manual refresh only. (The stock picker's repeating scan timer is inert for the same reason.)
2. **Queue the select onto the scan thread** (`g_pending_select`), not the UI thread. The UI thread and the scan thread both retune the radio; a select issued from the UI thread concurrently with a scan is lost.
3. **No scan right after a select.** `SelectChn` returns immediately but the tune is async; a scan-after runs `GetScanResult`, which restores the channel active at scan *entry* (the pre-select channel), reverting the switch. The OSD and active highlight refresh from the passive `SYS_GetInfo` poll; the grid SNR refreshes on the next screen entry.
4. **Validate scan data.** Concurrent binder transactions on the IPC (e.g. running `seltest`/`midsurvey` while the menu is live) cross replies and yield junk records (out-of-range `number`, out-of-band freq). The scan thread rejects records with `number > 15` or freq outside ~5000-6100 MHz and keeps the last good cache. Do not probe the IPC while `test_uidesign` is running.
5. The firmware scan handler (`0x45c108`) self-restores the active channel after sweeping (saves at `0x45c1a0`, re-tunes at `0x45c344`), so a one-shot scan is safe.

## The stock's numbering

The stock **OSD** shows the raw `ChnIdx` ("CH 9" for R8); the stock **picker** shows tile-index + 1 ("CH 8" for the 8th tile): two different numbers for the same channel. The menu uses one scheme (the firmware `number`) on both screens.

## RE / tooling provenance

- `SYS_GetInfo +0x3E` = ChnIdx: stock OSD QML (zlib RCC at file offset `0xd6211a`, `s24`).
- Select arg = a single byte = the value the handler reads at request +4: `liblowdelay_mid.so` wrapper `0x6084`, serializer `0x2014`; firmware SELECT handler `0x45cc08`; scan handler `0x45c108`.
- Mapping + select test: a local `seltest` diagnostic tool (not committed; built like qcap, `build-qcap/seltest`).

See also [mid-api.md](mid-api.md) and [rf-modes.md](rf-modes.md).
