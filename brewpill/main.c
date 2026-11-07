/*
 * Brew Pill firmware for the CJMCU-8223 (nRF51822 + LIS3DH).
 *
 * Every measure_s seconds the pill wakes the accelerometer and averages a
 * burst of readings. Every adv_ms milliseconds it broadcasts its ID and the
 * latest raw reading (acceleration, die temperature, supply) as "BrewPill"
 * manufacturer data. Tilt, gravity and the temperature correction are computed by the
 * receiver (the gateway or BrewPill.app), which holds the calibration.
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
};

/* Latest reading; the host reads this over SWD (tools/status.py) */
struct status {
    uint32_t magic;
    uint32_t measurements;
    uint32_t broadcasts;
    uint8_t  accel_ok, lfxo, configured, pad0;
    int16_t  x_mg, y_mg, z_mg;
    int16_t  temp_c100;               /* raw die temperature, no correction */
    uint16_t vdd_mv;
    uint16_t pad1;
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
    return t * 0.25f;
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
        S.x_mg = lroundf(g[0] * 1000);
        S.y_mg = lroundf(g[1] * 1000);
        S.z_mg = lroundf(g[2] * 1000);
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
    p[n++] = 2;                               /* payload version: raw readings */
    p[n++] = (S.accel_ok << 0) | (S.lfxo << 2);
    /* Pill ID: the address's low 4 bytes in display order, so it matches the
     * end of the MAC. macOS hides MACs, so the app keys calibrations by this. */
    const uint8_t *addr = radio_address();
    for (int i = 3; i >= 0; i--) p[n++] = addr[i];
    n += put16(&p[n], S.temp_c100);
    n += put16(&p[n], S.vdd_mv);
    n += put16(&p[n], S.x_mg);
    n += put16(&p[n], S.y_mg);
    n += put16(&p[n], S.z_mg);
    return n;                                 /* 30 bytes */
}

static void broadcast(void)
{
    uint8_t buf[31];
    hfxo(1);
    radio_advertise(buf, brewpill_packet(buf));
    hfxo(0);
    S.broadcasts++;
}

int main(void)
{
    S.magic = STATUS_MAGIC;
    S.configured = CONFIG->magic == CONFIG_MAGIC;
    cfg = S.configured ? CONFIG : &defaults;

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
