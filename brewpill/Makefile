TOOLCHAIN ?= $(HOME)/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-
OPENOCD   ?= $(HOME)/.platformio/packages/tool-openocd
OCD = $(OPENOCD)/bin/openocd -s $(OPENOCD)/openocd/scripts -f interface/stlink.cfg -f target/nordic/nrf51.cfg

SRC = main.c i2c.c radio.c startup.c
CFLAGS = -mcpu=cortex-m0 -mthumb -Os -g -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS = -T nrf51.ld -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -lm

build/brewpill.elf: $(SRC) *.h nrf51.ld
	@mkdir -p build
	$(TOOLCHAIN)gcc $(CFLAGS) $(SRC) $(LDFLAGS) -o $@
	$(TOOLCHAIN)size $@

# Only erases the pages the firmware occupies, so the config page survives
flash: build/brewpill.elf
	$(OCD) -c "program build/brewpill.elf verify reset exit"

clean:
	rm -rf build
.PHONY: flash clean
