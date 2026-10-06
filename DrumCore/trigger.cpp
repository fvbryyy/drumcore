#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "trigger.h"
#include "config.h"
#include "hardware.h"
#include "velocity.h"
#include "audio.h"

namespace
{
    // ==================================================
    // TRIGGER TASK (Core 1 / priority 23)
    // ==================================================
    //
    // Real-time trigger scanning. Target: Core 1, priority
    // 23 (di atas MIDI priority 8 di Core 1). Dipindah ke
    // Core 1 supaya ADC scan TIDAK ter-preempt AudioTask
    // (Core 0 / priority 24). Value dijaga < 25 sesuai
    // batas FreeRTOS environment ini.
    //
    // Trigger menulis padding SPSC hit ring via audioTrigger(),
    // jadi tidak memerlukan akses langsung ke audio state.

    constexpr size_t TRIGGER_TASK_STACK_SIZE = 4096;
    constexpr UBaseType_t TRIGGER_TASK_PRIORITY = 23;

    TaskHandle_t triggerTaskHandle = nullptr;

    void triggerTaskLoop(void *)
    {
        for (;;)
        {
            triggerTask();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    constexpr uint32_t TIMING_PROFILE_REPORT_MS = 5000;

    struct TimingStats
    {
        uint32_t count = 0;
        uint32_t min = UINT32_MAX;
        uint32_t max = 0;
        uint64_t total = 0;
    };

    void updateTimingStats(
        TimingStats &stats,
        uint32_t value
    )
    {
        stats.count++;
        stats.total += value;

        if (value < stats.min)
        {
            stats.min = value;
        }

        if (value > stats.max)
        {
            stats.max = value;
        }
    }

    // --------------------------------------------------
    // ADC poll timing (per active voice count)
    // Bucket 0..VOICE_COUNT (histogram mengikuti polyphony)
    // --------------------------------------------------

    TimingStats pollTimingStats[VOICE_COUNT + 1];
    uint32_t lastPadUpdateTime = 0;

    // --------------------------------------------------
    // ADC Staleness profiling
    // --------------------------------------------------

    TimingStats adcStaleness[PAD_COUNT];
    TimingStats overallStaleness;

    void timingProfileUpdatePad(uint32_t now)
    {
        if constexpr (!PROFILE_TRIGGER_LATENCY)
        {
            return;
        }

        if (lastPadUpdateTime != 0)
        {
            const uint8_t activeVoiceCount =
                audioActiveVoiceCount();

            updateTimingStats(
                pollTimingStats[activeVoiceCount],
                now - lastPadUpdateTime
            );
        }

        lastPadUpdateTime = now;
    }

    // --------------------------------------------------
    // Trigger latency: attackTime -> fireTime
    // --------------------------------------------------

    uint32_t padAttackTime[PAD_COUNT] = {0};
    TimingStats triggerLatency;

    // ----------------------------------------------
    // Peak-lock timestamp (RUNTIME + profiling):
    // waktu terakhir kali peak NAIK. Selalu di-update
    // (tidak digate profiling) sehingga dipakai untuk
    // early-fire DAN pengukuran rise/stable. DISET di
    // setiap awal window peak: attack (IDLE) maupun
    // retrigger (WAIT_RELEASE) -> hindari timestamp
    // stale lintas hit (root cause underflow data lalu).
    //   rise   = peakLock - attack   (threshold→peak)
    //   stable = fire     - peakLock  (wasted wait)

    uint32_t padPeakLockTime[PAD_COUNT] = {0};
    TimingStats peakRiseLatency;
    TimingStats peakStableLatency;

#if DEBUG_ROLLING_TEST
    // ==================================================
    // DIAGNOSTIC ROLLING TEST (DEBUG_ROLLING_TEST)
    // ==================================================
    //
    // Tujuan: membedakan 3 kemungkinan hilangnya hit:
    //   1. ADC tidak membaca peak pukulan cepat
    //   2. ADC baca peak tapi state/mask menolak hit
    //   3. Trigger menghasilkan HIT tapi audio/queue drop
    //
    // Producer = TriggerTask (Core 1, real-time). Di sini
    // HANYA increment counter + tulis event ring (non-blocking,
    // tanpa Serial print). Consumer = rollingTestReport()
    // yang dipanggil dari triggerProfileReport() (Core 1,
    // non-real-time) untuk mencetak ke Serial0.
    //
    // Event ring SPSC (drop-oldest saat penuh supaya event
    // TERBARU selalu tersimpan; jumlah yang di-drop dicatat).

    enum RollingEventType : uint8_t
    {
        ROLL_EVT_ATTACK = 0, // threshold terlewati, window dibuka
        ROLL_EVT_HIT    = 1, // audioTrigger() menghasilkan hit
        ROLL_EVT_REJECT = 2  // ada energi tapi ditolak state/mask
    };

    enum RollingRejectReason : uint8_t
    {
        ROLL_REJ_NONE       = 0,
        ROLL_REJ_MASK       = 1, // masih di dalam MASK / belum decay
        ROLL_REJ_WAIT_PEAK  = 2, // threshold silang saat masih WAIT_PEAK
        ROLL_REJ_WAIT_REL   = 3  // di WAIT_RELEASE tapi belum release
    };

    struct RollingEvent
    {
        uint32_t tsUs;               // micros() saat event
        uint8_t  pad;
        uint8_t  type;               // RollingEventType
        uint8_t  reason;             // RollingRejectReason (reject)
        uint16_t adc;                // nilai ADC terkait (attack/reject)
        uint8_t  velocity;           // velocity (hit)
        uint32_t dtUsPrev;           // interval sejak hit sebelumnya (hit)
        uint32_t maskUs;             // mask terpasang (us) — Phase 3C
        uint32_t elapsedUs;          // waktu sejak hit diterima (us)
        uint32_t remainingUs;        // sisa mask (us) saat ditolak
    };

    constexpr size_t ROLLING_EVENT_RING_SIZE = 128;
    constexpr size_t ROLLING_EVENT_RING_MASK =
        ROLLING_EVENT_RING_SIZE - 1;

    RollingEvent rollingEventRing[ROLLING_EVENT_RING_SIZE];
    volatile size_t rollingEventHead = 0;
    volatile size_t rollingEventTail = 0;
    volatile uint32_t rollingEventDropped = 0;

    // per-pad counter selama diagnostic
    uint32_t rollAdcPeakCount[PAD_COUNT] = {0};
    uint32_t rollThresholdCrossCount[PAD_COUNT] = {0};
    uint32_t rollTriggerHitCount[PAD_COUNT] = {0};
    uint32_t rollMaskRejectCount[PAD_COUNT] = {0};
    uint32_t rollLastHitUs[PAD_COUNT] = {0};
    uint32_t rollHitIntervalMaxUs[PAD_COUNT] = {0};
    uint32_t rollHitIntervalMinUs[PAD_COUNT] = {0};   // Phase 3C
    uint32_t rollRejectIntervalMinUs[PAD_COUNT] = {0}; // Phase 3C

    // Phase 4 — full trigger scan timing (Core 0)
    uint64_t p4ScanAccumUs = 0;
    uint32_t p4ScanCount = 0;
    uint32_t p4ScanMaxUs = 0;
    uint32_t p4UpdatePadMaxUs = 0;

    void p4ResetScanTimingReportAccum()
    {
        p4ScanAccumUs = 0;
        p4ScanCount = 0;
        p4ScanMaxUs = 0;
        p4UpdatePadMaxUs = 0;
    }

    void rollingPush(
        uint8_t pad,
        uint8_t type,
        uint8_t reason,
        uint16_t adc,
        uint8_t velocity,
        uint32_t dtUs,
        uint32_t maskUs,
        uint32_t elapsedUs,
        uint32_t remainingUs)
    {
        const size_t head = rollingEventHead;
        const size_t next =
            (head + 1) & ROLLING_EVENT_RING_MASK;

        if (next ==
            (size_t)__atomic_load_n(
                &rollingEventTail, __ATOMIC_ACQUIRE))
        {
            // penuh -> drop OLDEST (ganti head), geser tail
            __atomic_fetch_add(
                &rollingEventDropped, 1, __ATOMIC_RELAXED);
            __atomic_store_n(
                &rollingEventTail,
                (size_t)(((size_t)__atomic_load_n(
                    &rollingEventTail, __ATOMIC_RELAXED) + 1) &
                          ROLLING_EVENT_RING_MASK),
                __ATOMIC_RELEASE);
        }

        rollingEventRing[head].tsUs = micros();
        rollingEventRing[head].pad = pad;
        rollingEventRing[head].type = type;
        rollingEventRing[head].reason = reason;
        rollingEventRing[head].adc = adc;
        rollingEventRing[head].velocity = velocity;
        rollingEventRing[head].dtUsPrev = dtUs;
        rollingEventRing[head].maskUs = maskUs;
        rollingEventRing[head].elapsedUs = elapsedUs;
        rollingEventRing[head].remainingUs = remainingUs;

        __atomic_store_n(
            &rollingEventHead, next, __ATOMIC_RELEASE);
    }

    // Catat hit yang berhasil dihasilkan trigger (sebelum
    // dikirim ke audio). dt_us = interval sejak hit terakhir.
    // maskUs = mask terpasang (us). Dipanggil TEPAT di titik
    // audioTrigger() dipanggil.

    void rollingRecordHit(
        uint8_t pad,
        uint8_t velocity,
        uint32_t nowUs,
        uint32_t maskUs)
    {
        rollTriggerHitCount[pad]++;

        uint32_t dtUs = 0;

        if (rollLastHitUs[pad] != 0)
        {
            dtUs = nowUs - rollLastHitUs[pad];

            if (dtUs > rollHitIntervalMaxUs[pad])
            {
                rollHitIntervalMaxUs[pad] = dtUs;
            }

            // Phase 3C: interval antar hit yang DITERIMA
            // (terkecil) -> ukuran kecepatan roll nyata.
            if (rollHitIntervalMinUs[pad] == 0 ||
                dtUs < rollHitIntervalMinUs[pad])
            {
                rollHitIntervalMinUs[pad] = dtUs;
            }
        }

        rollLastHitUs[pad] = nowUs;

        rollingPush(
            pad,
            ROLL_EVT_HIT,
            ROLL_REJ_NONE,
            0,
            velocity,
            dtUs,
            maskUs,
            dtUs,
            0);
    }

    // --------------------------------------------------
    // Report (Ring drain + counter summary) — Core 1
    // --------------------------------------------------

    uint32_t lastRollingReportMs = 0;
    constexpr uint32_t ROLLING_REPORT_MS = 1000;

    void rollingTestReport()
    {
        if constexpr (DEBUG_ROLLING_TEST == 0)
        {
            return;
        }

        // ---- Drain event ring ----

        for (;;)
        {
            const size_t tail =
                (size_t)__atomic_load_n(
                    &rollingEventTail, __ATOMIC_ACQUIRE);
            const size_t head =
                (size_t)__atomic_load_n(
                    &rollingEventHead, __ATOMIC_ACQUIRE);

            if (tail == head)
            {
                break;
            }

            const RollingEvent &e = rollingEventRing[tail];

            __atomic_store_n(
                &rollingEventTail,
                (size_t)((tail + 1) & ROLLING_EVENT_RING_MASK),
                __ATOMIC_RELEASE);

            switch (e.type)
            {
                case ROLL_EVT_HIT:
                    Serial0.printf(
                        "HIT PAD=%u VEL=%u DT_US=%lu MASK_US=%lu\n",
                        e.pad, e.velocity,
                        (unsigned long)e.dtUsPrev,
                        (unsigned long)e.maskUs);
                    break;

                case ROLL_EVT_ATTACK:
                    Serial0.printf(
                        "ROLL PAD=%u ADC=%u TH=YES PEAK=YES HIT=NO \n",
                        e.pad, e.adc);
                    break;

                case ROLL_EVT_REJECT:
                {
                    const char *reason = "?";
                    switch (e.reason)
                    {
                        case ROLL_REJ_MASK:
                            reason = "MASK";
                            break;
                        case ROLL_REJ_WAIT_PEAK:
                            reason = "WAIT_PEAK";
                            break;
                        case ROLL_REJ_WAIT_REL:
                            reason = "WAIT_RELEASE";
                            break;
                        default:
                            break;
                    }

                    if (e.maskUs != 0)
                    {
                        // Phase 3C: info timing mask untuk REASON=MASK
                        Serial0.printf(
                            "PAD=%u REASON=%s MASK_US=%lu ELAPSED_US=%lu REMAINING_US=%lu PEAK=%u\n",
                            e.pad, reason,
                            (unsigned long)e.maskUs,
                            (unsigned long)e.elapsedUs,
                            (unsigned long)e.remainingUs,
                            e.adc);
                    }
                    else
                    {
                        Serial0.printf(
                            "PAD=%u PEAK=%u HIT=NO REASON=%s\n",
                            e.pad, e.adc, reason);
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // ---- Ring overflow info ----

        const uint32_t dropped =
            __atomic_load_n(
                &rollingEventDropped, __ATOMIC_RELAXED);

        if (dropped != 0)
        {
            Serial0.printf(
                "(event ring dropped=%lu, oldest overwritten)\n",
                (unsigned long)dropped);
        }

        // ---- Periodic counter summary (tiap 1 s) ----

        const uint32_t nowMs = millis();

        if ((nowMs - lastRollingReportMs) < ROLLING_REPORT_MS)
        {
            return;
        }

        lastRollingReportMs = nowMs;

        const uint32_t audioOverflow = audioHitRingOverflowCount();

        Serial0.println("=== MASK TUNING DIAGNOSTIC ===");

        for (uint8_t p = 0; p < PAD_COUNT; p++)
        {
            const uint32_t maskUs =
                (uint32_t)PAD_MASK_MS[p] * 1000;

            Serial0.printf(
                "PAD%u MASK_US=%lu HIT=%lu MASK_REJECT=%lu "
                "MIN_HIT_US=%lu MIN_REJECT_US=%lu\n",
                p,
                (unsigned long)maskUs,
                (unsigned long)rollTriggerHitCount[p],
                (unsigned long)rollMaskRejectCount[p],
                (unsigned long)rollHitIntervalMinUs[p],
                (unsigned long)rollRejectIntervalMinUs[p]);

            rollAdcPeakCount[p] = 0;
            rollThresholdCrossCount[p] = 0;
            rollTriggerHitCount[p] = 0;
            rollMaskRejectCount[p] = 0;
            rollHitIntervalMaxUs[p] = 0;
            rollHitIntervalMinUs[p] = 0;
            rollRejectIntervalMinUs[p] = 0;
        }

        if (audioOverflow != 0)
        {
            Serial0.printf(
                "AUDIO_HIT_RING_OVERFLOW=%lu (hit dropped by audio)\n",
                (unsigned long)audioOverflow);
        }

        // ---- Phase 4: Trigger Scan Timing ----

        Serial0.println("=== TRIGGER SCAN TIMING ===");

        {
            const uint32_t fullAvg =
                (p4ScanCount != 0)
                ? (uint32_t)(p4ScanAccumUs / p4ScanCount)
                : 0;

            Serial0.printf(
                "FULL_SCAN_AVG_US=%lu\n",
                (unsigned long)fullAvg);

            Serial0.printf(
                "FULL_SCAN_MAX_US=%lu\n",
                (unsigned long)p4ScanMaxUs);

            Serial0.printf(
                "UPDATE_PAD_MAX_US=%lu\n",
                (unsigned long)p4UpdatePadMaxUs);

            Serial0.printf(
                "ADC_READ_MAX_US=%lu\n",
                (unsigned long)p4AdcReadOverallMaxUs());
        }

        // Pad yang paling lambat (ADC read) + scan interval
        {
            uint8_t slowestPad = 0;
            uint32_t slowestMax = 0;

            for (uint8_t p = 0; p < PAD_COUNT; p++)
            {
                const uint32_t maxUs = p4AdcReadMaxUs(p);

                if (maxUs > slowestMax)
                {
                    slowestMax = maxUs;
                    slowestPad = p;
                }
            }

            Serial0.printf(
                "SLOWEST_ADC_READ PAD%u MAX=%lu us\n",
                slowestPad,
                (unsigned long)slowestMax);
        }

        for (uint8_t p = 0; p < PAD_COUNT; p++)
        {
            const uint32_t minInt = p4ScanIntervalMinUs(p);
            const uint32_t maxInt = p4ScanIntervalMaxUs(p);

            if (minInt != 0 || maxInt != 0)
            {
                Serial0.printf(
                    "PAD%u_SCAN_INTERVAL_MIN_US=%lu "
                    "PAD%u_SCAN_INTERVAL_MAX_US=%lu\n",
                    p, (unsigned long)minInt,
                    p, (unsigned long)maxInt);
            }
        }

        // reset phase-4 accumulators + hardware counters
        p4ResetScanTimingReportAccum();
        p4ResetScanTiming();

        Serial0.println("=== END TRIGGER SCAN TIMING ===");

        Serial0.println("=== END MASK DIAGNOSTIC ===");
    }
#endif // DEBUG_ROLLING_TEST

    // --------------------------------------------------
    // Report
    // --------------------------------------------------

    uint32_t lastTimingReportTime = 0;

    void timingProfileReport()
    {
        if constexpr (!PROFILE_TRIGGER_LATENCY)
        {
            return;
        }

        const uint32_t now = millis();

        if ((now - lastTimingReportTime) <
            TIMING_PROFILE_REPORT_MS)
        {
            return;
        }

        lastTimingReportTime = now;

        // ----------------------------------------------
        // ADC poll timing
        // ----------------------------------------------

        for (uint8_t v = 0; v < VOICE_COUNT + 1; v++)
        {
            TimingStats &stats = pollTimingStats[v];

            if (stats.count == 0)
            {
                continue;
            }

            Serial0.printf(
                "[PROFILE ADC] v=%u n=%lu poll=%lu/%lu/%lu us\n",
                v,
                stats.count,
                (uint32_t)(stats.total / stats.count),
                stats.min,
                stats.max
            );

            stats = TimingStats{};
        }

        // ----------------------------------------------
        // Trigger latency
        // ----------------------------------------------

        if (triggerLatency.count > 0)
        {
            Serial0.println(
                "=== TRIGGER LATENCY PROFILE ==="
            );

            Serial0.printf(
                "hits=%lu\n",
                triggerLatency.count
            );

            Serial0.printf(
                "latency min=%lu us\n",
                triggerLatency.min
            );

            Serial0.printf(
                "avg=%lu us\n",
                (uint32_t)(
                    triggerLatency.total /
                    triggerLatency.count
                )
            );

            Serial0.printf(
                "max=%lu us\n",
                triggerLatency.max
            );

            Serial0.println(
                "================================"
            );

            triggerLatency = TimingStats{};

            // ------------------------------------------
            // Peak onset (MAX-C2): rise vs wasted wait
            // ------------------------------------------

            if (peakRiseLatency.count > 0)
            {
                Serial0.println(
                    "=== PEAK ONSET PROFILE ==="
                );

                Serial0.printf(
                    "rise   min/avg/max = %lu/%lu/%lu us\n",
                    peakRiseLatency.min,
                    (uint32_t)(
                        peakRiseLatency.total /
                        peakRiseLatency.count
                    ),
                    peakRiseLatency.max
                );

                Serial0.printf(
                    "stable min/avg/max = %lu/%lu/%lu us\n",
                    peakStableLatency.min,
                    (uint32_t)(
                        peakStableLatency.total /
                        peakStableLatency.count
                    ),
                    peakStableLatency.max
                );

                Serial0.println(
                    "===================="
                );

                peakRiseLatency = TimingStats{};
                peakStableLatency = TimingStats{};
            }
        }

        // ----------------------------------------------
        // ADC Staleness
        // ----------------------------------------------

        if constexpr (PROFILE_ADC_STALENESS)
        {
            if (overallStaleness.count > 0)
            {
                Serial0.println(
                    "=== ADC STALENESS PROFILE ==="
                );

                for (uint8_t i = 0; i < PAD_COUNT; i++)
                {
                    TimingStats &stats =
                        adcStaleness[i];

                    if (stats.count == 0)
                    {
                        Serial0.printf(
                            "P%u  n=0  (no data)\n",
                            i
                        );

                        continue;
                    }

                    Serial0.printf(
                        "P%u  n=%lu  age min/avg/max = %lu/%lu/%lu us\n",
                        i,
                        stats.count,
                        stats.min,
                        (uint32_t)(
                            stats.total /
                            stats.count
                        ),
                        stats.max
                    );

                    stats = TimingStats{};
                }

                const uint32_t avg =
                    (uint32_t)(
                        overallStaleness.total /
                        overallStaleness.count
                    );

                Serial0.printf(
                    "overall  n=%lu  min/avg/max = %lu/%lu/%lu us\n",
                    overallStaleness.count,
                    overallStaleness.min,
                    avg,
                    overallStaleness.max
                );

                Serial0.println(
                    "================================"
                );

                overallStaleness = TimingStats{};
            }
        }

        // ----------------------------------------------
        // Audio timing
        // ----------------------------------------------

        audioTimingProfileReport();

        audioEndToEndProfileReport();

        lastPadUpdateTime = 0;
    }

    // --------------------------------------------------
    // Dynamic mask time berdasarkan velocity
    // --------------------------------------------------
    //
    // Hard hit = mask lebih panjang (ringing lebih lama)
    // Soft hit = mask lebih pendek (retrigger lebih cepat)
    // NOTE: Sekarang pakai per-pad mask dari config.
    // Fungsi ini masih dipakai untuk adjust halus berdasarkan velocity.

    uint32_t adjustMaskTime(uint32_t baseMaskMs, uint8_t velocity)
    {
        // Adjust ±30% berdasarkan velocity
        // Hard hit (velocity tinggi) = mask lebih panjang
        // Soft hit (velocity rendah) = mask lebih pendek
        constexpr int32_t ADJUST_RANGE = 30; // persen

        int32_t adjustment = ((int32_t)velocity - 64) * ADJUST_RANGE / 127;
        int32_t result = (int32_t)baseMaskMs + adjustment;

        // Clamp ke range yang wajar
        if (result < 8) result = 8;
        if (result > 50) result = 50;

        return (uint32_t)result;
    }

    // --------------------------------------------------
    // PLAYABILITY scan timing (DEBUG_PLAYABILITY)
    // --------------------------------------------------
    // Scan pass = adcCacheScanBatch + updatePad semua pad.
    // pbMaxPadGapUs diisi dari hardware.cpp (gap antar
    // pembacaan pad yang sama). Dibaca oleh playabilityReport()
    // dari Core 1; nol biaya saat DEBUG_PLAYABILITY = 0.

#if DEBUG_PLAYABILITY
    uint32_t pbScanCount = 0;
    uint32_t pbScanAccumUs = 0;
    uint32_t pbScanMaxUs = 0;
#endif
}

// ======================================================
// Playability Report (DEBUG_PLAYABILITY)
// ======================================================
// Dipanggil dari loop() (Core 1), bukan real-time path.
// Mencetak ringkasan periodik 1 detik, tidak per-hit.

void playabilityReport()
{
#if DEBUG_PLAYABILITY
    static uint32_t lastPrintUs = 0;
    const uint32_t nowUs = micros();

    if (nowUs - lastPrintUs < 1000000UL)
    {
        return;
    }

    lastPrintUs = nowUs;

    const AudioPlayabilityStats s = audioPlayabilityGet();
    const uint32_t maxPadGapUs = adcPlayabilityMaxPadGapUs();

    Serial0.println("=== PLAYABILITY ===");

    if (pbScanCount != 0)
    {
        Serial0.printf(
            "TRIGGER hits=%lu scanMax=%lu scanAvg=%lu maxPadGap=%lu us\n",
            (unsigned long)s.triggerHits,
            (unsigned long)pbScanMaxUs,
            (unsigned long)(pbScanAccumUs / pbScanCount),
            (unsigned long)maxPadGapUs
        );
    }
    else
    {
        Serial0.printf(
            "TRIGGER hits=%lu (no scan data yet)\n",
            (unsigned long)s.triggerHits
        );
    }

    Serial0.printf(
        "AUDIO push=%lu drop=%lu consumed=%lu activeMax=%lu steal=%lu\n",
        (unsigned long)s.pushed,
        (unsigned long)s.dropped,
        (unsigned long)s.consumed,
        (unsigned long)s.maxActive,
        (unsigned long)s.steal
    );

    Serial0.printf(
        "PREFETCH fail=%lu maxUs=%lu\n",
        (unsigned long)s.prefetchFail,
        (unsigned long)s.prefetchMaxUs
    );

    // Reset counter periodik biar tiap jendela 1 dtk mandiri.
    pbScanCount = 0;
    pbScanAccumUs = 0;
    pbScanMaxUs = 0;
#endif
}

// ======================================================
// Trigger Initialization
// ======================================================

void triggerInit()
{
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        Pad &pad = pads[i];

        pad.state = TRIGGER_IDLE;

        pad.currentValue = 0;
        pad.peak = 0;
        pad.lastPeak = 0;

        pad.stateTime = 0;
        pad.releaseTime = 0;

        pad.velocity = 0;

        pad.hitReady = false;
    }
}

// ======================================================
// Trigger Task
// ======================================================

void triggerTask()
{
    // ADC/trigger sampling deterministik: TIDAK bergantung pada
    // jumlah active audio voices. Sebelumnya batch turun 6 -> 4
    // saat ada voice audio aktif, membuat cache ADC lebih stale
    // persis saat permainan multi-pad -> transient cepat
    // (hi-hat/snare) bisa terlewat & velocity tidak konsisten.
    //
    // Data diukur (DEBUG_PLAYABILITY): scanAvg ~300us utk batch 6
    // => ~50us per analogRead, bukan ~100us. Dengan batch 12 semua
    // 12 pad di-refresh SETIAP pass (adcNextPad maju 12 -> wrap ke
    // awal), sehingga kadence pad sama = 1 pass (~1.3-1.8ms) tidak
    // lagi 2 pass (~3.5ms). Batch 12 = ~600us kerja, masih muat di
    // jendela preemption AudioTask (~1ms) karena audio (prio 24)
    // tetap preempt trigger (prio 23); 12 read masih < 1 budget tsb.
    constexpr uint8_t ADC_SCAN_BATCH = 12;

#if DEBUG_ROLLING_TEST
    const uint32_t p4ScanStart = micros();
#endif

#if DEBUG_PLAYABILITY
    const uint32_t pbScanStart = micros();
#endif

    adcCacheScanBatch(ADC_SCAN_BATCH);

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        updatePad(pads[i], i);
    }

#if DEBUG_PLAYABILITY
    {
        const uint32_t pbScanDur = micros() - pbScanStart;
        pbScanCount++;
        pbScanAccumUs += pbScanDur;
        if (pbScanDur > pbScanMaxUs)
        {
            pbScanMaxUs = pbScanDur;
        }
    }
#endif

#if DEBUG_ROLLING_TEST
    {
        const uint32_t p4ScanDur = micros() - p4ScanStart;
        p4ScanAccumUs += p4ScanDur;
        p4ScanCount++;
        if (p4ScanDur > p4ScanMaxUs)
        {
            p4ScanMaxUs = p4ScanDur;
        }
    }
#endif

    // Laporan profiling TIDAK lagi dipanggil dari sini:
    // sekarang berjalan dari loop() (Core 1, non-real-time)
    // lewat triggerProfileReport(). Real-time path Core 0
    // bebas dari blocking Serial print.
}

// ======================================================
// Update Single Pad
// ======================================================

void updatePad(
    Pad &pad,
    uint8_t padIndex
)
{
#if DEBUG_ROLLING_TEST
    const uint32_t p4UpStart = micros();
#endif

    const uint32_t now = micros();

    if constexpr (PROFILE_TRIGGER_LATENCY)
    {
        timingProfileUpdatePad(now);
    }

    switch (pad.state)
    {
        // ==============================================
        // IDLE
        // ==============================================

        case TRIGGER_IDLE:
        {
            pad.currentValue =
                readPadADC(padIndex);

            // ------------------------------------------
            // Attack detection
            // ------------------------------------------

            if (pad.currentValue >=
                pad.config.threshold)
            {
#if DEBUG_ROLLING_TEST
                rollAdcPeakCount[padIndex]++;
                rollThresholdCrossCount[padIndex]++;
                rollingPush(
                    padIndex,
                    ROLL_EVT_ATTACK,
                    ROLL_REJ_NONE,
                    pad.currentValue,
                    0,
                    0,
                    0,
                    0,
                    0);
#endif

                // Peak-lock selalu di-reset di awal window
                // (runtime early-fire + profiling).

                padPeakLockTime[padIndex] = now;

                if constexpr (PROFILE_TRIGGER_LATENCY)
                {
                    padAttackTime[padIndex] = now;
                }

                if constexpr (PROFILE_ADC_STALENESS)
                {
                    const uint32_t age =
                        adcCacheAge(padIndex);

                    updateTimingStats(
                        adcStaleness[padIndex],
                        age
                    );

                    updateTimingStats(
                        overallStaleness,
                        age
                    );
                }

                pad.peak =
                    pad.currentValue;

                pad.hitReady =
                    false;

                pad.state =
                    TRIGGER_WAIT_PEAK;

                pad.stateTime =
                    now;

#if DEBUG_TRIGGER

                Serial0.print(
                    ">>> ATTACK pad="
                );

                Serial0.print(
                    padIndex
                );

                Serial0.print(
                    " ADC="
                );

                Serial0.println(
                    pad.currentValue
                );

#endif
            }

            break;
        }

        // ==============================================
        // WAIT PEAK
        // ==============================================

        case TRIGGER_WAIT_PEAK:
        {
            pad.currentValue =
                readPadADC(padIndex);

            // ------------------------------------------
            // Track maximum peak
            // ------------------------------------------

#if DEBUG_ROLLING_TEST
            if (pad.currentValue >= pad.config.threshold)
            {
                rollAdcPeakCount[padIndex]++;
            }
#endif

            if (pad.currentValue >
                pad.peak)
            {
                pad.peak =
                    pad.currentValue;

                // Peak naik -> perbarui lock (runtime + profiling).

                padPeakLockTime[padIndex] = now;
            }

            // ------------------------------------------
            // Adaptive peak: fire lebih awal jika
            // sudah jelas hard hit.
            // ------------------------------------------
            //
            // Jika peak sudah sangat tinggi (>70% max),
            // tidak perlu tunggu seluruh peak window.
            // Ini mengurangi latensi untuk pukulan keras.

            constexpr uint16_t ADAPTIVE_PEAK_THRESHOLD = 2200;
            constexpr uint32_t ADAPTIVE_PEAK_TIME = 800;

            if (pad.peak >= ADAPTIVE_PEAK_THRESHOLD &&
                (now - pad.stateTime) >= ADAPTIVE_PEAK_TIME)
            {
                pad.lastPeak =
                    pad.peak;

                // Per-pad sensitivity scaling (tuning): peak ADC
                // dikali sensitivity/100 sebelum velocity lookup.
                // sensitivity=100 (default) -> hasil identik,
                // baseline pad besar tidak berubah.
                uint32_t sensPeak =
                    (uint32_t)pad.lastPeak *
                    (uint32_t)pad.config.sensitivity / 100u;
                if (sensPeak > 4095u) sensPeak = 4095u;

                pad.velocity =
                    calculateVelocity(
                        (uint16_t)sensPeak
                    );

                pad.hitReady =
                    true;

#if DEBUG_TRIGGER

                Serial0.print(
                    ">>> HIT (adaptive) pad="
                );

                Serial0.print(
                    padIndex
                );

                Serial0.print(
                    " peak="
                );

                Serial0.print(
                    pad.lastPeak
                );

                Serial0.print(
                    " vel="
                );

                Serial0.println(
                    pad.velocity
                );

#endif

                if constexpr (PROFILE_TRIGGER_LATENCY)
                {
                    updateTimingStats(
                        triggerLatency,
                        now - padAttackTime[padIndex]
                    );

                    if (padPeakLockTime[padIndex] >=
                        padAttackTime[padIndex])
                    {
                        updateTimingStats(
                            peakRiseLatency,
                            padPeakLockTime[padIndex] -
                                padAttackTime[padIndex]
                        );
                    }

                if (now >= padPeakLockTime[padIndex])
                {
                    updateTimingStats(
                        peakStableLatency,
                        now - padPeakLockTime[padIndex]
                    );
                }
            }

// (adaptive) state -> MASK
            pad.state =
                TRIGGER_MASK;

            pad.stateTime =
                now;

            // Per-pad mask + velocity adjustment
            // Phase 3D fix: base dari pad.config.maskBaseMs
            // (ms; diisi dari PAD_MASK_MS di padInit). JANGAN
            // pakai pad.maskTime (sudah µs setelah x1000).
            pad.maskTime =
                adjustMaskTime(
                    pad.config.maskBaseMs,
                    pad.velocity
                ) * 1000; // convert ms ke us

#if DEBUG_ROLLING_TEST
            rollingRecordHit(
                padIndex,
                pad.velocity,
                now,
                pad.maskTime);
#endif

            audioTrigger(
                padIndex,
                pad.velocity
            );

            pad.releaseTime =
                0;

            break;
        }

            // ------------------------------------------
            // Normal peak window
            // ------------------------------------------
            //
            // Soft/medium: tunggu penuh peak window (peakTime),
            // fire dengan peak yang SUDAH tercapture.

            if ((now - pad.stateTime) >=
                pad.config.peakTime)
            {
                pad.lastPeak =
                    pad.peak;

                // Per-pad sensitivity scaling (tuning): peak ADC
                // dikali sensitivity/100 sebelum velocity lookup.
                // sensitivity=100 (default) -> hasil identik,
                // baseline pad besar tidak berubah.
                uint32_t sensPeak =
                    (uint32_t)pad.lastPeak *
                    (uint32_t)pad.config.sensitivity / 100u;
                if (sensPeak > 4095u) sensPeak = 4095u;

                pad.velocity =
                    calculateVelocity(
                        (uint16_t)sensPeak
                    );

                pad.hitReady =
                    true;

#if DEBUG_TRIGGER

                Serial0.print(
                    ">>> HIT pad="
                );

                Serial0.print(
                    padIndex
                );

                Serial0.print(
                    " peak="
                );

                Serial0.print(
                    pad.lastPeak
                );

                Serial0.print(
                    " vel="
                );

                Serial0.println(
                    pad.velocity
                );

#endif

                if constexpr (PROFILE_TRIGGER_LATENCY)
                {
                    updateTimingStats(
                        triggerLatency,
                        now - padAttackTime[padIndex]
                    );

                    if (padPeakLockTime[padIndex] >=
                        padAttackTime[padIndex])
                    {
                        updateTimingStats(
                            peakRiseLatency,
                            padPeakLockTime[padIndex] -
                                padAttackTime[padIndex]
                        );
                    }

                if (now >= padPeakLockTime[padIndex])
                {
                    updateTimingStats(
                        peakStableLatency,
                        now - padPeakLockTime[padIndex]
                    );
                }
            }

pad.state =
                TRIGGER_MASK;

            pad.stateTime =
                now;

            // Per-pad mask + velocity adjustment
            // Phase 3D fix: base dari pad.config.maskBaseMs
            // (ms; diisi dari PAD_MASK_MS di padInit). JANGAN
            // pakai pad.maskTime (sudah µs setelah x1000).
            pad.maskTime =
                adjustMaskTime(
                    pad.config.maskBaseMs,
                    pad.velocity
                ) * 1000; // convert ms ke us

#if DEBUG_ROLLING_TEST
            rollingRecordHit(
                padIndex,
                pad.velocity,
                now,
                pad.maskTime);
#endif

            audioTrigger(
                padIndex,
                pad.velocity
            );

            pad.releaseTime =
                0;
        }

            break;
        }

        // ==============================================
        // MASK
        // ==============================================

        case TRIGGER_MASK:
        {
            // Gunakan maskTime dinamis dari pad
            if ((now - pad.stateTime) >=
                pad.maskTime)
            {
                pad.releaseTime =
                    0;

                pad.stateTime =
                    0;

                pad.state =
                    TRIGGER_WAIT_RELEASE;
            }

            break;
        }

        // ==============================================
        // WAIT RELEASE
        // ==============================================

        case TRIGGER_WAIT_RELEASE:
        {
            pad.currentValue =
                readPadADC(
                    padIndex
                );

            // ------------------------------------------
            // Decay marker
            // ------------------------------------------

            if (pad.releaseTime == 0 &&
                pad.currentValue <
                    pad.config.threshold)
            {
                pad.releaseTime =
                    now;
            }

            // ------------------------------------------
            // Retrigger detection
            // ------------------------------------------

            if (pad.currentValue >
                pad.config.threshold)
            {
#if DEBUG_ROLLING_TEST
                rollAdcPeakCount[padIndex]++;
#endif

                if (pad.releaseTime != 0)
                {
#if DEBUG_ROLLING_TEST
                    rollThresholdCrossCount[padIndex]++;
                    rollingPush(
                        padIndex,
                        ROLL_EVT_ATTACK,
                        ROLL_REJ_NONE,
                        pad.currentValue,
                        0,
                        0,
                        0,
                        0,
                        0);
#endif

                    pad.releaseTime =
                        0;

                    pad.stateTime =
                        0;

                    pad.peak =
                        pad.currentValue;

                    pad.hitReady =
                        false;

                    pad.state =
                        TRIGGER_WAIT_PEAK;

                    pad.stateTime =
                        now;

                    padPeakLockTime[padIndex] =
                        now;

                    padAttackTime[padIndex] =
                        now;

#if DEBUG_TRIGGER

                    Serial0.println(
                        ">>> RETRIGGER"
                    );

#endif
                }
                else
                {
                    // Masih di dalam envelope yang sama
                    // (belum decay) -> hit baru ditolak MASK.
#if DEBUG_ROLLING_TEST
                    rollMaskRejectCount[padIndex]++;

                    const uint32_t maskUs = pad.maskTime;
                    const uint32_t elapsedUs =
                        (rollLastHitUs[padIndex] != 0)
                        ? (now - rollLastHitUs[padIndex])
                        : 0;
                    const uint32_t remainingUs =
                        (elapsedUs < maskUs)
                        ? (maskUs - elapsedUs)
                        : 0;

                    // Minimum interval ditolak (ukur
                    // seberapa cepat ditolak vs mask)
                    if (rollRejectIntervalMinUs[padIndex] == 0 ||
                        elapsedUs < rollRejectIntervalMinUs[padIndex])
                    {
                        rollRejectIntervalMinUs[padIndex] = elapsedUs;
                    }

                    rollingPush(
                        padIndex,
                        ROLL_EVT_REJECT,
                        ROLL_REJ_MASK,
                        pad.currentValue,
                        0,
                        elapsedUs,
                        maskUs,
                        elapsedUs,
                        remainingUs);
#endif
                }

                break;
            }

            // ------------------------------------------
            // Release detection
            // ------------------------------------------

            if (pad.currentValue <=
                pad.config.releaseThreshold)
            {
                if (pad.stateTime == 0)
                {
                    pad.stateTime =
                        now;
                }

                if ((now - pad.stateTime) >=
                    pad.config.releaseStableTime)
                {
                    pad.stateTime =
                        0;

                    pad.releaseTime =
                        0;

                    pad.state =
                        TRIGGER_IDLE;

#if DEBUG_TRIGGER

                    Serial0.println(
                        ">>> READY"
                    );

#endif
                }
            }
            else
            {
                pad.stateTime =
                    0;
            }

            break;
        }
    }

#if DEBUG_ROLLING_TEST
    {
        const uint32_t p4UpDur = micros() - p4UpStart;
        if (p4UpDur > p4UpdatePadMaxUs)
        {
            p4UpdatePadMaxUs = p4UpDur;
        }
    }
#endif
}

// ======================================================
// Trigger Task Start (Core 1 / priority 23)
// ======================================================

void triggerTaskStart()
{
    if (triggerTaskHandle != nullptr)
    {
        return;
    }

    // Pin ke Core 1 supaya ADC scan TIDAK ter-preempt AudioTask
    // (Core 0 / priority 24). Sebelumnya TriggerTask di Core 0 (prio 23)
    // selalu dipreempt AudioTask di core yang sama, men-stretch scan
    // (scanMax hingga ~2.1 ms) & melebarkan pass jadi ~3 ms.
    // Di Core 1, trigger lepas dari beban mixing audio -> cadence ADC
    // mandiri & lebih cepat. Priority tetap 23 (di atas MIDI prio 8).
    // Value < 25 sesuai batas FreeRTOS environment ini.

    const BaseType_t created = xTaskCreatePinnedToCore(
        triggerTaskLoop,
        "TriggerTask",
        TRIGGER_TASK_STACK_SIZE,
        nullptr,
        TRIGGER_TASK_PRIORITY,
        &triggerTaskHandle,
        1
    );

    if (created != pdPASS)
    {
        triggerTaskHandle = nullptr;
        Serial0.println("ERROR: Trigger task creation failed");
    }
}

// ======================================================
// Trigger Profile Report (Core 1, non-real-time)
// ======================================================

void triggerProfileReport()
{
#if DEBUG_ROLLING_TEST
    rollingTestReport();
#endif
    timingProfileReport();
}
