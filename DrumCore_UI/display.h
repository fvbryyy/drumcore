#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Custom character registrations used by the LCD layer.
constexpr uint8_t DISPLAY_DOT_GLYPH = 0x06;
constexpr uint8_t DISPLAY_RULE_GLYPH = 0x07;

void displayInit();

void displayClear();

// Tulis teks dari kolom 0; baris dipad ke 16 kolom dan di-cache sehingga baris
// yang tak berubah tidak ditulis ulang ke LCD. Selama transition reveal aktif
// (displayTransitionBegin), baris ditulis progresif dari kiri (wipe kiri-ke-kanan).
void displayPrintLine(
    uint8_t row,
    const char* text);

// Write text starting at an explicit column (0..LCD_COLS-1). No padding.
void displayPrintAt(
    uint8_t row,
    uint8_t col,
    const char* text);

// Center a single line on a row, space-padded to 16 columns.
void displayPrintCentered(
    uint8_t row,
    const char* text);

// Right-align a single line on a row (column 16 - length).
void displayPrintRight(
    uint8_t row,
    const char* text);

// Gambar progress bar horizontal `cells` sel mulai `col`; `percent` dijepit
// 0..100. Sel terisi KIRI-KE-KANAN (0x00 kosong, 0x05 penuh, 0x01..0x04
// sebagian), jadi bar selalu tumbuh seperti gauge yang benar.
void displayDrawProgress(
    uint8_t row,
    uint8_t col,
    uint8_t cells,
    uint8_t percent);

// Gambar satu glyph dot kustom (reserved; tak lagi dipakai animasi boot —
// lihat screens.cpp BOOT_STEP_TEXT).
void displayDrawDot(
    uint8_t row,
    uint8_t col);

// Gambar glyph aksen garis horizontal di sel baris; dipakai membangun delimiter
// simetris "--- DRUM COK ---" di frame splash boot. Posisi register:
// DISPLAY_RULE_GLYPH (custom char 0x07).
void displayDrawRule(
    uint8_t row,
    uint8_t col);

// Mulai reveal kiri-ke-kanan non-blocking untuk displayPrintLine /
// displayPrintCentered berikutnya. Berlangsung TRANSITION_MS (config.h) dan
// auto-expire, jadi tidak pernah memblokir loop.
void displayTransitionBegin();

// True while a transition reveal is in progress (and has not yet expired).
bool displayTransitionActive();

void displayShow(
    const char* line1,
    const char* line2);

#endif