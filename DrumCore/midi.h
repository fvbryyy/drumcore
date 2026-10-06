#ifndef MIDI_H
#define MIDI_H

#include "pad.h"

// ======================================================
// MIDI
// ======================================================

void midiInit();

// Phase 1: jalankan MIDI sebagai FreeRTOS task
// (Core 1 / priority 8). Panggil SETELAH midiInit().

void midiTaskStart();

void midiTask();

void midiSendPad(Pad &pad);

#endif // MIDI_H
