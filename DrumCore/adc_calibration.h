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
//   1. Set #define DEBUG_ADC_CALIBRATION 1 di config.h
//   2. Build & flash ESP32
//   3. Buka Serial Monitor 115200 baud
//   4. Pukul pad → satu baris per pukulan, contoh:
//        HIT p03 pk420 : 12 25 420 38 15 20 18 22 19 31 28 24
//      12 angka = peak PAD01..PAD12 berurutan: peak source +
//      bocoran (crosstalk) ke semua pad lain langsung terbaca.
//   5. Copy barisnya untuk dihitung threshold/sensitivity.
//
// DATA: hanya angka mentah ADC (observer-only, non-realtime).
// Tidak ada auto-tuning, tidak mengubah parameter apapun.

// Arsitektur:
//   - Dipanggil dari loop() (Core 1, non-realtime)
//   - Membaca readPadADC() — existing, non-blocking
//   - Tidak ada malloc/free, String, SD, atau delay
//   - Storage: static arrays, zero heap
//
// Compile-time ON/OFF via config.h:
//   #define DEBUG_ADC_CALIBRATION 1  → aktif (fase tuning)
//   #define DEBUG_ADC_CALIBRATION 0  → seluruh kode compile-out,
//                                     zero overhead
// Kembalikan ke 0 setelah tuning selesai.
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
    // Baseline (idle noise) — dicadangkan untuk mode baseline
    uint16_t idleMin;
    uint16_t idleMax;
    uint32_t idleSum;
    uint32_t idleCount;

    // Hit peak accumulation
    uint16_t peakMax;     // largest peak ever seen on this pad
    uint16_t peakLast;    // peak of most recent hit

    // Crosstalk: max leak received from other pads' hits
    // (dicadangkan untuk mode crosstalk)
    uint16_t xtalkMax;

    // Timing
    uint32_t lastEventUs; // micros() of last hit (cooldown)
};

// ======================================================
// Public API
// ======================================================

// Reset state, print startup banner.
// Call from setup() after hardwareInit().
void adcCalibrationInit();

// Auto-capture task: hit detection + one-line peak report
// per hit. Call from loop() every iteration.
void adcCalibrationTask();

#endif // DEBUG_ADC_CALIBRATION

#endif // ADC_CALIBRATION_H
