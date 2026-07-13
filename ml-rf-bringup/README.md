# ml-rf-bringup

One-shot AR8030 RF link bring-up for the ground side: release the AR8030 from reset, re-probe the SDIO host, download the baseband firmware into the chip, bring `sdio0` up as `10.0.0.1`. Static, no dependencies.

## Usage

```
ml-rf-bringup <fw_name> <cfg_name>
```

Both arguments are required. `ml-video` invokes it at boot as `ml-rf-bringup bb_demo_gnd_d.img bb_config_gnd.json.usr_cfg.json`.

## Sequence

| Phase | Action |
|---|---|
| guard | exit 0 if `artosyn_sdio` is already loaded (warm reload hangs the device) |
| reset release | pulse `ar-gpio1` line 0 (GPIO23) low then high via the GPIO chardev v2 ABI |
| re-probe | unbind/bind the RF host (`1b00000.mmc`) on `dwmmc_artosyn` |
| enumerate | poll the SDIO bus (up to 10 s) for the AR8030 (`device id 0x8030`) |
| firmware | `finit_module` `artosyn_sdio.ko` with `fw_name=`/`cfg_name=` |
| netdev | wait (up to 10 s) for `sdio0`, bring it up, set `10.0.0.1/24` |

A failed phase logs the reason and exits non-zero. Dependency modules (`ar_dtbo_sdio`, `artosyn_gpio`, `dw_mci-artosyn`) are loaded by the coldplug, not here.

## Environment overrides

- `ML_RF_FW_PATH` - kernel firmware loader search dir. Default: `/lib/firmware`.
- `ML_RF_KO` - `artosyn_sdio.ko` path. Default: `/lib/modules/$(uname -r)/kernel/artosyn_sdio.ko`.
