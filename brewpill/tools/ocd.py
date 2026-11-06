"""Run OpenOCD against the nRF51 through the ST-Link."""
import os, re, subprocess

OPENOCD = os.path.expanduser("~/.platformio/packages/tool-openocd")
TOOLCHAIN = os.path.expanduser("~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-")
HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, "..", "build", "brewpill.elf")


def run(*commands):
    args = [f"{OPENOCD}/bin/openocd", "-s", f"{OPENOCD}/openocd/scripts",
            "-f", "interface/stlink.cfg", "-f", "target/nordic/nrf51.cfg", "-c", "init"]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "shutdown"]
    out = subprocess.run(args, capture_output=True, text=True)
    if "Examination succeed" not in out.stderr:
        raise SystemExit("Could not reach the nRF51 over SWD:\n" + out.stderr[-800:])
    return out.stderr + out.stdout


def read_bytes(addr, n):
    out = run(f'echo "MEM: [read_memory {addr:#x} 8 {n}]"')
    line = re.search(r"MEM: (.*)", out).group(1)
    return bytes(int(v, 16) for v in line.split())


def symbol(name):
    out = subprocess.run([TOOLCHAIN + "nm", ELF], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit(f"{name} not found in {ELF}; run make first")
