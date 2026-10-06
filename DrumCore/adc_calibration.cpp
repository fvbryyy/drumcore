#include "adc_calibration.h"

#if DEBUG_ADC_CALIBRATION

#include "hardware.h"

// ======================================================
// Auto-Capture ADC Calibration
// ======================================================
//
// Semua mode berjalan otomatis:
//   BASELINE  — akumulasi idle noise selama BASELINE_MS,
//               lalu cetak & restart. Tidak pernah berhenti.
//   HIT       — setiap pad yang melewati CAL_HIT_THRESHOLD
//               langsung dicetak saat peak-nya selesai.
//   CROSSTALK — window pengukuran leak dibuka SAAT hit
//               terdeteksi (threshold crossed), karena peak
//               leak mekanik terjadi bersamaan dengan
//               transient source — bukan setelah hit selesai.
//
// Tidak ada Serial input, tidak ada command, tidak ada
// mode state machine yang butuh user intervention.

namespace
{
    // --------------------------------------------------
    // Timing constants
    // --------------------------------------------------

    // Baseline: akumulasi selama ini, lalu cetak & reset
    constexpr uint32_t BASELINE_PRINT_INTERVAL_MS = 3000;

    // Threshold deteksi hit pada loop() non-realtime.
    // 30 dipilih supaya pad kecil (peak ADC 20-150) ikut
    // terdeteksi. Lihat [BASELINE] print tiap 3 detik untuk
    // noise floor — naikkan nilai ini jika idle noise
    // mendekati/menyentuh threshold.
    constexpr uint16_t CAL_HIT_THRESHOLD = 30;

    // Threshold minimum source peak untuk cetak [CROSSTALK].
    // Hit di bawah ini tetap dicetak sebagai [HIT] tapi
    // crosstalk tidak ditampilkan (kurang informatif).
    constexpr uint16_t XTALK_SOURCE_THRESHOLD = 400;

    // Window observasi crosstalk (µs) setelah source terdeteksi.
    constexpr uint32_t XTALK_WINDOW_US = 50000; // 50 ms

    // Minimal elapsed sebelum window crosstalk bisa ditutup (µs)
    constexpr uint32_t XTALK_MIN_CLOSE_US = 5000;

    // Cooldown antara dua hit event dari pad yang sama (µs)
    // agar satu pukulan tidak tercatat dua kali.
    constexpr uint32_t HIT_COOLDOWN_US = 30000; // 30 ms

    // --------------------------------------------------
    // Per-pad state
    // --------------------------------------------------

    PadCalData g_calData[PAD_COUNT];

    // Baseline timing
    uint32_t g_baselineStartMs = 0;

    // Per-pad hit detection state (non-realtime observer)
    bool     g_hitArmed[PAD_COUNT]    = {};
    uint16_t g_hitPeak[PAD_COUNT]     = {};
    uint32_t g_hitStartUs[PAD_COUNT]  = {};

    // Crosstalk capture state
    bool     g_xtalkActive            = false;
    uint8_t  g_xtalkSource            = 0;
    uint16_t g_xtalkSourcePeak        = 0;
    uint16_t g_xtalkVictim[PAD_COUNT] = {};
    uint32_t g_xtalkStartUs           = 0;

    // --------------------------------------------------
    // Baseline
    // --------------------------------------------------

    void baselineReset()
    {
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            g_calData[i].idleMin   = 0xFFFF;
            g_calData[i].idleMax   = 0;
            g_calData[i].idleSum   = 0;
            g_calData[i].idleCount = 0;
        }
        g_baselineStartMs = millis();
    }

    void baselineSample()
    {
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            const uint16_t v = readPadADC(i);

            if (v < g_calData[i].idleMin) g_calData[i].idleMin = v;
            if (v > g_calData[i].idleMax) g_calData[i].idleMax = v;

            g_calData[i].idleSum += v;
            g_calData[i].idleCount++;
        }
    }

    void baselinePrint()
    {
        Serial0.println("\n[BASELINE]");

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            const PadCalData &d = g_calData[i];

            const uint16_t avg =
                (d.idleCount > 0)
                ? (uint16_t)(d.idleSum / d.idleCount)
                : 0;

            const uint16_t idleMin =
                (d.idleMin == 0xFFFF) ? 0 : d.idleMin;

            const uint16_t range =
                (d.idleMax >= idleMin)
                ? (d.idleMax - idleMin)
                : 0;

            char line[80];
            snprintf(line, sizeof(line),
                     "PAD%02u GPIO%-2u  idle_min=%4u  idle_avg=%4u"
                     "  idle_max=%4u  range=%4u",
                     (unsigned)(i + 1),
                     (unsigned)PAD_PINS[i],
                     (unsigned)idleMin,
                     (unsigned)avg,
                     (unsigned)d.idleMax,
                     (unsigned)range);
            Serial0.println(line);
        }
    }

    void taskBaseline()
    {
        // Selalu sample setiap call
        baselineSample();

        // Cetak & reset setiap BASELINE_PRINT_INTERVAL_MS
        const uint32_t nowMs = millis();
        if ((nowMs - g_baselineStartMs) >= BASELINE_PRINT_INTERVAL_MS)
        {
            baselinePrint();
            baselineReset();
        }
    }

    // --------------------------------------------------
    // Crosstalk
    // --------------------------------------------------

    void startCrosstalk(uint8_t source, uint16_t sourcePeak)
    {
        g_xtalkActive     = true;
        g_xtalkSource     = source;
        g_xtalkSourcePeak = sourcePeak;
        g_xtalkStartUs    = micros();

        // Snapshot semua pad sebagai baseline victim
        for (uint8_t i = 0; i < PAD_COUNT; i++)
            g_xtalkVictim[i] = readPadADC(i);
    }

    void taskCrosstalk()
    {
        if (!g_xtalkActive) return;

        const uint32_t nowUs   = micros();
        const uint32_t elapsed = nowUs - g_xtalkStartUs;
        const uint16_t vSrc    = readPadADC(g_xtalkSource);

        // Update source peak selama window
        if (vSrc > g_xtalkSourcePeak)
            g_xtalkSourcePeak = vSrc;

        // Update victim max
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            if (i == g_xtalkSource) continue;
            const uint16_t vVic = readPadADC(i);
            if (vVic > g_xtalkVictim[i])
                g_xtalkVictim[i] = vVic;
        }

        // Tutup window: source turun + minimal elapsed, atau timeout
        const bool sourceDone =
            (vSrc < CAL_HIT_THRESHOLD) && (elapsed >= XTALK_MIN_CLOSE_US);
        const bool timeout = (elapsed >= XTALK_WINDOW_US);

        if (!sourceDone && !timeout) return;

        // Cetak [CROSSTALK]
        Serial0.println("\n[CROSSTALK]");

        char srcLine[64];
        snprintf(srcLine, sizeof(srcLine),
                 "SOURCE PAD%02u GPIO%u  peak=%u",
                 (unsigned)(g_xtalkSource + 1),
                 (unsigned)PAD_PINS[g_xtalkSource],
                 (unsigned)g_xtalkSourcePeak);
        Serial0.println(srcLine);

        uint16_t worstPeak   = 0;
        uint8_t  worstVictim = 0;

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            if (i == g_xtalkSource) continue;

            const uint16_t leak = g_xtalkVictim[i];

            char vicLine[48];
            snprintf(vicLine, sizeof(vicLine),
                     "PAD%02u GPIO%-2u  leak=%u",
                     (unsigned)(i + 1),
                     (unsigned)PAD_PINS[i],
                     (unsigned)leak);
            Serial0.println(vicLine);

            // Update cumulative xtalkMax per victim pad
            if (leak > g_calData[i].xtalkMax)
                g_calData[i].xtalkMax = leak;

            if (leak > worstPeak)
            {
                worstPeak   = leak;
                worstVictim = i;
            }
        }

        char worstLine[64];
        snprintf(worstLine, sizeof(worstLine),
                 "WORST PAD%02u GPIO%u  peak=%u",
                 (unsigned)(worstVictim + 1),
                 (unsigned)PAD_PINS[worstVictim],
                 (unsigned)worstPeak);
        Serial0.println(worstLine);

        // Reset crosstalk state
        g_xtalkActive     = false;
        g_xtalkSourcePeak = 0;
        for (uint8_t i = 0; i < PAD_COUNT; i++)
            g_xtalkVictim[i] = 0;
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
                        g_hitArmed[i]   = true;
                        g_hitPeak[i]    = v;
                        g_hitStartUs[i] = nowUs;

                        // Buka window crosstalk SEJAK hit terdeteksi
                        // (bukan saat hit selesai): peak leak mekanik
                        // terjadi bersamaan dengan transient source.
                        // Keputusan cetak/batal ada di penutupan hit.
                        if (!g_xtalkActive)
                            startCrosstalk(i, v);
                    }
                }
                continue;
            }

            // Armed: track peak
            if (v > g_hitPeak[i])
                g_hitPeak[i] = v;

            // Close: ADC turun kembali di bawah threshold
            if (v < CAL_HIT_THRESHOLD)
            {
                const uint16_t peak = g_hitPeak[i];

                // Cetak [HIT]
                char line[64];
                snprintf(line, sizeof(line),
                         "[HIT] PAD%02u GPIO%u  peak=%u",
                         (unsigned)(i + 1),
                         (unsigned)PAD_PINS[i],
                         (unsigned)peak);
                Serial0.println(line);

                // Update per-pad stats
                if (peak > g_calData[i].peakMax)
                    g_calData[i].peakMax = peak;
                g_calData[i].peakLast    = peak;
                g_calData[i].lastEventUs  = nowUs;

                // Window crosstalk sudah dibuka saat arm (deteksi).
                // Jika peak terlalu lemah, batalkan tanpa cetak;
                // jika cukup kuat, biarkan taskCrosstalk menutup
                // window & mencetak blok [CROSSTALK].
                if (g_xtalkActive && g_xtalkSource == i &&
                    peak < XTALK_SOURCE_THRESHOLD)
                {
                    g_xtalkActive = false;
                }

                // Reset hit state
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
        g_calData[i]     = PadCalData{};
        g_calData[i].idleMin = 0xFFFF;
        g_hitArmed[i]    = false;
        g_hitPeak[i]     = 0;
        g_hitStartUs[i]  = 0;
        g_xtalkVictim[i] = 0;
    }

    g_xtalkActive     = false;
    g_xtalkSource     = 0;
    g_xtalkSourcePeak = 0;
    g_xtalkStartUs    = 0;
    g_baselineStartMs = millis();

    Serial0.println("\n[ADC-CAL] Auto-capture active.");
    Serial0.println("[ADC-CAL] Keep pads idle — baseline in progress...");
}

// ======================================================
// Public: adcCalibrationTask
// ======================================================

void adcCalibrationTask()
{
    // Drain any stray Serial input — not used, just discard
    while (Serial0.available() > 0)
        Serial0.read();

    taskBaseline();
    taskHitDetection();
    taskCrosstalk();
}

#endif // DEBUG_ADC_CALIBRATION
