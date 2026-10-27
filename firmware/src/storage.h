#ifndef INKSIGHT_STORAGE_H
#define INKSIGHT_STORAGE_H

#include <Arduino.h>

void storageBegin();
uint16_t storageIdleMinutes();
void storageSetIdleMinutes(uint16_t minutes);
uint8_t storagePanelRev();
void storageSetPanelRev(uint8_t rev);

#endif
