#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

// Custom character registrations used by the LCD layer.
constexpr uint8_t DISPLAY_DOT_GLYPH = 0x06;
constexpr uint8_t DISPLAY_RULE_GLYPH = 0x07;

void displayInit();

void displayClear();

// Write text starting at column 0 of a row. The row is space-padded to the
// full 16 columns and cached, so unchanged rows are not rewritten to the LCD.
// While a transition reveal is active (displayTransitionBegin), the line is
// instead written progressively from the left (a left-to-right wipe).
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

// Draw a horizontal progress bar of `cells` cells starting at `col`.
// `percent` clamps to 0..100. The bar is made of custom graphics characters
// that fill LEFT-TO-RIGHT inside every cell (0x00 = empty, 0x05 = full,
// 0x01..0x04 = 1/5..4/5 of the cell filled), so the bar always grows
// horizontally like a proper gauge.
void displayDrawProgress(
    uint8_t row,
    uint8_t col,
    uint8_t cells,
    uint8_t percent);

// Draw a single custom dot glyph (reserved; no blinking boot animation
// uses it any more — see screens.cpp BOOT_STEP_TEXT).
void displayDrawDot(
    uint8_t row,
    uint8_t col);

// Draw a horizontal-rule accent glyph (solid mid-row dash) at the given
// cell on a row. Used to build the symmetric "--- DRUM COK ---" delimiter
// on the boot splash frames.
// Register position is DISPLAY_RULE_GLYPH (custom char 0x07).
void displayDrawRule(
    uint8_t row,
    uint8_t col);

// Begin a brief, non-blocking left-to-right reveal for subsequent
// displayPrintLine / displayPrintCentered writes. The reveal lasts
// TRANSITION_MS (config.h) and auto-expires, so it never blocks the loop.
void displayTransitionBegin();

// True while a transition reveal is in progress (and has not yet expired).
bool displayTransitionActive();

void displayShow(
    const char* line1,
    const char* line2);

#endif