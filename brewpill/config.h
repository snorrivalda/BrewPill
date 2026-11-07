/*
 * Settings stored in the last flash page (0x3FC00) so they survive firmware
 * updates. Written from the host with tools/config.py. If the page is blank,
 * the defaults in main.c apply. Pages written by older firmware, which also
 * held calibration after these fields, still work: only the start is read.
 *
 * Calibration (capsule axis, gravity polynomial, temperature offset) is not
 * stored here: the gateway and BrewPill.app keep it and do the maths.
 */
#pragma once
#include <stdint.h>

#define CONFIG_MAGIC 0x31435042u   /* "BPC1" */

struct config {
    uint32_t magic;
    uint16_t measure_s;     /* seconds between sensor readings */
    uint16_t adv_ms;        /* milliseconds between broadcasts */
};

#define CONFIG ((const struct config *)0x0003FC00)
