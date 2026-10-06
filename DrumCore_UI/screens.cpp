#include "screens.h"
#include "display.h"
#include "config.h"
#include "menu.h"
#include <cstdio>
#include <cstring>

// ------------------------------------------------------------
// G22 — LCD PRODUCT NAME
//
// "DRUM COK" adalah SATU-SATUNYA sumber teks branding yang dirender
// ke LCD. String ini tidak boleh bocor ke nama project/folder/
// source/binary/protokol: project tetap "DrumCore", firmware tetap
// "DrumCore v0.4.0". Panjang "DRUM COK" sama dengan "DRUMCORE"
// (8 kolom), jadi semua layout 16x2 tidak bergeser.
// ------------------------------------------------------------

static const char BRAND_LCD[] = "DRUM COK";

// Baris brand terpusat dengan aksen rule simetris di kiri/kanan.
//   dashes 0 -> "    DRUM COK    "   (baris brand polos)
//   dashes 1 -> "   -DRUM COK-   "
//   dashes 2 -> "  --DRUM COK--  "
// Aksen memakai DISPLAY_RULE_GLYPH supaya terbaca sebagai garis
// tipis, bukan karakter '-'.
static void buildBrandRow(
    char* out,
    uint8_t dashes)
{
    const uint8_t block = 2 * dashes + (uint8_t)(sizeof(BRAND_LCD) - 1);
    const uint8_t start = (LCD_COLS - block) / 2;

    for (uint8_t i = 0; i < LCD_COLS; i++)
    {
        out[i] = ' ';
    }

    for (uint8_t i = 0; i < dashes; i++)
    {
        out[start + i] = (char)DISPLAY_RULE_GLYPH;
        out[start + block - 1 - i] = (char)DISPLAY_RULE_GLYPH;
    }

    for (uint8_t i = 0; i < sizeof(BRAND_LCD) - 1; i++)
    {
        out[start + dashes + i] = BRAND_LCD[i];
    }

    out[LCD_COLS] = '\0';
}

// Lockup home "--- DRUM COK ---" (aksen '-' ASCII, bukan glyph rule,
// supaya baris home tidak berkedip saat progress bar digambar).
static void buildHomeBrandRow(char* out)
{
    for (uint8_t i = 0; i < LCD_COLS; i++)
    {
        out[i] = ' ';
    }

    out[0] = '-';
    out[1] = '-';
    out[2] = '-';
    out[3] = ' ';
    out[12] = ' ';
    out[13] = '-';
    out[14] = '-';
    out[15] = '-';

    for (uint8_t i = 0; i < sizeof(BRAND_LCD) - 1; i++)
    {
        out[4 + i] = BRAND_LCD[i];
    }

    out[LCD_COLS] = '\0';
}

// G11 §3: seluruh UI KIT bersifat name-based. Helper display nomor
// slot tidak lagi dibutuhkan dan dihapus agar nomor slot tidak dapat
// muncul di LCD dari jalur mana pun.

// ------------------------------------------------------------
// G22 — BOOT STEP TEXT
//
// Baris status boot. Semua string muat di 16 kolom sehingga tidak
// pernah terpotong, dan tidak ada elipsis Berkedip: teks berganti
// hanya ketika step boot berubah.
// ------------------------------------------------------------

static const char* const BOOT_STEP_TEXT[BOOT_STEP_COUNT] =
{
    "Initializing",      // 12
    "Checking SD",       // 11
    "Loading Kit",       // 11
    "Loading Samples",   // 14
    "Starting Engine"    // 15
};

void screenShowBoot(uint8_t frame)
{
    char line1[LCD_COLS + 1];

    // Frame splash: brand melebar dari tengah ke tepi. Baris bawah
    // langsung menampilkan step boot pertama supaya layar tidak
    // pernah kosong sejak frame 0.
    switch (frame)
    {
        case 0:
            buildBrandRow(line1, 0);
            break;

        case 1:
            buildBrandRow(line1, 1);
            break;

        default:
            buildBrandRow(line1, 2);
            break;
    }

    displayPrintLine(0, line1);
    displayPrintLine(1, BOOT_STEP_TEXT[0]);
}

void screenShowBootStep(uint8_t step)
{
    if (step >= BOOT_STEP_COUNT)
    {
        step = (uint8_t)(BOOT_STEP_COUNT - 1);
    }

    char line1[LCD_COLS + 1];
    buildBrandRow(line1, 0);

    displayPrintLine(0, line1);
    displayPrintLine(1, BOOT_STEP_TEXT[step]);
}

void screenShowReady()
{
    char line1[LCD_COLS + 1];
    buildBrandRow(line1, 0);

    displayPrintLine(0, line1);
    displayPrintCentered(1, "READY");
}

void screenShowCheckConn()
{
    char line1[LCD_COLS + 1];
    buildBrandRow(line1, 0);

    displayPrintLine(0, line1);
    displayPrintLine(1, "CHECK CONNECTION");
}

void screenShowMainHome(
    uint8_t kitIndex,
    bool kitKnown,
    const char* kitName)
{
    char row0[LCD_COLS + 1];
    buildHomeBrandRow(row0);
    displayPrintLine(0, row0);

    (void)kitIndex;

    if (kitKnown)
    {
        char line2[LCD_COLS + 1];

        if (kitName && kitName[0] != '\0')
        {
            snprintf(line2, sizeof(line2), "%s", kitName);
        }
        else
        {
            // G24: nama belum/juga tidak termuat. G24 §7 melarang
            // "..." (berkedip dan tidak informatif) dan nomor slot
            // ("KIT %02u") juga dilarang tampil (G11 §3). Jadi
            // laporkan kondisi sebenarnya: masih membaca.
            snprintf(line2, sizeof(line2), "READING");
        }

        displayPrintCentered(1, line2);
    }
    else
    {
        displayPrintCentered(1, "CONNECTING");
    }
}

// ------------------------------------------------------------
// Two-item scrolling list with cursor (reused by multiple menus).
// line1: "> ITEM_A"
// line2:   "  ITEM_B"
// ------------------------------------------------------------
static void showScrollingList(
    uint8_t itemIndex,
    uint8_t itemCount,
    const char* (*getLabel)(uint8_t))
{
    uint8_t next = (itemIndex + 1 < itemCount) ? itemIndex + 1 : 0;

    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];

    snprintf(line1, sizeof(line1), "> %s", getLabel(itemIndex));
    snprintf(line2, sizeof(line2), "  %s", getLabel(next));

    displayPrintLine(0, line1);
    displayPrintLine(1, line2);
}

void screenShowMenuList(uint8_t topIndex)
{
    showScrollingList(
        topIndex,
        menuGetTopCount(),
        menuGetTopLabel);
}

void screenShowKitMenu(uint8_t subIndex)
{
    showScrollingList(
        subIndex,
        menuKitSubGetCount(),
        menuKitSubGetLabel);
}

void screenShowKitSelect(uint8_t kitIndex, const char* kitName)
{
    (void)kitIndex;

    char line2[LCD_COLS + 1];

    if (kitName && kitName[0] != '\0')
    {
        snprintf(line2, sizeof(line2), "%s", kitName);
    }
    else
    {
        // G24: "..." dihapus (G24 §7 — berkedip, tidak informatif).
        // Tampilkan status nyata: nama kit belum diterima dari ESP32.
        // Nomor slot tetap tidak boleh tampil (G11 §3).
        snprintf(line2, sizeof(line2), "READING");
    }

    displayPrintCentered(0, "KIT SELECT");
    displayPrintCentered(1, line2);
}

void screenShowKitCreate(uint8_t methodIndex)
{
    showScrollingList(
        methodIndex,
        menuKitCreateGetCount(),
        menuKitCreateGetLabel);
}

void screenShowKitConfirm(
    const char* verb,
    const char* name,
    uint8_t choice)
{
    char line1[LCD_COLS + 1];

    // G11: name-based; berapa pun panjang nama, layar 16 kolom
    // dipotong agar baris konfirmasi tetap terbaca.
    static const uint8_t NAME_MAX = 12;
    char nameRow[NAME_MAX + 1];

    if (name && name[0] != '\0')
    {
        size_t n = strlen(name);
        if (n > NAME_MAX)
        {
            n = NAME_MAX;
        }
        memcpy(nameRow, name, n);
        nameRow[n] = '\0';
    }
    else
    {
        // G24: konfirmasi tidak boleh menebak nama. Kalau nama kosong,
        // ui.cpp memakai screenShowKitConfirmName() (layar "> YES/NO"
        // tanpa nama) - path ini hanya defensif.
        snprintf(nameRow, sizeof(nameRow), "?");
    }

    snprintf(
        line1,
        sizeof(line1),
        "%s %s?",
        verb,
        nameRow);

    displayPrintCentered(0, line1);
    displayPrintLine(1, (choice == 0) ? "> YES" : "> NO");
}

void screenShowKitConfirmName(
    const char* verb,
    uint8_t choice)
{
    char line1[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "%s?", verb);

    displayPrintCentered(0, line1);
    displayPrintLine(1, (choice == 0) ? "> YES" : "> NO");
}

void screenShowNameEditor(
    const char* name,
    uint8_t cursor)
{
    // Satu-satunya renderer editor nama (G24.15). RENAME dan CREATE
    // memakai fungsi yang sama persis, jadi-delete ini harus tetap
    // menghasilkan layar yang identik dengan RENAME lama.
    //
    // Jendela 14 kolom (sama seperti layar RENAME lama). Begitu
    // cursor melewati jendela, jendela ikut geser ke kanan supaya
    // caret tetap terlihat untuk nama panjang (CREATE maks 28).
    const uint8_t winW = 14;

    uint8_t winStart = 0;

    if (cursor >= winW)
    {
        winStart = (uint8_t)(cursor - (winW - 1));
    }

    char line1[LCD_COLS + 1];
    memset(line1, ' ', winW);
    line1[winW] = '\0';

    if (name && name[0] != '\0')
    {
        size_t n = strlen(name);

        for (uint8_t i = 0; i < winW; i++)
        {
            size_t idx = (size_t)winStart + i;

            if (idx < n)
            {
                line1[i] = name[idx];
            }
        }
    }

    char line2[LCD_COLS + 1];
    memset(line2, ' ', winW);
    line2[winW] = '\0';

    uint8_t caret = (uint8_t)(cursor - winStart);

    if (caret < winW)
    {
        line2[caret] = '^';
    }

    displayPrintLine(0, line1);
    displayPrintLine(1, line2);
}

void screenDrawKitActionTitled(
    const char* action)
{
    displayPrintLine(0, action);

    displayPrintLine(1, "");
    displayPrintAt(1, 1, "[");
    displayDrawProgress(1, 2, 12, 0);
    displayPrintAt(1, 14, "]");
}

void screenUpdateKitProgress(uint8_t percent)
{
    displayDrawProgress(1, 2, 12, percent);
}

void screenShowKitResultName(
    const char* name,
    const char* status)
{
    if (name && name[0] != '\0')
    {
        displayPrintCentered(0, name);
        displayPrintCentered(1, (status && status[0]) ? status : "");
    }
    else
    {
        // Nama tidak diketahui — status dinaikkan agar tidak ada baris
        // kosong yang membingungkan.
        displayPrintCentered(0, (status && status[0]) ? status : "");
        displayPrintLine(1, "");
    }
}

void screenShowKitResultNameBar(const char* name)
{
    if (name && name[0] != '\0')
    {
        displayPrintCentered(0, name);
    }
    else
    {
        displayPrintCentered(0, "COMPLETED");
    }

    displayPrintLine(1, "");

    displayPrintAt(1, 1, "[");
    displayDrawProgress(1, 2, 12, 100);
    displayPrintAt(1, 14, "]");
}

void screenShowActionResult(
    const char* title,
    const char* status)
{
    displayPrintCentered(0, title);
    displayPrintCentered(1, status);
}

void screenShowPadSelect(uint8_t padIndex)
{
    char line2[LCD_COLS + 1];
    snprintf(line2, sizeof(line2), "<PAD %02u>", padIndex + 1);

    displayPrintCentered(0, "PAD SELECT");
    displayPrintCentered(1, line2);
}

void screenShowPadEdit(
    uint8_t padIndex,
    uint8_t editIndex)
{
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];

    snprintf(line1, sizeof(line1), "PAD %02u", padIndex + 1);
    snprintf(line2, sizeof(line2), "> %s", menuGetPadEditLabel(editIndex));

    displayPrintCentered(0, line1);
    displayPrintLine(1, line2);
}

void screenShowPadSubList(
    uint8_t padIndex,
    uint8_t listId,
    uint8_t itemIndex)
{
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];

    snprintf(line1, sizeof(line1), "PAD %02u", padIndex + 1);
    snprintf(
        line2,
        sizeof(line2),
        "> %s",
        menuSubListGetLabel(listId, itemIndex));

    displayPrintCentered(0, line1);
    displayPrintLine(1, line2);
}

void screenShowPadSound(
    uint8_t padIndex,
    const char* name)
{
    char line1[LCD_COLS + 1];
    char line2[LCD_COLS + 1];

    snprintf(line1, sizeof(line1), "PAD %02u", padIndex + 1);

    const size_t nameLen = (name) ? strlen(name) : 0;
    const size_t maxFit = LCD_COLS - 2;   // "> " + nama
    size_t shown = (nameLen < maxFit) ? nameLen : maxFit;

    line2[0] = '>';
    line2[1] = ' ';
    for (size_t i = 0; i < shown; i++)
    {
        char c = name[i];
        if (c < 0x20)
        {
            c = ' ';
        }
        line2[2 + i] = c;
    }
    line2[2 + shown] = '\0';

    displayPrintCentered(0, line1);
    displayPrintLine(1, line2);
}

void screenShowPadSoundLoading(uint8_t padIndex)
{
    char line1[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "PAD %02u", padIndex + 1);

    displayPrintCentered(0, line1);
    // G24: "SOUNDS..." -> status eksplisit (G24 §7, no fake progress).
    displayPrintCentered(1, "READING");
}

void screenShowPadSetting(uint8_t padIndex)
{
    char line1[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), "PAD %02u", padIndex + 1);

    displayPrintCentered(0, line1);
    displayPrintCentered(1, "SETTING");
}

void screenShowPadValue(
    const char* label,
    uint16_t value)
{
    char line2[LCD_COLS + 1];
    snprintf(line2, sizeof(line2), "%u", value);

    displayPrintCentered(0, label ? label : "");
    displayPrintCentered(1, line2);
}

void screenShowPadValueLoading(const char* label)
{
    displayPrintCentered(0, label ? label : "");
    displayPrintCentered(1, "READING");
}
