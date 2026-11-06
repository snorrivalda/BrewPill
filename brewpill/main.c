/*
 * Brew Pill firmware for the CJMCU-8223 (nRF51822 + LIS3DH).
 *
 * Every measure_s seconds the pill wakes the accelerometer, averages a burst
 * of readings, and turns the capsule's tilt into specific gravity using the
 * calibration polynomial from flash. Every adv_ms milliseconds it broadcasts
 * the latest reading as two BLE advertisements:
 *   - "BrewPill" manufacturer data with raw tilt, temperature, supply and SG,
 *   - optionally a Tilt-compatible iBeacon so Tilt receivers and apps see it.
 * In between it sleeps on the 32 kHz clock with everything else powered down.
 */
#include <math.h>
#include "hw.h"
#include "config.h"
#include "i2c.h"
#include "radio.h"

#define LIS3DH        0x19
#define STATUS_MAGIC  0xB4E3B112u

static const struct config defaults = {
    .magic       = CONFIG_MAGIC,
    .measure_s   = 60,
    .adv_ms      = 2000,
    .axis        = 0,
    .tilt_color  = 1,
    .temp_offset = 0,
    /* Placeholder until calibrated: 25° → 1.000, 75° → 1.100 */
    .poly        = { 0.950f, 0.002f, 0.0f, 0.0f },
};

/* Latest reading; the host reads this over SWD (tools/status.py) */
struct status {
    uint32_t magic;
    uint32_t measurements;
    uint32_t broadcasts;
    uint8_t  accel_ok, lfxo, calibrated, axis;
    int16_t  x_mg, y_mg, z_mg;
    int16_t  temp_c100;
    uint16_t tilt_c100;
    uint16_t vdd_mv;
    uint16_t sg_x10000;
    uint16_t pad;
};
volatile struct status S;

static const struct config *cfg;

/* ---- Clocks and sleep ------------------------------------------------- */

/* RTC1 ticks at 32768 / 33 ≈ 993 Hz, close enough to milliseconds */
#define LFCLK_HZ      32768u
#define RTC_PRESCALER 32

static void lfclk_start(void)
{
    CLOCK_LFCLKSRC = 1;                       /* 32.768 kHz crystal */
    CLOCK_EVENTS_LFCLKSTARTED = 0;
    CLOCK_TASKS_LFCLKSTART = 1;
    for (volatile uint32_t t = 0; t < 2000000 && !CLOCK_EVENTS_LFCLKSTARTED; t++);
    S.lfxo = CLOCK_EVENTS_LFCLKSTARTED != 0;
    if (!S.lfxo) {                            /* no crystal: fall back to RC */
        CLOCK_TASKS_LFCLKSTOP = 1;
        CLOCK_LFCLKSRC = 0;
        CLOCK_EVENTS_LFCLKSTARTED = 0;
        CLOCK_TASKS_LFCLKSTART = 1;
        while (!CLOCK_EVENTS_LFCLKSTARTED);
    }
    RTC1_PRESCALER = RTC_PRESCALER;
    RTC1_INTENSET = 1u << 16;                 /* COMPARE0 pends the IRQ... */
    SCB_SCR |= 1u << 4;                       /* ...which wakes WFE (SEVONPEND) */
    RTC1_TASKS_START = 1;
}

/*
 * Clear COMPARE0 and the RTC1 pending bit. Clearing an event takes a few
 * cycles to reach the interrupt line, so read it back first; otherwise the
 * still-asserted line re-pends the IRQ, and with the bit stuck SEVONPEND
 * never fires again (it only signals on the not-pending -> pending edge).
 */
static void rtc_clear(void)
{
    RTC1_EVENTS_COMPARE0 = 0;
    (void)RTC1_EVENTS_COMPARE0;
    NVIC_ICPR = 1u << RTC1_IRQn;
}

static void sleep_ms(uint32_t ms)
{
    uint32_t ticks = ms * LFCLK_HZ / ((RTC_PRESCALER + 1) * 1000u);
    if (ticks < 2) ticks = 2;
    rtc_clear();                              /* before arming, so the edge is guaranteed */
    RTC1_CC0 = (RTC1_COUNTER + ticks) & 0xFFFFFF;
    while (!RTC1_EVENTS_COMPARE0) wfe();
    rtc_clear();
}

/*
 * Reset if a loop iteration stalls (a clock that never starts, a lost
 * wake-up, a fault): the pill is sealed, so rebooting is the only recovery.
 */
static void watchdog_start(uint32_t timeout_ms)
{
    WDT_CONFIG = 1;                           /* run in sleep, pause on halt */
    WDT_CRV = timeout_ms * LFCLK_HZ / 1000u;
    WDT_RREN = 1;
    WDT_TASKS_START = 1;
}

static void watchdog_feed(void) { WDT_RR0 = WDT_RELOAD; }

static void hfxo(int on)
{
    if (on) {
        CLOCK_EVENTS_HFCLKSTARTED = 0;
        CLOCK_TASKS_HFCLKSTART = 1;
        while (!CLOCK_EVENTS_HFCLKSTARTED);
    } else {
        CLOCK_TASKS_HFCLKSTOP = 1;
    }
}

/* ---- Sensors ---------------------------------------------------------- */

static int lis3dh_read(float *g)
{
    i2c_write(LIS3DH, 0x23, 0x88);            /* CTRL_REG4: BDU, high-res, ±2 g */
    i2c_write(LIS3DH, 0x20, 0x57);            /* CTRL_REG1: 100 Hz, X/Y/Z on */
    sleep_ms(100);                            /* high-res turn-on time is 7/ODR */

    int32_t sum[3] = { 0, 0, 0 };
    int n = 0;
    for (int tries = 0; n < 16 && tries < 64; tries++) {
        uint8_t st, d[6];
        if (!i2c_read(LIS3DH, 0x27, &st, 1)) break;
        if (!(st & 0x08)) { sleep_ms(5); continue; }     /* ZYXDA */
        if (!i2c_read(LIS3DH, 0x28 | 0x80, d, 6)) break;
        for (int i = 0; i < 3; i++) sum[i] += (int16_t)(d[2 * i] | d[2 * i + 1] << 8) >> 4;
        n++;
    }
    i2c_write(LIS3DH, 0x20, 0x00);            /* power down: ~0.5 µA */

    if (!n) return 0;
    for (int i = 0; i < 3; i++) g[i] = (float)sum[i] / n / 1000.0f;
    return 1;
}

static float die_temp_c(void)
{
    TEMP_EVENTS_DATARDY = 0;
    TEMP_TASKS_START = 1;
    while (!TEMP_EVENTS_DATARDY);
    int32_t t = (int32_t)TEMP_TEMP;           /* 0.25 °C steps */
    TEMP_TASKS_STOP = 1;
    return t * 0.25f + cfg->temp_offset / 100.0f;
}

static uint16_t vdd_mv(void)
{
    /* 10-bit, VDD/3 against the 1.2 V bandgap */
    ADC_CONFIG = 2 | (6u << 2);
    ADC_ENABLE = 1;
    ADC_EVENTS_END = 0;
    ADC_TASKS_START = 1;
    while (!ADC_EVENTS_END);
    uint32_t r = ADC_RESULT;
    ADC_ENABLE = 0;
    return r * 3600u / 1023u;
}

static void measure(void)
{
    float g[3];
    S.accel_ok = lis3dh_read(g);
    if (S.accel_ok) {
        /* Angle between the capsule's long axis and vertical */
        int a = cfg->axis, b = (a + 1) % 3, c = (a + 2) % 3;
        float tilt = atan2f(sqrtf(g[b] * g[b] + g[c] * g[c]), fabsf(g[a])) * (180.0f / 3.14159265f);
        const float *p = cfg->poly;
        float sg = p[0] + tilt * (p[1] + tilt * (p[2] + tilt * p[3]));
        S.x_mg = g[0] * 1000; S.y_mg = g[1] * 1000; S.z_mg = g[2] * 1000;
        S.tilt_c100 = tilt * 100 + 0.5f;
        S.sg_x10000 = sg > 0 ? sg * 10000 + 0.5f : 0;
    }
    S.temp_c100 = lroundf(die_temp_c() * 100);
    S.vdd_mv = vdd_mv();
    S.measurements++;
}

/* ---- Advertising payloads -------------------------------------------- */

static int put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; return 2; }

static int brewpill_packet(uint8_t *p)
{
    static const char name[] = "BrewPill";
    int n = 0;
    p[n++] = 1 + 8; p[n++] = 0x09;            /* complete local name */
    for (int i = 0; i < 8; i++) p[n++] = name[i];

    p[n++] = 3 + 16; p[n++] = 0xFF;           /* manufacturer data */
    n += put16(&p[n], 0xFFFF);                /* no company ID (test/DIY) */
    p[n++] = 1;                               /* payload version */
    p[n++] = (S.accel_ok << 0) | (S.calibrated << 1) | (S.lfxo << 2);
    n += put16(&p[n], S.tilt_c100);
    n += put16(&p[n], S.temp_c100);
    n += put16(&p[n], S.vdd_mv);
    n += put16(&p[n], S.sg_x10000);
    n += put16(&p[n], S.x_mg);
    n += put16(&p[n], S.y_mg);
    n += put16(&p[n], S.z_mg);
    return n;                                 /* 30 bytes */
}

/* Tilt hydrometer iBeacon: major = °F, minor = SG × 1000 */
static int tilt_packet(uint8_t *p)
{
    static const uint8_t uuid[16] = {
        0xA4, 0x95, 0xBB, 0x00, 0xC5, 0xB1, 0x4B, 0x44,
        0xB5, 0x12, 0x13, 0x70, 0xF0, 0x2D, 0x74, 0xDE,
    };
    int n = 0;
    p[n++] = 2; p[n++] = 0x01; p[n++] = 0x04;                 /* flags */
    p[n++] = 26; p[n++] = 0xFF; p[n++] = 0x4C; p[n++] = 0x00; /* iBeacon */
    p[n++] = 0x02; p[n++] = 0x15;
    for (int i = 0; i < 16; i++) p[n++] = uuid[i];
    p[3 + 6 + 3] = cfg->tilt_color << 4;                      /* A495BBx0 */
    uint16_t f = lroundf(S.temp_c100 * 0.018f + 32);
    uint16_t sg = (S.sg_x10000 + 5) / 10;
    p[n++] = f >> 8;  p[n++] = f;
    p[n++] = sg >> 8; p[n++] = sg;
    p[n++] = (uint8_t)-59;                                    /* RSSI at 1 m */
    return n;                                                 /* 30 bytes */
}

static void broadcast(void)
{
    uint8_t buf[31];
    hfxo(1);
    radio_advertise(buf, brewpill_packet(buf));
    /* Tilt receivers can't see the status flags, so only send gravity that
     * is both current and calibrated rather than 0, stale or placeholder SG */
    if (cfg->tilt_color >= 1 && cfg->tilt_color <= 8 && S.accel_ok && S.calibrated)
        radio_advertise(buf, tilt_packet(buf));
    hfxo(0);
    S.broadcasts++;
}

int main(void)
{
    S.magic = STATUS_MAGIC;
    S.calibrated = CONFIG->magic == CONFIG_MAGIC;
    cfg = S.calibrated ? CONFIG : &defaults;
    S.axis = cfg->axis;

    lfclk_start();
    i2c_init();
    radio_init();

    uint32_t adv_ms = cfg->adv_ms ? cfg->adv_ms : 2000;
    uint32_t every = cfg->measure_s * 1000u / adv_ms;
    if (!every) every = 1;

    /* One iteration is a measurement (< 1 s), a broadcast and adv_ms of sleep; 10 s slack */
    watchdog_start(adv_ms + 10000);

    for (uint32_t i = 0;; i++) {
        watchdog_feed();
        if (i % every == 0) measure();
        broadcast();
        sleep_ms(adv_ms);
    }
}
