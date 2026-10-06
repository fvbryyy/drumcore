#ifndef ADC_CALIBRATION_H
#define ADC_CALIBRATION_H

// ======================================================
// ADC Calibration — Auto-Capture Hardware Debug Tool
// ======================================================
//
// Tujuan: mengukur DATA dari 12 pad fisik prototype
// sebelum menentukan threshold atau velocity curve.
//
// Cara pakai:
//   1. Set DEBUG_ADC_CALIBRATION = true di config.h
//   2. Build & flash ESP32
//   3. Buka Serial Monitor 115200 baud
//   4. Diamkan pad → baseline otomatis terkumpul & dicetak
//   5. Pukul pad mana saja → [HIT] dan [CROSSTALK] muncul
//      otomatis, tidak perlu mengetik command apa pun
//
// DATA yang dikumpulkan (observer-only, non-realtime):
//   - Baseline idle: min, avg, max, range per pad
//   - Hit peak: setiap pukulan langsung dicetak
//   - Crosstalk: leak ke 11 pad lain per event
//
// Arsitektur:
//   - Dipanggil dari loop() (Core 1, non-realtime)
//   - Membaca readPadADC() — existing, non-blocking
//   - Tidak ada malloc/free, String, SD, atau delay
//   - Storage: static arrays, zero heap
//
// Compile-time ON/OFF via config.h:
//   constexpr bool DEBUG_ADC_CALIBRATION = true/false
//   false → seluruh kode compile-out, zero overhead
//
// PAD numbering: PAD01..PAD12 = index 0..11
// GPIO: dari PAD_PINS[i] di config.h (single source)
// ======================================================

#include "config.h"

#if DEBUG_ADC_CALIBRATION

#include <Arduino.h>

// ======================================================
// Per-Pad Calibration Data
// ======================================================

struct PadCalData
{
    // Baseline (idle noise)
    uint16_t idleMin;
    uint16_t idleMax;
    uint32_t idleSum;
    uint32_t idleCount;

    // Hit peak accumulation
    uint16_t peakMax;     // largest peak ever seen on this pad
    uint16_t peakLast;    // peak of most recent hit

    // Crosstalk: max leak received from other pads' hits
    uint16_t xtalkMax;

    // Timing
    uint32_t lastEventUs;
};

// ======================================================
// Public API
// ======================================================

// Reset state, print startup banner.
// Call from setup() after hardwareInit().
void adcCalibrationInit();

// Auto-capture task: baseline collection, hit detection,
// crosstalk measurement. Call from loop() every iteration.
void adcCalibrationTask();

#endif // DEBUG_ADC_CALIBRATION

#endif // ADC_CALIBRATION_H
