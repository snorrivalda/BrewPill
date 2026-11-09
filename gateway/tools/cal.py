#!/usr/bin/env python3
"""Save pill calibrations to a file and upload them to a gateway.

  cal.py save FILE [--from app|GATEWAY]   read from BrewPill.app (default) or a gateway's /api/cal
  cal.py upload FILE GATEWAY              POST every calibration in FILE to GATEWAY's /api/cal
  cal.py show GATEWAY                     print what a gateway has stored

FILE is JSON in the same shape as GET /api/cal:
  {"A2A62E61": {"axis": 1, "poly": [p0, p1, p2, p3], "temp_offset": 0.5}}
Saved from the app, each entry also has "degree" and "points" (tilt, sg, brix, wcf,
t = Unix time, and manual / edited / off flags), the measurements the curve was fitted to.
GATEWAY is a host name or address, e.g. brewpill-gw.local or 192.168.1.216.
"""
import json
import sys
import urllib.parse
import urllib.request

APP = "http://localhost:8088"


def get(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.load(r)


def from_app():
    state = get(APP + "/api/state")
    live = get(APP + "/api/live")
    dev = state.get("device")
    ids = [d["pillId"] for d in live.get("devices", []) if d.get("pillId") and (not dev or d.get("device") == dev)]
    if not ids:
        sys.exit("BrewPill.app isn't hearing the selected pill, so its pill ID is unknown")
    poly = (list(state["poly"]) + [0, 0, 0, 0])[:4]
    # The measurement points and curve degree ride along for the record; upload ignores them
    points = [{k: p[k] for k in ("tilt", "sg", "brix", "wcf", "t", "manual", "edited", "off") if k in p} for p in state.get("points", [])]
    return {ids[0]: {"axis": "xyz".index(state.get("axis", "x")), "poly": poly,
                     "temp_offset": state.get("tempOffset", 0),
                     "degree": state.get("degree"), "points": points}}


def upload(cals, host):
    for pid, c in cals.items():
        form = {"device": pid, "axis": str(c["axis"]), "temp_offset": repr(float(c.get("temp_offset", 0)))}
        for i, p in enumerate((list(c["poly"]) + [0, 0, 0, 0])[:4]):
            form[f"p{i}"] = repr(float(p))
        req = urllib.request.Request(f"http://{host}/api/cal", urllib.parse.urlencode(form).encode())
        with urllib.request.urlopen(req, timeout=10) as r:
            print(pid, r.read().decode())


def main(a):
    if len(a) >= 2 and a[0] == "save":
        src = a[a.index("--from") + 1] if "--from" in a else "app"
        cals = from_app() if src == "app" else get(f"http://{src}/api/cal")
        with open(a[1], "w") as f:
            json.dump(cals, f, indent=2)
            f.write("\n")
        print(f"Saved {len(cals)} calibration(s) to {a[1]}: {', '.join(cals)}")
    elif len(a) == 3 and a[0] == "upload":
        upload(json.load(open(a[1])), a[2])
    elif len(a) == 2 and a[0] == "show":
        print(json.dumps(get(f"http://{a[1]}/api/cal"), indent=2))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
