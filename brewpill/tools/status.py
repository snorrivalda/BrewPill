#!/usr/bin/env python3
"""Print the pill's latest reading over SWD (ST-Link must be attached)."""
import struct
import ocd

FMT = "<IIIBBBBhhhhHHHH"
raw = ocd.read_bytes(ocd.symbol("S"), struct.calcsize(FMT))
(magic, meas, bcast, accel_ok, lfxo, calibrated, axis,
 x, y, z, temp, tilt, vdd, sg, _) = struct.unpack(FMT, raw)
if magic != 0xB4E3B112:
    raise SystemExit(f"Firmware not running (magic {magic:#x})")

print(f"measurements {meas}   broadcasts {bcast}")
print(f"accelerometer {'ok' if accel_ok else 'NOT RESPONDING'}   "
      f"32 kHz {'crystal' if lfxo else 'RC fallback'}   "
      f"{'calibrated' if calibrated else 'UNCALIBRATED (default curve)'}")
print(f"accel  x {x/1000:+.3f} g  y {y/1000:+.3f} g  z {z/1000:+.3f} g   long axis {'XYZ'[axis]}")
print(f"tilt   {tilt/100:.2f}°")
print(f"SG     {sg/10000:.4f}")
print(f"temp   {temp/100:.2f} °C")
print(f"supply {vdd} mV")
