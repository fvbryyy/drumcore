#include "pad.h"

// ======================================================
// Global Pad Array
// ======================================================

Pad pads[PAD_COUNT];

// ======================================================
// Pad Configuration Layer (Phase 5)
// ======================================================

PadConfig padConfigs[PAD_COUNT];

const PadConfig& getPadConfig(uint8_t padIndex)
{
    return padConfigs[padIndex];
}

PadConfig& getPadConfigMutable(uint8_t padIndex)
{
    return padConfigs[padIndex];
}

// ======================================================
// Pad Initialization
// ======================================================

void padInit()
{
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        // ==============================================
        // Configuration layer (satu sumber)
        // ==============================================

        PadConfig &c = padConfigs[i];

        c.pin          = PAD_PINS[i];
        c.midiNote     = PAD_MIDI_NOTES[i];

        // Responsiveness: threshold lebih rendah (350 -> 300)
        // agar pukulan pelan lebih gampang terdeteksi.
        // releaseThreshold harus < threshold (300 -> 250)
        // untuk histeresis valid.
        c.threshold         = 300;
        c.releaseThreshold  = 250;

        c.peakTime          = 1200;
        c.releaseStableTime = 3000;

        // Base mask (ms) — runtime = adjustMaskTime()*1000
        c.maskBaseMs        = PAD_MASK_MS[i];

        c.sensitivity       = 100;
        c.curve             = 0;

        c.padType           = PAD_TYPE_GENERIC;
        c.sampleId          = i;   // default: pad i -> sample i
        c.chokeGroup        = 0;   // future-ready

        // ==============================================
        // Copy config -> runtime pad
        // ==============================================

        Pad &pad = pads[i];

        pad.config = c;

        // Nilai awal mask = base (ms) tanpa adjustment;
        // trigger engine menimpa dengan adjustMaskTime()*1000
        // per hit.
        pad.maskTime = PAD_MASK_MS[i];

        // ==============================================
        // Runtime Data
        // ==============================================

        pad.state          = TRIGGER_IDLE;

        pad.currentValue   = 0;
        pad.peak           = 0;
        pad.lastPeak       = 0;

        pad.stateTime      = 0;
        pad.releaseTime    = 0;
        pad.lastTriggerTime = 0;

        pad.velocity       = 0;

        pad.hitReady       = false;
        pad.noteOn         = false;
        pad.noteOffReady   = false;
    }
}
