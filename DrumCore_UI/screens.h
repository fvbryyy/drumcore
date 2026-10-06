#ifndef SCREENS_H
#define SCREENS_H

#include <Arduino.h>

// ------------------------------------------------------------
// Product-style renderers. All strings stay inside the 16x2 grid;
// every line is drawn through the display.cpp layout helpers.
//
// G22: branding yang dirender ke LCD adalah "DRUM COK". Nama ini
// hanya untuk DISPLAY — project directory, source, binary, protocol,
// dan identifier internal tetap "DrumCore"/"DRUMCORE".
// ------------------------------------------------------------

// Jumlah step pada baris status boot (lihat screens.cpp).
constexpr uint8_t BOOT_STEP_COUNT = 5;

// BOOT splash, brand melebar dari tengah ke tepi (3 frame, ~1 s):
//   frame 0:     DRUM COK      (plain, centered)
//   frame 1:      -DRUM COK-    (rule accent mulai)
//   frame 2:     --DRUM COK--   (rule accent melebar)
// Baris bawah sudah menampilkan step boot pertama ("Initializing")
// sejak frame 0, jadi layar tidak pernah kosong.
void screenShowBoot(uint8_t frame);

// Baris status boot: baris 0 tetap brand "DRUM COK", baris 1 =
// teks step (step 0..BOOT_STEP_COUNT-1). Dipanggil HANYA saat step
// berubah — tidak ada animasi per-loop.
void screenShowBootStep(uint8_t step);

// "DRUM COK / READY"
void screenShowReady();

// "DRUM COK / CHECK CONNECTION"
void screenShowCheckConn();

// MAIN home (premium):
//   line 1: "--- DRUM COK ---"
//   line 2: centered kit name or "..." fallback, or "CONNECTING"
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

// Kit browser: "KIT SELECT / <kit name>" (name fallback "<KIT 01>")
void screenShowKitSelect(
    uint8_t kitIndex,
    const char* kitName);

// Kit CREATE method list: "> EMPTY /   COPY CURRENT" (no kit number:
// the CREATE destination is chosen by the ESP32, which picks the first
// free writable slot itself — the user never sees a slot index).
void screenShowKitCreate(
    uint8_t methodIndex);

// YES/NO confirmation: "LOAD <kit name>? / > YES" (or "> NO").
// Name-based (G11); an empty name falls back to verb-only "LOAD?"
// on the caller side.
void screenShowKitConfirm(
    const char* verb,
    const char* name,
    uint8_t choice);

// YES/NO confirmation for CREATE-with-name (G4): "CREATE? / > YES".
// Used when the ESP32 picks the target slot internally, so no kit
// number may appear.
void screenShowKitConfirmName(
    const char* verb,
    uint8_t choice);

// Shared kit name editor (G24.15) — SATU renderer untuk RENAME dan
// CREATE, supaya keduanya terasa identik:
//   line 1: name field, 14-char window, space-padded
//   line 2: '^' caret under the current edit position
// The window slides right once the cursor passes it, so a 28-char
// CREATE name stays fully editable on a 16x2 LCD.
void screenShowNameEditor(
    const char* name,
    uint8_t cursor);

// Kit LOAD/SAVE with horizontal progress bar:
//   line 1: "LOADING"
//   line 2: " [<12-cell bar>] "
// Titled (verb-only) layout — the result screens below are
// name-based; no kit number may ever appear (G11 §3 / §6).
void screenDrawKitActionTitled(
    const char* action);

// Update only the 12 progress bar cells (flicker-free).
void screenUpdateKitProgress(uint8_t percent);

// Kit result (failed / timeout), name-based (G11):
//   line 1: <kit name (or status if the name is unknown)>
//   line 2: <status>
// No kit number may ever appear (G11 §3 / §11).
void screenShowKitResultName(
    const char* name,
    const char* status);

// Kit result (success), name-based (G11):
//   line 1: <kit name>
//   line 2: " [<fully-filled 12-cell bar>] "
void screenShowKitResultNameBar(
    const char* name);

// Generic two-line result without any kit number (G4): centered
// title over centered status, e.g. "KIT CREATED" / "NAME IN USE".
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

// Pad sub-list (MAPPING / TRIGGER params): "PAD 01 / > SAMPLE"
void screenShowPadSubList(
    uint8_t padIndex,
    uint8_t listId,
    uint8_t itemIndex);

// SOUND browser (G9): "PAD 01" / "> <suara aktif>". Nama dipotong
// aman agar pas di 16 kolom.
void screenShowPadSound(
    uint8_t padIndex,
    const char* name);

// SOUND loading (G9): "PAD 01" / "SOUNDS..."
void screenShowPadSoundLoading(
    uint8_t padIndex);

// SOUND write-in-progress (G9): "PAD 01" / "SETTING"
void screenShowPadSetting(
    uint8_t padIndex);

// Numeric editor (G9): "THRESHOLD" / "  850" — label field di baris
// atas, nilai (16-bit) di baris bawah.
void screenShowPadValue(
    const char* label,
    uint16_t value);

// Numeric editor — membaca nilai (G9): "THRESHOLD" / "READING"
void screenShowPadValueLoading(
    const char* label);

// ------------------------------------------------------------
// G24 dihapus dari layers ini (tidak ada lagi pemanggilnya):
//   screenShowCreateSlot()  — C/dead: layar SELECT SLOT, tanpa entry
//                              point sejak G4
//   screenShowExternal()    — B/placeholder: menu EXTERNAL
//   screenShowExtSub()      — B/placeholder: param EXTERNAL
//   screenShowSystemSub()   — B/placeholder: menu SYSTEM
//   screenShowComingSoon()  — C/dead: hanya dari EXTERNAL/SYSTEM
// Backend ESP32 tidak ikut dihapus; hanya render UI-nya.
// ------------------------------------------------------------

#endif
