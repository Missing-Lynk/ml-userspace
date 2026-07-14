# The air unit (P1_SKY): access and firmware

The air-side (TX) unit, the flying counterpart to the goggle (`P1_GND`). Same SoC family, same firmware version (`1.0.44.rel`, build `09de6e7`), same userland. This is **validated on real air-unit hardware**: we have a direct root shell on it.

## TL;DR

- **Direct root shell over the air unit's own USB** (`192.168.3.100`, root / `artosyn`), the same USB-ethernet gadget the goggle exposes. This is the working access path. No RF link, no firmware decryption needed. Plug the air unit's USB into a PC (it can run from USB; note its main power is the flight battery, so on a bench plug expect limited time).
- The air unit has a **latent RTSP server** (`libar_minirtsp.so` is present, `ar_minirtsp` is not running, no `:554` listener), the same situation the goggle was in before our patch. This is the path to **source-quality** H.265 off the camera.
- It has the **same persistence surface as the goggle**: `run.sh` runs `/usrdata/run_dbg.sh` if present, and `/usrdata` is writable ubifs, so the MissingLynk boot-hook approach (patches-and-deployment.md) works on the air too.

## Naming

`SKY` = air unit (camera + VTX, transmits). `GND` = goggle (receives + displays). Both images ship together, same version/hash. Build host paths: `/home/glzhou/workspace/haming/air/ar9401sw/...` (air) vs the goggle's ground SDK.

## Reaching the air unit

1. **Its own USB gadget (PRIMARY, validated).** Plug the air unit's USB into the host. It enumerates a USB-ethernet gadget exactly like the goggle (a new `enx*`, gadget at `192.168.3.100`). Run `host-setup/net-up.sh` (it now identifies goggle vs air by `product_version`), then `ssh root@192.168.3.100` (password `artosyn`, legacy crypto, same as `missinglynk/connection.py`). Confirmed identity: `product_version P1_SKY` (`/usr/usrdata/product/config.json`), `ar_lowdelay -m 2 -t 1` (`-t 1` = Tx), `auto_sync -sky`, hostname `art_sirius` (shared with the goggle, so use `product_version`/`-t` to tell them apart, not hostname).
2. **Over the RF link from the goggle - a REAL root shell (validated, corrects an earlier wrong note).** Once the RF link is associated, the goggle's `sdio0` (`10.0.0.1`) reaches the air at `10.0.0.100`, and **the air's SSH (`:22`) IS open over the link.** The earlier "ping only, not a shell" conclusion was wrong: the failure was only that the goggle cannot *originate* the connection itself - it has no `ssh`/`nc` client, and the vendor `run.sh`'s `iptables :8822 -> 10.0.0.100:22` DNAT silently fails because `iptables` is not installed on either slot. The fix is a tiny userspace TCP forwarder, `libre/tools/ml-tcprelay` (static aarch64), run on the goggle: `ml-tcprelay 8822 10.0.0.100 22 &`. Then from the host: `ssh -p 8822 -o KexAlgorithms=+diffie-hellman-group14-sha1 -o HostKeyAlgorithms=+ssh-rsa -o Ciphers=+aes128-ctr,aes128-cbc,3des-cbc -o MACs=+hmac-sha1 root@192.168.3.100` (root / `artosyn`). This works from **either** goggle slot (A validated; B is the same, since Alpine also lacks `iptables` - the relay is the portable path), so we can read the air's live state **while it is associated with our open slot-B stack**. It is the decisive way to see whether the air is actually transmitting: watch its `sdio0` **TX** counter (`/proc/net/dev`; streaming = ~1 MB/s+, flat = not sending) and its `ar_lowdelay -t 1` TX-FSM log in `/tmp/usrlog/*.log` (`u8VideoOn=%u`, `LinkDown u8VideoOn=%u`, `LinkUp`, `media`, `LOCK -> CONNECT`). This is the primary path when the air is powered by the drone in flight configuration (no bench USB). Requires the RF link up (goggle stack associated + air on); before association `10.0.0.100` is unreachable.
3. **WiFi**: unconfirmed and probably absent (the goggle has no radio, `hardware-overview.md`). The live air unit showed only `:22` and `:50000` listening, no AP services.

> Both units share the same gadget IP (`192.168.3.100`) and hostname (`art_sirius`); only plug in one at a time. `net-up.sh` prints which one answered.

## What is on the air unit (from the live shell)

- BusyBox 1.25.0, dropbear (root / `artosyn`), `ar_lowdelay -m 2 -t 1` (TX low-delay), `auto_sync -sky`.
- `/usr/lib/libar_minirtsp.so` (143768 B) present; the RTSP server is **not running** (latent).
- `/usr/bin/dump_file` present (the file-transfer tool; on the goggle it is the missing client, the air runs the matching side). Also `ota_upgrade`, `vtx_start_report`.
- rootfs `/dev/root` = squashfs on `ubiblock0_0`, 15.5M on flash (34.5M decompressed, 629 inodes). `/usrdata` ubifs (rw) holds `lowdelay/` config; `/factory` ubifs.
- Pulled to `out/air-probe/airfs/`: `dump_file`, `ar_lowdelay` (Tx variant, distinct md5 from the goggle's), `libar_minirtsp.so`, `passwd`, `run.sh`. (A full rootfs mirror is pending a fresh battery, tar to a file on the air then `read_file` it, do not stream binary `tar` over the command channel.)

## Hardware (from the embedded DTB + symbols)

- **SoC**: Artosyn **Sirius/Proxima** family (the bundled DTB is a generic bring-up board, `compatible = "artosyn, sirius-demo"`; the live units report `artosyn,proxima-9311`, kernel Linux 4.9.38 aarch64, see `datasheets/soc-proxima-9311.md`).
- **Boot/storage**: QSPI with two SPI-NOR + one SPI-NAND. Console `serial0:115200n8` (the goggle's debug UART runs faster, 1.152 Mbaud, `docs/guides/serial-and-debug-access.md`).
- **RF link chip**: **AR8030** (AR803x family), loads the `bb_demo_gnd_d.img` baseband blob (+ `bb_config_gnd.json`) via `artosyn_sdio.ko`. The air<->goggle link rides this (goggle `sdio0`/`10.0.0.x`).
- **NPU**: on-chip CNN inference (`ar_cnn_infere`, `AR_MPI_NPU_LoadModel`). Purpose not determined.
- **Camera**: sensor VIN pipeline (`AR_LDRT_TX_VIN_*`). **Onboard DVR** to microSD (`mmcblk1`, `BETAFPV_*.mp4`) plus **Gyroflow IMU logging** (`imu_record_gcsv`).

## Software stack

- Same `ar_mpp` / `ar_lowdelay` MPP pipeline as the goggle but the **TX** side (`AR_LDRT_TX_*`).
- `libar_minirtsp` / `ar_minirtsp`: a dedicated RTSP server with a full HEVC RTP/RTSP stack (`DESCRIBE`/`SETUP`, `AR_RTSP_StartService`, `rtsp_startRtspServ`, HEVC-NALU parser).
- AAC audio codec (`ar_aacdec`). Root password `artosyn` (shared GND/SKY).

## Firmware image format + extraction

`P1_SKY_*.img` / `P1_GND_*.img` are Artosyn **`OTRA`** containers ("ARTO" byte-swapped), signed. The community **`fpv-wtf/ar-firmware-tools`** (`node otra.js extract <img> <dir>`) opens the container to a `rom.img` blob; its `otra_sirius.ksy` matches our images (the repo primarily targets the DJI Avatar, JFFS2/EXT4, so it does not handle the squashfs scramble below). Inside `rom.img`: a DTB, an LZ4 kernel, and an LZ4 SquashFS rootfs (magic at image offset `0x4B4233`).

**The OTA image squashfs does not `unsquashfs` directly.** The superblock's 32-bit fields are valid (629 inodes, 131072 block, LZ4) but the **64-bit pointer block (0x20-0x5F) is obfuscated**: the image packs the 8 table pointers into `0x20-0x47` with `0x48+` zeroed, while a valid superblock has them as `[u32 offset][0]*4`. This is a **custom pack/unpack applied at flash time, not a simple XOR** (computing a keystream from the goggle's known plaintext does not transfer to the air image). Verified by reading the goggle's live `/dev/root` (`ubiblock0_0`): the on-flash superblock is already de-obfuscated/valid, so the transform runs in the installer, not at read time. Data blocks are cleartext (LZ4).

**With live shell access this is moot for getting files**: read the mounted rootfs directly, or `dd` the rootfs partition and feed it to sasquatch (appendix). The installer that performs the de-obfuscation is `/usr/bin/artosyn_upgrade`.

## Keys / crypto

`/usr/bin/artosyn_upgrade` (the firmware flasher, links the full LibTomCrypt suite) carries the firmware crypto constants as hex-encoded ASCII strings. Recover them with `firmware/extract_keys.py` against a `firmware/bin/artosyn_upgrade` you pulled off a unit; the keys are not committed to the repo, only the extractor is.

- **Two 65-byte ECC public keys** (P-256 points, at file offsets ~0x91e10 and ~0x91f20) used to **verify** the firmware signature. The matching **private signing key is vendor-side, not on the device**, so firmware cannot be re-signed.
- **No secret symmetric key is present.** The other long hex constants are standard **NIST P-192 / P-256 / P-384 curve parameters** and AES test vectors from LibTomCrypt, not keys (for example `4FE342E2...51F5` is the P-256 generator Gy coordinate, a public constant). The extractor recognizes and filters these.
- This is consistent with the squashfs de-obfuscation being a **keyless pack/unpack** transform (see "Firmware image format"), not encryption.
- We do not need to forge the image anyway: both units give a live root shell, so we modify the running system via the `run_dbg.sh` hook (patches-and-deployment.md), not by re-flashing.
- Root login password is `artosyn` on both units.

## Why this matters for the RTSP goal

The goggle path (rtsp-reverse-engineering.md) decodes the RF feed and re-encodes it on `venc8`. The air unit is the **source**: enabling `libar_minirtsp` on the air (the same idea as patching `ar_lowdelay` on the goggle, and now doable with a live shell and the `run_dbg.sh` hook) would yield the camera's native H.265 with one fewer transcode. It is reachable over the air's USB gadget on the bench (`192.168.3.100`). Worth a dedicated investigation now that air access is solved.

## Appendix: building sasquatch (for a clean rootfs dump)

Stock `unsquashfs` (4.5.1 here) reads a de-obfuscated/live squashfs fine; `sasquatch` is the fallback for the LZ4 + vendor variants if needed. Build (Debian/Ubuntu, needs `liblz4-dev liblzma-dev zlib1g-dev build-essential`):

```sh
git clone https://github.com/devttys0/sasquatch && cd sasquatch
wget https://downloads.sourceforge.net/project/squashfs/squashfs/squashfs4.3/squashfs4.3.tar.gz
tar xzf squashfs4.3.tar.gz && cd squashfs4.3 && patch -p0 < ../patches/patch0.txt
cd squashfs-tools
# modern-gcc fixes: enable LZ4, disable LZO (no headers), relax -Werror, add -fcommon
sed -i 's/^#LZ4_SUPPORT = 1/LZ4_SUPPORT = 1/; s/^LZO_SUPPORT = 1/#LZO_SUPPORT = 1/' Makefile
sed -i 's/-Wall -Werror/-Wall -Wno-error -fcommon/' Makefile
make
./sasquatch -d out <rootfs.squashfs>   # a live/dd'd rootfs, not the OTA .img
```
