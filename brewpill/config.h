/*
 * Settings stored in the last flash page (0x3FC00) so they survive firmware
 * updates. Written from the host with tools/config.py. If the page is blank,
 * the defaults in main.c apply and the pill reports itself as uncalibrated.
 */
#pragma once
#include <stdint.h>

#define CONFIG_MAGIC 0x31435042u   /* "BPC1" */

struct config {
    uint32_t magic;
    uint16_t measure_s;     /* seconds between sensor readings */
    uint16_t adv_ms;        /* milliseconds between broadcasts */
    uint8_t  axis;          /* capsule long axis: 0 = X, 1 = Y, 2 = Z */
    uint8_t  tilt_color;    /* Tilt-compatible iBeacon: 0 = off, 1..8 = Red..Pink */
    int16_t  temp_offset;   /* added to the die temperature, centi-°C */
    float    poly[4];       /* SG = p0 + p1·t + p2·t² + p3·t³, t = tilt in degrees */
};

#define CONFIG ((const struct config *)0x0003FC00)
