#include "audio.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "psram_loader.h"

namespace
{

// ======================================================
// I2S
// ======================================================

I2SClass i2s;

constexpr int PIN_BCLK = 42;
constexpr int PIN_LRCK = 21;
constexpr int PIN_DOUT = 2;

constexpr uint32_t SAMPLE_RATE = 48000;

// ======================================================
// PCM
// ======================================================

constexpr uint16_t FRAME_SIZE = 4;
constexpr uint16_t BUFFER_FRAMES = 64;
constexpr size_t BUFFER_BYTES =
    BUFFER_FRAMES * FRAME_SIZE;

// ======================================================
// PLAYABILITY counters (DEBUG_PLAYABILITY)
// ======================================================
// Single-writer per counter di real-time path; dibaca oleh
// laporan dari Core 1. Non-invasif: hanya increment, tanpa
// atomik/flush/print di sini. Nol biaya saat flag = 0.

#if DEBUG_PLAYABILITY
uint32_t pbTriggerHits = 0;
uint32_t pbPushed = 0;
uint32_t pbConsumed = 0;
uint32_t pbMaxActive = 0;
uint32_t pbSteal = 0;
uint32_t pbPrefetchMaxUs = 0;
uint32_t pbPrefetchFail = 0;
#endif

// ======================================================
// PACING PRODUCER (konsumsi DMA aktual)
// ======================================================
//
// Backlog diukur dari driver, bukan estimasi micros():
//   ahead = byte yang sudah dimuat ke DMA (accepted)
//         - byte yang BENAR-BENAR selesai dimainkan
//           (callback TX on_sent, event->size per
//            deskriptor).
//
// Chunk baru hanya diproduksi saat ahead < batas.
// Phase 3A: BUFFER_FRAMES 128 -> 64, jadi satu chunk =
// 64 frame * 4 byte = 256 byte = 1.33 ms. Batas 512B =
// 2 chunk = 2.67 ms. Deskriptor DMA di library (ESP_I2S)
// tetap 960 B / 6 deskriptor, tidak diubah.

constexpr uint32_t AUDIO_PACING_AHEAD_MAX_BYTES =
    512;

volatile uint32_t audioDmaSentBytes = 0;
uint32_t audioLoadedBytes = 0;
bool audioPacingCbInstalled = false;

bool IRAM_ATTR audioOnTxSent(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *userCtx
)
{
    (void)handle;
    (void)userCtx;

    if (event)
    {
        audioDmaSentBytes +=
            (uint32_t)event->size;
    }

    return false;
}

inline uint32_t dmaCurrentAheadBytes()
{
    // Wrap-safe uint32: loaded selalu monotonic >= sent

    return audioLoadedBytes - audioDmaSentBytes;
}

bool dmaRoomForNextChunk()
{
    // Fail-open: jika callback tidak terpasang,
    // perilaku identik dengan baseline lama.

    if (!audioPacingCbInstalled)
    {
        return true;
    }

    return dmaCurrentAheadBytes() <
           AUDIO_PACING_AHEAD_MAX_BYTES;
}

// ======================================================
// GAIN TABLE (fixed-point, Q8)
// ======================================================

constexpr int32_t GAIN_SCALE = 256;

int32_t gainTable[128];

void buildGainTable()
{
    for (uint8_t v = 0; v < 127; v++)
    {
        gainTable[v] =
            (int32_t)((uint32_t)v * GAIN_SCALE / 127);
    }

    gainTable[127] = GAIN_SCALE;
}

// ======================================================
// VOICE
// ======================================================
//
// VOICE_COUNT didefinisikan di config.h (dipakai juga
// oleh profiling histogram di trigger.cpp).
//
// Semua state voice didukung penuh untuk VOICE_COUNT
// (bukan hardcoded 8). Didefinisikan zero-initialized;
// nilai default non-zero (voiceVelocityIndex = 127,
// voiceChannels = 2) di-set oleh initVoiceState(), yang
// dipanggil sekali dari audioInit().

bool voiceActive[VOICE_COUNT];
uint32_t voiceRemaining[VOICE_COUNT];
uint8_t voiceVelocityIndex[VOICE_COUNT];

uint32_t voiceAgeCounter = 0;
uint32_t voiceStartStamp[VOICE_COUNT];
uint8_t voiceSampleIndex[VOICE_COUNT];

const uint8_t *voicePtr[VOICE_COUNT];
uint32_t voiceReadPos[VOICE_COUNT];
uint32_t voiceTotalSize[VOICE_COUNT];
uint8_t voiceChannels[VOICE_COUNT];

uint8_t voiceBuffer[VOICE_COUNT][BUFFER_BYTES];
size_t voiceBufferBytes[VOICE_COUNT];
size_t voiceBufferTarget[VOICE_COUNT];
uint8_t mixBuffer[BUFFER_BYTES];
int32_t accumBuffer[BUFFER_BYTES / 2];

// ======================================================
// INIT VOICE STATE
// ======================================================
//
// Menyiapkan default seluruh slot voice untuk
// VOICE_COUNT. Dipanggil SEKALI dari audioInit().
// Tidak boleh dipanggil saat audio task sudah berjalan
// (state voice dibaca oleh audio task).

void initVoiceState()
{
    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        voiceActive[v]        = false;
        voiceRemaining[v]     = 0;
        voiceVelocityIndex[v] = 127;
        voiceStartStamp[v]    = 0;
        voiceSampleIndex[v]   = 0;
        voicePtr[v]           = nullptr;
        voiceReadPos[v]       = 0;
        voiceTotalSize[v]     = 0;
        voiceChannels[v]      = 2;
        voiceBufferBytes[v]   = 0;
        voiceBufferTarget[v]  = 0;
    }
}

size_t pendingBytes = 0;
size_t pendingOffset = 0;

volatile uint8_t cachedActiveVoiceCount = 0;

bool audioReady = false;

// ======================================================
// PROFILING
// ======================================================

struct TimingStats
{
    uint32_t count = 0;
    uint32_t min = UINT32_MAX;
    uint32_t max = 0;
    uint64_t total = 0;
};

struct AudioTimingStats
{
    TimingStats task;
    TimingStats psramRead;
    TimingStats mixing;
    TimingStats i2sWrite;
};

AudioTimingStats audioTimingStats[VOICE_COUNT + 1];

// ======================================================
// AUDIO END-TO-END PROFILING (event-based)
// ======================================================

constexpr uint32_t BUFFER_DURATION_US =
    (uint32_t)BUFFER_FRAMES * 1000000UL / SAMPLE_RATE;

struct AudioEndToEndStats
{
    TimingStats triggerToVoice;
    TimingStats voiceToPCM;
    TimingStats pcmToI2s;
    TimingStats total;
};

AudioEndToEndStats e2eStats[VOICE_COUNT + 1];

struct MixPerVoiceStats
{
    TimingStats timing;
};

MixPerVoiceStats mixPerVoice[VOICE_COUNT + 1];

struct AudioEvent
{
    uint32_t t0 = 0;
    uint32_t t1 = 0;
    uint32_t t2 = 0;
    uint32_t t3 = 0;
    bool t0Valid = false;
    bool t1Valid = false;
    bool t2Valid = false;
    bool t3Valid = false;
};

AudioEvent audioEvent[VOICE_COUNT];

void clearAudioEvent(uint8_t voiceIndex)
{
    audioEvent[voiceIndex] = AudioEvent{};
}

void updateTimingStats(
    TimingStats &stats,
    uint32_t duration
)
{
    stats.count++;
    stats.total += duration;

    if (duration < stats.min)
        stats.min = duration;
    if (duration > stats.max)
        stats.max = duration;
}

uint32_t averageTiming(const TimingStats &stats)
{
    if (stats.count == 0) return 0;
    return (uint32_t)(stats.total / stats.count);
}

void mergeTimingStats(
    TimingStats &dst,
    const TimingStats &src
)
{
    if (src.count == 0) return;

    dst.count += src.count;
    dst.total += src.total;

    if (src.min < dst.min)
        dst.min = src.min;
    if (src.max > dst.max)
        dst.max = src.max;
}

void recordE2eDelta(
    uint8_t voiceIndex,
    uint8_t voiceCount
)
{
    if (voiceCount < 1 || voiceCount > VOICE_COUNT)
        return;

    AudioEvent &e = audioEvent[voiceIndex];

    if (!e.t0Valid || !e.t1Valid ||
        !e.t2Valid || !e.t3Valid)
    {
        return;
    }

    if (e.t1 < e.t0 || e.t2 < e.t1 || e.t3 < e.t2)
    {
        return;
    }

    AudioEndToEndStats &s = e2eStats[voiceCount];

    updateTimingStats(s.triggerToVoice, e.t1 - e.t0);
    updateTimingStats(s.voiceToPCM, e.t2 - e.t1);
    updateTimingStats(s.pcmToI2s, e.t3 - e.t2);
    updateTimingStats(s.total, e.t3 - e.t0);
}

// ======================================================
// 2-VOICE DEBUG PROFILING (audit instrumentation only)
// ======================================================
//
// TIDAK mengubah behavior audio apa pun.
// Semua hook di bawah hanya: increment counter,
// tulis event ke ring buffer RAM, dan dump chunked
// (1 baris per loop pass agar UART tidak memblokir
// audioTask lebih dari ~4ms; TX DMA backlog ~29ms
// menutupi stall sebesar itu).

enum : uint8_t
{
    DBG_EVT_HIT      = 1,
    DBG_EVT_START    = 2,
    DBG_EVT_CLOSE    = 3,
    DBG_EVT_PREFETCH = 4,
    DBG_EVT_STEAL    = 5
};

enum : uint8_t
{
    DBG_CLOSE_END    = 1,  // sample habis (natural end)
    DBG_CLOSE_STOLEN = 2,  // slot diambil startVoice baru
    DBG_CLOSE_ZERO   = 3   // target nol (sisa < FRAME_SIZE)
};

constexpr uint8_t DBG_RING_SIZE = 20;
constexpr uint8_t DBG_STEAL_AUDIT_MAX = 8;
constexpr uint8_t DBG_FAIL_AUDIT_MAX = 8;
constexpr uint32_t DBG_REPORT_INTERVAL_MS = 5000;

// Field packing event CLOSE:
//   sample = sample index voice tsb
//   aux    = actual lifetime (us)
//   aux2   = expected duration sample (us)
//
// Field packing utk event STEAL:
//   voice   = slot korban
//   sample  = sample BARU yang mengambil slot
//   velocity= velocity BARU
//   hitNo   = sample index KORBAN (yang terpotong)
//   aux     = lifetime korban (us, sejak startVoice)
//   aux2    = expected duration sample korban (us)
//   reason  = DBG_CLOSE_STOLEN (penanda)
//   detail remaining/total bytes -> ring DbgStealAudit

struct DbgEvent
{
    uint32_t ts;
    uint32_t aux;
    uint32_t aux2;
    uint16_t bytes;
    uint16_t hitNo;
    uint8_t type;
    uint8_t voice;
    uint8_t sample;
    uint8_t velocity;
    uint8_t activeCount;
    uint8_t activeMask;
    uint8_t reason;
};

// Detail satu kejadian steal (untuk STEAL AUDIT)

struct DbgStealAudit
{
    uint32_t ts;
    uint32_t expectedDurUs;      // durasi teoretis sample korban
    uint32_t lifeUs;             // lifetime aktual korban
    uint32_t remainingBytes;     // voiceRemaining korban saat steal
    uint32_t bufferBytes;        // data di voiceBuffer yang belum dimix
    uint32_t totalBytes;         // voiceTotalSize korban
    uint8_t voice;               // slot korban
    uint8_t victimSample;        // sample index korban
};

// Audit prefetch failure: state saat prefetch tidak menambah byte.
// Klasifikasi dari state existing (tanpa perubahan perilaku):
//   1 = inactive, 2 = drained-await-close (target==0 && remaining==0),
//   3 = zero-target, 4 = already-full (bytes >= target)

struct DbgFailAudit
{
    uint32_t ts;
    uint32_t targetBytes;
    uint32_t bufferedBytes;
    uint32_t remainingBytes;
    uint8_t voice;
    uint8_t sample;
    uint8_t reasonCode;
};

DbgEvent dbgRing[DBG_RING_SIZE];
uint8_t dbgRingHead = 0;
uint32_t dbgEventsTotal = 0;
uint16_t dbgHitOrdinal = 0;

DbgStealAudit dbgStealAudit[DBG_STEAL_AUDIT_MAX];
uint8_t dbgStealAuditHead = 0;
uint32_t dbgStealAuditTotal = 0;

DbgFailAudit dbgFailAudit[DBG_FAIL_AUDIT_MAX];
uint8_t dbgFailAuditHead = 0;
uint32_t dbgFailAuditTotal = 0;

uint32_t dbgHitCount = 0;
uint32_t dbgAllocCount[VOICE_COUNT] = {0};
uint32_t dbgCloseCount[VOICE_COUNT] = {0};
uint32_t dbgPrefetchCount[VOICE_COUNT] = {0};
uint32_t dbgPrefetchFailCount[VOICE_COUNT] = {0};
uint32_t dbgStealCount = 0;
uint32_t dbgCloseUnderflow = 0;
uint8_t dbgMaxSimultaneous = 0;

// ---- Voice lifetime ----

uint32_t voiceStartMicros[VOICE_COUNT] = {0};

// Durasi teoretis sample yang sedang dimainkan tiap voice

uint32_t voiceExpectedDurUs[VOICE_COUNT] = {0};

uint32_t dbgLifeMinUs[VOICE_COUNT] = {0};
uint32_t dbgLifeMaxUs[VOICE_COUNT] = {0};
uint64_t dbgLifeTotalUs[VOICE_COUNT] = {0};
uint32_t dbgLifeCount[VOICE_COUNT] = {0};

// ---- Statistik PER SAMPLE (index = pad/sample index) ----

// G21: indeks dbg* di bawah adalah SLOT PSRAM (0..PSRAM_MAX_SAMPLES-1),
// bukan index pad. Slot >= PSRAM_KIT_SLOT_COUNT dipakai runtime
// PAD -> SOUND, jadi tabel debug harus mengikuti domain slot yang
// sama dengan startVoice() — kalau tidak, voice pada slot tambahan
// akan menulis di luar tabel saat PROFILE_2VOICE_DEBUG aktif.

uint32_t dbgSmpStarts[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpNatural[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpStolen[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpExhausted[PSRAM_MAX_SAMPLES] = {0};

uint32_t dbgSmpLifeMinUs[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpLifeMaxUs[PSRAM_MAX_SAMPLES] = {0};
uint64_t dbgSmpLifeTotalUs[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpLifeCount[PSRAM_MAX_SAMPLES] = {0};

// remainingBytesAtSteal = voiceRemaining + voiceBufferBytes
// (data yang belum sempat dimainkan saat korban dibunuh)

uint32_t dbgSmpRemMin[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpRemMax[PSRAM_MAX_SAMPLES] = {0};
uint64_t dbgSmpRemTotal[PSRAM_MAX_SAMPLES] = {0};
uint32_t dbgSmpRemCount[PSRAM_MAX_SAMPLES] = {0};

// ---- Close reason breakdown ----
//
// zero/exhausted dihitung dalam "exhaustedClose"
// (sisa sample < FRAME_SIZE = sample habis).

uint32_t dbgNaturalClose = 0;
uint32_t dbgStolenClose = 0;
uint32_t dbgExhaustedClose = 0;

// Korban steal yang benar-benar masih punya data
// belum dimainkan (rem+buffer > 0)

uint32_t dbgStolenBeforeEnd = 0;

// ---- Active voice count distribution ----
// Di-sample sekali per loop pass (weighted by time).

uint32_t dbgActiveDist[VOICE_COUNT + 1] = {0};

// ACTIVE_WITH_ZERO_REMAINING: voice aktif dengan
// voiceRemaining == 0. Dicatat saat snapshot.
// Catatan: ada window legit (tail terakhir sudah dipindah
// ke voiceBuffer tapi belum dimix/belum ter-close) —
// varian strict (buffer juga kosong) ikut dihitung.

uint32_t dbgZeroRemainActive = 0;
uint32_t dbgZeroRemainActiveStrict = 0;

// ---- Hit-to-hit interval ----

uint32_t dbgHitLastMicros = 0;
uint32_t dbgHitIntMinUs = 0;
uint32_t dbgHitIntMaxUs = 0;
uint64_t dbgHitIntTotalUs = 0;
uint32_t dbgHitIntCount = 0;

// ---- Expected duration range (snapshot) ----

uint32_t dbgExpDurMinUs = UINT32_MAX;
uint32_t dbgExpDurMaxUs = 0;

// Durasi teoretis sample (us) dari metadata SampleInfo existing

uint32_t dbgSampleExpectedDurUs(uint8_t sampleIndex)
{
    if (!psramIsLoaded())
    {
        return 0;
    }

    if (sampleIndex >= PSRAM_MAX_SAMPLES)
    {
        return 0;
    }

    const SampleInfo &info =
        psramGetInfo(sampleIndex);

    if (!info.valid || info.dataSize == 0)
    {
        return 0;
    }

    const uint32_t bytesPerSecond =
        info.sampleRate *
        info.channels *
        (info.bitsPerSample / 8U);

    if (bytesPerSecond == 0)
    {
        return 0;
    }

    return (uint32_t)(
        (uint64_t)info.dataSize * 1000000ULL /
        bytesPerSecond);
}

uint8_t dbgActiveMask()
{
    uint8_t mask = 0;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (voiceActive[v])
        {
            mask |= (uint8_t)(1u << v);
        }
    }

    return mask;
}

void dbgLogEvent(
    uint8_t type,
    uint8_t voiceIndex,
    uint8_t sampleIndex,
    uint8_t velocity,
    uint8_t activeCount,
    uint16_t bytes,
    uint16_t hitNo,
    uint8_t reason,
    uint32_t aux = 0,
    uint32_t aux2 = 0
)
{
    DbgEvent ev;

    ev.ts = micros();
    ev.aux = aux;
    ev.aux2 = aux2;
    ev.bytes = bytes;
    ev.hitNo = hitNo;
    ev.type = type;
    ev.voice = voiceIndex;
    ev.sample = sampleIndex;
    ev.velocity = velocity;
    ev.activeCount = activeCount;

    ev.activeMask = dbgActiveMask();
    ev.reason = reason;

    dbgRing[dbgRingHead] = ev;
    dbgRingHead =
        (uint8_t)((dbgRingHead + 1) % DBG_RING_SIZE);
    dbgEventsTotal++;
}

void dbgCountClose()
{
    if (cachedActiveVoiceCount == 0)
    {
        dbgCloseUnderflow++;
    }
}

struct DbgSnapshot
{
    DbgEvent ring[DBG_RING_SIZE];
    DbgStealAudit stealAudit[DBG_STEAL_AUDIT_MAX];
    uint8_t stealAuditCount;
    DbgFailAudit failAudit[DBG_FAIL_AUDIT_MAX];
    uint8_t failAuditCount;
    uint32_t eventsTotal;
    uint32_t hitCount;
    uint32_t allocCount[VOICE_COUNT];
    uint32_t closeCount[VOICE_COUNT];
    uint32_t prefetchCount[VOICE_COUNT];
    uint32_t prefetchFailCount[VOICE_COUNT];
    uint32_t lifeMinUs[VOICE_COUNT];
    uint32_t lifeMaxUs[VOICE_COUNT];
    uint64_t lifeTotalUs[VOICE_COUNT];
    uint32_t lifeCount[VOICE_COUNT];
    uint32_t smpStarts[PSRAM_MAX_SAMPLES];
    uint32_t smpNatural[PSRAM_MAX_SAMPLES];
    uint32_t smpStolen[PSRAM_MAX_SAMPLES];
    uint32_t smpExhausted[PSRAM_MAX_SAMPLES];
    uint32_t smpLifeMinUs[PSRAM_MAX_SAMPLES];
    uint32_t smpLifeMaxUs[PSRAM_MAX_SAMPLES];
    uint64_t smpLifeTotalUs[PSRAM_MAX_SAMPLES];
    uint32_t smpLifeCount[PSRAM_MAX_SAMPLES];
    uint32_t smpRemMin[PSRAM_MAX_SAMPLES];
    uint32_t smpRemMax[PSRAM_MAX_SAMPLES];
    uint64_t smpRemTotal[PSRAM_MAX_SAMPLES];
    uint32_t smpRemCount[PSRAM_MAX_SAMPLES];
    uint32_t naturalClose;
    uint32_t stolenClose;
    uint32_t exhaustedClose;
    uint32_t stolenBeforeEnd;
    uint32_t activeDist[VOICE_COUNT + 1];
    uint32_t zeroRemainActive;
    uint32_t zeroRemainActiveStrict;
    uint32_t expDurMinUs;
    uint32_t expDurMaxUs;
    uint32_t failAuditTotal;
    uint32_t hitIntMinUs;
    uint32_t hitIntMaxUs;
    uint64_t hitIntTotalUs;
    uint32_t hitIntCount;
    uint32_t longestActiveNowUs;
    uint32_t avgActiveAgeNowUs;
    uint32_t stealCount;
    uint32_t closeUnderflow;
    uint8_t maxSimultaneous;
    uint8_t ringHead;
};

DbgSnapshot dbgSnap;
bool dbgSnapPending = false;
uint16_t dbgPrintLine = 0;
uint16_t dbgSnapEventCount = 0;
uint32_t dbgLastReportMs = 0;

const char *dbgCloseReasonName(uint8_t reason)
{
    switch (reason)
    {
        case DBG_CLOSE_END:    return "end";
        case DBG_CLOSE_STOLEN: return "stolen";
        case DBG_CLOSE_ZERO:   return "zero";
        default:               return "?";
    }
}

void dbgTakeSnapshot()
{
    dbgSnap.ringHead = dbgRingHead;
    dbgSnap.eventsTotal = dbgEventsTotal;
    dbgSnap.hitCount = dbgHitCount;
    dbgSnap.stealCount = dbgStealCount;
    dbgSnap.closeUnderflow = dbgCloseUnderflow;
    dbgSnap.maxSimultaneous = dbgMaxSimultaneous;
    dbgSnap.naturalClose = dbgNaturalClose;
    dbgSnap.stolenClose = dbgStolenClose;
    dbgSnap.exhaustedClose = dbgExhaustedClose;
    dbgSnap.stolenBeforeEnd = dbgStolenBeforeEnd;
    dbgSnap.hitIntMinUs = dbgHitIntMinUs;
    dbgSnap.hitIntMaxUs = dbgHitIntMaxUs;
    dbgSnap.hitIntTotalUs = dbgHitIntTotalUs;
    dbgSnap.hitIntCount = dbgHitIntCount;
    dbgSnap.failAuditTotal = dbgFailAuditTotal;

    // Range durasi sample valid (dihitung ulang tiap
    // snapshot dari metadata existing — murah, 12 slot)

    dbgExpDurMinUs = UINT32_MAX;
    dbgExpDurMaxUs = 0;

    for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
    {
        const uint32_t dur =
            dbgSampleExpectedDurUs(s);

        if (dur == 0)
        {
            continue;
        }

        if (dur < dbgExpDurMinUs)
        {
            dbgExpDurMinUs = dur;
        }

        if (dur > dbgExpDurMaxUs)
        {
            dbgExpDurMaxUs = dur;
        }
    }

    if (dbgExpDurMinUs == UINT32_MAX)
    {
        dbgExpDurMinUs = 0;
    }

    dbgSnap.expDurMinUs = dbgExpDurMinUs;
    dbgSnap.expDurMaxUs = dbgExpDurMaxUs;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        dbgSnap.allocCount[v] = dbgAllocCount[v];
        dbgSnap.closeCount[v] = dbgCloseCount[v];
        dbgSnap.prefetchCount[v] = dbgPrefetchCount[v];
        dbgSnap.prefetchFailCount[v] =
            dbgPrefetchFailCount[v];
        dbgSnap.lifeMinUs[v] = dbgLifeMinUs[v];
        dbgSnap.lifeMaxUs[v] = dbgLifeMaxUs[v];
        dbgSnap.lifeTotalUs[v] = dbgLifeTotalUs[v];
        dbgSnap.lifeCount[v] = dbgLifeCount[v];
    }

    for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
    {
        dbgSnap.smpStarts[s] = dbgSmpStarts[s];
        dbgSnap.smpNatural[s] = dbgSmpNatural[s];
        dbgSnap.smpStolen[s] = dbgSmpStolen[s];
        dbgSnap.smpExhausted[s] = dbgSmpExhausted[s];
        dbgSnap.smpLifeMinUs[s] = dbgSmpLifeMinUs[s];
        dbgSnap.smpLifeMaxUs[s] = dbgSmpLifeMaxUs[s];
        dbgSnap.smpLifeTotalUs[s] = dbgSmpLifeTotalUs[s];
        dbgSnap.smpLifeCount[s] = dbgSmpLifeCount[s];
        dbgSnap.smpRemMin[s] = dbgSmpRemMin[s];
        dbgSnap.smpRemMax[s] = dbgSmpRemMax[s];
        dbgSnap.smpRemTotal[s] = dbgSmpRemTotal[s];
        dbgSnap.smpRemCount[s] = dbgSmpRemCount[s];
    }

    for (uint8_t b = 0; b <= VOICE_COUNT; b++)
    {
        dbgSnap.activeDist[b] = dbgActiveDist[b];
    }

    // Umur voice yang MASIH aktif saat snapshot

    uint32_t longestNow = 0;
    uint64_t ageSum = 0;
    uint32_t ageCount = 0;

    const uint32_t nowUs = micros();

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v])
        {
            continue;
        }

        const uint32_t age =
            nowUs - voiceStartMicros[v];

        if (age > longestNow)
        {
            longestNow = age;
        }

        ageSum += age;
        ageCount++;
    }

    dbgSnap.longestActiveNowUs = longestNow;

    dbgSnap.avgActiveAgeNowUs =
        (ageCount > 0)
            ? (uint32_t)(ageSum / ageCount)
            : 0;

    // ACTIVE_WITH_ZERO_REMAINING (saat snapshot)

    uint32_t zeroRem = 0;
    uint32_t zeroRemStrict = 0;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v] ||
            voiceRemaining[v] != 0)
        {
            continue;
        }

        zeroRem++;

        if (voiceBufferBytes[v] == 0 &&
            voiceBufferTarget[v] == 0)
        {
            zeroRemStrict++;
        }
    }

    dbgZeroRemainActive += zeroRem;
    dbgZeroRemainActiveStrict += zeroRemStrict;

    dbgSnap.zeroRemainActive = dbgZeroRemainActive;
    dbgSnap.zeroRemainActiveStrict =
        dbgZeroRemainActiveStrict;

    for (uint8_t i = 0; i < DBG_RING_SIZE; i++)
    {
        dbgSnap.ring[i] = dbgRing[i];
    }

    // Ring steal audit + fail audit

    for (uint8_t i = 0; i < DBG_STEAL_AUDIT_MAX; i++)
    {
        dbgSnap.stealAudit[i] = dbgStealAudit[i];
    }

    dbgSnap.stealAuditCount =
        (dbgStealAuditTotal < DBG_STEAL_AUDIT_MAX)
            ? (uint8_t)dbgStealAuditTotal
            : DBG_STEAL_AUDIT_MAX;

    for (uint8_t i = 0; i < DBG_FAIL_AUDIT_MAX; i++)
    {
        dbgSnap.failAudit[i] = dbgFailAudit[i];
    }

    dbgSnap.failAuditCount =
        (dbgFailAuditTotal < DBG_FAIL_AUDIT_MAX)
            ? (uint8_t)dbgFailAuditTotal
            : DBG_FAIL_AUDIT_MAX;

    dbgSnapEventCount =
        (dbgEventsTotal < DBG_RING_SIZE)
            ? (uint16_t)dbgEventsTotal
            : (uint16_t)DBG_RING_SIZE;

    dbgPrintLine = 0;
    dbgSnapPending = true;
}

void dbgResetCounters()
{
    dbgHitCount = 0;
    dbgStealCount = 0;
    dbgCloseUnderflow = 0;
    dbgMaxSimultaneous = 0;
    dbgEventsTotal = 0;
    dbgRingHead = 0;
    dbgHitOrdinal = 0;
    dbgNaturalClose = 0;
    dbgStolenClose = 0;
    dbgExhaustedClose = 0;
    dbgStolenBeforeEnd = 0;
    dbgZeroRemainActive = 0;
    dbgZeroRemainActiveStrict = 0;
    dbgStealAuditTotal = 0;
    dbgStealAuditHead = 0;
    dbgFailAuditTotal = 0;
    dbgFailAuditHead = 0;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        dbgAllocCount[v] = 0;
        dbgCloseCount[v] = 0;
        dbgPrefetchCount[v] = 0;
        dbgPrefetchFailCount[v] = 0;
        dbgLifeMinUs[v] = 0;
        dbgLifeMaxUs[v] = 0;
        dbgLifeTotalUs[v] = 0;
        dbgLifeCount[v] = 0;
    }

    for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
    {
        dbgSmpStarts[s] = 0;
        dbgSmpNatural[s] = 0;
        dbgSmpStolen[s] = 0;
        dbgSmpExhausted[s] = 0;
        dbgSmpLifeMinUs[s] = 0;
        dbgSmpLifeMaxUs[s] = 0;
        dbgSmpLifeTotalUs[s] = 0;
        dbgSmpLifeCount[s] = 0;
        dbgSmpRemMin[s] = 0;
        dbgSmpRemMax[s] = 0;
        dbgSmpRemTotal[s] = 0;
        dbgSmpRemCount[s] = 0;
    }

    for (uint8_t b = 0; b <= VOICE_COUNT; b++)
    {
        dbgActiveDist[b] = 0;
    }

    dbgHitLastMicros = 0;
    dbgHitIntMinUs = 0;
    dbgHitIntMaxUs = 0;
    dbgHitIntTotalUs = 0;
    dbgHitIntCount = 0;
}

void dbgPrintEventLine(const DbgEvent &ev)
{
    switch (ev.type)
    {
        case DBG_EVT_HIT:
            Serial0.printf(
                "HIT#%u t=%lu smp=%u voice=%u act=%u mask=%c%c%c%c%c%c%c%c\n",
                ev.hitNo,
                ev.ts,
                ev.sample,
                ev.voice,
                ev.activeCount,
                (ev.activeMask & 1) ? '1' : '0',
                (ev.activeMask & 2) ? '1' : '0',
                (ev.activeMask & 4) ? '1' : '0',
                (ev.activeMask & 8) ? '1' : '0',
                (ev.activeMask & 16) ? '1' : '0',
                (ev.activeMask & 32) ? '1' : '0',
                (ev.activeMask & 64) ? '1' : '0',
                (ev.activeMask & 128) ? '1' : '0'
            );
            break;

        case DBG_EVT_START:
            Serial0.printf(
                "START t=%lu voice=%u sample=%u vel=%u expected=%luus\n",
                ev.ts,
                ev.voice,
                ev.sample,
                ev.velocity,
                ev.aux2
            );
            break;

        case DBG_EVT_CLOSE:
            Serial0.printf(
                "CLOSE t=%lu voice=%u sample=%u reason=%s life=%luus expected=%luus diff=%ldus\n",
                ev.ts,
                ev.voice,
                ev.sample,
                dbgCloseReasonName(ev.reason),
                ev.aux,
                ev.aux2,
                (int32_t)(ev.aux - ev.aux2)
            );
            break;

        case DBG_EVT_PREFETCH:
            Serial0.printf(
                "PREFETCH t=%lu voice=%u bytes=%u\n",
                ev.ts,
                ev.voice,
                ev.bytes
            );
            break;

        case DBG_EVT_STEAL:
            Serial0.printf(
                "STEAL t=%lu newSmp=%u newVel=%u stoleVoice=%u victimSmp=%u victimLife=%luus expected=%luus act=%u mask=%c%c%c%c%c%c%c%c\n",
                ev.ts,
                ev.sample,
                ev.velocity,
                ev.voice,
                ev.hitNo,
                ev.aux,
                ev.aux2,
                ev.activeCount,
                (ev.activeMask & 1) ? '1' : '0',
                (ev.activeMask & 2) ? '1' : '0',
                (ev.activeMask & 4) ? '1' : '0',
                (ev.activeMask & 8) ? '1' : '0',
                (ev.activeMask & 16) ? '1' : '0',
                (ev.activeMask & 32) ? '1' : '0',
                (ev.activeMask & 64) ? '1' : '0',
                (ev.activeMask & 128) ? '1' : '0'
            );
            break;

        default:
            break;
    }
}

// ======================================================
// MIX PIPELINE AUDIT (instrumentation-only)
// ======================================================
constexpr uint8_t DBG_ONSET_MAX = 10;
constexpr uint8_t DBG_MIX_MAX = 10;
constexpr uint8_t DBG_WR_MAX = 16;
constexpr uint32_t DBG_MIX_PERIOD_MS = 4000;

struct DbgOnsetRec
{
    uint32_t tsUs;
    uint32_t expectedDurUs;
    uint16_t ordinal;
    uint8_t voiceSlot;
    uint8_t sampleIndex;
};

struct DbgMixRec
{
    uint32_t tsUs;
    uint32_t playEtaUs;
    uint32_t readPos[VOICE_COUNT];
    uint32_t remaining[VOICE_COUNT];
    uint16_t seq;
    uint16_t mixBytes;
    uint16_t pendAfter;
    uint8_t activeCount;
    uint8_t slotsMask;
};

struct DbgWrRec
{
    uint32_t tsUs;
    uint16_t bytesWritten;
};

DbgOnsetRec dbgOnsetRing[DBG_ONSET_MAX];
DbgMixRec dbgMixRing[DBG_MIX_MAX];
DbgWrRec dbgWrRing[DBG_WR_MAX];

uint8_t dbgOnsetHead = 0;
uint8_t dbgMixHead = 0;
uint8_t dbgWrHead = 0;
uint16_t dbgOnsetTotal = 0;
uint16_t dbgMixTotal = 0;
uint16_t dbgWrTotal = 0;
uint16_t dbgWrZeroCount = 0;
uint16_t dbgMixSeq = 0;
uint16_t dbgHitOrdinalCounter = 0;

uint32_t dbgPreStartUs[VOICE_COUNT];
uint32_t dbgPrefillUs[VOICE_COUNT];
bool dbgPrefilled[VOICE_COUNT];

// Observational-only producer-ahead tracker
// (identik dengan pacing watermark yang di-rollback,
//  TAPI tidak pernah meng-gate produksi apa pun)

uint32_t dbgObsAheadBytes = 0;
uint32_t dbgObsRefUs = 0;
bool dbgObsValid = false;
uint32_t dbgObsAheadMinUs = UINT32_MAX;
uint32_t dbgObsAheadMaxUs = 0;

// Occupancy DMA NYATA (dari counter on_sent)

uint32_t dbgDmaAheadMinUs = UINT32_MAX;
uint32_t dbgDmaAheadMaxUs = 0;
uint32_t dbgGateBlocks = 0;

uint32_t dbgObsAheadUs()
{
    return (uint32_t)(((uint64_t)dbgObsAheadBytes *
                       1000UL) /
                      192UL);
}

void dbgObsDecay()
{
    const uint32_t now = micros();

    if (!dbgObsValid)
    {
        dbgObsRefUs = now;
        dbgObsValid = true;
        return;
    }

    const uint64_t consumed =
        ((uint64_t)(now - dbgObsRefUs)) * 192UL /
        1000UL;

    dbgObsRefUs = now;

    if (dbgObsAheadBytes > consumed)
    {
        dbgObsAheadBytes -= (uint32_t)consumed;
    }
    else
    {
        dbgObsAheadBytes = 0;
    }
}

void dbgMixAuditResetWindow()
{
    dbgOnsetHead = 0;
    dbgMixHead = 0;
    dbgWrHead = 0;
    dbgOnsetTotal = 0;
    dbgMixTotal = 0;
    dbgWrTotal = 0;
    dbgWrZeroCount = 0;
    dbgMixSeq = 0;
    dbgHitOrdinalCounter = 0;
    dbgObsAheadMinUs = UINT32_MAX;
    dbgObsAheadMaxUs = 0;
    dbgDmaAheadMinUs = UINT32_MAX;
    dbgDmaAheadMaxUs = 0;
    dbgGateBlocks = 0;
}

void dbgMixAuditRun()
{
    if constexpr (!PROFILE_MIX_AUDIT)
    {
        return;
    }

    static bool primed = false;
    static uint32_t phaseTsMs = 0;
    static uint16_t step = 0;
    static uint16_t prnOnCount = 0;
    static uint16_t prnMixCount = 0;
    static uint16_t prnMixIdx = 0;
    static uint8_t prnScan = 0;
    static char line[192];

    if (!primed)
    {
        primed = true;
        phaseTsMs = millis();
        return;
    }

    if (step == 0)
    {
        if (millis() - phaseTsMs <
            DBG_MIX_PERIOD_MS)
        {
            return;
        }

        prnOnCount =
            dbgOnsetTotal < DBG_ONSET_MAX
                ? dbgOnsetTotal
                : DBG_ONSET_MAX;
        prnMixCount =
            dbgMixTotal < DBG_MIX_MAX
                ? dbgMixTotal
                : DBG_MIX_MAX;
        step = 1;

        Serial0.println("=== MIX AUDIT ===");
        return;
    }

    if (step == 1)
    {
        snprintf(line, sizeof(line),
                 "w=%lu zw=%lu obs=%lu/%lu/%luus dma=%lu/%luus blk=%lu",
                 (unsigned long)dbgWrTotal,
                 (unsigned long)dbgWrZeroCount,
                 (unsigned long)dbgObsAheadUs(),
                 (unsigned long)(dbgObsAheadMinUs ==
                                         UINT32_MAX
                                     ? 0UL
                                     : dbgObsAheadMinUs),
                 (unsigned long)dbgObsAheadMaxUs,
                 (unsigned long)(dbgDmaAheadMinUs ==
                                         UINT32_MAX
                                     ? 0UL
                                     : dbgDmaAheadMinUs),
                 (unsigned long)dbgDmaAheadMaxUs,
                 (unsigned long)dbgGateBlocks);
        Serial0.println(line);
        Serial0.println("-- ONSETS --");

        step = 2;
        return;
    }

    if (step < (uint16_t)(2 + prnOnCount))
    {
        const uint16_t k =
            (uint16_t)((2 + prnOnCount) - 1 - step);
        const uint16_t idx =
            (uint16_t)((dbgOnsetHead + DBG_ONSET_MAX -
                        1 - k) %
                       DBG_ONSET_MAX);
        const DbgOnsetRec &r = dbgOnsetRing[idx];

        snprintf(line, sizeof(line),
                 "ONSET t=%lu slot=%u smp=%u ord=%u exp=%luus prefill=%s%luus",
                 (unsigned long)r.tsUs,
                 (unsigned)r.voiceSlot,
                 (unsigned)r.sampleIndex,
                 (unsigned)r.ordinal,
                 (unsigned long)r.expectedDurUs,
                 dbgPrefilled[r.voiceSlot] ? ""
                                           : "pend>",
                 (unsigned long)dbgPrefillUs[r.voiceSlot]);
        Serial0.println(line);

        step++;

        if (step == (uint16_t)(2 + prnOnCount))
        {
            Serial0.println("-- MIXES --");
            prnMixIdx = 0;
            prnScan = 0;
            step++;
        }

        return;
    }

    if (step == (uint16_t)(2 + prnOnCount))
    {
        Serial0.println("-- MIXES --");
        prnMixIdx = 0;
        prnScan = 0;
        step++;
        return;
    }

    if (prnMixIdx < prnMixCount)
    {
        const uint16_t idx =
            (uint16_t)((dbgMixHead + DBG_MIX_MAX - 1 -
                        prnMixIdx) %
                       DBG_MIX_MAX);
        const DbgMixRec &r = dbgMixRing[idx];

        if (prnScan == 0)
        {
            char msk[VOICE_COUNT + 1];

            for (uint8_t b = 0; b < VOICE_COUNT;
                 ++b)
            {
                msk[b] =
                    (r.slotsMask & (1 << b))
                        ? '1'
                        : '0';
            }

            msk[VOICE_COUNT] = '\0';

            snprintf(line, sizeof(line),
                     "MIX#%u t=%lu eta=+%luus act=%u slots=%s bytes=%u pend->%u",
                     (unsigned)r.seq,
                     (unsigned long)r.tsUs,
                     (unsigned long)(r.playEtaUs -
                                     r.tsUs),
                     (unsigned)r.activeCount,
                     msk,
                     (unsigned)r.mixBytes,
                     (unsigned)r.pendAfter);
            Serial0.println(line);

            prnScan = 1;
            return;
        }

        int8_t vFound = -1;

        for (uint8_t b = (uint8_t)(prnScan - 1);
             b < VOICE_COUNT; ++b)
        {
            if (r.slotsMask & (1 << b))
            {
                vFound = (int8_t)b;
                break;
            }
        }

        if (vFound >= 0)
        {
            snprintf(line, sizeof(line),
                     "  v%u rp=%lu rem=%lu",
                     (unsigned)vFound,
                     (unsigned long)r.readPos[vFound],
                     (unsigned long)
                         r.remaining[vFound]);
            Serial0.println(line);

            prnScan = (uint8_t)(vFound + 2);
            return;
        }

        prnMixIdx++;
        prnScan = 0;
        return;
    }

    Serial0.println("=================");

    dbgMixAuditResetWindow();
    phaseTsMs = millis();
    step = 0;
}

// ======================================================
// VOICE MANAGEMENT
// ======================================================

void clearVoiceBuffer(uint8_t voiceIndex)
{
    voiceBufferBytes[voiceIndex] = 0;
    voiceBufferTarget[voiceIndex] = 0;
}

void closeVoice(
    uint8_t voiceIndex,
    uint8_t dbgReason
)
{
    if constexpr (PROFILE_2VOICE_DEBUG)
    {
        const uint32_t nowUs = micros();
        const uint32_t lifeUs =
            nowUs - voiceStartMicros[voiceIndex];
        const uint8_t smpIdx =
            voiceSampleIndex[voiceIndex];

        dbgCloseCount[voiceIndex]++;
        dbgCountClose();

        if (dbgReason == DBG_CLOSE_END)
        {
            dbgNaturalClose++;
            dbgSmpNatural[smpIdx]++;
        }
        else if (dbgReason == DBG_CLOSE_STOLEN)
        {
            dbgStolenClose++;
            dbgSmpStolen[smpIdx]++;
        }
        else
        {
            dbgExhaustedClose++;
            dbgSmpExhausted[smpIdx]++;
        }

        // Lifetime stat per slot

        if (dbgLifeCount[voiceIndex] == 0)
        {
            dbgLifeMinUs[voiceIndex] = lifeUs;
        }
        else if (lifeUs < dbgLifeMinUs[voiceIndex])
        {
            dbgLifeMinUs[voiceIndex] = lifeUs;
        }

        if (lifeUs > dbgLifeMaxUs[voiceIndex])
        {
            dbgLifeMaxUs[voiceIndex] = lifeUs;
        }

        dbgLifeTotalUs[voiceIndex] += lifeUs;
        dbgLifeCount[voiceIndex]++;

        // Lifetime stat per sample

        if (dbgSmpLifeCount[smpIdx] == 0)
        {
            dbgSmpLifeMinUs[smpIdx] = lifeUs;
        }
        else if (lifeUs < dbgSmpLifeMinUs[smpIdx])
        {
            dbgSmpLifeMinUs[smpIdx] = lifeUs;
        }

        if (lifeUs > dbgSmpLifeMaxUs[smpIdx])
        {
            dbgSmpLifeMaxUs[smpIdx] = lifeUs;
        }

        dbgSmpLifeTotalUs[smpIdx] += lifeUs;
        dbgSmpLifeCount[smpIdx]++;

        dbgLogEvent(
            DBG_EVT_CLOSE,
            voiceIndex,
            smpIdx,
            0,
            (uint8_t)(cachedActiveVoiceCount - 1),
            0,
            0,
            dbgReason,
            lifeUs,
            voiceExpectedDurUs[voiceIndex]
        );
    }

    voiceActive[voiceIndex] = false;
    voicePtr[voiceIndex] = nullptr;
    voiceReadPos[voiceIndex] = 0;
    voiceTotalSize[voiceIndex] = 0;
    clearVoiceBuffer(voiceIndex);

    cachedActiveVoiceCount--;
}

bool prefetchVoiceImpl(uint8_t voiceIndex)
{
    if (!voiceActive[voiceIndex])
        return false;

    if (voiceBufferTarget[voiceIndex] == 0)
    {
        if (voiceRemaining[voiceIndex] == 0)
        {
            closeVoice(voiceIndex, DBG_CLOSE_END);
            return false;
        }

        const bool mono =
            (voiceChannels[voiceIndex] == 1);
        const size_t maxRead =
            mono ? (size_t)(BUFFER_BYTES / 2)
                 : (size_t)BUFFER_BYTES;

        voiceBufferTarget[voiceIndex] =
            min(
                maxRead,
                (size_t)voiceRemaining[voiceIndex]
            );

        voiceBufferTarget[voiceIndex] -=
            voiceBufferTarget[voiceIndex] % FRAME_SIZE;

        if (voiceBufferTarget[voiceIndex] == 0)
        {
            closeVoice(voiceIndex, DBG_CLOSE_ZERO);
            return false;
        }
    }

    if (voiceBufferBytes[voiceIndex] >=
        voiceBufferTarget[voiceIndex])
    {
        return false;
    }

    const size_t bytesPending =
        voiceBufferTarget[voiceIndex] -
        voiceBufferBytes[voiceIndex];

    const uint8_t *src =
        voicePtr[voiceIndex] + voiceReadPos[voiceIndex];

    memcpy(
        voiceBuffer[voiceIndex] +
            voiceBufferBytes[voiceIndex],
        src,
        bytesPending
    );

    voiceBufferBytes[voiceIndex] += bytesPending;
    voiceReadPos[voiceIndex] += bytesPending;
    voiceRemaining[voiceIndex] -= bytesPending;

    if (voiceChannels[voiceIndex] == 1 &&
        voiceBufferBytes[voiceIndex] > 0)
    {
        size_t monoSamples =
            voiceBufferBytes[voiceIndex] / 2;
        int16_t *buf = reinterpret_cast<int16_t *>(
            voiceBuffer[voiceIndex]
        );

        for (int i = (int)monoSamples - 1; i >= 0; i--)
        {
            buf[i * 2] = buf[i];
            buf[i * 2 + 1] = buf[i];
        }

        voiceBufferBytes[voiceIndex] *= 2;
    }

    return true;
}

bool prefetchVoice(uint8_t voiceIndex)
{
    if constexpr (!PROFILE_2VOICE_DEBUG)
    {
        return prefetchVoiceImpl(voiceIndex);
    }

    dbgPrefetchCount[voiceIndex]++;

    const size_t bytesBefore =
        voiceBufferBytes[voiceIndex];

    const bool ok =
        prefetchVoiceImpl(voiceIndex);

    const size_t bytesAdded =
        voiceBufferBytes[voiceIndex] - bytesBefore;

    if (bytesAdded == 0)
    {
        dbgPrefetchFailCount[voiceIndex]++;

        // Klasifikasi state SAAT gagal, dari state
        // existing saja (tanpa fallback/perilaku baru):
        //   1 inactive | 2 drained-await-close
        //   3 zero-target | 4 already-full

        uint8_t failReason = 0;

        if (!voiceActive[voiceIndex])
        {
            failReason = 1;
        }
        else if (voiceBufferTarget[voiceIndex] == 0 &&
                 voiceRemaining[voiceIndex] == 0)
        {
            failReason = 2;
        }
        else if (voiceBufferTarget[voiceIndex] == 0)
        {
            failReason = 3;
        }
        else
        {
            failReason = 4;
        }

        dbgFailAudit[dbgFailAuditHead] =
            DbgFailAudit{
                micros(),
                (uint32_t)voiceBufferTarget[voiceIndex],
                (uint32_t)voiceBufferBytes[voiceIndex],
                voiceRemaining[voiceIndex],
                voiceIndex,
                voiceSampleIndex[voiceIndex],
                failReason
            };

        dbgFailAuditHead =
            (uint8_t)((dbgFailAuditHead + 1) %
                      DBG_FAIL_AUDIT_MAX);
        dbgFailAuditTotal++;
    }
    else
    {
        dbgLogEvent(
            DBG_EVT_PREFETCH,
            voiceIndex,
            0,
            0,
            cachedActiveVoiceCount,
            (uint16_t)bytesAdded,
            0,
            0
        );
    }

    return ok;
}

void startVoice(
    uint8_t voiceIndex,
    uint8_t sampleIndex,
    uint8_t velocity
)
{
    if (!audioReady)
        return;

    if (!psramIsLoaded())
        return;

    if (sampleIndex >= PSRAM_MAX_SAMPLES)
        return;

    const uint8_t *data = psramGetData(sampleIndex);

    if (!data)
    {
        return;
    }

    const uint32_t dataSize =
        psramGetDataSize(sampleIndex);

    if (dataSize < FRAME_SIZE)
        return;

    if (voiceActive[voiceIndex])
    {
        if constexpr (PROFILE_2VOICE_DEBUG)
        {
            dbgStealCount++;

            // Data korban SEBELUM slot ditutup.
            // remainingBytesAtSteal = unread PSRAM +
            // data yang sudah di-prefetch tapi belum dimix.

            const uint8_t victimSmp =
                voiceSampleIndex[voiceIndex];
            const uint32_t victimRem =
                voiceRemaining[voiceIndex];
            const uint32_t victimBuf =
                voiceBufferBytes[voiceIndex];
            const uint32_t victimLife =
                micros() - voiceStartMicros[voiceIndex];

            if (victimRem + victimBuf > 0)
            {
                dbgStolenBeforeEnd++;
            }

            if (dbgSmpRemCount[victimSmp] == 0)
            {
                dbgSmpRemMin[victimSmp] = victimRem;
            }
            else if (victimRem < dbgSmpRemMin[victimSmp])
            {
                dbgSmpRemMin[victimSmp] = victimRem;
            }

            if (victimRem > dbgSmpRemMax[victimSmp])
            {
                dbgSmpRemMax[victimSmp] = victimRem;
            }

            dbgSmpRemTotal[victimSmp] += victimRem;
            dbgSmpRemCount[victimSmp]++;

            dbgStealAudit[dbgStealAuditHead] =
                DbgStealAudit{
                    micros(),
                    voiceExpectedDurUs[voiceIndex],
                    victimLife,
                    victimRem,
                    victimBuf,
                    voiceTotalSize[voiceIndex],
                    voiceIndex,
                    victimSmp
                };

            dbgStealAuditHead =
                (uint8_t)((dbgStealAuditHead + 1) %
                          DBG_STEAL_AUDIT_MAX);
            dbgStealAuditTotal++;

            // Event STEAL: log SEBELUM korban di-close
            // supaya activeMask masih menampilkan slot
            // korban sebagai aktif.

            dbgLogEvent(
                DBG_EVT_STEAL,
                voiceIndex,
                sampleIndex,
                velocity,
                cachedActiveVoiceCount,
                0,
                victimSmp,
                DBG_CLOSE_STOLEN,
                victimLife,
                voiceExpectedDurUs[voiceIndex]
            );
        }

        closeVoice(voiceIndex, DBG_CLOSE_STOLEN);
    }

    if constexpr (PROFILE_TRIGGER_LATENCY)
    {
        clearAudioEvent(voiceIndex);
    }

    voicePtr[voiceIndex] = data;
    voiceTotalSize[voiceIndex] = dataSize;
    voiceReadPos[voiceIndex] = 0;
    voiceSampleIndex[voiceIndex] = sampleIndex;
    voiceChannels[voiceIndex] =
        psramGetInfo(sampleIndex).channels;

    clearVoiceBuffer(voiceIndex);

    voiceRemaining[voiceIndex] = dataSize;

    voiceVelocityIndex[voiceIndex] = velocity;

    voiceActive[voiceIndex] = true;

    voiceStartStamp[voiceIndex] =
        voiceAgeCounter++;

    cachedActiveVoiceCount++;

    if constexpr (PROFILE_2VOICE_DEBUG)
    {
        if (cachedActiveVoiceCount >
            dbgMaxSimultaneous)
        {
            dbgMaxSimultaneous =
                cachedActiveVoiceCount;
        }

        // Titik nol lifetime voice ini

        voiceStartMicros[voiceIndex] = micros();
        voiceExpectedDurUs[voiceIndex] =
            dbgSampleExpectedDurUs(sampleIndex);

        dbgSmpStarts[sampleIndex]++;

        dbgLogEvent(
            DBG_EVT_START,
            voiceIndex,
            sampleIndex,
            velocity,
            cachedActiveVoiceCount,
            0,
            0,
            0,
            0,
            voiceExpectedDurUs[voiceIndex]
        );
    }

    if constexpr (PROFILE_MIX_AUDIT)
    {
        DbgOnsetRec &r = dbgOnsetRing[dbgOnsetHead];

        r.tsUs = micros();
        r.expectedDurUs =
            dbgSampleExpectedDurUs(sampleIndex);
        r.ordinal = dbgHitOrdinalCounter++;
        r.voiceSlot = voiceIndex;
        r.sampleIndex = sampleIndex;

        dbgOnsetHead =
            (uint8_t)((dbgOnsetHead + 1) %
                      DBG_ONSET_MAX);
        dbgOnsetTotal++;

        dbgPreStartUs[voiceIndex] = r.tsUs;
        dbgPrefilled[voiceIndex] = false;
        dbgPrefillUs[voiceIndex] = 0;
    }
}

void fireHit(uint8_t padIndex, uint8_t velocity)
{
    if (padIndex >= PAD_COUNT)
        return;

    uint8_t voiceIndex = 0;
    bool foundIdle = false;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v])
        {
            voiceIndex = v;
            foundIdle = true;
            break;
        }
    }

    if (!foundIdle)
    {
        // Steal weakest voice (lowest velocity), not oldest.
        // Ghost notes less noticeable when cut off than loud accents.
        uint8_t weakestVelocity = 255;

        for (uint8_t v = 0; v < VOICE_COUNT; v++)
        {
            const uint8_t vel = voiceVelocityIndex[v];

            if (v == 0 || vel < weakestVelocity)
            {
                weakestVelocity = vel;
                voiceIndex = v;
            }
        }
    }

#if DEBUG_PLAYABILITY
    if (!foundIdle)
    {
        pbSteal++;
    }
    if (cachedActiveVoiceCount > pbMaxActive)
    {
        pbMaxActive = cachedActiveVoiceCount;
    }
#endif

    if constexpr (PROFILE_2VOICE_DEBUG)
    {
        dbgHitCount++;
        dbgAllocCount[voiceIndex]++;

        // Hit-to-hit interval

        const uint32_t nowUsHit = micros();

        if (dbgHitLastMicros != 0)
        {
            const uint32_t interval =
                nowUsHit - dbgHitLastMicros;

            if (dbgHitIntCount == 0)
            {
                dbgHitIntMinUs = interval;
            }
            else if (interval < dbgHitIntMinUs)
            {
                dbgHitIntMinUs = interval;
            }

            if (interval > dbgHitIntMaxUs)
            {
                dbgHitIntMaxUs = interval;
            }

            dbgHitIntTotalUs += interval;
            dbgHitIntCount++;
        }

        dbgHitLastMicros = nowUsHit;

        if (dbgHitOrdinal < 0xFFFF)
        {
            dbgHitOrdinal++;
        }

        dbgLogEvent(
            DBG_EVT_HIT,
            voiceIndex,
            padIndex,
            velocity,
            cachedActiveVoiceCount,
            0,
            dbgHitOrdinal,
            foundIdle ? 0 : 1
        );
    }

    // ---------------------------------------------------
    // Resolusi sample: padIndex → PSRAM slot via registry.
    // O(1), memory-only. Dibangun oleh kit loader di
    // control plane (saat kit commit) DAN di-update oleh
    // PAD -> SOUND (psramSetPadSlot). AudioTask hanya
    // membaca tabel ini — tidak ada SD/malloc/String.
    // Nilai slot boleh 0..PSRAM_MAX_SAMPLES-1 (slot runtime
    // milik file yang baru dimuat) atau PSRAM_SLOT_NONE
    // (pad di-UNASSIGN). startVoice() menolak keduanya di
    // luar range dengan bail senyap, jadi:
    //   - kit belum dimuat / source slot invalid -> fallback
    //     identitas (backward-compat), psramGetData()
    //     nullptr -> startVoice bails aman;
    //   - pad UNASSIGN -> senyap tanpa menyentuh data
    //     sample pad lain.
    // Remap runtime = satu store uint8 (atomic) dari
    // control plane, jadi hit bersamaan memakai slot lama
    // atau baru — keduanya valid & PCM-nya sudah termuat.
    // ---------------------------------------------------

    const uint8_t psramSlot = psramSlotForPad(padIndex);

    startVoice(voiceIndex, psramSlot, velocity);

    if constexpr (PROFILE_TRIGGER_LATENCY)
    {
        audioEvent[voiceIndex].t0 = micros();
        audioEvent[voiceIndex].t0Valid = true;
    }
}

    // ======================================================
    // PHASE 2: FreeRTOS AudioTask + SPSC hit ring
    // ======================================================
    //
    // Transport trigger -> audio. Producer = loopTask
    // (trigger), consumer = AudioTask. AudioTask adalah
    // owner tunggal seluruh voice state; fireHit() hanya
    // dipanggil dari sini.
    //
    // Overflow ring: DROP NEWEST + counter, tidak pernah
    // blocking di producer maupun consumer.

    struct AudioHitEvent
    {
        uint8_t pad;
        uint8_t velocity;
    };

    // Power-of-two; usable slot = SIZE - 1 (>= 32)

    constexpr size_t AUDIO_HIT_RING_SIZE = 64;
    constexpr size_t AUDIO_HIT_RING_MASK =
        AUDIO_HIT_RING_SIZE - 1;

    constexpr uint32_t AUDIO_TASK_STACK_SIZE = 4096;

    // Phase 1 hotfix: priority 24 (FreeRTOS max di environment
    // ini adalah <25, jadi 24 adalah tertinggi yang valid).
    // Core 0. TriggerTask kini di Core 1 (tidak lagi bersaing).

    constexpr UBaseType_t AUDIO_TASK_PRIORITY = 24;

    // Park maksimal 1 tick per iterasi: fail-safe jika
    // notification hilang/terlambat — flush pendingBytes,
    // pacing gate, prefetch, mix tetap berjalan.

    constexpr TickType_t AUDIO_TASK_PARK_TICKS = 1;

    AudioHitEvent audioHitRing[AUDIO_HIT_RING_SIZE];
    volatile uint8_t audioHitHead = 0;
    volatile uint8_t audioHitTail = 0;
    volatile uint32_t audioHitOverflowCount = 0;

    // Hit acceptance gate (kit-switch support). Ditulis oleh
    // UART task (Core 1, control plane), dibaca oleh producer
    // TriggerTask (Core 1) dan consumer AudioTask (Core 0).
    // Selalu diakses via __atomic_* (satu byte, lock-free,
    // tanpa mutex/blocking di realtime path).
    bool audioHitAccept = true;

    inline bool audioHitAcceptNow()
    {
        return __atomic_load_n(
            &audioHitAccept, __ATOMIC_RELAXED);
    }

    TaskHandle_t audioTaskHandle = nullptr;

    // Core tempat I2S driver dibuat = core tempat setup()/
    // loop() berjalan. I2S/DMA berjalan di ISR secara
    // independen dari core task, sehingga AudioTask aman
    // di-pin ke Core 0 meskipun driver di-init di Core 1.

    int audioI2SCore = -1;

    // Core aktual tempat AudioTask dipin. Phase 1: Core 0
    // (core real-time audio/trigger).

    int audioTaskRunCore = -1;

    bool audioHitPush(uint8_t padIndex, uint8_t velocity)
    {
        const uint8_t head = audioHitHead;
        const uint8_t tail =
            __atomic_load_n(&audioHitTail, __ATOMIC_ACQUIRE);
        const uint8_t next =
            (uint8_t)((head + 1) & AUDIO_HIT_RING_MASK);

        if (next == tail)
        {
            __atomic_fetch_add(
                &audioHitOverflowCount,
                1,
                __ATOMIC_RELAXED
            );

            return false;
        }

        audioHitRing[head].pad = padIndex;
        audioHitRing[head].velocity = velocity;

        __atomic_store_n(&audioHitHead, next, __ATOMIC_RELEASE);

#if DEBUG_PLAYABILITY
        pbPushed++;
#endif

        return true;
    }

    bool audioHitPop(AudioHitEvent *out)
    {
        const uint8_t tail = audioHitTail;
        const uint8_t head =
            __atomic_load_n(&audioHitHead, __ATOMIC_ACQUIRE);

        if (tail == head)
        {
            return false;
        }

        out->pad = audioHitRing[tail].pad;
        out->velocity = audioHitRing[tail].velocity;

        __atomic_store_n(
            &audioHitTail,
            (uint8_t)((tail + 1) & AUDIO_HIT_RING_MASK),
            __ATOMIC_RELEASE
        );

#if DEBUG_PLAYABILITY
        pbConsumed++;
#endif

        return true;
    }

    void audioTaskDrainHits()
    {
        AudioHitEvent ev;

        // Baca gate sekali per burst drain. Nol biaya saat tidak
        // ada hit (while tidak pernah masuk).
        const bool accept = audioHitAcceptNow();

        while (audioHitPop(&ev))
        {
            // Kit switch pending: buang hit yang masih tersisa di
            // ring TANPA membuat voice. Voice yang sudah aktif
            // tetap berjalan normal (tidak dipotong/dibunuh).
            if (!accept)
            {
                continue;
            }

            fireHit(ev.pad, ev.velocity);
        }
    }

    void audioTaskLoop(void *)
    {
        for (;;)
        {
            // 1. Drain hit event -> voice allocation/steal
            //    (context AudioTask)

            audioTaskDrainHits();

            // 2. Engine pass existing apa adanya:
            //    flush pending -> pacing gate -> prefetch ->
            //    allReady -> mix -> set pending

            audioTask();

            ulTaskNotifyTake(pdTRUE, AUDIO_TASK_PARK_TICKS);
        }
    }
}

// ======================================================
// PUBLIC API
// ======================================================

void audioMixAuditTask()
{
    if constexpr (PROFILE_MIX_AUDIT)
    {
        dbgMixAuditRun();
    }
}

void audioInit()
{
    buildGainTable();

    initVoiceState();

    i2s.setPins(PIN_BCLK, PIN_LRCK, PIN_DOUT);

    // Catat core yang mengeksekusi init ini = core tempat
    // I2S driver dibuat. AudioTask akan dipin ke core
    // yang sama (runtime-determined, bukan asumsi).

    audioI2SCore = (int)xPortGetCoreID();

    if (!i2s.begin(
            I2S_MODE_STD,
            SAMPLE_RATE,
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO))
    {
        Serial0.println("[AUDIO] I2S INIT FAILED");
        return;
    }

    i2s_event_callbacks_t pacingCbs = {};

    pacingCbs.on_sent = audioOnTxSent;

    audioPacingCbInstalled =
        i2s_channel_register_event_callback(
            i2s.txChan(),
            &pacingCbs,
            nullptr
        ) == ESP_OK;

    audioReady = true;

    if (!audioPacingCbInstalled)
    {
        Serial0.println(
            "[AUDIO] PACING CB FAILED (fail-open)"
        );
    }

    Serial0.println("[AUDIO] I2S OK, 48kHz/16bit/Stereo");
}

uint8_t audioActiveVoiceCount()
{
    return cachedActiveVoiceCount;
}

void audioSetHitAccept(bool accept)
{
    __atomic_store_n(
        &audioHitAccept, accept, __ATOMIC_RELEASE);
}

bool audioHitRingEmpty()
{
    const uint8_t head =
        __atomic_load_n(&audioHitHead, __ATOMIC_RELAXED);
    const uint8_t tail =
        __atomic_load_n(&audioHitTail, __ATOMIC_ACQUIRE);

    return (head == tail);
}

uint32_t audioActiveVoiceMaxRemainingUs()
{
    if (cachedActiveVoiceCount == 0)
    {
        return 0;
    }

    uint32_t maxUs = 0;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v])
        {
            continue;
        }

        const SampleInfo &info =
            psramGetInfo(voiceSampleIndex[v]);

        if (!info.valid || info.sampleRate == 0)
        {
            continue;
        }

        // Sisa PCM yang belum diputar, dalam byte setara
        // STEREO 16-bit (unit yang dikonsumsi mixer):
        //   stereo : voiceRemaining sudah byte stereo
        //   mono   : voiceRemaining byte mono x2
        // voiceBufferBytes sudah selalu byte stereo.
        uint32_t remainingBytes = voiceBufferBytes[v];

        if (voiceChannels[v] == 2)
        {
            remainingBytes += voiceRemaining[v];
        }
        else
        {
            remainingBytes +=
                (uint32_t)voiceRemaining[v] * 2u;
        }

        const uint64_t bytesPerSec =
            (uint64_t)info.sampleRate * 4u; // 2ch * 16bit

        const uint32_t us = (uint32_t)(
            (uint64_t)remainingBytes * 1000000ULL /
            bytesPerSec);

        if (us > maxUs)
        {
            maxUs = us;
        }
    }

    return maxUs;
}

int audioTaskCore()
{
    return audioTaskRunCore;
}

void audioTrigger(
    uint8_t padIndex,
    uint8_t velocity
)
{
    // Phase 2: producer side. Push event ke SPSC ring,
    // wake AudioTask via notification (hint — tidak
    // kritis; task tetap berjalan dengan park 1 tick).

#if DEBUG_PLAYABILITY
    pbTriggerHits++;
#endif

    // Kit switch pending: buang hit baru (tidak di-enqueue).
    // Sisa hit yang sudah masuk ring di-handle consumer
    // (audioTaskDrainHits). Pengecekan 1 atomic load, tanpa
    // mutex/blocking — timing audio lain tidak berubah.
    if (!audioHitAcceptNow())
    {
        return;
    }

    if (!audioHitPush(padIndex, velocity))
    {
        return;
    }

    if (audioTaskHandle != nullptr)
    {
        xTaskNotifyGive(audioTaskHandle);
    }
}

uint32_t audioHitRingOverflowCount()
{
    return (uint32_t)__atomic_load_n(
        &audioHitOverflowCount, __ATOMIC_RELAXED);
}

AudioPlayabilityStats audioPlayabilityGet()
{
    AudioPlayabilityStats s;

#if DEBUG_PLAYABILITY
    s.triggerHits  = pbTriggerHits;
    s.pushed       = pbPushed;
    s.dropped      = (uint32_t)__atomic_load_n(
                         &audioHitOverflowCount,
                         __ATOMIC_RELAXED);
    s.consumed     = pbConsumed;
    s.maxActive    = pbMaxActive;
    s.steal        = pbSteal;
    s.prefetchMaxUs = pbPrefetchMaxUs;
    s.prefetchFail = pbPrefetchFail;
#else
    s.triggerHits = 0;
    s.pushed = 0;
    s.dropped = 0;
    s.consumed = 0;
    s.maxActive = 0;
    s.steal = 0;
    s.prefetchMaxUs = 0;
    s.prefetchFail = 0;
#endif

    return s;
}

void audioTask()
{
    uint32_t taskStart = micros();

    if constexpr (PROFILE_MIX_AUDIT)
    {
        dbgObsDecay();

        const uint32_t aheadUs = dbgObsAheadUs();

        if (aheadUs < dbgObsAheadMinUs)
        {
            dbgObsAheadMinUs = aheadUs;
        }

        if (aheadUs > dbgObsAheadMaxUs)
        {
            dbgObsAheadMaxUs = aheadUs;
        }
    }

    // 1. Write pending I2S data

    if (pendingBytes != 0)
    {
        size_t bytesWritten = 0;

        i2s_channel_write(
            i2s.txChan(),
            mixBuffer + pendingOffset,
            pendingBytes - pendingOffset,
            &bytesWritten,
            0
        );

        pendingOffset += bytesWritten;

        audioLoadedBytes += (uint32_t)bytesWritten;

        if constexpr (PROFILE_MIX_AUDIT)
        {
            DbgWrRec &wr = dbgWrRing[dbgWrHead];

            wr.tsUs = micros();
            wr.bytesWritten =
                (uint16_t)bytesWritten;

            dbgWrHead =
                (uint8_t)((dbgWrHead + 1) %
                          DBG_WR_MAX);
            dbgWrTotal++;

            if (bytesWritten == 0)
            {
                dbgWrZeroCount++;
            }

            dbgObsAheadBytes +=
                (uint32_t)bytesWritten;
        }

        if (pendingOffset >= pendingBytes)
        {
            pendingBytes = 0;
            pendingOffset = 0;
        }

        if constexpr (PROFILE_TRIGGER_LATENCY)
        {
            const uint32_t now = micros();

            updateTimingStats(
                audioTimingStats[cachedActiveVoiceCount].i2sWrite,
                now - taskStart
            );

            for (uint8_t v = 0; v < VOICE_COUNT; v++)
            {
                if (!voiceActive[v])
                    continue;

                if (audioEvent[v].t2Valid &&
                    !audioEvent[v].t3Valid)
                {
                    audioEvent[v].t3 = now;
                    audioEvent[v].t3Valid = true;

                    recordE2eDelta(
                        v,
                        cachedActiveVoiceCount
                    );

                    clearAudioEvent(v);
                }
            }
        }
    }

    if (pendingBytes > 0)
    {
        return;
    }

    // 2. Tidak ada voice aktif

    if (cachedActiveVoiceCount == 0)
    {
        return;
    }

    // 2b. Pacing: hanya produksi jika ahead DMA < batas

    if constexpr (PROFILE_MIX_AUDIT)
    {
        const uint32_t paceAhead =
            dmaCurrentAheadBytes();

        const uint32_t paceAheadUs =
            (uint32_t)(((uint64_t)paceAhead *
                        1000UL) /
                       192UL);

        if (paceAheadUs < dbgDmaAheadMinUs)
        {
            dbgDmaAheadMinUs = paceAheadUs;
        }

        if (paceAheadUs > dbgDmaAheadMaxUs)
        {
            dbgDmaAheadMaxUs = paceAheadUs;
        }
    }

    if (!dmaRoomForNextChunk())
    {
        if constexpr (PROFILE_MIX_AUDIT)
        {
            dbgGateBlocks++;
        }

        return;
    }

    // 3. Prefetch semua voice (dari PSRAM)

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v])
            continue;

        if (voiceBufferTarget[v] == 0 ||
            voiceBufferBytes[v] <
                voiceBufferTarget[v])
        {
            uint32_t readStart = micros();

            prefetchVoice(v);

#if DEBUG_PLAYABILITY
            {
                const uint32_t pfUs = micros() - readStart;
                if (pfUs > pbPrefetchMaxUs)
                {
                    pbPrefetchMaxUs = pfUs;
                }
                if (voiceBufferTarget[v] > 0 &&
                    voiceBufferBytes[v] <
                        voiceBufferTarget[v])
                {
                    pbPrefetchFail++;
                }
            }
#endif

            if constexpr (PROFILE_MIX_AUDIT)
            {
                if (!dbgPrefilled[v] &&
                    voiceBufferTarget[v] > 0 &&
                    voiceBufferBytes[v] >=
                        voiceBufferTarget[v])
                {
                    dbgPrefilled[v] = true;
                    dbgPrefillUs[v] =
                        micros() - dbgPreStartUs[v];
                }
            }

            if constexpr (PROFILE_TRIGGER_LATENCY)
            {
                updateTimingStats(
                    audioTimingStats[cachedActiveVoiceCount].psramRead,
                    micros() - readStart
                );

                if (audioEvent[v].t0Valid &&
                    !audioEvent[v].t1Valid)
                {
                    audioEvent[v].t1 = micros();
                    audioEvent[v].t1Valid = true;
                }
            }
        }
    }

    // 4. Mix voice yang SUDAH ready saja.
    //
    // Sebelumnya: gate "allReady" menghentikan SELURUH mixer
    // jika SATU voice belum penuh buffernya — satu voice yang
    // terlambat bisa membuat kick+snare+hat lain berhenti
    // (dropout/glitch saat multi-pad). Sekarang voice yang
    // belum siap diskip (buffernya dipertahankan, di-prefetch
    // + dimix pada pass berikutnya) tanpa menghentikan voice
    // lain yang sudah siap.

    // 5. MIX (integer fixed-point)

    uint32_t mixStart = micros();

    uint8_t mixVoiceCount = cachedActiveVoiceCount;

    memset(accumBuffer, 0, sizeof(accumBuffer));

    size_t mixSize = 0;

    for (uint8_t v = 0; v < VOICE_COUNT; v++)
    {
        if (!voiceActive[v])
            continue;

        // Hanya mix voice yang buffernya sudah penuh target.
        // Voice yang belum siap diskip; buffernya tidak
        // di-clear, jadi tetap dipakai pass berikutnya.
        if (voiceBufferBytes[v] <
            voiceBufferTarget[v])
        {
            continue;
        }

        const size_t bytesRead =
            voiceBufferBytes[v];

        const uint8_t velIdx =
            voiceVelocityIndex[v];
        const int32_t gain =
            (velIdx < 128)
                ? gainTable[velIdx]
                : GAIN_SCALE;

        const int16_t *samples =
            reinterpret_cast<const int16_t *>(
                voiceBuffer[v]
            );

        const size_t sampleCount =
            bytesRead / 2;

        for (size_t i = 0; i < sampleCount; i++)
        {
            accumBuffer[i] +=
                (samples[i] * gain) >> 8;
        }

        if (bytesRead > mixSize)
            mixSize = bytesRead;

        clearVoiceBuffer(v);
    }

    if constexpr (PROFILE_TRIGGER_LATENCY)
    {
        const uint32_t mixDur = micros() - mixStart;

        updateTimingStats(
            audioTimingStats[cachedActiveVoiceCount].mixing,
            mixDur
        );

        if (mixVoiceCount >= 1 &&
            mixVoiceCount <= VOICE_COUNT)
        {
            updateTimingStats(
                mixPerVoice[mixVoiceCount].timing,
                mixDur
            );
        }

        const uint32_t mixEnd = micros();

        for (uint8_t v = 0; v < VOICE_COUNT; v++)
        {
            if (!voiceActive[v])
                continue;

            if (audioEvent[v].t1Valid &&
                !audioEvent[v].t2Valid)
            {
                audioEvent[v].t2 = mixEnd;
                audioEvent[v].t2Valid = true;
            }
        }
    }

    if (mixSize == 0)
    {
        return;
    }

    // Clamp dengan headroom -6 dB (MIX_HEADROOM_SHIFT = 1):
    // hasil sum dibagi 2 sebelum clamp. Tanpa ini, beberapa
    // voice full-scale yang bersamaan (mis. kick+snare+hat)
    // melebihi int16 dan di-hard-clip, merusak keseimbangan —
    // suara lain kedengaran hilang/terlalu quiet.
    constexpr int MIX_HEADROOM_SHIFT = 1;

    int16_t *out =
        reinterpret_cast<int16_t *>(mixBuffer);

    size_t sampleCount = mixSize / 2;

    for (size_t i = 0; i < sampleCount; i++)
    {
        int32_t value =
            accumBuffer[i] >> MIX_HEADROOM_SHIFT;

        if (value > 32767)
            value = 32767;
        else if (value < -32768)
            value = -32768;

        out[i] = (int16_t)value;
    }

    if constexpr (PROFILE_MIX_AUDIT)
    {
        DbgMixRec &r = dbgMixRing[dbgMixHead];

        r.tsUs = micros();
        r.playEtaUs =
            r.tsUs +
            (uint32_t)(((uint64_t)(dbgObsAheadBytes +
                                   mixSize)) *
                       1000UL / 192UL);
        r.seq = dbgMixSeq++;
        r.mixBytes = (uint16_t)mixSize;
        r.pendAfter = (uint16_t)mixSize;
        r.activeCount = cachedActiveVoiceCount;
        r.slotsMask = 0;

        for (uint8_t v = 0; v < VOICE_COUNT; v++)
        {
            if (!voiceActive[v])
            {
                continue;
            }

            r.slotsMask |=
                (uint8_t)((1u << v) & 0xFF);
            r.readPos[v] = voiceReadPos[v];
            r.remaining[v] = voiceRemaining[v];
        }

        dbgMixHead =
            (uint8_t)((dbgMixHead + 1) % DBG_MIX_MAX);
        dbgMixTotal++;
    }

    // 6. Set pending for next I2S write

    pendingBytes = mixSize;
    pendingOffset = 0;

    if constexpr (PROFILE_TRIGGER_LATENCY)
    {
        updateTimingStats(
            audioTimingStats[cachedActiveVoiceCount].task,
            micros() - taskStart
        );
    }
}

void audioTimingProfileReport()
{
    if constexpr (!PROFILE_TRIGGER_LATENCY)
    {
        return;
    }

    TimingStats aggTask, aggRead, aggMix, aggI2S;
    aggTask.min = UINT32_MAX;
    aggRead.min = UINT32_MAX;
    aggMix.min = UINT32_MAX;
    aggI2S.min = UINT32_MAX;

    for (uint8_t v = 0; v <= VOICE_COUNT; v++)
    {
        AudioTimingStats &s = audioTimingStats[v];

        if (s.task.count == 0)
            continue;

        mergeTimingStats(aggTask, s.task);
        mergeTimingStats(aggRead, s.psramRead);
        mergeTimingStats(aggMix, s.mixing);
        mergeTimingStats(aggI2S, s.i2sWrite);

        s = AudioTimingStats{};
    }

    if (aggTask.count == 0)
        return;

    Serial0.println("=== AUDIO TIMING PROFILE ===");

    Serial0.printf(
        "task avg/min/max = %lu / %lu / %lu us\n",
        averageTiming(aggTask),
        aggTask.min,
        aggTask.max
    );

    Serial0.printf(
        "read avg/min/max = %lu / %lu / %lu us\n",
        averageTiming(aggRead),
        aggRead.min,
        aggRead.max
    );

    Serial0.printf(
        "MIX  avg/min/max = %lu / %lu / %lu us\n",
        averageTiming(aggMix),
        aggMix.min,
        aggMix.max
    );

    Serial0.printf(
        "I2S  avg/min/max = %lu / %lu / %lu us\n",
        averageTiming(aggI2S),
        aggI2S.min,
        aggI2S.max
    );

    Serial0.println("============================");
}

void audioEndToEndProfileReport()
{
    if constexpr (!PROFILE_TRIGGER_LATENCY)
    {
        return;
    }

    // ----------------------------------------------
    // Aggregate end-to-end stats
    // ----------------------------------------------

    TimingStats aggTTV, aggVTP, aggPTI, aggTotal;
    aggTTV.min = UINT32_MAX;
    aggVTP.min = UINT32_MAX;
    aggPTI.min = UINT32_MAX;
    aggTotal.min = UINT32_MAX;

    for (uint8_t v = 1; v <= VOICE_COUNT; v++)
    {
        AudioEndToEndStats &s = e2eStats[v];

        if (s.total.count == 0)
            continue;

        mergeTimingStats(aggTTV, s.triggerToVoice);
        mergeTimingStats(aggVTP, s.voiceToPCM);
        mergeTimingStats(aggPTI, s.pcmToI2s);
        mergeTimingStats(aggTotal, s.total);

        e2eStats[v] = AudioEndToEndStats{};
    }

    Serial0.println(
        "=== AUDIO END-TO-END PROFILE ==="
    );

    if (aggTotal.count == 0)
    {
        Serial0.println("hits=0 (no valid samples)");
    }
    else
    {
        Serial0.printf(
            "hits=%lu\n",
            aggTotal.count
        );

        if (aggTTV.count > 0)
        {
            Serial0.printf(
                "trigger->voice min/avg/max = %lu/%lu/%lu us\n",
                aggTTV.min,
                averageTiming(aggTTV),
                aggTTV.max
            );
        }
        else
        {
            Serial0.println(
                "trigger->voice n=0 (no valid samples)"
            );
        }

        if (aggVTP.count > 0)
        {
            Serial0.printf(
                "voice->PCM     min/avg/max = %lu/%lu/%lu us\n",
                aggVTP.min,
                averageTiming(aggVTP),
                aggVTP.max
            );
        }
        else
        {
            Serial0.println(
                "voice->PCM     n=0 (no valid samples)"
            );
        }

        if (aggPTI.count > 0)
        {
            Serial0.printf(
                "PCM->I2S       min/avg/max = %lu/%lu/%lu us\n",
                aggPTI.min,
                averageTiming(aggPTI),
                aggPTI.max
            );
        }
        else
        {
            Serial0.println(
                "PCM->I2S       n=0 (no valid samples)"
            );
        }

        Serial0.printf(
            "total fw audio min/avg/max = %lu/%lu/%lu us\n",
            aggTotal.min,
            averageTiming(aggTotal),
            aggTotal.max
        );

        Serial0.printf(
            "buffer duration = %lu us\n",
            BUFFER_DURATION_US
        );
    }

    Serial0.println(
        "================================"
    );

    // ----------------------------------------------
    // Mix per voice count
    // ----------------------------------------------

    Serial0.println("=== MIX PROFILE ===");

    for (uint8_t v = 1; v <= VOICE_COUNT; v++)
    {
        MixPerVoiceStats &s = mixPerVoice[v];

        if (s.timing.count == 0)
        {
            Serial0.printf(
                "%u voice n=0 (no valid samples)\n",
                v
            );

            continue;
        }

        Serial0.printf(
            "%u voice  n=%lu  min/avg/max = %lu/%lu/%lu us\n",
            v,
            s.timing.count,
            s.timing.min,
            averageTiming(s.timing),
            s.timing.max
        );

        s = MixPerVoiceStats{};
    }

    Serial0.println("====================");

    // ----------------------------------------------
    // PSRAM read (existing, preserved)
    // ----------------------------------------------

    Serial0.println("=== PSRAM READ PROFILE ===");

    bool anyPsram = false;

    for (uint8_t v = 1; v <= VOICE_COUNT; v++)
    {
        AudioTimingStats &s = audioTimingStats[v];

        if (s.psramRead.count == 0)
            continue;

        anyPsram = true;

        Serial0.printf(
            "%u active  n=%lu  min/avg/max = %lu/%lu/%lu us\n",
            v,
            s.psramRead.count,
            s.psramRead.min,
            averageTiming(s.psramRead),
            s.psramRead.max
        );

        s = AudioTimingStats{};
    }

    if (!anyPsram)
    {
        Serial0.println("n=0 (no valid samples)");
    }

    Serial0.println("===========================");

    // ----------------------------------------------
    // I2S write (existing, preserved)
    // ----------------------------------------------

    Serial0.println("=== I2S WRITE PROFILE ===");

    bool anyI2s = false;

    for (uint8_t v = 1; v <= VOICE_COUNT; v++)
    {
        AudioTimingStats &s = audioTimingStats[v];

        if (s.i2sWrite.count == 0)
            continue;

        anyI2s = true;

        Serial0.printf(
            "%u active  n=%lu  min/avg/max = %lu/%lu/%lu us\n",
            v,
            s.i2sWrite.count,
            s.i2sWrite.min,
            averageTiming(s.i2sWrite),
            s.i2sWrite.max
        );

        s = AudioTimingStats{};
    }

    if (!anyI2s)
    {
        Serial0.println("n=0 (no valid samples)");
    }

    Serial0.println("==========================");
}

// ======================================================
// 2-VOICE DEBUG DUMP
// ======================================================
//
// Snapshot counter+event ring tiap 5 detik, lalu
// print SATU BARIS per panggilan (chunked) supaya
// UART tidak memblokir superloop terlalu lama.

void audioTwoVoiceDebugTask()
{
    if constexpr (!PROFILE_2VOICE_DEBUG)
    {
        return;
    }

    // Active voice count distribution:
    // di-sample sekali per loop pass.

    if (cachedActiveVoiceCount <= VOICE_COUNT)
    {
        dbgActiveDist[cachedActiveVoiceCount]++;
    }

    if (!dbgSnapPending)
    {
        const uint32_t now = millis();

        if ((now - dbgLastReportMs) <
            DBG_REPORT_INTERVAL_MS)
        {
            return;
        }

        dbgLastReportMs = now;

        dbgTakeSnapshot();

        return;
    }

    const uint16_t firstEventLine = 8;
    const uint16_t lifeHeaderLine =
        firstEventLine + dbgSnapEventCount;
    const uint16_t distLabelLine =
        lifeHeaderLine + 1 + VOICE_COUNT + 1;
    const uint16_t hitIntLine = distLabelLine + 1 + VOICE_COUNT + 1;

    // Section audit sample duration vs lifetime

    const uint16_t sdaHdrLine = hitIntLine + 1;
    const uint16_t vlsHdrLine =
        sdaHdrLine + 1 + PSRAM_MAX_SAMPLES;
    const uint16_t zraHdrLine =
        vlsHdrLine + 1 + PSRAM_MAX_SAMPLES * 2;
    const uint16_t saHdrLine = zraHdrLine + 3;
    const uint16_t pfHdrLine =
        saHdrLine + 1 + DBG_STEAL_AUDIT_MAX;
    const uint16_t pftLine =
        pfHdrLine + 1 + DBG_FAIL_AUDIT_MAX;
    const uint16_t sumHdrLine = pftLine + 1;
    const uint16_t footerLine = sumHdrLine + 6;

    const uint16_t line = dbgPrintLine;

    if (line >= footerLine)
    {
        dbgSnapPending = false;
        return;
    }

    if (line == 0)
    {
        Serial0.println("=== 2 VOICE DEBUG ===");
    }
    else if (line == 1)
    {
        Serial0.printf(
            "hits=%lu maxSimul=%u steal=%lu closeUnderflow=%lu\n",
            dbgSnap.hitCount,
            dbgSnap.maxSimultaneous,
            dbgSnap.stealCount,
            dbgSnap.closeUnderflow
        );
    }
    else if (line == 2)
    {
        Serial0.printf(
            "alloc[] = %lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
            dbgSnap.allocCount[0],
            dbgSnap.allocCount[1],
            dbgSnap.allocCount[2],
            dbgSnap.allocCount[3],
            dbgSnap.allocCount[4],
            dbgSnap.allocCount[5],
            dbgSnap.allocCount[6],
            dbgSnap.allocCount[7]
        );
    }
    else if (line == 3)
    {
        Serial0.printf(
            "close[] = %lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
            dbgSnap.closeCount[0],
            dbgSnap.closeCount[1],
            dbgSnap.closeCount[2],
            dbgSnap.closeCount[3],
            dbgSnap.closeCount[4],
            dbgSnap.closeCount[5],
            dbgSnap.closeCount[6],
            dbgSnap.closeCount[7]
        );
    }
    else if (line == 4)
    {
        Serial0.printf(
            "pre[]   = %lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
            dbgSnap.prefetchCount[0],
            dbgSnap.prefetchCount[1],
            dbgSnap.prefetchCount[2],
            dbgSnap.prefetchCount[3],
            dbgSnap.prefetchCount[4],
            dbgSnap.prefetchCount[5],
            dbgSnap.prefetchCount[6],
            dbgSnap.prefetchCount[7]
        );
    }
    else if (line == 5)
    {
        Serial0.printf(
            "preFail[] = %lu/%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
            dbgSnap.prefetchFailCount[0],
            dbgSnap.prefetchFailCount[1],
            dbgSnap.prefetchFailCount[2],
            dbgSnap.prefetchFailCount[3],
            dbgSnap.prefetchFailCount[4],
            dbgSnap.prefetchFailCount[5],
            dbgSnap.prefetchFailCount[6],
            dbgSnap.prefetchFailCount[7]
        );
    }
    else if (line == 6)
    {
        Serial0.printf(
            "naturalClose=%lu stolenClose=%lu exhaustedClose=%lu maxSimul=%u\n",
            dbgSnap.naturalClose,
            dbgSnap.stolenClose,
            dbgSnap.exhaustedClose,
            dbgSnap.maxSimultaneous
        );
    }
    else if (line == 7)
    {
        Serial0.printf(
            "-- LAST %u EVENTS (of %lu) --\n",
            dbgSnapEventCount,
            dbgSnap.eventsTotal
        );
    }
    else if (line < lifeHeaderLine)
    {
        const uint16_t evIdx =
            (uint16_t)((line - firstEventLine +
                        dbgSnap.ringHead +
                        DBG_RING_SIZE -
                        dbgSnapEventCount) %
                       DBG_RING_SIZE);

        dbgPrintEventLine(dbgSnap.ring[evIdx]);
    }
    else if (line == lifeHeaderLine)
    {
        Serial0.println("=== VOICE LIFETIME PROFILE ===");
    }
    else if (line < lifeHeaderLine + 1 + VOICE_COUNT)
    {
        const uint8_t v =
            (uint8_t)(line - lifeHeaderLine - 1);

        if (dbgSnap.lifeCount[v] == 0)
        {
            Serial0.printf(
                "voice %u min/avg/max = n=0\n",
                v
            );
        }
        else
        {
            Serial0.printf(
                "voice %u min/avg/max = %lu/%lu/%lu us (n=%lu)\n",
                v,
                dbgSnap.lifeMinUs[v],
                (uint32_t)(
                    dbgSnap.lifeTotalUs[v] /
                    dbgSnap.lifeCount[v]),
                dbgSnap.lifeMaxUs[v],
                dbgSnap.lifeCount[v]
            );
        }
    }
    else if (line < distLabelLine - 1)
    {
        // Baris ringkas setelah tabel lifetime

        Serial0.printf(
            "longestActiveNow=%luus avgActiveAgeNow=%luus maxSimul=%u\n",
            dbgSnap.longestActiveNowUs,
            dbgSnap.avgActiveAgeNowUs,
            dbgSnap.maxSimultaneous
        );
    }
    else if (line == distLabelLine - 1)
    {
        Serial0.println("active distribution:");
    }
    else if (line < hitIntLine - 1)
    {
        const uint8_t b =
            (uint8_t)(line - distLabelLine);

        Serial0.printf(
            "%u=%lu\n",
            b,
            dbgSnap.activeDist[b]
        );
    }
    else if (line == hitIntLine - 1)
    {
        if (dbgSnap.hitIntCount == 0)
        {
            Serial0.println(
                "hit interval min/avg/max = n=0"
            );
        }
        else
        {
            Serial0.printf(
                "hit interval min/avg/max = %lu/%lu/%lu us (n=%lu)\n",
                dbgSnap.hitIntMinUs,
                (uint32_t)(
                    dbgSnap.hitIntTotalUs /
                    dbgSnap.hitIntCount),
                dbgSnap.hitIntMaxUs,
                dbgSnap.hitIntCount
            );
        }
    }

    // ================================================
    // SAMPLE DURATION AUDIT
    // ================================================

    else if (line == sdaHdrLine)
    {
        Serial0.println("=== SAMPLE DURATION AUDIT ===");
    }
    else if (line > sdaHdrLine && line < vlsHdrLine)
    {
        const uint8_t s =
            (uint8_t)(line - sdaHdrLine - 1);

        const SampleInfo &info = psramGetInfo(s);

        if (!psramIsLoaded() || !info.valid)
        {
            Serial0.printf(
                "sample %u path=%s INVALID\n",
                s,
                PAD_SAMPLE_PATHS[s]
            );
        }
        else
        {
            Serial0.printf(
                "sample %u path=%s rate=%lu ch=%u bits=%u dataBytes=%lu expectedDuration=%luus\n",
                s,
                PAD_SAMPLE_PATHS[s],
                info.sampleRate,
                info.channels,
                info.bitsPerSample,
                info.dataSize,
                dbgSampleExpectedDurUs(s)
            );
        }
    }

    // ================================================
    // VOICE LIFETIME vs SAMPLE (per sample)
    // ================================================

    else if (line == vlsHdrLine)
    {
        Serial0.println("=== VOICE LIFETIME vs SAMPLE ===");
    }
    else if (line > vlsHdrLine && line < zraHdrLine)
    {
        const uint16_t k =
            (uint16_t)(line - vlsHdrLine - 1);
        const uint8_t s =
            (uint8_t)(k / 2);

        if ((k % 2) == 0)
        {
            Serial0.printf(
                "sample %u: starts=%lu naturalClose=%lu stolenClose=%lu exhaustedClose=%lu expectedDuration=%luus\n",
                s,
                dbgSnap.smpStarts[s],
                dbgSnap.smpNatural[s],
                dbgSnap.smpStolen[s],
                dbgSnap.smpExhausted[s],
                dbgSampleExpectedDurUs(s)
            );
        }
        else
        {
            if (dbgSnap.smpLifeCount[s] == 0 &&
                dbgSnap.smpRemCount[s] == 0)
            {
                Serial0.printf(
                    "sample %u: life=n=0 remAtSteal=n=0\n",
                    s
                );
            }
            else
            {
                const uint32_t lifeAvg =
                    (dbgSnap.smpLifeCount[s] > 0)
                        ? (uint32_t)(
                              dbgSnap.smpLifeTotalUs[s] /
                              dbgSnap.smpLifeCount[s])
                        : 0;

                const uint32_t remAvg =
                    (dbgSnap.smpRemCount[s] > 0)
                        ? (uint32_t)(
                              dbgSnap.smpRemTotal[s] /
                              dbgSnap.smpRemCount[s])
                        : 0;

                Serial0.printf(
                    "sample %u: life min/avg/max=%lu/%lu/%luus(n=%lu) remAtSteal min/avg/max=%lu/%lu/%lu(n=%lu)\n",
                    s,
                    dbgSnap.smpLifeMinUs[s],
                    lifeAvg,
                    dbgSnap.smpLifeMaxUs[s],
                    dbgSnap.smpLifeCount[s],
                    dbgSnap.smpRemMin[s],
                    remAvg,
                    dbgSnap.smpRemMax[s],
                    dbgSnap.smpRemCount[s]
                );
            }
        }
    }

    // ================================================
    // ACTIVE WITH ZERO REMAINING
    // ================================================

    else if (line == zraHdrLine)
    {
        Serial0.println(
            "=== ACTIVE WITH ZERO REMAINING ==="
        );
    }
    else if (line == zraHdrLine + 1)
    {
        Serial0.printf(
            "count=%lu (snapshot-sampled, active && remaining==0)\n",
            dbgSnap.zeroRemainActive
        );
    }
    else if (line == zraHdrLine + 2)
    {
        Serial0.printf(
            "strict(buffer also empty)=%lu  WARNING threshold: >0 berarti ada voice hidup tanpa data\n",
            dbgSnap.zeroRemainActiveStrict
        );
    }

    // ================================================
    // STEAL AUDIT
    // ================================================

    else if (line == saHdrLine)
    {
        Serial0.println("=== STEAL AUDIT ===");
    }
    else if (line > saHdrLine && line < pfHdrLine)
    {
        const uint8_t idx =
            (uint8_t)(line - saHdrLine - 1);

        if (idx < dbgSnap.stealAuditCount)
        {
            // Terbaru dulu

            const uint8_t ringIdx = (uint8_t)(
                (dbgStealAuditHead + DBG_STEAL_AUDIT_MAX -
                 1 - idx) % DBG_STEAL_AUDIT_MAX);

            const DbgStealAudit &sa =
                dbgSnap.stealAudit[ringIdx];

            Serial0.printf(
                "STEAL: voice=%u sample=%u expectedDuration=%luus actualLifetime=%luus remainingBytes=%lu(+buf %lu) totalBytes=%lu%s\n",
                sa.voice,
                sa.victimSample,
                sa.expectedDurUs,
                sa.lifeUs,
                sa.remainingBytes,
                sa.bufferBytes,
                sa.totalBytes,
                (sa.remainingBytes + sa.bufferBytes) > 0
                    ? " [NOT FINISHED]"
                    : " [data exhausted]"
            );
        }
    }

    // ================================================
    // PREFETCH FAILURE
    // ================================================

    else if (line == pfHdrLine)
    {
        Serial0.println("=== PREFETCH FAILURE ===");
    }
    else if (line > pfHdrLine && line < pftLine)
    {
        const uint8_t idx =
            (uint8_t)(line - pfHdrLine - 1);

        if (idx < dbgSnap.failAuditCount)
        {
            const uint8_t ringIdx = (uint8_t)(
                (dbgFailAuditHead + DBG_FAIL_AUDIT_MAX -
                 1 - idx) % DBG_FAIL_AUDIT_MAX);

            const DbgFailAudit &fa =
                dbgSnap.failAudit[ringIdx];

            static const char *failNames[] = {
                "?",
                "inactive",
                "drained-await-close (natural end)",
                "zero-target (< FRAME_SIZE left)",
                "already-full (bytes >= target)"
            };

            Serial0.printf(
                "FAIL: voice=%u sample=%u reason/state=%s target=%lu buffered=%lu remaining=%lu\n",
                fa.voice,
                fa.sample,
                failNames[fa.reasonCode <= 4
                              ? fa.reasonCode
                              : 0],
                fa.targetBytes,
                fa.bufferedBytes,
                fa.remainingBytes
            );
        }
    }
    else if (line == pftLine)
    {
        Serial0.printf(
            "prefetch failure total = %lu (per-voice counter di atas)\n",
            dbgSnap.failAuditTotal
        );
    }

    // ================================================
    // SUMMARY
    // ================================================

    else if (line == sumHdrLine)
    {
        Serial0.println("=== SUMMARY ===");
    }
    else if (line > sumHdrLine && line < footerLine)
    {
        switch (line - sumHdrLine - 1)
        {
            case 0:
                Serial0.printf(
                    "sample expected duration range = %lu..%lu us\n",
                    dbgSnap.expDurMinUs,
                    dbgSnap.expDurMaxUs
                );
                break;

            case 1:
            {
                // Range avg lifetime antar slot

                uint32_t avgMin = UINT32_MAX;
                uint32_t avgMax = 0;
                bool any = false;

                for (uint8_t v = 0; v < VOICE_COUNT; v++)
                {
                    if (dbgSnap.lifeCount[v] == 0)
                    {
                        continue;
                    }

                    const uint32_t a = (uint32_t)(
                        dbgSnap.lifeTotalUs[v] /
                        dbgSnap.lifeCount[v]);

                    any = true;

                    if (a < avgMin) avgMin = a;
                    if (a > avgMax) avgMax = a;
                }

                if (!any) avgMin = 0;

                Serial0.printf(
                    "voice lifetime avg range = %lu..%lu us\n",
                    avgMin,
                    avgMax
                );
                break;
            }

            case 2:
                Serial0.printf(
                    "stolen before sample end = %lu / stolen total %lu\n",
                    dbgSnap.stolenBeforeEnd,
                    dbgSnap.stolenClose
                );
                break;

            case 3:
                Serial0.printf(
                    "active with zero remaining = %lu (strict %lu)\n",
                    dbgSnap.zeroRemainActive,
                    dbgSnap.zeroRemainActiveStrict
                );
                break;

            default:
                Serial0.printf(
                    "prefetch failures = %lu\n",
                    dbgSnap.failAuditTotal
                );
                break;
        }
    }
    else
    {
        Serial0.println("============================");

        dbgResetCounters();
        dbgSnapPending = false;
        return;
    }

    dbgPrintLine++;
}

// ======================================================
// PHASE 2: AudioTask lifecycle
// ======================================================

void audioTaskStart()
{
    if (audioTaskHandle != nullptr)
    {
        return;
    }

    // Phase 1: pin ke Core 0 (core real-time audio/trigger)
    // dengan priority 25 (tertinggi). I2S/DMA berjalan di
    // ISR secara independen dari core task, sehingga aman
    // walau driver di-init di Core 1 (setup).

    const BaseType_t core = 0;

    const BaseType_t created = xTaskCreatePinnedToCore(
        audioTaskLoop,
        "AudioTask",
        AUDIO_TASK_STACK_SIZE,
        nullptr,
        AUDIO_TASK_PRIORITY,
        &audioTaskHandle,
        core
    );

    audioTaskRunCore = (int)core;

    if (created != pdPASS)
    {
        audioTaskHandle = nullptr;

        Serial0.println("ERROR: Audio task creation failed");
    }
}
