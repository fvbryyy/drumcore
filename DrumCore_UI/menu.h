#ifndef MENU_H
#define MENU_H

#include <Arduino.h>

// ------------------------------------------------------------
// Menu data model.
//
// menu.cpp owns the pure navigable lists (index + labels). Screen/state
// transitions between menus live in ui.cpp, which reads the current index
// and calls the matching progress functions. This keeps menu.cpp free of any
// UI/state dependency while still giving a single source of truth for the
// on-screen cursor position.
//
// Tree (G24 — hierarchy final, hanya fitur yang benar-benar bekerja):
//   MENU (top level)
//     KIT        -> KIT actions (LOAD / CREATE / RENAME / DELETE / SAVE / BACK)
//       LOAD     -> KIT_SELECT (kit browser, ui.cpp)
//       CREATE   -> KIT_CREATE (EMPTY / COPY CURRENT / BACK)
//                   lalu KIT_CREATE_NAME (ESP32 memilih slot sendiri)
//       RENAME   -> KIT_SELECT (kit browser, then editor)
//       DELETE   -> KIT_SELECT (kit browser, then confirm)
//       SAVE     -> KIT_ACTION directly (menyimpan kit AKTIF)
//     PAD        -> PAD 01..12 browser
//       PAD xx   -> PAD_EDIT (SOUND / MAPPING / TRIGGER / BACK)
//         SOUND   -> daftar WAV kit aktif (scan folder samples;
//                    pilih -> PAD_SOUND_SET assign by filename)
//         MAPPING -> MIDI NOTE (edit numerik -> PAD_SET)
//         TRIGGER -> THRESHOLD / RELEASE / PEAK TIME /
//                    RELEASE STABLE / MASK (edit numerik -> PAD_SET)
//
// G24 MENGHAPUS dari navigasi user: SYSTEM, VOLUME, MIDI, SD CARD,
// INFO, EXTERNAL, TRIGGER EXTERNAL, HI-HAT PEDAL — semuanya tidak
// punya backend/command nyata. Backend ESP32 tidak disentuh; hanya
// eksposinya yang dibuang. Lihat classifyAudit() di host tests.
// ------------------------------------------------------------

void menuInit();

// Reset every cursor to its root (top-level item 0, pad 01, etc.).
void menuReset();

// --- Top-level menu (KIT / PAD) -------------------------------

void menuNavTop(int8_t delta);

uint8_t menuGetTopIndex();

uint8_t menuGetTopCount();

const char* menuGetTopLabel(uint8_t index);

// --- PAD browser (PAD 01 .. PAD 12) -------------------------

void menuNavPad(int8_t delta);

uint8_t menuGetPadIndex();

uint8_t menuGetPadCount();

// --- PAD edit list (per-pad parameter groups) ----------------

void menuNavPadEdit(int8_t delta);

uint8_t menuGetPadEditIndex();

uint8_t menuGetPadEditCount();

const char* menuGetPadEditLabel(uint8_t index);

// Index of the trailing "BACK" entry in the pad edit list.
uint8_t menuGetPadEditBackIndex();

// Reset a freshly-entered list cursor to its first entry.
void menuPadEditReset();

// --- Kit action menu (LOAD / CREATE / RENAME / DELETE / SAVE / BACK) ---

void menuKitSubReset();

void menuKitSubNavigate(int8_t delta);

uint8_t menuKitSubGetIndex();

uint8_t menuKitSubGetCount();

const char* menuKitSubGetLabel(uint8_t index);

// Index of the "BACK" entry in the kit sub-menu.
uint8_t menuKitSubGetBackIndex();

// --- Kit CREATE sub-menu (EMPTY / COPY CURRENT / BACK) -------

void menuKitCreateReset();

void menuKitCreateNavigate(int8_t delta);

uint8_t menuKitCreateGetIndex();

uint8_t menuKitCreateGetCount();

const char* menuKitCreateGetLabel(uint8_t index);

// Index of the "BACK" entry in the kit create sub-menu.
uint8_t menuKitCreateGetBackIndex();

// --- Generic sub-lists (PAD MAPPING / PAD TRIGGER) -------------
//
// Both behave identically: a navigable label list terminated by
// BACK. The list ids keep the data ownership explicit and let ui.cpp
// reuse one handler for every state.
//
// G24: MENU_LIST_EXT_TRIGGER / MENU_LIST_EXT_HHP dihapus — External
// Trigger dan Hi-Hat Pedal belum punya backend dan tidak boleh tampil.

constexpr uint8_t MENU_LIST_PAD_MAPPING = 0;   // MIDI NOTE / BACK
constexpr uint8_t MENU_LIST_PAD_TRIGGER = 1;   // THRESHOLD / RELEASE / PEAK TIME / RELEASE STABLE / MASK / BACK
constexpr uint8_t MENU_LIST_COUNT       = 2;

void menuSubListReset(uint8_t listId);

void menuSubListNavigate(uint8_t listId, int8_t delta);

uint8_t menuSubListGetIndex(uint8_t listId);

uint8_t menuSubListGetCount(uint8_t listId);

const char* menuSubListGetLabel(uint8_t listId, uint8_t index);

uint8_t menuSubListGetBackIndex(uint8_t listId);

#endif
