#include "adc_calibration.h"

#if DEBUG_ADC_CALIBRATION

#include "hardware.h"

// ======================================================
// Simple ADC Hit Reporter
// ======================================================
//
// Sengaja dibuat SESIMPLE mungkin: setiap pukulan yang
// terdeteksi mencetak SATU BARIS berisi peak ADC ke-12 pad.
//
// Format:
//   HIT p03 pk420 : 12 25 420 38 15 20 18 22 19 31 28 24
//         ^         ^
//     pad yang      12 angka = peak PAD01..PAD12 berurutan
//     dipukul
//
// Cara pakai: pukul pad, copy barisnya, kirim ke yang
// menentukan settingan. Dari satu baris langsung terbaca:
// peak source + bocoran (crosstalk) ke semua pad lain.
//
// Tidak ada Serial input, tidak ada command, tidak ada
// baseline print berkala — hanya angka mentah ADC.

namespace
{
    // Threshold deteksi hit pada loop() non-realtime.
    // 30 dipilih supaya pad kecil (peak ADC 20-150) ikut
    // terdeteksi. Naikkan jika idle noise sering memicu
    // baris HIT tanpa ada pukulan.
    constexpr uint16_t CAL_HIT_THRESHOLD = 30;

    // Cooldown antara dua hit event dari pad yang sama (µs)
    // agar satu pukulan tidak tercatat dua kali.
    constexpr uint32_t HIT_COOLDOWN_US = 30000; // 30 ms

    // --------------------------------------------------
    // Per-pad state
    // --------------------------------------------------

    PadCalData g_calData[PAD_COUNT];

    // Hit detection state (non-realtime observer)
    bool     g_hitArmed[PAD_COUNT] = {};
    uint16_t g_hitPeak[PAD_COUNT]  = {};

    // Peak semua pad selama satu pukulan berlangsung.
    // Diisi saat hit terdeteksi (bukan setelah selesai)
    // karena peak bocoran mekanik terjadi bersamaan
    // dengan transient pukulan.
    bool     g_capActive          = false;
    uint8_t  g_capSource          = 0;
    uint16_t g_capPeak[PAD_COUNT] = {};

    // --------------------------------------------------
    // Capture: track peak ke-12 pad selama window pukulan
    // --------------------------------------------------

    void capStart(uint8_t source, uint16_t v)
    {
        g_capActive = true;
        g_capSource = source;
        for (uint8_t i = 0; i < PAD_COUNT; i++)
            g_capPeak[i] = readPadADC(i);
        g_capPeak[source] = v;
    }

    void capSample()
    {
        if (!g_capActive) return;
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            const uint16_t v = readPadADC(i);
            if (v > g_capPeak[i])
                g_capPeak[i] = v;
        }
    }

    void capStopAndPrint()
    {
        // Satu baris: HIT pNN pkNNNN : v1 v2 ... v12
        char line[160];
        int n = snprintf(line, sizeof(line), "HIT p%02u pk%u :",
                         (unsigned)(g_capSource + 1),
                         (unsigned)g_capPeak[g_capSource]);
        for (uint8_t i = 0; i < PAD_COUNT && n < (int)sizeof(line) - 6; i++)
        {
            n += snprintf(line + n, sizeof(line) - n, " %u",
                          (unsigned)g_capPeak[i]);
        }
        Serial0.println(line);

        g_capActive = false;
        g_capSource = 0;
    }

    // --------------------------------------------------
    // Hit detection
    // --------------------------------------------------

    void taskHitDetection()
    {
        const uint32_t nowUs = micros();

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            const uint16_t v = readPadADC(i);

            // Arm: pad melewati threshold
            if (!g_hitArmed[i])
            {
                if (v >= CAL_HIT_THRESHOLD)
                {
                    // Cooldown: jangan rearm terlalu cepat
                    const bool cooled =
                        (g_calData[i].lastEventUs == 0) ||
                        ((nowUs - g_calData[i].lastEventUs) >= HIT_COOLDOWN_US);

                    if (cooled)
                    {
                        g_hitArmed[i] = true;
                        g_hitPeak[i]  = v;

                        // Langsung mulai capture semua pad
                        if (!g_capActive)
                            capStart(i, v);
                    }
                }
                continue;
            }

            // Armed: track peak pad ini
            if (v > g_hitPeak[i])
                g_hitPeak[i] = v;

            // Close: ADC turun kembali di bawah threshold
            if (v < CAL_HIT_THRESHOLD)
            {
                const uint16_t peak = g_hitPeak[i];

                if (peak > g_calData[i].peakMax)
                    g_calData[i].peakMax = peak;
                g_calData[i].peakLast   = peak;
                g_calData[i].lastEventUs = nowUs;

                // Cetak satu baris & tutup capture
                // (hanya jika capture ini milik hit ini)
                if (g_capActive && g_capSource == i)
                    capStopAndPrint();

                g_hitArmed[i] = false;
                g_hitPeak[i]  = 0;
            }
        }
    }

} // namespace

// ======================================================
// Public: adcCalibrationInit
// ======================================================

void adcCalibrationInit()
{
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        g_calData[i]  = PadCalData{};
        g_hitArmed[i] = false;
        g_hitPeak[i]  = 0;
        g_capPeak[i]  = 0;
    }

    g_capActive = false;
    g_capSource = 0;

    Serial0.println();
    Serial0.println("[ADC-CAL] Pukul pad — satu baris per pukulan.");
    Serial0.println("[ADC-CAL] Format: HIT pNN pkNNNN : v1 v2 ... v12");
}

// ======================================================
// Public: adcCalibrationTask
// ======================================================

void adcCalibrationTask()
{
    // Drain any stray Serial input — not used, just discard
    while (Serial0.available() > 0)
        Serial0.read();

    taskHitDetection();
    capSample();
}

#endif // DEBUG_ADC_CALIBRATION
