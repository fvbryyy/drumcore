#include "menu.h"

// ------------------------------------------------------------
// Top-level menu
//
// G24: hanya KIT dan PAD. EXTERNAL dan SYSTEM dibuang karena tidak
// punya backend/command nyata — prinsip "menu tampil = fungsi
// bekerja". Backend ESP32 (CMD_GLOBAL_SAVE, dll) TIDAK ikut dihapus;
// hanya eksposinya di UI yang hilang.
// ------------------------------------------------------------

static const char* topLabels[] =
{
    "KIT",
    "PAD"
};

static constexpr uint8_t TOP_COUNT =
    sizeof(topLabels) / sizeof(topLabels[0]);

// ------------------------------------------------------------
// PAD browser
// ------------------------------------------------------------

static constexpr uint8_t PAD_COUNT = 12;

// ------------------------------------------------------------
// PAD edit list (per-pad parameter groups)
// ------------------------------------------------------------

/*
 * PAD edit list (per-pad parameter groups) — G9: semua entri MENJADI
 * fungsional. SOUND = daftar WAV kit aktif (scan folder samples;
 * pilih = PAD_SOUND_SET assign by filename), MAPPING =
 * MIDI NOTE, TRIGGER = lima field trigger yang terbukti efektif
 * runtime (G8). CHOKE / SENSITIVITY / CURVE / PAD TYPE tidak
 * diekspos (metadata-only, dan dilarang sebagai UI fungsional).
 */
static const char* padEditLabels[] =
{
    "SOUND",
    "MAPPING",
    "TRIGGER",
    "BACK"
};

static constexpr uint8_t PAD_EDIT_COUNT =
    sizeof(padEditLabels) / sizeof(padEditLabels[0]);

static constexpr uint8_t PAD_EDIT_BACK_INDEX =
    PAD_EDIT_COUNT - 1;

// ------------------------------------------------------------
// Kit action menu (actions shown after entering KIT)
//
// G24: urutan mengikuti hierarchy final — LOAD / CREATE / RENAME /
// DELETE / SAVE. Semua lima punya command + respons backend nyata.
// SAVE berada di akhir karena ia memakai kit AKTIF, bukan hasil
// browser, jadi tidak/grouping bersama LOAD/CREATE/RENAME/DELETE.
// ------------------------------------------------------------

static const char* kitSubLabels[] =
{
    "LOAD",
    "CREATE",
    "RENAME",
    "DELETE",
    "SAVE",
    "BACK"
};

static constexpr uint8_t KIT_SUB_COUNT =
    sizeof(kitSubLabels) / sizeof(kitSubLabels[0]);

static constexpr uint8_t KIT_SUB_BACK_INDEX =
    KIT_SUB_COUNT - 1;

// ------------------------------------------------------------
// Kit CREATE sub-menu (CREATE -> EMPTY / COPY CURRENT / BACK)
// ------------------------------------------------------------

static const char* kitCreateLabels[] =
{
    "EMPTY",
    "COPY CURRENT",
    "BACK"
};

static constexpr uint8_t KIT_CREATE_COUNT =
    sizeof(kitCreateLabels) / sizeof(kitCreateLabels[0]);

static constexpr uint8_t KIT_CREATE_BACK_INDEX =
    KIT_CREATE_COUNT - 1;

// ------------------------------------------------------------
// Generic sub-lists (PAD MAPPING / PAD TRIGGER) — navigable label
// list terminated by BACK.
// ------------------------------------------------------------

/*
 * MAPPING: hanya MIDI NOTE yang fungsional runtime (G8). SAMPLE
 * diekspos lewat SOUND (menu PAD_EDIT); FUNCTION tidak ada di backend.
 */
static const char* padMappingLabels[] =
{
    "MIDI NOTE",
    "BACK"
};

/*
 * TRIGGER: hanya field yang terbukti runtime-effective (G8). Release
 * dibatasi < threshold oleh backend (editor menghindarkan kombinasi
 * mustahil). SENSITIVITY / CURVE / PAD TYPE / CHOKE = metadata-only
 * -> TIDAK tampil.
 */
static const char* padTriggerLabels[] =
{
    "THRESHOLD",
    "RELEASE",
    "PEAK TIME",
    "RELEASE STABLE",
    "MASK",
    "BACK"
};

static const char* const* subListLabels[MENU_LIST_COUNT] =
{
    padMappingLabels,
    padTriggerLabels
};

static const uint8_t subListCounts[MENU_LIST_COUNT] =
{
    sizeof(padMappingLabels) / sizeof(padMappingLabels[0]),
    sizeof(padTriggerLabels) / sizeof(padTriggerLabels[0])
};

static uint8_t subListIndex[MENU_LIST_COUNT] = { 0, 0 };

void menuSubListReset(uint8_t listId)
{
    if (listId < MENU_LIST_COUNT)
    {
        subListIndex[listId] = 0;
    }
}

void menuSubListNavigate(uint8_t listId, int8_t delta)
{
    if (listId >= MENU_LIST_COUNT)
    {
        return;
    }

    uint8_t count = subListCounts[listId];

    if (delta > 0)
    {
        subListIndex[listId] =
            (subListIndex[listId] + 1 < count) ? subListIndex[listId] + 1 : 0;
    }
    else if (delta < 0)
    {
        subListIndex[listId] =
            (subListIndex[listId] > 0) ? subListIndex[listId] - 1 : count - 1;
    }
}

uint8_t menuSubListGetIndex(uint8_t listId)
{
    if (listId < MENU_LIST_COUNT)
    {
        return subListIndex[listId];
    }
    return 0;
}

uint8_t menuSubListGetCount(uint8_t listId)
{
    if (listId < MENU_LIST_COUNT)
    {
        return subListCounts[listId];
    }
    return 0;
}

const char* menuSubListGetLabel(uint8_t listId, uint8_t index)
{
    if (listId < MENU_LIST_COUNT && index < subListCounts[listId])
    {
        return subListLabels[listId][index];
    }
    return "";
}

uint8_t menuSubListGetBackIndex(uint8_t listId)
{
    if (listId < MENU_LIST_COUNT)
    {
        return (uint8_t)(subListCounts[listId] - 1);
    }
    return 0;
}

// ------------------------------------------------------------
// Cursor state
// ------------------------------------------------------------

static uint8_t topIndex = 0;
static uint8_t padIndex = 0;
static uint8_t padEditIndex = 0;
static uint8_t kitSubIndex = 0;
static uint8_t kitCreateIndex = 0;

void menuInit()
{
    menuReset();
}

void menuReset()
{
    topIndex = 0;
    padIndex = 0;
    padEditIndex = 0;
    kitSubIndex = 0;
    kitCreateIndex = 0;
}

// ------------------------------------------------------------
// Top-level menu
// ------------------------------------------------------------

void menuNavTop(int8_t delta)
{
    if (delta > 0)
    {
        topIndex = (topIndex + 1 < TOP_COUNT) ? topIndex + 1 : 0;
    }
    else if (delta < 0)
    {
        topIndex = (topIndex > 0) ? topIndex - 1 : TOP_COUNT - 1;
    }
}

uint8_t menuGetTopIndex()
{
    return topIndex;
}

uint8_t menuGetTopCount()
{
    return TOP_COUNT;
}

const char* menuGetTopLabel(uint8_t index)
{
    if (index < TOP_COUNT)
    {
        return topLabels[index];
    }
    return "";
}

// ------------------------------------------------------------
// PAD browser
// ------------------------------------------------------------

void menuNavPad(int8_t delta)
{
    if (delta > 0)
    {
        padIndex = (padIndex + 1 < PAD_COUNT) ? padIndex + 1 : 0;
    }
    else if (delta < 0)
    {
        padIndex = (padIndex > 0) ? padIndex - 1 : PAD_COUNT - 1;
    }
}

uint8_t menuGetPadIndex()
{
    return padIndex;
}

uint8_t menuGetPadCount()
{
    return PAD_COUNT;
}

// ------------------------------------------------------------
// PAD edit list
// ------------------------------------------------------------

void menuNavPadEdit(int8_t delta)
{
    if (delta > 0)
    {
        padEditIndex =
            (padEditIndex + 1 < PAD_EDIT_COUNT) ? padEditIndex + 1 : 0;
    }
    else if (delta < 0)
    {
        padEditIndex =
            (padEditIndex > 0) ? padEditIndex - 1 : PAD_EDIT_COUNT - 1;
    }
}

uint8_t menuGetPadEditIndex()
{
    return padEditIndex;
}

uint8_t menuGetPadEditCount()
{
    return PAD_EDIT_COUNT;
}

const char* menuGetPadEditLabel(uint8_t index)
{
    if (index < PAD_EDIT_COUNT)
    {
        return padEditLabels[index];
    }
    return "";
}

uint8_t menuGetPadEditBackIndex()
{
    return PAD_EDIT_BACK_INDEX;
}

void menuPadEditReset()
{
    padEditIndex = 0;
}

// ------------------------------------------------------------
// Kit action menu
// ------------------------------------------------------------

void menuKitSubReset()
{
    kitSubIndex = 0;
}

void menuKitSubNavigate(int8_t delta)
{
    if (delta > 0)
    {
        kitSubIndex =
            (kitSubIndex + 1 < KIT_SUB_COUNT) ? kitSubIndex + 1 : 0;
    }
    else if (delta < 0)
    {
        kitSubIndex =
            (kitSubIndex > 0) ? kitSubIndex - 1 : KIT_SUB_COUNT - 1;
    }
}

uint8_t menuKitSubGetIndex()
{
    return kitSubIndex;
}

uint8_t menuKitSubGetCount()
{
    return KIT_SUB_COUNT;
}

const char* menuKitSubGetLabel(uint8_t index)
{
    if (index < KIT_SUB_COUNT)
    {
        return kitSubLabels[index];
    }
    return "";
}

uint8_t menuKitSubGetBackIndex()
{
    return KIT_SUB_BACK_INDEX;
}

// ------------------------------------------------------------
// Kit CREATE sub-menu
// ------------------------------------------------------------

void menuKitCreateReset()
{
    kitCreateIndex = 0;
}

void menuKitCreateNavigate(int8_t delta)
{
    if (delta > 0)
    {
        kitCreateIndex =
            (kitCreateIndex + 1 < KIT_CREATE_COUNT) ? kitCreateIndex + 1 : 0;
    }
    else if (delta < 0)
    {
        kitCreateIndex =
            (kitCreateIndex > 0) ? kitCreateIndex - 1 : KIT_CREATE_COUNT - 1;
    }
}

uint8_t menuKitCreateGetIndex()
{
    return kitCreateIndex;
}

uint8_t menuKitCreateGetCount()
{
    return KIT_CREATE_COUNT;
}

const char* menuKitCreateGetLabel(uint8_t index)
{
    if (index < KIT_CREATE_COUNT)
    {
        return kitCreateLabels[index];
    }
    return "";
}

uint8_t menuKitCreateGetBackIndex()
{
    return KIT_CREATE_BACK_INDEX;
}
