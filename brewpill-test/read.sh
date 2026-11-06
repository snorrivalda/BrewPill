#!/bin/bash
# Read the test firmware's result struct over SWD without halting the CPU
cd "$(dirname "$0")"
O=~/.platformio/packages/tool-openocd
T=~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-
ADDR=0x$(${T}nm build/test.elf | awk '$3=="R"{print $1}')
$O/bin/openocd -s $O/openocd/scripts -f interface/stlink.cfg -f target/nordic/nrf51.cfg \
  -c init -c "echo \"R: [read_memory $ADDR 8 80]\"" -c shutdown 2>&1 | grep '^R:'
