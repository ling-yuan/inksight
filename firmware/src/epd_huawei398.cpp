#include "epd_driver.h"
#include "config.h"

#if defined(EPD_PANEL_398_HUAWEI)

static_assert(EPD_WIDTH == 768 && EPD_HEIGHT == 552,
              "Huawei 3.98 panel is 768x552");
// A0 and A1 share the glass size. Their controller handshake differs.
// Pixel buffer is InkSight raw 2bpp: row 0 is the top, 4 pixels per byte,
// MSB first, 0=black 1=white 2=yellow 3=red.
// Each row is mirrored horizontally before it is sent. BUSY is active-low.

static const int HW_W = 768;
static const int HW_H = 552;
static const int HW_ROW = HW_W / 4;          // 192
static const int HW_A1_GATES = 600;          // visible 552 + 48 border gates
static const int HW_A0_Y_BASE = -1;
static const unsigned long HW_BUSY_INIT_MS = 10000;
static const unsigned long HW_BUSY_REFRESH_MS = 45000;
static const unsigned long HW_BUSY_LONG_MS = 180000;

static uint8_t s_rev = 1;  // 0=A0, 1=A1

void epdSetHuaweiRev(uint8_t rev) {
    s_rev = rev ? 1 : 0;
    Serial.printf("[EPD] Huawei panel %s\n", s_rev ? "A1" : "A0");
}

uint8_t epdGetHuaweiRev() { return s_rev; }

const char *epdHuaweiRevName() { return s_rev ? "A1" : "A0"; }

static void spiWriteByte(uint8_t data) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(PIN_EPD_MOSI, (data & 0x80) ? HIGH : LOW);
        data <<= 1;
        digitalWrite(PIN_EPD_SCK, HIGH);
        digitalWrite(PIN_EPD_SCK, LOW);
    }
}

static void hwCmd(uint8_t cmd, const uint8_t *data, size_t len) {
    digitalWrite(PIN_EPD_DC, LOW);
    digitalWrite(PIN_EPD_CS, LOW);
    spiWriteByte(cmd);
    if (len > 0 && data) {
        digitalWrite(PIN_EPD_DC, HIGH);
        for (size_t i = 0; i < len; i++) spiWriteByte(data[i]);
    }
    digitalWrite(PIN_EPD_CS, HIGH);
}

static void hwCmd1(uint8_t cmd, uint8_t a) { hwCmd(cmd, &a, 1); }

static void hwCmd2(uint8_t cmd, uint8_t a, uint8_t b) {
    uint8_t d[2] = {a, b};
    hwCmd(cmd, d, 2);
}

static void hwCmd4(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c, uint8_t d0) {
    uint8_t d[4] = {a, b, c, d0};
    hwCmd(cmd, d, 4);
}

static void hwCmd6(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c,
                   uint8_t d0, uint8_t e, uint8_t f) {
    uint8_t d[6] = {a, b, c, d0, e, f};
    hwCmd(cmd, d, 6);
}

static void hwReset() {
    delay(20);
    digitalWrite(PIN_EPD_RST, LOW);
    delay(40);
    digitalWrite(PIN_EPD_RST, HIGH);
    delay(50);
}

// Returns elapsed milliseconds, or -1 on timeout. BUSY low means busy.
static int hwWaitBusy(unsigned long timeoutMs) {
    unsigned long t0 = millis();
    while (digitalRead(PIN_EPD_BUSY) == LOW) {
        delay(10);
        if (millis() - t0 > timeoutMs) {
            Serial.printf("[EPD] BUSY timeout %lu ms, level=%d\n",
                          timeoutMs, digitalRead(PIN_EPD_BUSY));
            return -1;
        }
    }
    return (int)(millis() - t0);
}

static void hwSetWindow(int width, int ys, int ye) {
    uint8_t d[9] = {
        0x00, 0x00,
        (uint8_t)(((width - 1) >> 8) & 0xFF), (uint8_t)((width - 1) & 0xFF),
        (uint8_t)((ys >> 8) & 0xFF), (uint8_t)(ys & 0xFF),
        (uint8_t)((ye >> 8) & 0xFF), (uint8_t)(ye & 0xFF),
        0x01,
    };
    hwCmd(0x83, d, sizeof(d));
}

static void hwMirror2bpp(uint8_t *dst, const uint8_t *src) {
    for (int i = 0; i < HW_ROW; i++) {
        uint8_t s = src[HW_ROW - 1 - i];
        dst[i] = (uint8_t)(((s & 0x03) << 6) | ((s & 0x0C) << 2) |
                           ((s & 0x30) >> 2) | ((s & 0xC0) >> 6));
    }
}

static void hwRowFromMono(uint8_t *dst, const uint8_t *image, int y) {
    uint8_t packed[HW_ROW];
    const int rowBytes = HW_W / 8;
    for (int x = 0; x < HW_W; x += 4) {
        uint8_t b = 0;
        for (int bit = 0; bit < 4; bit++) {
            int px = x + bit;
            bool isBlack = (image[y * rowBytes + px / 8] & (0x80 >> (px % 8))) == 0;
            b |= (uint8_t)((isBlack ? 0x00 : 0x01) << (6 - bit * 2));
        }
        packed[x / 4] = b;
    }
    hwMirror2bpp(dst, packed);
}

static void hwLoadRow(uint8_t *dst, const uint8_t *image, bool is2bpp, int y) {
    if (is2bpp) {
        hwMirror2bpp(dst, image + (size_t)y * HW_ROW);
    } else {
        hwRowFromMono(dst, image, y);
    }
}

static void hwSendRaw(const uint8_t *row) {
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    for (int i = 0; i < HW_ROW; i++) spiWriteByte(row[i]);
    digitalWrite(PIN_EPD_CS, HIGH);
}

static int hwInitA0() {
    uint8_t psr = 0x0B;
    uint8_t tres[4] = {
        (uint8_t)(HW_W >> 8), (uint8_t)(HW_W & 0xFF),
        (uint8_t)(HW_H >> 8), (uint8_t)(HW_H & 0xFF),
    };
    hwReset();
    if (hwWaitBusy(HW_BUSY_INIT_MS) < 0) return -1;
    delay(30);
    hwCmd(0x00, &psr, 1);
    hwCmd(0x61, tres, 4);
    hwCmd(0x04, nullptr, 0);
    if (hwWaitBusy(HW_BUSY_REFRESH_MS) < 0) return -1;
    return 0;
}

static int hwInitA1() {
    hwReset();
    if (hwWaitBusy(HW_BUSY_LONG_MS) < 0) return -1;
    delay(30);

    hwCmd6(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);
    hwCmd1(0x01, 0x3F);
    hwCmd2(0x00, 0x4B, 0x69);
    hwCmd4(0x05, 0x40, 0x1F, 0x1F, 0x2C);
    hwCmd4(0x08, 0x6F, 0x1F, 0x1F, 0x22);
    hwCmd4(0x06, 0x6F, 0x1F, 0x14, 0x14);
    hwCmd4(0x03, 0x00, 0x54, 0x00, 0x44);
    hwCmd2(0x60, 0x02, 0x00);
    hwCmd1(0x30, 0x08);
    hwCmd1(0x50, 0x3F);

    uint8_t tres[4] = {
        (uint8_t)(HW_W >> 8), (uint8_t)(HW_W & 0xFF),
        (uint8_t)(HW_A1_GATES >> 8), (uint8_t)(HW_A1_GATES & 0xFF),
    };
    hwCmd(0x61, tres, 4);
    hwCmd4(0x65, 0x10, 0x00, 0x20, 0x00);
    hwCmd1(0xE3, 0x2F);
    hwCmd1(0x84, 0x01);
    hwSetWindow(HW_W, 0, HW_A1_GATES - 1);
    hwCmd(0x04, nullptr, 0);
    if (hwWaitBusy(HW_BUSY_LONG_MS) < 0) return -1;
    return 0;
}

static int hwWriteA0(const uint8_t *image, bool is2bpp) {
    uint8_t row[HW_ROW];
    hwCmd(0x04, nullptr, 0);
    if (hwWaitBusy(HW_BUSY_REFRESH_MS) < 0) return -1;

    for (int j = 0; j < HW_H; j++) {
        int ys = (j < HW_H / 2) ? (j * 2) : (2 * (HW_H - j) + HW_A0_Y_BASE);
        if (ys < 0) ys = 0;
        if (ys > HW_H - 1) ys = HW_H - 1;
        hwLoadRow(row, image, is2bpp, j);
        hwSetWindow(HW_W, ys, ys);
        hwCmd(0x10, nullptr, 0);
        hwSendRaw(row);
    }
    return 0;
}

static int hwWriteA1(const uint8_t *image, bool is2bpp) {
    uint8_t row[HW_ROW];
    hwCmd(0x04, nullptr, 0);
    if (hwWaitBusy(HW_BUSY_REFRESH_MS) < 0) return -1;

    hwSetWindow(HW_W, 0, HW_H - 1);
    hwCmd(0x10, nullptr, 0);
    digitalWrite(PIN_EPD_DC, HIGH);
    digitalWrite(PIN_EPD_CS, LOW);
    for (int j = 0; j < HW_H; j++) {
        hwLoadRow(row, image, is2bpp, j);
        for (int i = 0; i < HW_ROW; i++) spiWriteByte(row[i]);
    }
    digitalWrite(PIN_EPD_CS, HIGH);
    return 0;
}

static int hwRefresh() {
    uint8_t z = 0x00;
    hwCmd(0x12, &z, 1);
    if (s_rev) {
        int elapsed = hwWaitBusy(HW_BUSY_LONG_MS);
        if (elapsed < 0) return -1;
        if (elapsed < 250) {
            Serial.printf("[EPD] A1 BUSY released after %d ms, wait 14s\n", elapsed);
            delay(14000);
        } else {
            delay(500);
        }
        return 0;
    }
    return hwWaitBusy(HW_BUSY_REFRESH_MS) < 0 ? -1 : 0;
}

static int hwPowerOff() {
    uint8_t z = 0x00;
    hwCmd(0x02, &z, 1);
    return hwWaitBusy(HW_BUSY_REFRESH_MS) < 0 ? -1 : 0;
}

static void hwShow(const uint8_t *image, bool is2bpp) {
    if (!image) return;
    int outH = s_rev ? HW_A1_GATES : HW_H;
    for (int attempt = 0; attempt < 3; attempt++) {
        unsigned long t0 = millis();
        Serial.printf("[EPD] %s attempt %d BUSY=%d\n",
                      epdHuaweiRevName(), attempt + 1, digitalRead(PIN_EPD_BUSY));
        int ok = s_rev ? hwInitA1() : hwInitA0();
        if (ok != 0) continue;
        ok = s_rev ? hwWriteA1(image, is2bpp) : hwWriteA0(image, is2bpp);
        if (ok != 0) continue;
        Serial.printf("[EPD] frame written %lu ms\n", millis() - t0);
        hwSetWindow(HW_W, 0, outH - 1);
        if (hwRefresh() != 0) continue;
        Serial.printf("[EPD] refresh done %lu ms\n", millis() - t0);
        hwPowerOff();
        return;
    }
    Serial.println("[EPD] display failed after 3 attempts");
}

void gpioInit() {
    pinMode(PIN_EPD_BUSY, INPUT);
    pinMode(PIN_EPD_RST, OUTPUT);
    pinMode(PIN_EPD_DC, OUTPUT);
    pinMode(PIN_EPD_CS, OUTPUT);
    pinMode(PIN_EPD_SCK, OUTPUT);
    pinMode(PIN_EPD_MOSI, OUTPUT);
    pinMode(PIN_CFG_BTN, INPUT_PULLUP);
    digitalWrite(PIN_EPD_RST, HIGH);
    digitalWrite(PIN_EPD_CS, HIGH);
    digitalWrite(PIN_EPD_DC, LOW);
    digitalWrite(PIN_EPD_SCK, LOW);
    Serial.printf("[EPD] Huawei 3.98 %dx%d pins MOSI=%d SCK=%d CS=%d DC=%d RST=%d BUSY=%d\n",
                  HW_W, HW_H, PIN_EPD_MOSI, PIN_EPD_SCK, PIN_EPD_CS,
                  PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY);
}

void epdInit() {
    Serial.println("[EPD] Huawei 3.98 ready");
}

void epdInitFast() { epdInit(); }

void epdDisplay(const uint8_t *image) { hwShow(image, false); }

void epdDisplay2bpp(const uint8_t *image2bpp) { hwShow(image2bpp, true); }

void epdDisplayFast(const uint8_t *image) { epdDisplay(image); }

void epdDisplayDeepClear(const uint8_t *image) { epdDisplay(image); }

bool epdSupportsPartialRefresh() { return false; }

void epdPartialDisplay(uint8_t *data, int xStart, int yStart, int xEnd, int yEnd) {
    (void)xStart; (void)yStart; (void)xEnd; (void)yEnd;
    epdDisplay(data);
}

void epdPartialDisplayWithOld(uint8_t *data, const uint8_t *oldData,
                              int xStart, int yStart, int xEnd, int yEnd) {
    (void)oldData;
    epdPartialDisplay(data, xStart, yStart, xEnd, yEnd);
}

void epdSleep() {
    uint8_t z = 0xA5;
    hwCmd(0x07, &z, 1);
    delay(200);
}

#endif
