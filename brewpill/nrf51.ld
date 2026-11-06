/* nRF51822-QFAA: 256 KB flash, 16 KB RAM. The last flash page holds the config. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 255K
  CONFIG (r)  : ORIGIN = 0x0003FC00, LENGTH = 1K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}
ENTRY(Reset_Handler)
SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    *(.text*) *(.rodata*)
    *(.ARM.exidx*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH
  .data : AT(_etext) {
    _sdata = .; *(.data*); . = ALIGN(4); _edata = .;
  } > RAM
  .bss (NOLOAD) : {
    _sbss = .; *(.bss*) *(COMMON); . = ALIGN(4); _ebss = .;
  } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
