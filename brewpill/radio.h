#pragma once
#include <stdint.h>

/* The high-frequency crystal must be running while advertising */
void radio_init(void);
void radio_advertise(const uint8_t *data, int len);
const uint8_t *radio_address(void);
