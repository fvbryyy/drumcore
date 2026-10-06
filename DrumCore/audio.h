#ifndef AUDIO_H
#define AUDIO_H

#include <Arduino.h>

void audioInit();

// Phase 2: mulai FreeRTOS AudioTask (panggil SETELAH
// audioInit()). Owner tunggal seluruh voice/audio state.

void audioTaskStart();

uint8_t audioActiveVoiceCount();

// ======================================================
// HIT GATE (kit-switch support)
// ======================================================
//
// Selama kit switch (control plane): hit BARU tidak boleh membuat voice baru;
// voice aktif harus selesai normal dulu sebelum PSRAM di-unload. Dipakai
// performKitSwitch() (UART task, Core 1):
//
//   audioSetHitAccept(false):
//     - audioTrigger() (producer, TriggerTask) membuang hit baru.
//     - AudioTask membuang hit yang masih antri di SPSC ring.
//     Hanya atomic flag lintas-core, tanpa mutex/malloc/print.
//
//   audioHitRingEmpty():
//     true jika seluruh hit di ring sudah dikonsumsi AudioTask. Dipakai
//     sebelum acceptance dihidupkan lagi supaya hit lama tidak "menyala"
//     setelah kit switch selesai.
//
//   audioActiveVoiceMaxRemainingUs():
//     Estimasi sisa waktu (us) hingga voice aktif terpanjang selesai alami.
//     Best-effort, read-only, non-blocking. Dipakai sebagai deadline adaptif
//     (bukan 600 ms tetap), karena crash/cymbal bisa berbunyi detik.

void audioSetHitAccept(bool accept);

bool audioHitRingEmpty();

uint32_t audioActiveVoiceMaxRemainingUs();

int audioTaskCore();

void audioTrigger(
    uint8_t padIndex,
    uint8_t velocity
);

// Read-only diagnostic: jumlah hit yang di-drop ring SPSC
// audio (overflow) karena consumer AudioTask tidak sempat
// menariknya. Non-invasif (hanya membaca counter existing).

uint32_t audioHitRingOverflowCount();

// Snapshot diagnostik playability (DEBUG_PLAYABILITY). Kembali sebagai
// struct; dibaca dari Core 1 oleh laporan.
struct AudioPlayabilityStats
{
    uint32_t triggerHits;
    uint32_t pushed;
    uint32_t dropped;
    uint32_t consumed;
    uint32_t maxActive;
    uint32_t steal;
    uint32_t prefetchMaxUs;
    uint32_t prefetchFail;
};
AudioPlayabilityStats audioPlayabilityGet();

void audioTask();

void audioTimingProfileReport();

void audioEndToEndProfileReport();

void audioTwoVoiceDebugTask();

void audioMixAuditTask();

#endif
