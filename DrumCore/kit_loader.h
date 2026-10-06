#ifndef KIT_LOADER_H
#define KIT_LOADER_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// Kit Loader (Phase 6A)
// ======================================================
//
// Memuat kit JSON dari SD ke PadConfig (config layer), lalu
// sample ke PSRAM.
//
// PENTING:
//   - Dipanggil SEKALI saat boot, SEBELUM task real-time
//     (TriggerTask/AudioTask/MidiTask) dimulai.
//   - TIDAK ada parsing JSON, malloc/free, atau baca SD di
//     hit/audio real-time path.
//   - Selalu fallback aman ke default config.h bila kit tidak
//     ditemukan / corrupt / field tidak valid.

// ======================================================
// Public API
// ======================================================

// Load kit JSON ke PadConfig + load sample ke PSRAM.
// Urutan boot: panggil setelah sdInit(), sebelum audioInit() /
// start task.
//
// path: path di SD (misal "/kits/kit00.json").
// return true bila kit ada (sukses penuh maupun fallback
//        per-field); false hanya bila PSRAM gagal total.
bool kitLoaderLoad(
    const char *path,
    const char * const fallbackSamplePaths[],
    uint8_t count
);

// Path sample TERKONFIRMASI valid per pad setelah loader jalan.
// Dipakai setup() untuk psramLoadKit (slot index == pad index,
// sinkron dgn audio engine).
const char *kitLoaderResolvedSamplePath(uint8_t padIndex);

// Resolve direktori kit untuk `slot` MENGIKUTI identitas:
//   - name-based (G2): scan /kits/*/kit.json dgn "kitId"/"id" ==
//     slot. Folder yang namanya PERSIS sama dgn field "name" JSON
//     adalah "canonical" (diprioritaskan); selain itu match pertama.
//   - legacy: /kits/kit%02u/kit.json bila scan tak menemukan apa-apa.
// out = "/kits/<dir>" (TANPA trailing slash), NUL-terminated.
// return true bila slot punya kit dir; false bila slot kosong.
// Side-effect free.
bool kitLoaderResolveKitDir(
    uint8_t slot,
    char *out,
    size_t len
);

bool kitLoaderGetKitInfo(
    uint8_t kitIndex,
    char *nameBuffer,
    size_t nameBufferSize,
    uint32_t *idOut
);

// Bangun path JSON kit di SD dari index kit: resolve via
// kitLoaderResolveKitDir() (name-based, fallback legacy kitNN),
// lalu "<dir>/kit.json". Menulis ke out (ruang >= len).
//
// Catatan: sekaligus menyimpan index kit aktif secara internal
// supaya resolve sample tahu direktori samples/ kit yang dimuat.
void kitLoaderBuildPath(
    uint8_t kitIndex,
    char *out,
    size_t len
);

// Bangun direktori kit dari index: hasil resolve (name-based /
// legacy kitNN) + trailing slash. Side-effect free.
// Contoh: kitIndex 1 untuk "ROCK LIVE" -> "/kits/ROCK LIVE/".
// Dipakai membentuk prefix path sample: <kitDir> + "samples/" +
// "<namaFile.wav>"
void kitLoaderBuildKitDir(
    uint8_t kitIndex,
    char *out,
    size_t len
);

// Arahkan ulang prefix sample aktif (kitSampleDir + resolvedBuf)
// setelah folder kit AKTIF dipindah ke nama baru (RENAME / CREATE
// overwrite). Tidak menyentuh PadConfig/PSRAM — hanya memastikan
// path sample ter-resolve tetap valid untuk copy/rollback.
void kitLoaderRetargetSampleDir(
    const char *newDir
);

// Nama kit aktif (field "name" JSON terakhir yang berhasil dimuat).
// "/none" bila belum ada kit valid. Dipakai full-sync (UART).
// Read-only.
const char *kitLoaderGetName();

// Update nama ter-cache kit aktif. Dipakai kit_manager setelah
// RENAME pada kit yang dimuat supaya KIT_INFO langsung benar
// tanpa reload. No-op bila kitIndex bukan kit aktif / nama kosong.
void kitLoaderSetCachedName(
    uint8_t kitIndex,
    const char *name
);

// Nama kit untuk index tertentu TANPA mengubah kit aktif.
//   - index aktif -> dari cache (tanpa baca SD).
//   - index lain  -> baca kit.json ter-resolve (control-plane:
//                    dipanggil dari UART task, bukan realtime).
// Fallback "UNNAMED" bila JSON tak ada / corrupt / tanpa field
// "name". Buffer DIJAMIN null-terminated. Selalu return true.
bool kitLoaderGetName(
    uint8_t kitIndex,
    char *buffer,
    size_t bufferSize
);

// kitId JSON ("kitId", legacy "id") untuk index tertentu.
// Index aktif memakai cache; index lain dibaca dari SD.
// Fallback = index itu sendiri bila JSON/field tidak ada.
uint32_t kitLoaderGetKitId(
    uint8_t kitIndex
);

// Resolved sampleId per pad untuk kit aktif: dari field "sampleId"
// kit.json (applyPadConfig), atau default = padIndex bila kit lama /
// field tidak ada. Dipakai DrumCore.ino membangun sampleIds[] untuk
// psramLoadKit().
uint8_t kitLoaderResolvedSampleId(uint8_t padIndex);

// Nama file (basename) sample aktif per pad untuk kit yang dimuat,
// dari kitLoaderResolvedSamplePath. Out null-terminated, dipotong
// di len-1. return false bila padIndex di luar range / path kosong.
bool kitLoaderResolvedSampleName(
    uint8_t padIndex,
    char *out,
    size_t len
);

// =============================================================
// KIT RUNTIME (PAD → SOUND, mapping by filename)
// =============================================================
//
// Mapping runtime per pad adalah RAM-only: resolved path tiap pad
// disimpan loader dan TIDAK menyentuh SD sampai KIT SAVE. Dipakai
// control plane (UART) untuk update mapping saat user memilih suara
// dari daftar folder samples kit aktif.

// Scan folder samples kit AKTIF untuk file WAV valid (nama ≤
// SAMPLE_NAME_MAX, ekstensi .wav/WAV, bukan direktori). Hasil
// diurutkan abjad (deterministik untuk UX dan test). Mengisi
// out[0..maxCount-1] (null-terminated). return jumlah file
// (≤ maxCount); folder kosong/tak ada -> 0 (bukan error).
uint8_t kitLoaderScanSampleNames(
    char out[][SAMPLE_NAME_STORAGE],
    uint8_t maxCount
);

// Validasi nama file sample untuk PAD_SOUND_SET (sebaris dgn
// isValidSampleName: non-empty, ≤ SAMPLE_NAME_MAX, tanpa '/',
// '\\', bukan ".."). return true jika valid.
bool kitLoaderValidateSampleName(
    const char *name
);

// Prefix folder samples kit AKTIF ("/kits/<dir>/samples/"),
// null-terminated di out. Dipakai membentuk path absolut saat
// PAD_SOUND_SET (SD.exists).
void kitLoaderGetSampleDir(
    char *out,
    size_t len
);

// Update mapping RAM pad -> resolved absolute sample path (dipakai
// PAD_SOUND_SET sukses sebelum reload PSRAM). Path kosong ==
// unassigned (pad SENYAP). Tidak menyentuh PadConfig, registry
// PSRAM, atau SD. padIndex di luar range -> no-op.
void kitLoaderSetResolvedSamplePath(
    uint8_t padIndex,
    const char *absPath
);

// (dokumentasi) Prefix selain kitSampleDir TIDAK boleh dipakai
// untuk mapping.

#endif // KIT_LOADER_H