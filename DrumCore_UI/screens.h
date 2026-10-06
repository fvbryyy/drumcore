#ifndef SCREENS_H
#define SCREENS_H

#include <Arduino.h>

// Renderer gaya produk: semua string muat di grid 16x2; tiap baris digambar
// lewat helper layout display.cpp. G22: branding yang dirender ke LCD adalah
// "DRUM COK" — hanya untuk DISPLAY; project directory, source, binary,
// protocol, dan identifier internal tetap "DrumCore"/"DRUMCORE".

// Jumlah step pada baris status boot (lihat screens.cpp).
constexpr uint8_t BOOT_STEP_COUNT = 5;

// BOOT splash: brand melebar dari tengah ke tepi (3 frame, ~1 s); baris bawah
// sudah menampilkan step boot pertama ("Initializing") sejak frame 0 agar
// layar tidak pernah kosong.
void screenShowBoot(uint8_t frame);

// Baris status boot: baris 0 brand "DRUM COK", baris 1 teks step
// (0..BOOT_STEP_COUNT-1). Dipanggil HANYA saat step berubah — tanpa animasi
// per-loop.
void screenShowBootStep(uint8_t step);

// "DRUM COK / READY"
void screenShowReady();

// "DRUM COK / CHECK CONNECTION"
void screenShowCheckConn();

// MAIN home: baris 1 "--- DRUM COK ---"; baris 2 nama kit (centered),
// "READING" bila nama belum termuat, atau "CONNECTING".
void screenShowMainHome(
    uint8_t kitIndex,
    bool kitKnown,
    const char* kitName);

// Top-level menu list with cursor: "> KIT /   PAD"
void screenShowMenuList(
    uint8_t topIndex);

// KIT action list with cursor: "> LOAD /   CREATE" (no kit number)
void screenShowKitMenu(
    uint8_t subIndex);

// Kit browser: "KIT SELECT / <kit name>" (fallback "READING" bila nama belum ada)
void screenShowKitSelect(
    uint8_t kitIndex,
    const char* kitName);

// Daftar method CREATE: "> EMPTY /   COPY CURRENT" (tanpa nomor kit —
// ESP32 memilih slot kosong sendiri; user tak pernah melihat index slot).
void screenShowKitCreate(
    uint8_t methodIndex);

// Konfirmasi YES/NO: "LOAD <kit name>? / > YES" (atau "> NO"). Name-based
// (G11); nama kosong -> verb-only "LOAD?" di sisi pemanggil.
void screenShowKitConfirm(
    const char* verb,
    const char* name,
    uint8_t choice);

// Konfirmasi YES/NO untuk CREATE bernama (G4): "CREATE? / > YES" — dipakai
// saat ESP32 memilih slot target sendiri, jadi nomor kit tak boleh tampil.
void screenShowKitConfirmName(
    const char* verb,
    uint8_t choice);

// Editor nama kit bersama (G24.15) — SATU renderer untuk RENAME dan CREATE
// agar keduanya terasa identik: baris 1 field nama (jendela 14 char,
// space-padded), baris 2 caret '^' di bawah posisi edit. Jendela bergeser
// ke kanan begitu cursor melewatinya, jadi nama CREATE 28 char tetap bisa
// diedit penuh di LCD 16x2.
void screenShowNameEditor(
    const char* name,
    uint8_t cursor);

// Progress bar horizontal KIT LOAD/SAVE: baris 1 "LOADING", baris 2
// " [<12-cell bar>] ". Layout verb-only — layar hasil di bawah name-based;
// nomor kit tak boleh tampil (G11 §3 / §6).
void screenDrawKitActionTitled(
    const char* action);

// Update only the 12 progress bar cells (flicker-free).
void screenUpdateKitProgress(uint8_t percent);

// Hasil kit (gagal/timeout), name-based (G11): baris 1 <nama kit (atau status
// bila nama tak diketahui)>, baris 2 <status>. Nomor kit tak boleh tampil
// (G11 §3 / §11).
void screenShowKitResultName(
    const char* name,
    const char* status);

// Hasil kit (sukses), name-based (G11): baris 1 <nama kit>, baris 2
// " [<12-cell bar penuh>] ".
void screenShowKitResultNameBar(
    const char* name);

// Hasil dua baris generik tanpa nomor kit (G4): judul centered di atas status
// centered, mis. "KIT CREATED" / "NAME IN USE".
void screenShowActionResult(
    const char* title,
    const char* status);

// Pad browser: "PAD SELECT / <PAD 01>"
void screenShowPadSelect(
    uint8_t padIndex);

// Pad parameter list: "PAD 01 / > MAPPING"
void screenShowPadEdit(
    uint8_t padIndex,
    uint8_t editIndex);

// Sub-list pad (param MAPPING / TRIGGER): "PAD 01 / > <item>"
void screenShowPadSubList(
    uint8_t padIndex,
    uint8_t listId,
    uint8_t itemIndex);

// Browser SOUND (G9): "PAD 01" / "> <suara aktif>". Nama dipotong aman agar
// pas 16 kolom.
void screenShowPadSound(
    uint8_t padIndex,
    const char* name);

// SOUND loading (G9): "PAD 01" / "READING"
void screenShowPadSoundLoading(
    uint8_t padIndex);

// SOUND write-in-progress (G9): "PAD 01" / "SETTING"
void screenShowPadSetting(
    uint8_t padIndex);

// Editor numerik (G9): "THRESHOLD" / "  850" — label field di baris atas,
// nilai (16-bit) di baris bawah.
void screenShowPadValue(
    const char* label,
    uint16_t value);

// Numeric editor — membaca nilai (G9): "THRESHOLD" / "READING"
void screenShowPadValueLoading(
    const char* label);

// G24 dihapus dari layer ini (tak ada lagi pemanggilnya): screenShowCreateSlot
// (C/dead — layar SELECT SLOT, tanpa entry point sejak G4), screenShowExternal,
// screenShowExtSub, screenShowSystemSub (B/placeholder), screenShowComingSoon
// (C/dead). Backend ESP32 tidak ikut dihapus; hanya render UI-nya.

#endif
