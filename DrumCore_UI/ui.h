#ifndef UI_H
#define UI_H

#include <Arduino.h>

// State machine UI DrumCore (Wemos D1 Mini). Tiap state menjawab:
//   - "Apa yang sedang saya pilih?"
//   - "Kalau ENTER, apa yang terjadi?"
//   - "Kalau LONG PRESS, saya kembali ke mana?"
// Aturan input global (tanpa kecuali): ROTATE -> pindah pilihan/ubah nilai;
// SHORT CLICK -> ENTER/SELECT/CONFIRM; LONG PRESS -> BACK/CANCEL.
// Di layar home: ROTATE/CLICK -> no-op; LONG PRESS -> no-op (layar keepalive).

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
    KIT_CREATE_NAME, // CREATE-with-name: nama diinput user via name editor (G4)
    KIT_CONFIRM, // YES/NO confirmation (LOAD / CREATE / DELETE)
    KIT_RENAME,  // name editor (encoder char cycler + caret)
    KIT_ACTION,  // kit transaction progress animation (LOAD/SAVE/CREATE/DELETE/RENAME)
    PAD,         // pad browser (PAD 01 .. PAD 12)
    PAD_EDIT,    // per-pad groups (SOUND / MAPPING / TRIGGER / BACK)
    PAD_SOUND,   // SOUND browser: daftar suara kit aktif (G9)
    PAD_MAPPING, // MIDI NOTE / BACK
    PAD_TRIGGER, // THRESHOLD / RELEASE / PEAK TIME / RELEASE STABLE / MASK / BACK
    PAD_VALUE    // editor numerik satu field pad (G9: GET -> edit -> SET -> GET)

    // G24 dihapus (lihat classifyAudit() di host tests): KIT_SLOT (C/dead —
    // tanpa entry point sejak G4; CREATE memakai KIT_CREATE_NAME, ESP32
    // memilih slot), EXT_MENU / EXT_TRIGGER / EXT_HHP (B/placeholder),
    // SYSTEM_SUB (B/placeholder: VOLUME/MIDI/SD CARD/INFO), COMING_SOON
    // (C/dead — hanya dari EXTERNAL/SYSTEM)
};

// Alasan masuk KIT_SELECT: menentukan apa yang dilakukan CLICK pada sebuah kit.
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