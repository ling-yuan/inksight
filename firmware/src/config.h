#ifndef INKSIGHT_CONFIG_H
#define INKSIGHT_CONFIG_H

#include <Arduino.h>

#if defined(BOARD_PROFILE_ESP32_C3)
#define PIN_EPD_MOSI   6
#define PIN_EPD_SCK    4
#define PIN_EPD_CS     7
#define PIN_EPD_DC     1
#define PIN_EPD_RST    2
#define PIN_EPD_BUSY   10
#define PIN_CFG_BTN    9
#else
#error "Unsupported board profile"
#endif

#ifndef EPD_WIDTH
#define EPD_WIDTH  768
#endif
#ifndef EPD_HEIGHT
#define EPD_HEIGHT 552
#endif

static const int W = EPD_WIDTH;
static const int H = EPD_HEIGHT;
static const int COLOR_BUF_LEN = (W * H) / 4;

static const uint16_t IDLE_MINUTES_DEFAULT = 5;
static const uint16_t IDLE_MINUTES_MIN = 0;   // 0 = never sleep
static const uint16_t IDLE_MINUTES_MAX = 240;

extern uint8_t colorBuf[];

#endif
