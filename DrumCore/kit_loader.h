#ifndef KIT_LOADER_H
#define KIT_LOADER_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// Kit Loader (Phase 6A)
// ======================================================
//
// Memuat kit JSON dari SD card dan mengaplikasikan
// konfigurasi per-pad ke PadConfig (config layer), lalu
// memuat sample ke mekanisme PSRAM yang sudah ada.
//
// PENTING:
//   - Dipanggil SEKALI saat boot (setup), SEBELUM task
//     real-time (TriggerTask/AudioTask/MidiTask) dimulai.
//   - TIDAK ada parsing JSON, malloc/free, atau baca SD
//     pada hit/audio real-time path.
//   - Selalu fallback aman ke default config.h kalau kit
//     tidak ditemukan / corrupt / field tidak valid.
//   - TIDAK mengganti mekanisme PSRAM existing.

// ======================================================
// Public API
// ======================================================

// Load kit JSON ke PadConfig + load sample ke PSRAM.
// Berdasar urutan boot: panggil setelah sdInit(),
// sebElum audioInit() / start task.
//
// path: path di SD (misal "/kits/kit00.json")
// return true jika ada kit (baik sukses penuh maupun
//        fallback per-field); false hanya jika PSRAM
//        benar-benar gagal total (tetap aman).
bool kitLoaderLoad(
    const char *path,
    const char * const fallbackSamplePaths[],
    uint8_t count
);

// Path sample yang TERKONFIRMASI valid untuk tiap pad
// setelah loader jalan. Dipakai setup() untuk psramLoadKit
// (slot index == pad index, sinkron dgn audio engine).
const char *kitLoaderResolvedSamplePath(uint8_t padIndex);

// Resolve direktori kit untuk `slot` MENGIKUTI identitas:
//   - name-based (G2): scan /kits/*/kit.json untuk "kitId"/"id"
//     == slot. Dir yang nama foldernya PERSIS sama dengan field
//     "name" json adalah "canonical" (diprioritaskan); selain itu
//     dipakai match pertama.
//   - legacy: bundle /kits/kit%02u/kit.json (folder index) bila
//     scan tidak menemukan apa-apa (compat 6D).
// out = "/kits/<dir>" (TANPA trailing slash), NUL-terminated.
// return true jika slot punya kit dir; false jika slot kosong.
// Side-effect free (tidak menggeser activeKitIndex/kitSampleDir).
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

// Bangun path JSON kit di SD dari index kit. Resolve dulu via
// kitLoaderResolveKitDir() (name-based, fallback legacy kitNN),
// lalu hasilkan "<dir>/kit.json". Menulis ke out (ruang >= len).
//
// Catatan: kitLoaderBuildPath juga menyimpan index kit aktif
// secara internal supaya resolve sample tau direktori samples/
// milik kit yang sedang dimuat.
void kitLoaderBuildPath(
    uint8_t kitIndex,
    char *out,
    size_t len
);

// Bangun direktori kit dari index item: hasil resolve (name-based
// atau legacy kitNN) + trailing slash. Side-effect free.
// Contoh: kitIndex 1 untuk "ROCK LIVE" -> "/kits/ROCK LIVE/".
// Dipakai untuk membentuk prefix path sample:
//   <kitDir> + "samples/" + "<namaFile.wav>"
void kitLoaderBuildKitDir(
    uint8_t kitIndex,
    char *out,
    size_t len
);

// Arahkan ulang prefix sample aktif (kitSampleDir + resolvedBuf)
// setelah folder kit AKTIF dipindah ke nama baru (RENAME / CREATE
// overwrite). Tidak menyentuh PadConfig / PSRAM — hanya memastikan
// path sample yang sudah di-resolve tetap valid untuk copy/rollback.
void kitLoaderRetargetSampleDir(
    const char *newDir
);

// Nama kit yang sedang aktif (dari field "name" JSON terakhir
// berhasil dimuat). "/none" jika belum ada kit valid. Dipakai
// full-sync (UART). Read-only.
const char *kitLoaderGetName();

// Update nama ter-cache dari kit aktif. Dipakai kit_manager
// setelah RENAME pada kit yang sedang dimuat supaya KIT_INFO
// active langsung benar tanpa reload. No-op bila kitIndex bukan
// kit aktif atau nama kosong.
void kitLoaderSetCachedName(
    uint8_t kitIndex,
    const char *name
);

// Nama kit untuk index tertentu TANPA mengubah kit aktif.
//   - index aktif  -> ambil dari cache (tanpa baca SD).
//   - index lain   -> baca "/kits/kitNN/kit.json" (control-plane:
//                     dipanggil dari UART task, bukan realtime).
// Fallback aman "UNNAMED" bila JSON tak ada / corrupt / tanpa
// field "name". buffer DIJAMIN null-terminated (dipotong di
// bufferSize-1). Selalu return true (fallback tidak pernah gagal).
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

// Resolved sampleId per pad untuk kit yang sedang aktif.
// Nilai dari field "sampleId" di kit.json (applyPadConfig),
// atau default = padIndex bila kit lama / field tidak ada.
// Dipakai DrumCore.ino untuk membangun sampleIds[] array
// yang dikirim ke psramLoadKit().
uint8_t kitLoaderResolvedSampleId(uint8_t padIndex);

// Nama file (basename) sample aktif per pad untuk kit yang
// sedang dimuat. Dihitung dari kitLoaderResolvedSamplePath —
// path kit-local maupun fallback root (mis. "/KICK.wav")
// sama-sama menghasilkan basename (mis. "KICK.wav").
// Keluar (out) null-terminated dan dipotong di len-1.
// return false jika padIndex di luar range / path tidak ada.
bool kitLoaderResolvedSampleName(
    uint8_t padIndex,
    char *out,
    size_t len
);

// =============================================================
// KIT RUNTIME (PAD → SOUND, mapping by filename)
// =============================================================
//
// Layer runtime mapping per pad adalah RAM-only: resolved path
// tiap pad disimpan loader dan TIDAK menyentuh SD sampai KIT SAVE.
// Chunk berikut dipakai control plane (UART) untuk update mapping
// saat user memilih suara dari daftar folder samples kit aktif.

// Scan folder samples kit AKTIF (kitSampleDir) untuk semua file
// WAV yang valid (nama ≤ SAMPLE_NAME_MAX, ekstensi .wav/WAV,
// bukan direktori). Hasil diurutkan abjad (deterministik untuk UX
// dan test). Mengisi out[0..maxCount-1][SAMPLE_NAME_STORAGE]
// (null-terminated). return jumlah file yang ditemukan (≤ maxCount).
// Kit tanpa folder samples / kosong -> 0 (bukan error).
uint8_t kitLoaderScanSampleNames(
    char out[][SAMPLE_NAME_STORAGE],
    uint8_t maxCount
);

// Validasi nama file sample untuk PAD_SOUND_SET (chart sebaris
// dengan isValidSampleName internal: non-empty, ≤ SAMPLE_NAME_MAX,
// tanpa '/', '\\', dan bukan ".."). return true jika valid.
bool kitLoaderValidateSampleName(
    const char *name
);

// Prefix folder samples kit AKTIF ("/kits/<dir>/samples/"),
// null-terminated di out (ukuran len). Dipakai untuk membentuk
// path absolut saat PAD_SOUND_SET (SD.exists).
void kitLoaderGetSampleDir(
    char *out,
    size_t len
);

// Update mapping RAM pad -> resolved absolute sample path
// (dipakai PAD_SOUND_SET sukses sebelum reload PSRAM). Path
// kosong == unassigned (pad SENYAP). Tidak menyentuh PadConfig,
// registry PSRAM, atau SD. padIndex di luar range -> no-op.
void kitLoaderSetResolvedSamplePath(
    uint8_t padIndex,
    const char *absPath
);

// Prefix lain yang TIDAK boleh di-access utk mapping: statis.
// (dokumentasi — tidak ada fungsi baru)

#endif // KIT_LOADER_H
