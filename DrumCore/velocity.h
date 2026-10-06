#ifndef VELOCITY_H
#define VELOCITY_H

#include <Arduino.h>

// ======================================================
// Velocity
// ======================================================

void buildVelocityLut();

uint8_t calculateVelocity(uint16_t peak);

#endif // VELOCITY_H
