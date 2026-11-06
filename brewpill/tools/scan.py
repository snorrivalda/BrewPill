#!/usr/bin/env python3
"""Listen for Brew Pill broadcasts over Bluetooth. Needs: pip3 install bleak"""
import asyncio, struct
from bleak import BleakScanner


def on_adv(device, adv):
    data = adv.manufacturer_data.get(0xFFFF)
    if not data or len(data) < 16 or data[0] != 1:
        return
    _, flags, tilt, temp, vdd, sg, x, y, z = struct.unpack("<BBHhHHhhh", data[:16])
    cal = "" if flags & 2 else "  (uncalibrated)"
    print(f"{device.address}  RSSI {adv.rssi:4d}  tilt {tilt/100:6.2f}°  SG {sg/10000:.4f}  "
          f"{temp/100:5.2f} °C  {vdd} mV{cal}")


async def main():
    async with BleakScanner(on_adv):
        print("Scanning for BrewPill... Ctrl-C to stop")
        await asyncio.Event().wait()

asyncio.run(main())
