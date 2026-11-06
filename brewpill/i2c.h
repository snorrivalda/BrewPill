#pragma once
#include <stdint.h>

void i2c_init(void);
int  i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, int n);
int  i2c_write(uint8_t addr, uint8_t reg, uint8_t val);
