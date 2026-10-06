#ifndef HARDWARE_H
#define HARDWARE_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// Hardware Initialization
// ======================================================

void hardwareInit();

// ======================================================
// ADC Cache + Round-Robin Scanner
// ======================================================

void adcCacheScan();
uint16_t readPadADC(uint8_t padIndex);

// ======================================================
// ADC Cache Age (untuk staleness profiling)
// ======================================================

uint32_t adcCacheAge(uint8_t padIndex);

// ======================================================
// Batch ADC (baca beberapa pad sekaligus saat idle)
// ======================================================

void adcCacheScanBatch(uint8_t maxReads);

// ======================================================
// Playability: gap maksimum antar pembacaan pad yang sama
// ======================================================

uint32_t adcPlayabilityMaxPadGapUs();

// ======================================================
// ADC Scan Profiling
// ======================================================

void adcScanProfileReport();

// ======================================================
// Phase 4 — Trigger Scan Timing Diagnostic accessors
// (self-contained counter/reset; dipanggil dari report
//  di Core 1 supaya real-time path Core 0 tetap bersih)
// ======================================================

#if DEBUG_ROLLING_TEST
uint32_t p4AdcReadMaxUs(uint8_t pad);
uint32_t p4AdcReadOverallMaxUs();
uint32_t p4ScanIntervalMinUs(uint8_t pad);
uint32_t p4ScanIntervalMaxUs(uint8_t pad);
void p4ResetScanTiming();
#endif

#endif
