# Gateway-side calibration

## Goal

The pill broadcasts raw sensor data only. The ESP32 gateway stores each pill's
calibration and computes tilt, SG and corrected temperature. Calibrating no
longer needs an ST-Link: BrewPill.app sends the fit to the gateway over Wi-Fi.

## Pill firmware (`brewpill/`)

- Manufacturer data, payload version 2 (little-endian):
  `FF FF | 02 | flags | id[4] | temp | vdd | x | y | z` (18 bytes; 30-byte advertisement)
  - flags: bit 0 accel_ok, bit 2 lfxo (bit 1, calibrated, is gone)
  - id: the low 4 bytes of the BLE address in display order, so the hex string
    equals the last 8 digits of the MAC. macOS never exposes MAC addresses to
    apps, so this is how the app and the gateway agree on which pill is which.
  - temp: int16, raw die temperature, centi-°C (no offset)
  - vdd: uint16, mV
  - x, y, z: int16, mg
- The pill no longer computes tilt or SG. The Tilt iBeacon is removed.
- Config page at 0x3FC00 holds `{magic "BPC1", measure_s, adv_ms}`, the start
  of the v1 layout, so pages written by v1 keep their intervals. A blank page
  means defaults: 60 s, 2000 ms.
- `tools/config.py` writes only the intervals. `status.py` and `scan.py`
  follow the new status struct and payload.

## Gateway (`gateway/src/main.cpp`)

- Per-pill calibration `{axis 0..2, poly[4], temp_offset °C}` stored in
  Preferences namespace `cal`, keyed by the pill ID (8 uppercase hex digits).
  All of them are one blob (`all`), so a save is a single write.
- Accepts payload version 2 only. On each broadcast:
  `tilt = atan2(hypot(g[b], g[c]), |g[a]|)` for axis a, `sg = poly(tilt)`,
  `temp = raw + temp_offset`.
- No stored calibration: axis X, poly 25° → 1.000 / 75° → 1.100
  (p0 = 0.95, p1 = 0.002), offset 0, reported as `calibrated:false`.
- `POST /api/cal` form fields `device` (pill ID or full MAC), `axis` (x/y/z or 0..2),
  `p0..p3`, `temp_offset` stores a calibration. `delete=1` removes it. Replies with JSON `{"ok":…}`.
- `GET /api/cal` returns `{pill ID: {axis, poly, temp_offset}}` for all
  stored calibrations.
- The status page shows each pill's calibration state. It has a remove button
  for stored calibrations.
- Webhook and Brewfather payloads keep their current fields. The webhook gains
  `pill_id`.

## BrewPill.app

- `main.swift` parses version 2. Rows keep the CSV layout, with tilt and sg
  written as 0. The dashboard already computes both from x/y/z.
- Live readings carry `pillId` (not logged to CSV).
- New `POST /api/gateway-cal {host, device: pillId, axis, poly, tempOffset}`. The Swift
  server forwards it as a form POST to `http://<host>/api/cal`, which avoids
  CORS and mDNS issues in the web view.
- The Calibrate tab's last step is **Send to gateway**, with a host field
  (default `brewpill-gw.local`, saved in state). It refuses to send until a fit
  is in use, so the placeholder is never stored as a calibration. "Write to pill" remains for
  the intervals only. The Tilt colour is removed. `pillTempOffset` is only
  subtracted from rows logged by v1 firmware (rows with a nonzero tilt or sg).
  Without a calibration, v2 rows use the same placeholder curve as the gateway.
- Info.plist gains `NSLocalNetworkUsageDescription` for reaching the gateway.

## Verification

Each component must build: `make` in `brewpill/`, `pio run` in `gateway/` and
`app/build.sh`. A host-side C check compares the gateway's tilt/SG maths with
the app's JS formula on sample vectors. On-hardware testing is left to the user.
