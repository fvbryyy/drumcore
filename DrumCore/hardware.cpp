#include "hardware.h"
#include "config.h"

namespace
{
    uint16_t adcCache[PAD_COUNT] = {0};
    uint32_t adcCacheTimestamp[PAD_COUNT] = {0};
    uint8_t adcNextPad = 0;

    // --------------------------------------------------
    // ADC Scan Profiling
    // --------------------------------------------------

    struct TimingStats
    {
        uint32_t count = 0;
        uint32_t min = UINT32_MAX;
        uint32_t max = 0;
        uint64_t total = 0;
    };

    uint32_t adcLastRefresh[PAD_COUNT] = {0};
    TimingStats adcScanStats[PAD_COUNT];

    TimingStats overallStats;

    uint32_t lastProfileReportTime = 0;
    constexpr uint32_t ADC_PROFILE_REPORT_MS = 5000;

#if DEBUG_ROLLING_TEST
    // ==================================================
    // PHASE 4 — TRIGGER SCAN TIMING DIAGNOSTIC (Core 0)
    // ==================================================
    // Non-blocking di real-time path: hanya simpan nilai
    // + counter. Dicetak dari Core 1 (rollingTestReport).

    uint32_t p4AdcReadMaxUsArr[PAD_COUNT] = {0};
    uint32_t p4AdcReadOverallMaxUsArr = 0;
    uint32_t p4ScanIntervalMinUsArr[PAD_COUNT] = {0};
    uint32_t p4ScanIntervalMaxUsArr[PAD_COUNT] = {0};
    uint32_t p4LastScanUsArr[PAD_COUNT] = {0};
#endif

#if DEBUG_PLAYABILITY
    // ==================================================
    // PLAYABILITY - gap antar pembacaan pad yang sama
    // ==================================================
    // pbMaxPadGapUs dibaca dari Core 1 oleh playabilityReport():
    // gap maksimum antar pembacaan pad SAMA (update max 1x
    // per pass).

    uint32_t pbMaxPadGapUs = 0;
    uint32_t pbLastScanUsArr[PAD_COUNT] = {0};
#endif
}

void hardwareInit()
{
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        pinMode(PAD_PINS[i], INPUT_PULLDOWN);
        adcCache[i] = 0;
        adcCacheTimestamp[i] = 0;
    }

    pinMode(EXT_TRIGGER_PIN, INPUT_PULLDOWN);
    pinMode(HIHAT_SWITCH_PIN, INPUT_PULLUP);

    adcNextPad = 0;
}

// ======================================================
// Single ADC Read (round-robin)
// ======================================================

void adcCacheScan()
{
    const uint8_t idx = adcNextPad;

#if DEBUG_ROLLING_TEST
    // Phase 4: scan interval + ADC read duration
    const uint32_t p4Now = micros();
    if (p4LastScanUsArr[idx] != 0)
    {
        const uint32_t interval = p4Now - p4LastScanUsArr[idx];
        if (p4ScanIntervalMinUsArr[idx] == 0 ||
            interval < p4ScanIntervalMinUsArr[idx])
        {
            p4ScanIntervalMinUsArr[idx] = interval;
        }
        if (interval > p4ScanIntervalMaxUsArr[idx])
        {
            p4ScanIntervalMaxUsArr[idx] = interval;
        }
    }
    p4LastScanUsArr[idx] = p4Now;
#endif

    if constexpr (PROFILE_ADC_SCAN)
    {
        const uint32_t now = micros();

        if (adcLastRefresh[idx] != 0)
        {
            const uint32_t interval =
                now - adcLastRefresh[idx];

            TimingStats &stats = adcScanStats[idx];

            stats.count++;
            stats.total += interval;

            if (interval < stats.min)
            {
                stats.min = interval;
            }

            if (interval > stats.max)
            {
                stats.max = interval;
            }

            overallStats.count++;
            overallStats.total += interval;

            if (interval < overallStats.min)
            {
                overallStats.min = interval;
            }

            if (interval > overallStats.max)
            {
                overallStats.max = interval;
            }
        }

        adcLastRefresh[idx] = now;
    }

#if DEBUG_ROLLING_TEST
    const uint32_t readStart = micros();
#endif

    adcCache[idx] =
        analogRead(PAD_PINS[idx]);

#if DEBUG_ROLLING_TEST
    const uint32_t readDur = micros() - readStart;
    if (readDur > p4AdcReadMaxUsArr[idx])
    {
        p4AdcReadMaxUsArr[idx] = readDur;
    }
    if (readDur > p4AdcReadOverallMaxUsArr)
    {
        p4AdcReadOverallMaxUsArr = readDur;
    }
#endif

    if constexpr (PROFILE_ADC_STALENESS)
    {
        adcCacheTimestamp[idx] = micros();
    }

    adcNextPad++;

    if (adcNextPad >= PAD_COUNT)
    {
        adcNextPad = 0;
    }
}

// ======================================================
// Batch ADC: baca beberapa pad sekaligus
// ======================================================
//
// Dipanggil saat idle (tidak ada voice aktif) untuk
// meningkatkan refresh rate. Membaca maxReads pad
// dalam satu panggilan.

void adcCacheScanBatch(uint8_t maxReads)
{
    for (uint8_t r = 0; r < maxReads; r++)
    {
        const uint8_t idx = adcNextPad;

#if DEBUG_ROLLING_TEST
        // Phase 4: waktu sejak pad ini terakhir dibaca
        const uint32_t p4Now = micros();
        if (p4LastScanUsArr[idx] != 0)
        {
            const uint32_t interval = p4Now - p4LastScanUsArr[idx];
            if (p4ScanIntervalMinUsArr[idx] == 0 ||
                interval < p4ScanIntervalMinUsArr[idx])
            {
                p4ScanIntervalMinUsArr[idx] = interval;
            }
            if (interval > p4ScanIntervalMaxUsArr[idx])
            {
                p4ScanIntervalMaxUsArr[idx] = interval;
            }
        }
        p4LastScanUsArr[idx] = p4Now;
#endif

#if DEBUG_PLAYABILITY
        {
            const uint32_t pbNow = micros();
            if (pbLastScanUsArr[idx] != 0)
            {
                const uint32_t gap = pbNow - pbLastScanUsArr[idx];
                if (gap > pbMaxPadGapUs)
                {
                    pbMaxPadGapUs = gap;
                }
            }
            pbLastScanUsArr[idx] = pbNow;
        }
#endif

        if constexpr (PROFILE_ADC_SCAN)
        {
            const uint32_t now = micros();

            if (adcLastRefresh[idx] != 0)
            {
                const uint32_t interval =
                    now - adcLastRefresh[idx];

                TimingStats &stats =
                    adcScanStats[idx];

                stats.count++;
                stats.total += interval;

                if (interval < stats.min)
                {
                    stats.min = interval;
                }

                if (interval > stats.max)
                {
                    stats.max = interval;
                }

                overallStats.count++;
                overallStats.total += interval;

                if (interval < overallStats.min)
                {
                    overallStats.min = interval;
                }

                if (interval > overallStats.max)
                {
                    overallStats.max = interval;
                }
            }

            adcLastRefresh[idx] = now;
        }

#if DEBUG_ROLLING_TEST
        const uint32_t readStart = micros();
#endif

        adcCache[idx] =
            analogRead(PAD_PINS[idx]);

#if DEBUG_ROLLING_TEST
        const uint32_t readDur = micros() - readStart;
        if (readDur > p4AdcReadMaxUsArr[idx])
        {
            p4AdcReadMaxUsArr[idx] = readDur;
        }
        if (readDur > p4AdcReadOverallMaxUsArr)
        {
            p4AdcReadOverallMaxUsArr = readDur;
        }
#endif

        if constexpr (PROFILE_ADC_STALENESS)
        {
            adcCacheTimestamp[idx] = micros();
        }

        adcNextPad++;

        if (adcNextPad >= PAD_COUNT)
        {
            adcNextPad = 0;
        }
    }
}

// ======================================================
// Playability: gap maksimum antar pembacaan pad yang sama
// ======================================================

uint32_t adcPlayabilityMaxPadGapUs()
{
#if DEBUG_PLAYABILITY
    return pbMaxPadGapUs;
#else
    return 0;
#endif
}

// ======================================================
// Read Pad ADC (direct index, bukan linear search)
// ======================================================
//
// Menerima padIndex langsung (0-11), bukan pin number —
// menghindari loop pencarian.

uint16_t readPadADC(uint8_t padIndex)
{
    if (padIndex < PAD_COUNT)
    {
        return adcCache[padIndex];
    }

    return 0;
}

// ======================================================
// ADC Cache Age
// ======================================================

uint32_t adcCacheAge(uint8_t padIndex)
{
    if constexpr (!PROFILE_ADC_STALENESS)
    {
        return 0;
    }

    if (padIndex >= PAD_COUNT)
    {
        return 0;
    }

    const uint32_t ts = adcCacheTimestamp[padIndex];

    if (ts == 0)
    {
        return 0;
    }

    return micros() - ts;
}

// ======================================================
// ADC Scan Profile Report
// ======================================================

void adcScanProfileReport()
{
    if constexpr (!PROFILE_ADC_SCAN)
    {
        return;
    }

    const uint32_t now = millis();

    if ((now - lastProfileReportTime) <
        ADC_PROFILE_REPORT_MS)
    {
        return;
    }

    lastProfileReportTime = now;

    Serial0.println("=== ADC SCAN PROFILE ===");

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        TimingStats &stats = adcScanStats[i];

        if (stats.count == 0)
        {
            Serial0.printf(
                "P%u  n=0  (no data)\n",
                i
            );

            continue;
        }

        const uint32_t avg =
            (uint32_t)(stats.total / stats.count);

        Serial0.printf(
            "P%u  n=%lu  interval min/avg/max = %lu/%lu/%lu us\n",
            i,
            stats.count,
            stats.min,
            avg,
            stats.max
        );

        stats = TimingStats{};
    }

    if (overallStats.count > 0)
    {
        const uint32_t avg =
            (uint32_t)(
                overallStats.total /
                overallStats.count
            );

        Serial0.printf(
            "overall  n=%lu  min/avg/max = %lu/%lu/%lu us\n",
            overallStats.count,
            overallStats.min,
            avg,
            overallStats.max
        );
    }

    Serial0.println("========================");
}

#if DEBUG_ROLLING_TEST
// ======================================================
// Phase 4 — Accessors & reset
// ======================================================

uint32_t p4AdcReadMaxUs(uint8_t pad)
{
    if (pad >= PAD_COUNT) { return 0; }
    return p4AdcReadMaxUsArr[pad];
}

uint32_t p4AdcReadOverallMaxUs()
{
    return p4AdcReadOverallMaxUsArr;
}

uint32_t p4ScanIntervalMinUs(uint8_t pad)
{
    if (pad >= PAD_COUNT) { return 0; }
    return p4ScanIntervalMinUsArr[pad];
}

uint32_t p4ScanIntervalMaxUs(uint8_t pad)
{
    if (pad >= PAD_COUNT) { return 0; }
    return p4ScanIntervalMaxUsArr[pad];
}

void p4ResetScanTiming()
{
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        p4AdcReadMaxUsArr[i] = 0;
        p4ScanIntervalMinUsArr[i] = 0;
        p4ScanIntervalMaxUsArr[i] = 0;
        p4LastScanUsArr[i] = 0;
    }
    p4AdcReadOverallMaxUsArr = 0;
}
#endif
