#include "config.h"
#include "epd_driver.h"
#include "storage.h"
#include "ble_link.h"

#include <esp_sleep.h>
#include <driver/gpio.h>

uint8_t colorBuf[COLOR_BUF_LEN];

static unsigned long s_lastActivityMs = 0;

void bleNoteActivity() {
    s_lastActivityMs = millis();
}

static unsigned long idleLimitMs() {
    uint16_t minutes = storageIdleMinutes();
    if (minutes == 0) return 0;  // 0 = never sleep
    return (unsigned long)minutes * 60UL * 1000UL;
}

static void waitButtonRelease() {
    while (digitalRead(PIN_CFG_BTN) == LOW) {
        delay(10);
    }
}

static void enterIdleSleep() {
    Serial.println("[SLEEP] idle timeout, disconnect and sleep");
    bleStop();
    epdSleep();

    waitButtonRelease();
    gpio_wakeup_enable((gpio_num_t)PIN_CFG_BTN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    Serial.println("[SLEEP] waiting for button");
    Serial.flush();
    esp_light_sleep_start();
    gpio_wakeup_disable((gpio_num_t)PIN_CFG_BTN);

    Serial.println("[WAKE] button");
    bleNoteActivity();
    bleStart();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== InkSight 398 BLE ===");

    gpioInit();
    epdInit();
    storageBegin();
    pinMode(PIN_CFG_BTN, INPUT_PULLUP);
    bleNoteActivity();
    bleStart();
}

void loop() {
    if (digitalRead(PIN_CFG_BTN) == LOW) {
        bleNoteActivity();
    }

    if (bleTakeShowRequest()) {
        Serial.println("[EPD] show image");
        epdDisplay2bpp(colorBuf);
        bleNotifyShowDone(true);
        bleNoteActivity();
    }

    unsigned long limit = idleLimitMs();
    if (limit != 0 && millis() - s_lastActivityMs >= limit) {
        enterIdleSleep();
    }
    delay(20);
}
