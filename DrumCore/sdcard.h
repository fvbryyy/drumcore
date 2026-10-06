#ifndef SDCARD_H
#define SDCARD_H

#include <Arduino.h>

// ======================================================
// SD Card
// ======================================================

void sdInit();
void sdTask();

// Status SD
bool sdIsReady();

#endif // SDCARD_H