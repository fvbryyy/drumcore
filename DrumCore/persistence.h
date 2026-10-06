#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <Arduino.h>
#include "config.h"
#include "pad.h"

// ======================================================
// NVS Persistence (Phase 6B)
// ======================================================
//
// Menyimpan user settings per-kit yang dapat diubah user/UI
// ke NVS (ESP32 Preferences).
// JSON di SD = base/factory config; NVS menyimpan override
// persistent di atasnya.
//
// PRINSIP:
//   - NVS TIDAK pernah di akses di realtime path
//     (TriggerTask/AudioTask/MidiTask). Semua pemanggilan
//     dilakukan di saat init / saat user menyimpan.
//   - TIDAK ada write tiap hit. Save hanya via fungsi
//     eksplisit persistenceSave*().
//   - NVS kosong/rusak = aman: firmware tetap boot dgn
//     default / base kit.
//   - Tidak menyimpan: pin, GPIO, scheduler, realtime
//     state, runtime maskTime, audio/voice state, PSRAM
//     pointer, sample PCM, debug counters.
//
// STORAGE:
//   namespace NVS : "drumcore"
//     "schema" (u8)  : schema version
//     "active" (u8)  : activeKit index
//     "volume" (u8)  : volume (placeholder; belum ada
//                      variable runtime yang jelas)
//     "kit0..kit4" (blob) : override per-kit (packed)

// ======================================================
// Public API
// ======================================================

// Begin NVS namespace "drumcore" + cek schema.
// Aman dipanggil sebelum realtime task. Return true jika
// NVS siap (termasuk schema baru / kosong).
bool persistenceInit();

// Baca global (activeKit, volume). Kalau NVS kosong/rusak,
// isi default: activeKit=0, volume=0. Return true jika ada
// nilai tersimpan.
bool persistenceLoadGlobal(
    uint8_t &activeKit,
    uint8_t &volume
);

// Baca override per-kit untuk kitIndex dan apply ke
// PadConfig (config layer + runtime copy).
// Kalau blob tidak ada/rusak -> tidak apply apa-apa.
// Return true jika berhasil apply.
bool persistenceLoadKit(
    uint8_t kitIndex
);

// Simpan global (activeKit, volume). Return true sukses.
bool persistenceSaveGlobal(
    uint8_t activeKit,
    uint8_t volume
);

// Simpan override per-kit dari PadConfig saat ini.
// Return true sukses.
bool persistenceSaveKit(
    uint8_t kitIndex
);

// Hapus override per-kit untuk kitIndex dari NVS.
// Dipakai kit management (CREATE menyapu override lama agar
// kit baru berdiri di atas JSON SD murni; DELETE menghapus
// jejak override kit yang dihilangkan). Idempoten: kit tanpa
// override tetap dianggap sukses.
bool persistenceClearKit(
    uint8_t kitIndex
);

// Simpan global + semua override kit (0..ktCount-1).
// Return true jika semua sukses.
bool persistenceSaveAll(
    uint8_t activeKit,
    uint8_t volume,
    uint8_t kitCount
);

// ------------------------------------------------------------
// MIGRASI NVS per-kit -> kit.json (KIT RUNTIME)
// ------------------------------------------------------------
//
// NVS per-kit blob (kit0..kit4) adalah override legacy yang sekarang
// TIDAK lagi ditulis/dibaca (kit.json = satu-satunya source of truth).
// Fungsi berikut dipakai kit_manager untuk MIGRASI sekali waktu:
// baca blob (tanpa apply ke runtime), merge ke kit.json, lalu hapus.

// true jika blob per-kit valid tersimpan (magic+schema+padCount ok).
bool persistenceHasKitBlob(uint8_t kitIndex);

// Baca override per-kit dari NVS TANPA apply ke config layer/runtime.
// Mengisi out[i] memakai decodePad: hanya field yang VALID menurut
// PAD_FIELD_MIN/MAX yang menimpa (out harus di-init dahulu dengan
// nilai dari kit.json). return false bila blob tidak ada/rusak.
bool persistenceLoadKitConfig(
    uint8_t kitIndex,
    PadConfig out[PAD_COUNT]
);

// Hapus SEMUA blob override per-kit (kecuali yang masih dipakai —
// dipanggil setelah migrasi selesai). Idempoten.
void persistenceClearAllKitOverrides();

#endif // PERSISTENCE_H
