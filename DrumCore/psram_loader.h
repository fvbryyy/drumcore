#ifndef PSRAM_LOADER_H
#define PSRAM_LOADER_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// PSRAM Sample Loader
// ======================================================
//
// Load WAV dari SD card → strip header → simpan PCM di PSRAM.
// Audio engine baca langsung dari PSRAM (~0μs latency).
//
// ======================================================
// G25: LAYOUT PCM — BLOK PER SAMPLE, BUKAN SATU BLOK KIT
// ======================================================
//
// SEBELUMNYA seluruh PCM satu kit tinggal di SATU blok
// ps_malloc(totalPCMBytes) ~5.5 MB, dan setiap SampleInfo
// menunjuk ke offset di dalamnya (psramBuffer + offset).
// Konsekuensinya satu KIT LOAD selalu butuh satu blok kontigu
// sebesar SELURUH kit, sehingga free-list PSRAM yang terpecah
// menggagalkan load:
//
//     free=8135KB  largest=5119KB  requested=5453KB
//     -> PSRAM ALLOC FAILED / cause=FRAGMENTED
//
// SEKARANG setiap sample unik diberi blok PSRAM-nya sendiri.
// Permintaan terbesar yang pernah dilakukan KIT LOAD hanya
// sebesar SATU sample, jadi load tetap berhasil selama free
// TOTAL cukup — ruang yang tersisa tidak perlu kontigu.
//
// Yang TIDAK berubah (kontrak ke audio):
//   - SampleInfo.data / .dataSize tetap per-slot
//   - psramGetData(slot) / psramGetDataSize(slot) tetap
//     per-slot, O(1), tanpa SD
//   - psramSlotForPad() tetap fixed array uint8, O(1),
//     memory-only — audio.cpp tidak berubah satu baris pun
//   - slot ALIAS (dua slot menunjuk file yang sama) tetap
//     berbagi satu blok; blok itu dibebaskan tepat satu kali
//   - KIT LOAD tetap melakukan audio gate + voice drain
//     (dijamin performKitSwitch sebelum fungsi ini dipanggil)

struct SampleInfo
{
    const uint8_t *data;    // pointer ke PSRAM (PCM data)
    uint32_t dataSize;      // bytes PCM (tanpa header)
    uint32_t sampleRate;    // sample rate dari WAV header
    uint8_t channels;       // 1=mono, 2=stereo
    uint8_t bitsPerSample;  // 16 untuk kita
    bool valid;
};

// ======================================================
// Public API
// ======================================================

// Cek PSRAM availability
bool psramSetup();

// Load kit: baca semua WAV dari SD → PSRAM
// samplePaths: array path file WAV (urutan physical PSRAM slot)
// sampleIds:   array sampleId tiap entry, parallel dgn samplePaths
//              (index 0 = pad/slot 0's sampleId, dll). Dipakai
//              untuk membangun runtime slot registry sehingga
//              padIndex tdk harus sama dgn PSRAM slot.
// count: jumlah sample
// return true jika semua berhasil
//
// G25: tiap sample unik dialokasikan sebagai blok PSRAM sendiri,
// jadi total kit TIDAK lagi harus muat dalam satu blok kontigu.
// Kegagalan allocation maupun SD read tetap mengembalikan state
// konsisten (semua blok staging dibuang, kit kosong) + return false
// — tidak pernah STATUS_OK palsu.
bool psramLoadKit(
    const char * const samplePaths[],
    const uint8_t sampleIds[],
    uint8_t count
);

// Unload kit, free PSRAM (SEMUA blok PCM per-slot: kit + runtime)
void psramUnloadKit();

// Dapatkan pointer PCM data untuk sample[index]
const uint8_t *psramGetData(uint8_t index);

// Dapatkan size PCM data untuk sample[index]
uint32_t psramGetDataSize(uint8_t index);

// Dapatkan info lengkap sample
const SampleInfo &psramGetInfo(uint8_t index);

// Kit loaded?
bool psramIsLoaded();

// ======================================================
// Pad → Slot Registry (dibangun saat kit commit)
// ======================================================
//
// Runtime lookup: "slot mana yang harus dipakai audio untuk
// memainkan sample dari PAD tertentu?". Mengembalikan
// physical PSRAM slot (0..PSRAM_MAX_SAMPLES-1).
// Jika kit belum dimuat atau sample tidak valid:
//   -> mengembalikan padIndex (backward-compat fallback).
//
// Kompleksitas: O(1), memory-only. TIDAK memakai SD,
// malloc, String, atau blocking.
//
// Contoh kit remap:
//   sampleId[0]=2, sampleId[1]=0, sampleId[2]=1
//   → slotForPad = [2, 0, 1] → PAD01 memainkan slot 2, dll.

uint8_t psramSlotForPad(uint8_t padIndex);

// Bangun ulang Pad -> Slot registry dari sampleId EFFECTIVE saat
// ini (config layer sudah memuat JSON + override NVS). Dipakai
// saat PAD_SET sampleId (G8-B): remap langsung runtime-effective
// tanpa unload/reload PSRAM. Aturan validasi SAMA dengan commit
// kit load (buildSlotRegistry). Tidak ada mallloc/SD/blocking.
// sampleIds: array PAD_COUNT entry, sampleId per pad.
void psramRebuildSlotRegistry(
    const uint8_t sampleIds[]
);

// ======================================================
// Sample Registry + Runtime Remap (G21)
// ======================================================
//
// Control-plane ini adalah JEMBATAN antara "user melihat nama file"
// dan "audio membutuhkan index slot PSRAM":
//
//     filename  --(psramFindSlotByPath)-->  PSRAM slot
//     pad      --(psramSetPadSlot)------->  PSRAM slot
//
// Registry diisi saat KIT LOAD commit (12 path kit) dan saat satu
// file runtime dimuat. Semua fungsi di sini NON-realtime: dipanggil
// dari UartTask (control plane), bukan dari AudioTask. Tidak ada
// lock, tidak ada SD, tidak ada String pada jalur remap.

// Cari slot PSRAM yang SUDAH memuat file ini.
//   return 0..PSRAM_MAX_SAMPLES-1  kalau ada (slot valid, PCM ada)
//   return PSRAM_SLOT_NONE         kalau belum dimuat
// Lookup linear atas tabel path (maks 16 entri) — murni memory,
// tanpa SD/malloc/file. Panjang path tidak pernah dihitung ulang
// dari SD, jadi aman dipanggil sesering mungkin di control plane.
uint8_t psramFindSlotByPath(const char *absPath);

// Set mapping runtime pad -> slot PSRAM. Ini SATU-SATUNYA operasi
// yang dibutuhkan PAD -> SOUND ketika file sudah ada di PSRAM.
//
// Mengubah satu entri uint8_t pada tabel fixed slotForPad. Store
// byte-aligned = atomic pada ESP32-S3: AudioTask (Core 0) yang
// membaca pada hit yang sama akan melihat nilai LAMA atau nilai
// BARU, tidak pernah setengah-updating, dan keduanya menunjuk slot
// yang PCM-nya sudah termuat. Karena itu TIDAK ada lock di
// realtime path (sesuai arsitektur G8-B yang sudah dipakai).
//
// slot boleh: 0..PSRAM_MAX_SAMPLES-1, atau PSRAM_SLOT_NONE (pad
// senyap). padIndex di luar range -> no-op.
void psramSetPadSlot(
    uint8_t padIndex,
    uint8_t slot
);

// Muat SATU file WAV ke slot PSRAM yang masih kosong, lalu daftarkan
// di registry. Dipakai hanya ketika psramFindSlotByPath() belum
// menemukan file tersebut (file baru di folder samples kit aktif).
//
// Hanya file itu yang dibaca dari SD: sample lain TIDAK di-unload,
// TIDAK di-reload, dan pointer PCM mereka tidak berubah. Slot baru
// memakai blok PSRAM sendiri, sehingga blok sample yang sedang dibaca
// voice tidak tersentuh dan tidak ada pointer yang perlu di-rebase
// (G25: tidak ada lagi satu blok kit yang harus di-offset-kan).
//
// PRECONDITION: kit sudah termuat (psramIsLoaded()) dan pemanggil
// menjamin tidak ada voice yang membaca data yang akan berubah.
// Jalur ini tidak mengubah buffer yang sudah ada, jadi penundaan
// voice tidak diperlukan (lihat performSampleAssign).
//
//   return 0..PSRAM_MAX_SAMPLES-1  slot baru yang terisi
//   return PSRAM_SLOT_NONE          gagal (file hilang / WAV rusak /
//                                   PSRAM penuh / tidak ada slot
//                                   kosong) — tidak ada yang di-commit
uint8_t psramLoadSingleSample(
    const char *absPath
);

// Print status ke serial
void psramPrintStatus();

#endif // PSRAM_LOADER_H
