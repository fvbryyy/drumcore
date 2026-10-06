#include "display.h"
#include "config.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <cstring>

static LiquidCrystal_I2C lcd(
    LCD_ADDRESS,
    LCD_COLS,
    LCD_ROWS
);

// Per-row shadow buffer: a line is only pushed to the I2C LCD when its
// content actually changes. This keeps animation and menu redraws from
// flooding a slow 100 kHz I2C bus.
static char lineBuffer[2][LCD_COLS + 1];

// ------------------------------------------------------------
// Custom graphics characters (HD44780 5x8, 8 rows per glyph).
// 0x00..0x05 encode the five+ levels of a single progress-bar cell and fill
// LEFT-TO-RIGHT (the bar is a horizontal gauge, never a rising column):
//   0x00 = empty   0x01..0x04 = left 1/5..4/5   0x05 = full
// 0x06 = small dot glyph (reserved; kept registered for compatibility).
// 0x07 = horizontal-rule accent (solid mid-row dash) for the symmetric
//        "--- DRUM COK ---" delimiter on the home / boot screens.
// ------------------------------------------------------------

constexpr uint8_t CUSTOM_CHARS = 8;

// Creates the custom dot and rule glyphs used by the boot splash.
static void displayCreateDotAndRule()
{
    static const uint8_t dot[8] =
    {
        0x00, 0x06, 0x0F, 0x0F, 0x0F, 0x06, 0x00, 0x00
    };

    // A line two pixels tall in the vertical middle of the cell reads as a
    // crisp horizontal dash on a 5x8 LCD without stealing the row height.
    static const uint8_t rule[8] =
    {
        0x00, 0x00, 0x00, 0x1F, 0x1F, 0x00, 0x00, 0x00
    };

    lcd.createChar(DISPLAY_DOT_GLYPH, (uint8_t*)dot);
    lcd.createChar(DISPLAY_RULE_GLYPH, (uint8_t*)rule);
}

static void displayCreateCustomChars()
{
    static const uint8_t glyphs[CUSTOM_CHARS][8] =
    {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 0x00 empty
        {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10}, // 0x01 1/5 (left col)
        {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18}, // 0x02 2/5 (2 cols)
        {0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C}, // 0x03 3/5
        {0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E}, // 0x04 4/5
        {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, // 0x05 full
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // 0x06 dot (recreated)
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}  // 0x07 rule (recreated)
    };

    for (uint8_t i = 0; i < CUSTOM_CHARS; i++)
    {
        lcd.createChar(i, (uint8_t*)glyphs[i]);
    }

    displayCreateDotAndRule();
}

// ------------------------------------------------------------
// Transition reveal (left-to-right wipe).
// displayTransitionBegin() arms the timer; every later displayPrintLine /
// displayPrintCentered while the window is active writes its row with the
// first `elapsed/TOTAL * 16` columns shown and the rest blanked. The reveal
// auto-expires so it never blocks and never leaves a half-drawn line behind.
// ------------------------------------------------------------

static uint32_t transitionStartMs = 0;
static bool transitionActive = false;

void displayTransitionBegin()
{
    transitionActive = true;
    transitionStartMs = millis();
}

bool displayTransitionActive()
{
    if (!transitionActive)
    {
        return false;
    }
    if (millis() - transitionStartMs >= TRANSITION_MS)
    {
        transitionActive = false;
        return false;
    }
    return true;
}

// Number of columns revealed so far during the active transition.
static uint8_t transitionCol()
{
    if (!transitionActive)
    {
        return LCD_COLS;
    }

    uint32_t elapsed = millis() - transitionStartMs;
    uint8_t col = (uint8_t)(
        (uint32_t)LCD_COLS * elapsed / TRANSITION_MS);

    if (col >= LCD_COLS)
    {
        col = LCD_COLS;   // full row
        transitionActive = false;
    }
    return col;
}

void displayInit()
{
    Wire.begin();

    lcd.init();
    lcd.backlight();
    lcd.clear();

    memset(lineBuffer, 0, sizeof(lineBuffer));

    displayCreateCustomChars();
}

void displayClear()
{
    lcd.clear();
    memset(lineBuffer, 0, sizeof(lineBuffer));
}

void displayPrintLine(
    uint8_t row,
    const char* text)
{
    if (row >= LCD_ROWS) return;

    char out[LCD_COLS + 1];

    uint8_t len = 0;
    while (text[len] != '\0' && len < LCD_COLS)
    {
        out[len] = text[len];
        len++;
    }

    while (len < LCD_COLS)
    {
        out[len] = ' ';
        len++;
    }
    out[LCD_COLS] = '\0';

    uint8_t wipeCol = transitionCol();

    // Full-line write (no transition): skip when the row is already drawn.
    if (wipeCol >= LCD_COLS &&
        strcmp(lineBuffer[row], out) == 0)
    {
        return;
    }

    lcd.setCursor(0, row);

    for (uint8_t i = 0; i < LCD_COLS; i++)
    {
        char ch = (i < wipeCol) ? out[i] : ' ';
        lcd.print(ch);
        lineBuffer[row][i] = ch;
    }
    lineBuffer[row][LCD_COLS] = '\0';
}

void displayPrintAt(
    uint8_t row,
    uint8_t col,
    const char* text)
{
    if (row >= LCD_ROWS) return;
    if (col >= LCD_COLS) return;

    uint8_t len = strlen(text);
    uint8_t room = LCD_COLS - col;
    if (len > room) len = room;

    lcd.setCursor(col, row);
    for (uint8_t i = 0; i < len; i++)
    {
        lcd.print(text[i]);
        if (col + i < LCD_COLS)
        {
            lineBuffer[row][col + i] = text[i];
        }
    }

    // Mark the rest of the row unknown so a later full-line write is not
    // skipped. Use the null-character position next to the segment.
    if (col + len < LCD_COLS)
    {
        lineBuffer[row][col + len] = '\0';
    }
}

void displayPrintCentered(
    uint8_t row,
    const char* text)
{
    if (row >= LCD_ROWS) return;

    uint8_t len = strlen(text);
    if (len >= LCD_COLS)
    {
        displayPrintLine(row, text);
        return;
    }

    char out[LCD_COLS + 1];
    uint8_t col = (LCD_COLS - len) / 2;

    for (uint8_t i = 0; i < LCD_COLS; i++)
    {
        uint8_t textSlot = (i >= col && (i - col) < len)
            ? i - col
            : 0xFF;

        if (textSlot != 0xFF)
        {
            out[i] = text[textSlot];
        }
        else
        {
            out[i] = ' ';
        }
    }
    out[LCD_COLS] = '\0';

    // Full-width write so no stale characters remain on the row.
    displayPrintLine(row, out);
}

void displayPrintRight(
    uint8_t row,
    const char* text)
{
    if (row >= LCD_ROWS) return;

    uint8_t len = strlen(text);
    if (len >= LCD_COLS)
    {
        displayPrintLine(row, text);
        return;
    }

    char out[LCD_COLS + 1];
    uint8_t col = LCD_COLS - len;

    for (uint8_t i = 0; i < LCD_COLS; i++)
    {
        uint8_t textSlot = (i >= col && (i - col) < len)
            ? i - col
            : 0xFF;

        if (textSlot != 0xFF)
        {
            out[i] = text[textSlot];
        }
        else
        {
            out[i] = ' ';
        }
    }
    out[LCD_COLS] = '\0';

    // Full-width write so no stale characters remain on the row.
    displayPrintLine(row, out);
}

void displayDrawProgress(
    uint8_t row,
    uint8_t col,
    uint8_t cells,
    uint8_t percent)
{
    if (row >= LCD_ROWS) return;
    if (percent > 100) percent = 100;

    // Total bar resolution = cells * 5 segments.
    uint16_t segs = ((uint16_t)percent * cells * 5u) / 100u;
    uint8_t fullCells = segs / 5u;
    uint8_t partial = segs % 5u;   // 0..4 -> char code 0x00..0x04

    lcd.setCursor(col, row);

    for (uint8_t i = 0; i < cells; i++)
    {
        uint8_t ch;
        if (i < fullCells)
        {
            ch = 0x05;                    // full cell
        }
        else if (i == fullCells)
        {
            ch = partial;                 // 0x00..0x04 partial level
        }
        else
        {
            ch = 0x00;                    // empty cell
        }

        if (col + i < LCD_COLS)
        {
            lcd.write(ch);
            lineBuffer[row][col + i] = (char)ch;
        }
    }

    // Mark end of bar as unknown in the shadow buffer.
    if (col + cells < LCD_COLS &&
        lineBuffer[row][col + cells] != '\0')
    {
        lineBuffer[row][col + cells] = '\0';
    }
}

void displayDrawDot(
    uint8_t row,
    uint8_t col)
{
    if (row >= LCD_ROWS) return;
    if (col >= LCD_COLS) return;

    lcd.setCursor(col, row);
    lcd.write(DISPLAY_DOT_GLYPH);
    lineBuffer[row][col] = (char)DISPLAY_DOT_GLYPH;
}

void displayDrawRule(
    uint8_t row,
    uint8_t col)
{
    if (row >= LCD_ROWS) return;
    if (col >= LCD_COLS) return;

    lcd.setCursor(col, row);
    lcd.write(DISPLAY_RULE_GLYPH);
    lineBuffer[row][col] = (char)DISPLAY_RULE_GLYPH;
}

void displayShow(
    const char* line1,
    const char* line2)
{
    displayPrintLine(0, line1);
    displayPrintLine(1, line2);
}