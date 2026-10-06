#ifndef UI_H
#define UI_H

#include <Arduino.h>

// ============================================================
// DrumCore UI state machine (Wemos D1 Mini).
//
// Every state answers:
//   - "Apa yang sedang saya pilih?"
//   - "Kalau ENTER, apa yang terjadi?"
//   - "Kalau LONG PRESS, saya kembali ke mana?"
//
// Global input rules (no exceptions):
//   ROTATE       -> move selection / change value
//   SHORT CLICK  -> ENTER / SELECT / CONFIRM
//   LONG PRESS   -> BACK / CANCEL
//
// Home screen value rules:
//   ROTATE/CLICK -> no-op
//   LONG PRESS   -> no-op (health keepalive screen)
// ============================================================

enum class UIState : uint8_t
{
    BOOT,        // "DRUM COK" splash (brand expands, no extra delay)
    INIT,        // "DRUM COK" + boot step line (PING handshake runs here)
    READY,       // DRUM COK / READY (brief hold before home)
    CHECK_CONN,  // DRUM COK / CHECK CONNECTION (silently retries PING)
    MAIN,        // home status screen (--- DRUM COK --- / ACTIVE KIT name)
    MENU,        // top-level menu (KIT / PAD)
    KIT,         // KIT action list (LOAD / CREATE / RENAME / DELETE / SAVE / BACK)
    KIT_SELECT,  // SELECT KIT browser (LOAD / RENAME / DELETE target)
    KIT_CREATE,  // CREATE method (EMPTY / COPY CURRENT / BACK)
    KIT_CREATE_NAME, // CREATE-with-name: nama diinput user via name
    //              editor (G4). Mode editor: RENAME/TEXT.
    KIT_CONFIRM, // YES/NO confirmation (LOAD / CREATE / DELETE)
    KIT_RENAME,  // name editor (encoder char cycler + caret)
    KIT_ACTION,  // kit transaction progress animation (LOAD/SAVE/CREATE/DELETE/RENAME)
    PAD,         // pad browser (PAD 01 .. PAD 12)
    PAD_EDIT,    // per-pad groups (SOUND / MAPPING / TRIGGER / BACK)
    PAD_SOUND,   // SOUND browser: daftar suara kit aktif (G9)
    PAD_MAPPING, // MIDI NOTE / BACK
    PAD_TRIGGER, // THRESHOLD / RELEASE / PEAK TIME / RELEASE STABLE / MASK / BACK
    PAD_VALUE    // editor numerik satu field pad (G9: GET -> edit -> SET -> GET)

    // G24 dihapus (lihat classifyAudit() di host tests):
    //   KIT_SLOT     — C/dead: tidak ada entry point sejak G4 (CREATE
    //                  memakai KIT_CREATE_NAME, ESP32 memilih slot).
    //                  Loop balik satu-satunya menunjuk ke state yang
    //                  tak pernah dimasuki.
    //   EXT_MENU     — B/placeholder: EXTERNAL (TRIGGER / HI-HAT PEDAL)
    //   EXT_TRIGGER  — B/placeholder: tidak ada command trigger eksternal
    //   EXT_HHP      — B/placeholder: tidak ada command hi-hat pedal
    //   SYSTEM_SUB   — B/placeholder: VOLUME / MIDI / SD CARD / INFO
    //   COMING_SOON  — C/dead: hanya bisa dimasuki dari EXTERNAL/SYSTEM
};

// Why we entered KIT_SELECT: decides what CLICK does on a kit.
enum class KitSelectPurpose : uint8_t
{
    LOAD,
    RENAME,
    DELETE
};

// Which CREATE method was chosen before picking a target slot.
enum class KitCreateMethod : uint8_t
{
    EMPTY,        // new kit from factory/default baseline
    COPY_CURRENT  // snapshot of the ACTIVE kit
};

void uiInit();

void uiUpdate();

UIState uiGetCurrentState();

#endif