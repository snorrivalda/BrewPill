/* Minimal nRF51 register map: only what the Brew Pill uses */
#pragma once
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

#define CLOCK_TASKS_HFCLKSTART     REG(0x40000000)
#define CLOCK_TASKS_HFCLKSTOP      REG(0x40000004)
#define CLOCK_TASKS_LFCLKSTART     REG(0x40000008)
#define CLOCK_TASKS_LFCLKSTOP      REG(0x4000000C)
#define CLOCK_EVENTS_HFCLKSTARTED  REG(0x40000100)
#define CLOCK_EVENTS_LFCLKSTARTED  REG(0x40000104)
#define CLOCK_LFCLKSRC             REG(0x40000518)

#define RADIO_TASKS_TXEN       REG(0x40001000)
#define RADIO_EVENTS_DISABLED  REG(0x40001110)
#define RADIO_SHORTS           REG(0x40001200)
#define RADIO_PACKETPTR        REG(0x40001504)
#define RADIO_FREQUENCY        REG(0x40001508)
#define RADIO_TXPOWER          REG(0x4000150C)
#define RADIO_MODE             REG(0x40001510)
#define RADIO_PCNF0            REG(0x40001514)
#define RADIO_PCNF1            REG(0x40001518)
#define RADIO_BASE0            REG(0x4000151C)
#define RADIO_PREFIX0          REG(0x40001524)
#define RADIO_TXADDRESS        REG(0x4000152C)
#define RADIO_CRCCNF           REG(0x40001534)
#define RADIO_CRCPOLY          REG(0x40001538)
#define RADIO_CRCINIT          REG(0x4000153C)
#define RADIO_DATAWHITEIV      REG(0x40001554)
#define RADIO_OVERRIDE(n)      REG(0x40001724 + 4 * (n))

#define ADC_TASKS_START        REG(0x40007000)
#define ADC_EVENTS_END         REG(0x40007100)
#define ADC_ENABLE             REG(0x40007500)
#define ADC_CONFIG             REG(0x40007504)
#define ADC_RESULT             REG(0x40007508)

#define TEMP_TASKS_START       REG(0x4000C000)
#define TEMP_TASKS_STOP        REG(0x4000C004)
#define TEMP_EVENTS_DATARDY    REG(0x4000C100)
#define TEMP_TEMP              REG(0x4000C508)

#define RTC1_TASKS_START       REG(0x40011000)
#define RTC1_EVENTS_COMPARE0   REG(0x40011140)
#define RTC1_INTENSET          REG(0x40011304)
#define RTC1_COUNTER           REG(0x40011504)
#define RTC1_PRESCALER         REG(0x40011508)
#define RTC1_CC0               REG(0x40011540)
#define RTC1_IRQn              17

#define WDT_TASKS_START        REG(0x40010000)
#define WDT_CRV                REG(0x40010504)
#define WDT_RREN               REG(0x40010508)
#define WDT_CONFIG             REG(0x4001050C)
#define WDT_RR0                REG(0x40010600)
#define WDT_RELOAD             0x6E524635u

#define GPIO_OUTSET            REG(0x50000508)
#define GPIO_OUTCLR            REG(0x5000050C)
#define GPIO_IN                REG(0x50000510)
#define PIN_CNF(n)             REG(0x50000700 + 4 * (n))

#define FICR_OVERRIDEEN        REG(0x100000AC)
#define FICR_DEVICEADDR(n)     REG(0x100000A4 + 4 * (n))
#define FICR_BLE_1MBIT(n)      REG(0x100000EC + 4 * (n))

#define SCB_SCR                REG(0xE000ED10)
#define NVIC_ICPR              REG(0xE000E280)

static inline void wfe(void) { __asm volatile("wfe"); }
