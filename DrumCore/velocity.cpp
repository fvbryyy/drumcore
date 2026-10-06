#include "velocity.h"
#include <math.h>

// ======================================================
// Velocity Calculation (LUT, zero float saat runtime)
// ======================================================
//
// Lookup table 4096 entries (12-bit ADC range), dibangun
// sekali saat startup.

constexpr uint16_t MIN_PEAK = 300;
constexpr uint16_t MAX_PEAK = 3240;

static uint8_t velocityLut[4096];

void buildVelocityLut()
{
    for (uint32_t i = 0; i < 4096; i++)
    {
        if (i <= MIN_PEAK)
        {
            velocityLut[i] = 1;
            continue;
        }

        if (i >= MAX_PEAK)
        {
            velocityLut[i] = 127;
            continue;
        }

        // Normalisasi 0.0 - 1.0
        float norm =
            (float)(i - MIN_PEAK) /
            (float)(MAX_PEAK - MIN_PEAK);

        // Kurva eksponensial 0.7 via log/exp
        float curved = expf(0.7f * logf(norm));

        velocityLut[i] =
            (uint8_t)(1.0f + curved * 126.0f);
    }
}

uint8_t calculateVelocity(uint16_t peak)
{
    if (peak > 4095) peak = 4095;

    return velocityLut[peak];
}
