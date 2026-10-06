#ifndef TRIGGER_H
#define TRIGGER_H

#include "pad.h"

// ======================================================
// Trigger
// ======================================================

void triggerInit();

// Phase 1: jalankan real-time trigger scanning sebagai
// FreeRTOS task (Core 1 / priority 23). Panggil SETELAH
// triggerInit() dan audioInit().

void triggerTaskStart();

// Real-time: scan ADC + updatePad (dipanggil task Core 1)

void triggerTask();

// Non-real-time: cetak laporan profiling trigger/audio.
// Dipanggil dari loop() (Core 1) supaya blocking Serial
// tidak mengganggu real-time path di Core 1.

void triggerProfileReport();

// Non-real-time: ringkasan playability 1 detik
// (DEBUG_PLAYABILITY). Dipanggil dari loop() (Core 1).

void playabilityReport();

void updatePad(
    Pad &pad,
    uint8_t padIndex
);

#endif // TRIGGER_H
