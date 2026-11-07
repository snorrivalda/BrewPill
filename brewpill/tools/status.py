#!/usr/bin/env python3
"""Print the pill's latest raw reading over SWD (ST-Link must be attached)."""
import struct
import ocd

FMT = "<IIIBBBxhhhhHxx"
raw = ocd.read_bytes(ocd.symbol("S"), struct.calcsize(FMT))
(magic, meas, bcast, accel_ok, lfxo, configured, x, y, z, temp, vdd) = struct.unpack(FMT, raw)
if magic != 0xB4E3B112:
    raise SystemExit(f"Firmware not running (magic {magic:#x})")

print(f"measurements {meas}   broadcasts {bcast}")
print(f"accelerometer {'ok' if accel_ok else 'NOT RESPONDING'}   "
      f"32 kHz {'crystal' if lfxo else 'RC fallback'}   "
      f"intervals {'from config page' if configured else 'defaults'}")
print(f"accel  x {x/1000:+.3f} g  y {y/1000:+.3f} g  z {z/1000:+.3f} g")
print(f"temp   {temp/100:.2f} °C (raw die temperature)")
print(f"supply {vdd} mV")
