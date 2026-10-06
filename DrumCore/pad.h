#ifndef PAD_H
#define PAD_H

#include <Arduino.h>
#include "config.h"

enum TriggerState
{
    TRIGGER_IDLE,
    TRIGGER_WAIT_PEAK,
    TRIGGER_MASK,
    TRIGGER_WAIT_RELEASE
};

// ======================================================
// Pad Type (metadata/config saja; belum ada behavior
// khusus di engine fase ini)
// ======================================================

enum PadType : uint8_t
{
    PAD_TYPE_GENERIC    = 0,
    PAD_TYPE_KICK       = 1,
    PAD_TYPE_SNARE      = 2,
    PAD_TYPE_TOM        = 3,
    PAD_TYPE_CYMBAL     = 4,
    PAD_TYPE_HIHAT      = 5,
    PAD_TYPE_PERCUSSION = 6
};

struct PadConfig
{
    uint8_t pin;
    uint8_t midiNote;

    // Trigger threshold & release (histeresis)
    uint16_t threshold;
    uint16_t releaseThreshold;

    // Peak window (µs) + release stable time (µs)
    uint16_t peakTime;
    uint32_t releaseStableTime;

    // Mask BASE (ms): runtime = adjustMaskTime(maskBaseMs,
    // velocity) * 1000. JANGAN dipakai langsung sebagai
    // mask runtime (satuan µs).
    uint32_t maskBaseMs;

    // Sensitivity: per-pad velocity scaling (100 = netral),
    // dipakai trigger engine sebelum velocity lookup.
    // curve masih metadata (belum dipakai).
    uint8_t sensitivity;
    uint8_t curve;

    // Generic mapping (Phase 5)
    uint8_t padType;    // PadType
    uint8_t sampleId;   // index sample terkait (default == padIndex)
    uint8_t chokeGroup; // future-ready (belum dipakai)
};

struct Pad
{
    PadConfig config;

    TriggerState state = TRIGGER_IDLE;

    uint16_t currentValue = 0;
    uint16_t peak = 0;
    uint16_t lastPeak = 0;

    uint32_t stateTime = 0;
    uint32_t releaseTime = 0;

    uint32_t maskTime = 0;        // runtime retrigger mask (µs)
    uint32_t lastTriggerTime = 0; // timestamp hit terakhir (untuk mask check)

    volatile uint8_t velocity = 0;

    // volatile = handoff lock-free lintas core: trigger
    // menulis velocity lalu set hitReady=true; MIDI task
    // membaca hitReady lalu velocity. Urutan tulis ini
    // yang membuat handoff aman tanpa lock.

    volatile bool hitReady = false;
    volatile bool noteOn = false;
    volatile bool noteOffReady = false;
};

extern Pad pads[PAD_COUNT];

// ======================================================
// Pad Configuration Layer (Phase 5)
// ======================================================
//
// Satu sumber konfigurasi pad (read-only saat runtime).
// padInit() copy ke pads[i].config untuk konsumsi
// real-time engine. Default dari tabel config.h
// (PAD_PINS / PAD_MIDI_NOTES / PAD_MASK_MS).

extern PadConfig padConfigs[PAD_COUNT];

const PadConfig& getPadConfig(uint8_t padIndex);
PadConfig& getPadConfigMutable(uint8_t padIndex);

void padInit();

#endif
