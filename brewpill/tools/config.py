#!/usr/bin/env python3
"""
Write the pill's measurement and broadcast intervals to its config flash page
over SWD.

Calibration (capsule axis, gravity curve, temperature offset) no longer lives
on the pill: fit it in BrewPill.app's Calibrate tab and use "Send to gateway".

  ./config.py --measure 60 --adv 2000
"""
import argparse, os, struct, tempfile
import ocd

CONFIG_ADDR = 0x3FC00

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--measure", type=int, default=60, help="seconds between readings")
ap.add_argument("--adv", type=int, default=2000, help="milliseconds between broadcasts")
args = ap.parse_args()

blob = struct.pack("<IHH", 0x31435042, args.measure, args.adv)

with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
    f.write(blob)
try:
    out = ocd.run(f"program {f.name} {CONFIG_ADDR:#x} verify reset")
finally:
    os.unlink(f.name)
print("Written and verified." if "Verified OK" in out else out[-800:])
