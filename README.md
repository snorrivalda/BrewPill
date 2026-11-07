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

The pill broadcasts raw readings only: its ID, acceleration (X/Y/Z), die
temperature and supply voltage. The app and the gateway compute tilt, gravity
and corrected temperature themselves, so the capsule axis, calibration and
temperature offset are all set from the Calibrate tab with the pill sealed.
**Send to gateway** copies them to the gateway. The ST-Link is only needed to
change the pill's measurement interval ("Write to pill" or `tools/config.py`).

Hardware notes: LIS3DH on I2C P0.06 (SDA) / P0.07 (SCL) at 0x19; 16 MHz and
32.768 kHz crystals; the nRF51 needs ≤ 3.6 V, so put an LDO between a LiPo and the board.

## Wi-Fi gateway (ESP32)

```bash
cd gateway && ~/.platformio/penv/bin/pio run -t upload
```

On first boot it opens the Wi-Fi network **BrewPill-Setup** (password `brewpill`).
Join it from a phone; the setup page opens (or browse to http://192.168.4.1).
Enter your Wi-Fi, Brewfather Custom Stream URL and/or webhook URL (pre-filled with
`https://bruko.net/api/brewpill`; clear it to turn the webhook off) with an optional
secret, sent as `Authorization: Bearer <secret>`. Afterwards the
status and settings page is at http://brewpill-gw.local. If the gateway is offline
for a minute, the setup network opens again.

The gateway stores a calibration per pill (keyed by the pill ID in each
broadcast, the last 8 hex digits of its Bluetooth address) and computes the
gravity it forwards. Calibrate in BrewPill.app and press **Send to gateway**.
Until then the pill is reported as uncalibrated, with gravity from a placeholder
curve. Stored calibrations are listed on the status page, and are also at
`GET /api/cal`. `POST /api/cal` takes form fields `device`, `axis` (x/y/z),
`p0`..`p3` and `temp_offset`, or `device` with `delete=1`.
Brewfather accepts one reading per 15 minutes; the webhook interval is configurable.
To check a webhook, press **Send test webhook** on the status page (or type `t` in
the serial monitor). It posts one reading marked `"test": true`, using the latest
pill's values or sample ones, and shows the HTTP status and reply.
