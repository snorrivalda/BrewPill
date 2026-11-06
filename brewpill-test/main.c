/*
 * Brew Pill bring-up test for the CJMCU-8223 (nRF51822 + accelerometer).
 *
 * The board's pinout is undocumented, so this firmware:
 *   1. tries every GPIO pair as a bit-banged I2C bus and records which
 *      pairs have devices answering (and at which addresses),
 *   2. reads the common accelerometer ID registers,
 *   3. if it is an LIS3DH/LIS2DH (WHO_AM_I = 0x33), streams X/Y/Z.
 * Everything lands in the `R` struct, which the host reads over SWD.
 */
#include <stdint.h>

#define REG(a)       (*(volatile uint32_t *)(a))
#define GPIO_OUTSET  REG(0x50000508)
#define GPIO_OUTCLR  REG(0x5000050C)
#define GPIO_IN      REG(0x50000510)
#define PIN_CNF(n)   REG(0x50000700 + 4 * (n))

/* Output, input connected, pull-up, drive S0D1 (open drain) */
#define CNF_OPEN_DRAIN  ((1u << 0) | (3u << 2) | (6u << 8))
#define CNF_DEFAULT     (1u << 1)   /* input, buffer disconnected */

#define MAX_PAIRS 4
#define MAX_ADDRS 8
#define MAGIC     0xB4E3B111u

struct results {
    uint32_t magic;
    uint32_t state;                 /* 1 scanning, 2 no sensor, 3 streaming */
    uint32_t pairs_found;
    uint8_t  sda[MAX_PAIRS], scl[MAX_PAIRS], naddr[MAX_PAIRS], pad0;
    uint8_t  addr[MAX_PAIRS][MAX_ADDRS];
    uint8_t  accel_addr, id_0f, id_00, id_75;   /* ID regs of first device */
    uint8_t  id_0d, pad1[3];
    int16_t  x, y, z, pad2;                     /* raw, left-justified */
    uint32_t samples;
};
volatile struct results R;

static uint8_t p_sda, p_scl;

static void delay(void) { for (volatile int i = 0; i < 20; i++); }

static void sda(int v) { if (v) GPIO_OUTSET = 1u << p_sda; else GPIO_OUTCLR = 1u << p_sda; }
static int  sda_in(void) { return (GPIO_IN >> p_sda) & 1; }

static void scl(int v)
{
    if (v) {
        GPIO_OUTSET = 1u << p_scl;
        /* Honour clock stretching, but don't hang on a shorted pin */
        for (int t = 0; t < 100 && !((GPIO_IN >> p_scl) & 1); t++) delay();
    } else {
        GPIO_OUTCLR = 1u << p_scl;
    }
}

static void i2c_start(void) { sda(1); scl(1); delay(); sda(0); delay(); scl(0); delay(); }
static void i2c_stop(void)  { sda(0); delay(); scl(1); delay(); sda(1); delay(); }

static int i2c_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        sda((b >> i) & 1); delay();
        scl(1); delay(); scl(0);
    }
    sda(1); delay();
    scl(1); delay();
    int ack = !sda_in();
    scl(0); delay();
    return ack;
}

static uint8_t i2c_read(int ack)
{
    uint8_t b = 0;
    sda(1);
    for (int i = 0; i < 8; i++) {
        scl(1); delay();
        b = (b << 1) | sda_in();
        scl(0); delay();
    }
    sda(!ack); delay();
    scl(1); delay(); scl(0);
    sda(1); delay();
    return b;
}

static void i2c_recover(void)
{
    sda(1);
    for (int i = 0; i < 9; i++) { scl(0); delay(); scl(1); delay(); }
    i2c_stop();
}

static int probe(uint8_t addr)
{
    i2c_start();
    int ack = i2c_write(addr << 1);
    i2c_stop();
    return ack;
}

static int read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, int n)
{
    i2c_start();
    if (!i2c_write(addr << 1) || !i2c_write(reg)) { i2c_stop(); return 0; }
    i2c_start();
    if (!i2c_write((addr << 1) | 1)) { i2c_stop(); return 0; }
    for (int i = 0; i < n; i++) buf[i] = i2c_read(i < n - 1);
    i2c_stop();
    return 1;
}

static uint8_t read_reg(uint8_t addr, uint8_t reg)
{
    uint8_t v = 0xEE;   /* sentinel for "no answer" */
    read_regs(addr, reg, &v, 1);
    return v;
}

static void write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_start(); i2c_write(addr << 1); i2c_write(reg); i2c_write(val); i2c_stop();
}

/* P0.26/P0.27 are the 32 kHz crystal pins; leave them alone */
static int usable(int p) { return p != 26 && p != 27; }

static void scan(void)
{
    for (int p = 0; p < 32; p++)
        if (usable(p)) { GPIO_OUTSET = 1u << p; PIN_CNF(p) = CNF_OPEN_DRAIN; }

    for (int a = 0; a < 32 && R.pairs_found < MAX_PAIRS; a++) {
        for (int b = 0; b < 32 && R.pairs_found < MAX_PAIRS; b++) {
            if (a == b || !usable(a) || !usable(b)) continue;
            p_sda = a; p_scl = b;
            i2c_recover();
            uint8_t found[MAX_ADDRS];
            int n = 0, total = 0;
            for (uint8_t addr = 0x08; addr < 0x78; addr++) {
                if (probe(addr)) {
                    if (n < MAX_ADDRS) found[n++] = addr;
                    total++;
                }
            }
            /* A pin held low "acks" everything; a real bus has a few devices */
            if (total == 0 || total > MAX_ADDRS) continue;
            int k = R.pairs_found;
            R.sda[k] = a; R.scl[k] = b; R.naddr[k] = n;
            for (int i = 0; i < n; i++) R.addr[k][i] = found[i];
            R.pairs_found = k + 1;
        }
    }

    /* Release every pin except the first bus found */
    for (int p = 0; p < 32; p++)
        if (usable(p) && !(R.pairs_found && (p == R.sda[0] || p == R.scl[0])))
            PIN_CNF(p) = CNF_DEFAULT;
}

int main(void)
{
    R.magic = MAGIC;
    R.state = 1;
    scan();

    if (!R.pairs_found) { R.state = 2; for (;;); }

    p_sda = R.sda[0]; p_scl = R.scl[0];
    uint8_t a = R.addr[0][0];
    R.accel_addr = a;
    R.id_0f = read_reg(a, 0x0F);   /* LIS3DH/LIS2DH: 0x33 */
    R.id_00 = read_reg(a, 0x00);   /* BMA2xx: 0xF8/0xFA, ADXL345: 0xE5 */
    R.id_75 = read_reg(a, 0x75);   /* MPU-6050/9250: 0x68/0x71 */
    R.id_0d = read_reg(a, 0x0D);   /* MMA845x: 0x1A/0x2A */

    if (R.id_0f != 0x33) { R.state = 2; for (;;); }

    write_reg(a, 0x20, 0x57);   /* CTRL_REG1: 100 Hz, X/Y/Z on */
    write_reg(a, 0x23, 0x88);   /* CTRL_REG4: BDU, high resolution, ±2 g */
    R.state = 3;

    for (;;) {
        uint8_t d[6];
        if (read_regs(a, 0x28 | 0x80, d, 6)) {   /* OUT_X_L, auto-increment */
            R.x = (int16_t)(d[0] | d[1] << 8);
            R.y = (int16_t)(d[2] | d[3] << 8);
            R.z = (int16_t)(d[4] | d[5] << 8);
            R.samples++;
        }
        for (volatile int i = 0; i < 20000; i++);
    }
}
