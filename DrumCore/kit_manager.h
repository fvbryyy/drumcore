#ifndef KIT_MANAGER_H
#define KIT_MANAGER_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// Kit Manager (TASK 3 — control plane)
// ======================================================
//
// Manajemen kit SAFE dan inkremental di layer kontrol:
//   - CREATE EMPTY : kit baru berisi baseline trigger stabil di
//                    slot 1..4. Folder mengikuti NAMA
//                    ("/kits/<sanitized name>/"), bukan index.
//   - CREATE COPY  : snapshot kit AKTIF (config efektif RAM +
//                    sampel) ke slot tujuan, lalu bersihkan
//                    override NVS tujuan.
//   - RENAME       : ubah nama DI kit.json DAN pindah FOLDER ke
//                    "/kits/<nama>" (G2). Bila slot adalah kit
//                    aktif, prefix sample loader diarahkan ulang.
//                    Gagal = tidak berubah (rollback nama + folder).
//   - DELETE       : hapus SELURUH pohon kit (samples/*, samples/,
//                    kit.json, folder, override NVS — G3, tanpa
//                    orphan). Slot 0 (factory) dilindungi. Bila yang
//                    dihapus kit AKTIF, RAM dipindah dulu ke kit00
//                    lewat jalur LOAD yang aman (performKitSwitch).
//   - SAVE / LOAD  : reuse API existing (performKitSwitch).
//
// PERSISTENSI & IDENTITAS:
//   - JSON SD = factory/identity layer (satu-satunya penentu
//     keberadaan & nama kit); resolver scan-based (kit_loader)
//     mencocokkan "kitId"/"id" untuk menemukan folder.
//   - NVS = override layer user (SAVE).
//   - CREATE menyapu override NVS lama slot tujuan (best-effort)
//     agar kit baru berdiri di atas JSON murni.
//
// TIDAK menyentuh realtime engine, GPIO, atau frame UART 8-byte.

// ======================================================
// Result Codes
// ======================================================

enum class KitOpResult : uint8_t
{
    OK             = 0,
    INVALID_INDEX  = 1,   // out of range (>= KIT_COUNT)
    NOT_FOUND      = 2,   // kit slot missing / corrupt
    INVALID_NAME   = 3,   // nama kosong / unsafe / terlalu panjang
    BUSY           = 4,   // SD / JSON / NVS / switch gagal
    PROTECTED      = 5,   // slot factory (0) dilindungi
    COLLISION      = 6,   // folder nama tujuan sudah dipakai kit lain
    NO_SPACE       = 7    // G24.15: semua slot user (1..4) sudah terisi
};

// ======================================================
// Public API
// ======================================================

// TRUE bila slot ter-resolve ke folder kit. Slot di luar range ->
// false.
bool kitManagerSlotOccupied(uint8_t slot);

// Slot kit yang sedang aktif di RAM (dari UART control state).
uint8_t kitManagerActiveSlot();

// Cari slot user (1..KIT_COUNT-1) pertama yang KOSONG untuk CREATE
// bernama. Return KIT_COUNT bila semua terisi. Konsep slot tetap
// INTERNAL — user hanya melihat nama kit.
uint8_t kitManagerFindFreeSlot();

// CREATE dengan NAMA dari user (G4). Validasi nama lalu pilih slot
// kosong otomatis; kit disimpan di "/kits/<nama>/" + samples/,
// kitId = slot terpilih (internal).
//   INVALID_NAME : nama invalid / kosong setelah trim.
//   COLLISION    : folder nama tujuan sudah dipakai kit lain.
//   NO_SPACE     : semua slot user terisi.
// Tidak memanggil LOAD — kit baru TIDAK otomatis jadi kit aktif.
KitOpResult kitManagerCreateEmptyNamed(const char *name);

// CREATE COPY CURRENT bernama (G4): snapshot kit AKTIF (config
// efektif RAM + mapping sample) ke "/kits/<nama>/" + samples/,
// slot kosong otomatis. Status sama dgn CreateEmptyNamed.
KitOpResult kitManagerCreateCopyNamed(const char *name);

// Kontrak KIT RUNTIME — CREATE EMPTY:
//   - KitPadSource = baseline aman murni (defaultPadSource: tabel
//     config.h), BUKAN kit factory (kit00).
//   - mapping sample KOSONG ("sample":"" per pad) -> kit senyap
//     sampai user assign via PAD → SOUND.
//   - samples/ dibuat tapi TIDAK ada file disalin (bukan clone kit00).
//   - TIDAK otomatis jadi kit aktif.

// Buat kit baru berisi baseline factory default di slot (1..4).
// Menyapu override NVS lama slot tsb supaya kit baru berdiri di
// JSON murni. Folder = "/kits/<nama>".
// (isi & mapping lihat komentar KIT RUNTIME di atas).
KitOpResult kitManagerCreateEmpty(uint8_t slot);

// Buat kit baru = snapshot kit AKTIF (config efektif RAM + mapping
// sample by filename + sampel) ke slot (1..4). Kit hasil INDEPENDEN
// (file disalin, mapping nama file tetap). Menyapu override NVS
// lama slot tujuan. Folder = "/kits/<nama>".
KitOpResult kitManagerCreateCopy(uint8_t slot);

// Ubah nama kit di kit.json slot + pindah folder ke "/kits/<nama>"
// (G2). Bila slot aktif, prefix sample loader ikut diarahkan.
// Validasi ketat: non-kosong, maks KIT_NAME_MAX, tanpa separator
// '/', '\\', control char, karakter ilegal FAT (" * : < > ? |),
// nama "."/"..", titik di ujung, spasi tepi. COLLISION bila folder
// tujuan sudah dipakai kit lain.
KitOpResult kitManagerRename(uint8_t slot, const char *name);

// Hapus kit di slot (1..4): seluruh pohon (samples/*, samples/,
// kit.json, folder) + override NVS. Slot 0 -> PROTECTED. Bila slot
// adalah kit aktif, RAM dipindah dulu ke kit00 (jalur LOAD aman)
// lalu NVS active diarahkan ke 0. Tidak menyisakan orphan.
KitOpResult kitManagerDelete(uint8_t slot);

// Load kit slot lewat jalur kit-switch aman (performKitSwitch).
KitOpResult kitManagerLoad(uint8_t slot);

// KIT SAVE (satu-satunya jalan tulis config efektif ke SD):
// serialisasi konfigurasi EFEKTIF RAM (PadConfig + mapping sample
// by filename) kit AKTIF ke kit.json-nya secara atomik. kit.json =
// satu-satunya source of truth. TIDAK menulis NVS per-kit.
KitOpResult kitManagerSaveActive();

// Migrasi sekali waktu: blob NVS per-kit legacy (kit0..kit4)
// di-merge ke kit.json masing-masing (best-effort; hanya slot yang
// kit.json-nya ada), lalu SEMUA blob dihapus. Setelah ini NVS hanya
// menyimpan global (active/volume). Dipanggil di boot sebelum load
// kit. Selalu return TRUE (best-effort).
bool kitManagerMigrateNvsOverrides();

// Validasi + bersihkan nama: trim spasi kiri/kanan; tolak separator
// '/', '\\', control chars (0..0x1F), karakter ilegal FAT
// (" * : < > ? |), nama "."/".." (setelah trim), titik di ujung,
// dan nama kosong / lebih dari KIT_NAME_MAX. Hasil bersih ditulis
// ke clean (NUL-terminated). return true jika valid.
bool kitManagerValidateName(
    const char *name,
    char *clean,
    size_t cleanCap
);

#endif // KIT_MANAGER_H