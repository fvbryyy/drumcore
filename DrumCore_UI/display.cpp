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

// Shadow buffer per baris: baris hanya dikirim ke LCD I2C jika isinya berubah,
// agar animasi/redraw menu tidak membanjiri bus I2C 100 kHz yang lambat.
static char lineBuffer[2][LCD_COLS + 1];

// Karakter grafis kustom (HD44780 5x8, 8 baris per glyph).
// 0x00..0x05 = level satu sel progress bar, terisi KIRI-KE-KANAN (bar
// horizontal, bukan kolom naik): 0x00 kosong, 0x01..0x04 kiri 1/5..4/5,
// 0x05 penuh. 0x06 = glyph dot kecil (reserved, tetap diregistrasi untuk
// kompatibilitas). 0x07 = aksen garis horizontal untuk delimiter simetris
// "--- DRUM COK ---" di layar home/boot.

constexpr uint8_t CUSTOM_CHARS = 8;

// Glyph dot & rule untuk splash boot.
static void displayCreateDotAndRule()
{
    static const uint8_t dot[8] =
    {
        0x00, 0x06, 0x0F, 0x0F, 0x0F, 0x06, 0x00, 0x00
    };

    // Garis 2 piksel di tengah vertikal sel terbaca sebagai dash horizontal
    // yang tajam di LCD 5x8 tanpa mencuri tinggi baris.
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

// Transition reveal (wipe kiri-ke-kanan): displayTransitionBegin()
// mempersenjatai timer; setiap displayPrintLine/displayPrintCentered selama
// jendela aktif menulis barisnya dengan `elapsed/TOTAL * 16` kolom pertama
// terlihat dan sisanya dikosongkan. Auto-expire sehingga tidak memblokir dan
// tak meninggalkan baris setengah tergambar.

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

    // Lewati bila baris sudah tergambar (tulis baris penuh, tanpa transisi).
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

    // Tandai sisa baris sebagai unknown agar full-line write berikutnya tidak
    // dilewati; pakai posisi null-character di sebelah segmen.
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

    // Tulis full-width agar tak ada karakter basi tertinggal di baris.
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

    // Tulis full-width agar tak ada karakter basi tertinggal di baris.
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

    // Tandai akhir bar sebagai unknown di shadow buffer.
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