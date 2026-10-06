#include "midi.h"

#include "config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "USB.h"
#include "USBMIDI.h"

// ======================================================
// USB MIDI Device
// ======================================================

USBMIDI MIDI("DrumCore");

namespace
{
    constexpr uint32_t NOTE_OFF_DELAY_US = 5000;

    // --------------------------------------------------
    // Note stacking ring buffer
    // --------------------------------------------------
    //
    // Mengatasi masalah single-note pending.
    // Jika beberapa pad dipukul dalam <5ms, semua
    // note terkirim, bukan hanya yang terakhir.

    constexpr uint8_t NOTE_STACK_SIZE = 8;

    struct PendingNote
    {
        uint8_t note;
        uint32_t offTimeUs;
        bool active;
    };

    PendingNote noteStack[NOTE_STACK_SIZE];

    // ==================================================
    // MIDI TASK (Core 1 / priority 8)
    // ==================================================
    //
    // Non-real-time: USB MIDI. Core 1, priority rendah,
    // agar tidak bersaing dengan audio/trigger di Core 0.
    // USB stack ESP32-S3 berjalan di task terpisah; pemanggil
    // noteOn/noteOff dari task Core 1 aman.

    constexpr size_t MIDI_TASK_STACK_SIZE = 4096;
    constexpr UBaseType_t MIDI_TASK_PRIORITY = 8;

    TaskHandle_t midiTaskHandle = nullptr;

    void midiTaskLoop(void *)
    {
        for (;;)
        {
            midiTask();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ======================================================
// Initialization
// ======================================================

void midiInit()
{
    for (uint8_t i = 0; i < NOTE_STACK_SIZE; i++)
    {
        noteStack[i].active = false;
    }

    MIDI.begin();
    USB.begin();
}

// ======================================================
// MIDI Task
// ======================================================

void midiTask()
{
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        Pad &pad = pads[i];

        if (!pad.hitReady)
        {
            continue;
        }

        midiSendPad(pad);

        pad.hitReady = false;
    }

    // --------------------------------------------------
    // Process note-off stack (non-blocking, microsecond
    // resolution)
    // --------------------------------------------------

    const uint32_t nowUs = micros();

    for (uint8_t i = 0; i < NOTE_STACK_SIZE; i++)
    {
        PendingNote &n = noteStack[i];

        if (!n.active)
        {
            continue;
        }

        if ((nowUs - n.offTimeUs) >= NOTE_OFF_DELAY_US)
        {
            MIDI.noteOff(n.note, 0);
            n.active = false;
        }
    }
}

// ======================================================
// Send MIDI Note
// ======================================================
//
// Masukkan ke ring buffer, bukan single pending.

void midiSendPad(Pad &pad)
{
#if DEBUG_TRIGGER
    Serial0.println("MIDI SEND");
#endif

    MIDI.noteOn(
        pad.config.midiNote,
        pad.velocity
    );

    // Cari slot kosong di ring buffer
    for (uint8_t i = 0; i < NOTE_STACK_SIZE; i++)
    {
        if (!noteStack[i].active)
        {
            noteStack[i].note =
                pad.config.midiNote;

            noteStack[i].offTimeUs =
                micros();

            noteStack[i].active =
                true;

            return;
        }
    }

    // Ring buffer penuh — overwrite oldest
    uint32_t oldestAge = 0;
    uint8_t oldestIdx = 0;
    const uint32_t nowUs = micros();

    for (uint8_t i = 0; i < NOTE_STACK_SIZE; i++)
    {
        const uint32_t age =
            nowUs - noteStack[i].offTimeUs;

        if (i == 0 || age > oldestAge)
        {
            oldestAge = age;
            oldestIdx = i;
        }
    }

    // Kirim note-off dulu untuk slot yang di-overwrite
    if (noteStack[oldestIdx].active)
    {
        MIDI.noteOff(
            noteStack[oldestIdx].note,
            0
        );
    }

    noteStack[oldestIdx].note =
        pad.config.midiNote;

    noteStack[oldestIdx].offTimeUs =
        nowUs;

    noteStack[oldestIdx].active =
        true;
}

// ======================================================
// MIDI Task Start (Core 1 / priority 8)
// ======================================================

void midiTaskStart()
{
    if (midiTaskHandle != nullptr)
    {
        return;
    }

    // Pin ke Core 1 (non-real-time). Priority 8 sesuai
    // Phase 1 target. Tidak memblok Core 0.

    const BaseType_t created = xTaskCreatePinnedToCore(
        midiTaskLoop,
        "MidiTask",
        MIDI_TASK_STACK_SIZE,
        nullptr,
        MIDI_TASK_PRIORITY,
        &midiTaskHandle,
        1
    );

    if (created != pdPASS)
    {
        midiTaskHandle = nullptr;
        Serial0.println("ERROR: MIDI task creation failed");
    }
}
