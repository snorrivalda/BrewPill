/* Bit-banged I2C on the board's LIS3DH bus (P0.06 SDA, P0.07 SCL, 10k pull-ups on board) */
#include "hw.h"
#include "i2c.h"

#define SDA 6
#define SCL 7
/* Output, input connected, no pull (board has them), drive S0D1 (open drain) */
#define CNF_OPEN_DRAIN ((1u << 0) | (6u << 8))

static void delay(void) { for (volatile int i = 0; i < 20; i++); }

static void sda(int v) { if (v) GPIO_OUTSET = 1u << SDA; else GPIO_OUTCLR = 1u << SDA; }
static void scl(int v) { if (v) GPIO_OUTSET = 1u << SCL; else GPIO_OUTCLR = 1u << SCL; }
static int  sda_in(void) { return (GPIO_IN >> SDA) & 1; }

static void start(void) { sda(1); scl(1); delay(); sda(0); delay(); scl(0); delay(); }
static void stop(void)  { sda(0); delay(); scl(1); delay(); sda(1); delay(); }

static int write_byte(uint8_t b)
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

static uint8_t read_byte(int ack)
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

void i2c_init(void)
{
    GPIO_OUTSET = (1u << SDA) | (1u << SCL);
    PIN_CNF(SDA) = CNF_OPEN_DRAIN;
    PIN_CNF(SCL) = CNF_OPEN_DRAIN;
    /* Clock out any transfer a reset interrupted */
    for (int i = 0; i < 9; i++) { scl(0); delay(); scl(1); delay(); }
    stop();
}

int i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, int n)
{
    start();
    if (!write_byte(addr << 1) || !write_byte(reg)) { stop(); return 0; }
    start();
    if (!write_byte((addr << 1) | 1)) { stop(); return 0; }
    for (int i = 0; i < n; i++) buf[i] = read_byte(i < n - 1);
    stop();
    return 1;
}

int i2c_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    start();
    int ok = write_byte(addr << 1) && write_byte(reg) && write_byte(val);
    stop();
    return ok;
}
