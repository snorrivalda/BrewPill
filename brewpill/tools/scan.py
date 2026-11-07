#!/usr/bin/env python3
"""Listen for Brew Pill broadcasts over Bluetooth. Needs: pip3 install bleak"""
import asyncio, struct
from bleak import BleakScanner


def on_adv(device, adv):
    data = adv.manufacturer_data.get(0xFFFF)
    if not data or len(data) < 16 or data[0] != 2:
        return
    pill_id = data[2:6].hex().upper()
    _, flags, temp, vdd, x, y, z = struct.unpack("<BBhHhhh", data[:2] + data[6:16])
    acc = "" if flags & 1 else "  (accelerometer not responding)"
    print(f"{pill_id}  RSSI {adv.rssi:4d}  x {x:+5d}  y {y:+5d}  z {z:+5d} mg  "
          f"{temp/100:5.2f} °C  {vdd} mV{acc}")


async def main():
    async with BleakScanner(on_adv):
        print("Scanning for BrewPill... Ctrl-C to stop")
        await asyncio.Event().wait()

asyncio.run(main())
