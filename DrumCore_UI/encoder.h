#ifndef ENCODER_H
#define ENCODER_H

#include <Arduino.h>

void encoderInit();

void encoderUpdate();

int8_t encoderGetDelta();

// Short press: a click released before the long-press threshold.
// Returns (and clears) a single press event.
bool encoderPressed();

// Long press: button held past ENCODER_LONG_PRESS_MS. Emitted exactly once
// per hold. A long press never also produces a short press.
bool encoderLongPressed();

// Atomically clear any pending rotation delta, short-press and long-press
// event. Use at deterministic state transitions so a leftover event from a
// previous state is never processed by the next state. Does not disable the
// rotary interrupt.
//
// If called while the button is still physically held (e.g. right after a
// long press triggered a state change), the encoder goes to "CONSUMED":
// the rest of that hold is ignored and NO short press is generated when the
// button is finally released. This prevents a release-click or a second
// long-press from firing in the freshly entered state.
void encoderFlush();

#endif