#include "storage.h"
#include "config.h"
#include "epd_driver.h"

#include <Preferences.h>

static Preferences prefs;
static uint16_t g_idleMin = IDLE_MINUTES_DEFAULT;
static uint8_t g_panelRev = 1;

static uint16_t clampIdle(uint16_t minutes) {
    if (minutes > IDLE_MINUTES_MAX) return IDLE_MINUTES_MAX;
    return minutes;
}

void storageBegin() {
    prefs.begin("inksight", true);
    g_idleMin = clampIdle(prefs.getUShort("idle_min", IDLE_MINUTES_DEFAULT));
    g_panelRev = prefs.getUChar("epd_rev", 1) ? 1 : 0;
    prefs.end();
    epdSetHuaweiRev(g_panelRev);
    Serial.printf("[NVS] idle=%u min panel=%s\n", g_idleMin, epdHuaweiRevName());
}

uint16_t storageIdleMinutes() { return g_idleMin; }

void storageSetIdleMinutes(uint16_t minutes) {
    g_idleMin = clampIdle(minutes);
    prefs.begin("inksight", false);
    prefs.putUShort("idle_min", g_idleMin);
    prefs.end();
    Serial.printf("[NVS] idle set to %u min\n", g_idleMin);
}

uint8_t storagePanelRev() { return g_panelRev; }

void storageSetPanelRev(uint8_t rev) {
    g_panelRev = rev ? 1 : 0;
    epdSetHuaweiRev(g_panelRev);
    prefs.begin("inksight", false);
    prefs.putUChar("epd_rev", g_panelRev);
    prefs.end();
}
