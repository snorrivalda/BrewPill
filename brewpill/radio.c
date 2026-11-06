/*
 * Bluetooth LE non-connectable advertising, driven straight from the RADIO
 * peripheral. A hydrometer only ever broadcasts, so no SoftDevice is needed.
 */
#include "hw.h"
#include "radio.h"

#define ADV_NONCONN_IND 0x02
#define TXADD_RANDOM    0x40

/* S0 (PDU header), LENGTH, then AdvA (6) + AdvData (≤ 31) */
static uint8_t pdu[2 + 6 + 31];

void radio_init(void)
{
    /* Older nRF51 parts need factory trim values copied in for BLE mode */
    if (!(FICR_OVERRIDEEN & (1u << 3))) {
        for (int i = 0; i < 4; i++) RADIO_OVERRIDE(i) = FICR_BLE_1MBIT(i);
        RADIO_OVERRIDE(4) = FICR_BLE_1MBIT(4) | 0x80000000u;
    }
    RADIO_MODE      = 3;                                 /* BLE 1 Mbit */
    RADIO_TXPOWER   = 4;                                 /* +4 dBm: beer eats signal */
    RADIO_PCNF0     = (1u << 8) | 8;                     /* S0 = 1 byte, LENGTH = 8 bits */
    RADIO_PCNF1     = 37 | (3u << 16) | (1u << 25);      /* maxlen, 3-byte base, whitening */
    RADIO_BASE0     = 0x89BED600;                        /* advertising access address */
    RADIO_PREFIX0   = 0x8E;                              /*   0x8E89BED6 */
    RADIO_TXADDRESS = 0;
    RADIO_CRCCNF    = 3 | (1u << 8);                     /* 24-bit, skip address */
    RADIO_CRCPOLY   = 0x00065B;
    RADIO_CRCINIT   = 0x555555;
    RADIO_SHORTS    = (1u << 0) | (1u << 1);             /* READY→START, END→DISABLE */
    RADIO_PACKETPTR = (uint32_t)pdu;

    /* Static random address from the factory-programmed device address */
    uint32_t lo = FICR_DEVICEADDR(0), hi = FICR_DEVICEADDR(1);
    for (int i = 0; i < 4; i++) pdu[2 + i] = lo >> (8 * i);
    pdu[6] = hi;
    pdu[7] = (hi >> 8) | 0xC0;
}

const uint8_t *radio_address(void) { return &pdu[2]; }

static void tx(int channel)
{
    static const uint8_t freq[3] = { 2, 26, 80 };   /* 2402, 2426, 2480 MHz */
    RADIO_FREQUENCY   = freq[channel - 37];
    RADIO_DATAWHITEIV = channel;
    RADIO_EVENTS_DISABLED = 0;
    RADIO_TASKS_TXEN = 1;
    while (!RADIO_EVENTS_DISABLED);
}

void radio_advertise(const uint8_t *data, int len)
{
    pdu[0] = ADV_NONCONN_IND | TXADD_RANDOM;
    pdu[1] = 6 + len;
    for (int i = 0; i < len; i++) pdu[8 + i] = data[i];
    for (int ch = 37; ch <= 39; ch++) tx(ch);
}
