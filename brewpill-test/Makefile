TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-
OPENOCD   ?= $(HOME)/.platformio/packages/tool-openocd
CFLAGS = -mcpu=cortex-m0 -mthumb -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS = -T nrf51.ld -nostartfiles -Wl,--gc-sections

build/test.elf: main.c startup.c nrf51.ld
	@mkdir -p build
	$(TOOLCHAIN)gcc $(CFLAGS) $(LDFLAGS) main.c startup.c -o $@
	$(TOOLCHAIN)size $@

flash: build/test.elf
	$(OPENOCD)/bin/openocd -s $(OPENOCD)/openocd/scripts \
	  -f interface/stlink.cfg -f target/nordic/nrf51.cfg \
	  -c "program build/test.elf verify reset exit"

clean:
	rm -rf build
.PHONY: flash clean
