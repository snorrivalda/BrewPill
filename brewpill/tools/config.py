#!/usr/bin/env python3
"""
Write the pill's settings and calibration to its config flash page over SWD.

Calibrate by floating the pill in a few liquids of known gravity, noting the
tilt from status.py (or the BLE broadcast) each time, then fitting:

  ./config.py --fit 24.8:1.000 33.1:1.020 41.0:1.040 52.7:1.070 --axis x --color red
"""
import argparse, os, struct, tempfile
import ocd

COLORS = ["off", "red", "green", "black", "purple", "orange", "blue", "yellow", "pink"]
CONFIG_ADDR = 0x3FC00


def fit(points, degree):
    """Least-squares polynomial fit via normal equations (no numpy needed)."""
    n = degree + 1
    A = [[sum(t ** (i + j) for t, _ in points) for j in range(n)] for i in range(n)]
    b = [sum(sg * t ** i for t, sg in points) for i in range(n)]
    for c in range(n):                      # Gaussian elimination
        p = max(range(c, n), key=lambda r: abs(A[r][c]))
        A[c], A[p], b[c], b[p] = A[p], A[c], b[p], b[c]
        for r in range(c + 1, n):
            f = A[r][c] / A[c][c]
            A[r] = [x - f * y for x, y in zip(A[r], A[c])]
            b[r] -= f * b[c]
    coef = [0.0] * n
    for r in reversed(range(n)):
        coef[r] = (b[r] - sum(A[r][k] * coef[k] for k in range(r + 1, n))) / A[r][r]
    return coef + [0.0] * (4 - n)


ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("--fit", nargs="+", metavar="TILT:SG", help="calibration points")
ap.add_argument("--degree", type=int, default=2, choices=[1, 2, 3])
ap.add_argument("--poly", type=float, nargs=4, metavar="P", help="set p0..p3 directly")
ap.add_argument("--axis", choices="xyz", default="x", help="capsule long axis on the board")
ap.add_argument("--color", choices=COLORS, default="red", help="Tilt iBeacon colour, or off")
ap.add_argument("--temp-offset", type=float, default=0.0, help="°C added to the die temperature")
ap.add_argument("--measure", type=int, default=60, help="seconds between readings")
ap.add_argument("--adv", type=int, default=2000, help="milliseconds between broadcasts")
args = ap.parse_args()

if args.fit:
    pts = [tuple(map(float, p.split(":"))) for p in args.fit]
    if len(pts) <= args.degree:
        raise SystemExit(f"Need at least {args.degree + 1} points for degree {args.degree}")
    poly = fit(pts, args.degree)
    for t, sg in pts:
        est = sum(c * t ** i for i, c in enumerate(poly))
        print(f"  tilt {t:6.2f}°  SG {sg:.4f}  fit {est:.4f}  error {est - sg:+.4f}")
elif args.poly:
    poly = args.poly
else:
    raise SystemExit("Give --fit points or --poly coefficients")

print("SG = " + " + ".join(f"{c:.6g}·t^{i}" for i, c in enumerate(poly)))
blob = struct.pack("<IHHBBh4f", 0x31435042, args.measure, args.adv, "xyz".index(args.axis),
                   COLORS.index(args.color), round(args.temp_offset * 100), *poly)

with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
    f.write(blob)
try:
    out = ocd.run(f"program {f.name} {CONFIG_ADDR:#x} verify reset")
finally:
    os.unlink(f.name)
print("Written and verified." if "Verified OK" in out else out[-800:])
