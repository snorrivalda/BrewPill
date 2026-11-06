# Brew Pill

A floating Bluetooth hydrometer built on a CJMCU-8223 (nRF51822 + LIS3DH).

| Folder | What it is |
|---|---|
| `brewpill/` | Pill firmware (bare-metal C). `make flash` programs it over the ST-Link. |
| `brewpill/tools/` | SWD and Bluetooth command-line tools: `status.py`, `config.py`, `scan.swift` |
| `app/` | BrewPill.app: Bluetooth logger, calibration and fermentation charts |
| `gateway/` | ESP32 firmware that forwards readings to Brewfather and/or a webhook over Wi-Fi |
| `brewpill-test/` | First bring-up test (I2C scan + accelerometer read) |

## App

```bash
app/build.sh && open app/build/BrewPill.app
```

The app window shows the dashboard; it is also at <http://localhost:8088> in any
browser on this Mac (menu: BrewPill > Open Dashboard in Browser). Readings and
settings live in `~/Library/Application Support/BrewPill/`.

The app recalculates tilt and gravity from the raw X/Y/Z in each broadcast, so
the capsule axis, calibration and temperature offset can all be set from the
Calibrate tab with the pill sealed. "Write to pill" is only needed for
Tilt-compatible receivers and requires the ST-Link.

Hardware notes: LIS3DH on I2C P0.06 (SDA) / P0.07 (SCL) at 0x19; 16 MHz and
32.768 kHz crystals; the nRF51 needs ≤ 3.6 V, so put an LDO between a LiPo and the board.

## Wi-Fi gateway (ESP32)

```bash
cd gateway && ~/.platformio/penv/bin/pio run -t upload
```

On first boot it opens the Wi-Fi network **BrewPill-Setup** (password `brewpill`).
Join it from a phone; the setup page opens (or browse to http://192.168.4.1).
Enter your Wi-Fi, Brewfather Custom Stream URL and/or webhook URL. Afterwards the
status and settings page is at http://brewpill-gw.local. If the gateway is offline
for a minute, the setup network opens again.

The gateway forwards the gravity the pill computes, so calibrate in BrewPill.app
and then use **Write to pill** (ST-Link) so Brewfather gets calibrated values.
Brewfather accepts one reading per 15 minutes; the webhook interval is configurable.
