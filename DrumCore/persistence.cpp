#include "persistence.h"

#include <Arduino.h>
#include <Preferences.h>

#include "pad.h"

// ======================================================
// NVS Layout Constants
// ======================================================

namespace
{
    // NVS namespace
    constexpr const char *NVS_NS = "drumcore";

    // Global keys
    constexpr const char *KEY_SCHEMA  = "schema";
    constexpr const char *KEY_ACTIVE  = "active";
    constexpr const char *KEY_VOLUME  = "volume";

    // Schema version (untuk future migration)
    constexpr uint8_t SCHEMA_VERSION  = 1;

    // Per-kit blob magic + header
    constexpr uint8_t  KIT_BLOB_MAGIC   = 0xD3;
    constexpr uint8_t  KIT_BLOB_SCHEMA  = 1;
    constexpr uint8_t  KIT_BLOB_HEADER  = 3;   // magic + schema + padCount
    constexpr uint8_t  KIT_PAD_BYTES    = 20;
    constexpr uint8_t  KIT_BLOB_BYTES   = KIT_BLOB_HEADER +
                                          (PAD_COUNT * KIT_PAD_BYTES);

    // Per-pad field offsets (explicit, no padding)
    constexpr uint8_t OFF_MIDI_NOTE         = 0;
    constexpr uint8_t OFF_THRESHOLD         = 1;
    constexpr uint8_t OFF_RELEASE_THRESHOLD = 3;
    constexpr uint8_t OFF_PEAK_TIME         = 5;
    constexpr uint8_t OFF_RELEASE_STABLE    = 7;
    constexpr uint8_t OFF_MASK_BASE_MS      = 11;
    constexpr uint8_t OFF_SENSITIVITY       = 15;
    constexpr uint8_t OFF_CURVE             = 16;
    constexpr uint8_t OFF_PAD_TYPE          = 17;
    constexpr uint8_t OFF_SAMPLE_ID         = 18;
    constexpr uint8_t OFF_CHOKE_GROUP       = 19;

    Preferences prefs;

    // --------------------------------------------------
    // Little-Endian helpers (portable, tanpa padding)
    // --------------------------------------------------

    inline void putU16(uint8_t *b, uint16_t v)
    {
        b[0] = (uint8_t)(v & 0xFF);
        b[1] = (uint8_t)((v >> 8) & 0xFF);
    }

    inline uint16_t getU16(const uint8_t *b)
    {
        return (uint16_t)(b[0] | (b[1] << 8));
    }

    inline void putU32(uint8_t *b, uint32_t v)
    {
        b[0] = (uint8_t)(v & 0xFF);
        b[1] = (uint8_t)((v >> 8) & 0xFF);
        b[2] = (uint8_t)((v >> 16) & 0xFF);
        b[3] = (uint8_t)((v >> 24) & 0xFF);
    }

    inline uint32_t getU32(const uint8_t *b)
    {
        return (uint32_t)b[0] |
               ((uint32_t)b[1] << 8) |
               ((uint32_t)b[2] << 16) |
               ((uint32_t)b[3] << 24);
    }

    // --------------------------------------------------
    // Encode: PadConfig -> blob (per pad)
    // --------------------------------------------------

    void encodePad(const PadConfig &c, uint8_t *blob)
    {
        blob[OFF_MIDI_NOTE]         = c.midiNote;

        putU16(&blob[OFF_THRESHOLD],         c.threshold);
        putU16(&blob[OFF_RELEASE_THRESHOLD], c.releaseThreshold);
        putU16(&blob[OFF_PEAK_TIME],         c.peakTime);

        putU32(&blob[OFF_RELEASE_STABLE],    c.releaseStableTime);
        putU32(&blob[OFF_MASK_BASE_MS],      c.maskBaseMs);

        blob[OFF_SENSITIVITY]  = c.sensitivity;
        blob[OFF_CURVE]        = c.curve;
        blob[OFF_PAD_TYPE]     = c.padType;
        blob[OFF_SAMPLE_ID]    = c.sampleId;
        blob[OFF_CHOKE_GROUP]  = c.chokeGroup;
    }

    // --------------------------------------------------
    // Decode + validasi blob -> PadConfig: nilai invalid =
    // keep default yang sudah ada. Batas validasi memakai
    // PAD_FIELD_MIN/MAX (config.h — sumber kebenaran G8-A),
    // sama dengan yang dipakai UART PAD_SET & JSON kit.
    // (Lihat PAD_FIELD_MIN/MAX untuk rentang tiap field.)
    // --------------------------------------------------

    void decodePad(const uint8_t *blob, PadConfig &c)
    {
        uint16_t t = getU16(&blob[OFF_THRESHOLD]);
        uint16_t rt = getU16(&blob[OFF_RELEASE_THRESHOLD]);
        uint16_t pt = getU16(&blob[OFF_PEAK_TIME]);
        uint32_t rst = getU32(&blob[OFF_RELEASE_STABLE]);
        uint32_t mask = getU32(&blob[OFF_MASK_BASE_MS]);

        // midiNote
        if (blob[OFF_MIDI_NOTE] >= PAD_FIELD_MIN[FIELD_MIDI_NOTE] &&
            blob[OFF_MIDI_NOTE] <= PAD_FIELD_MAX[FIELD_MIDI_NOTE])
        {
            c.midiNote = blob[OFF_MIDI_NOTE];
        }

        // threshold (G8-A: efektif sampai 4095)
        if (t >= PAD_FIELD_MIN[FIELD_THRESHOLD] &&
            t <= PAD_FIELD_MAX[FIELD_THRESHOLD])
        {
            c.threshold = t;
        }

        // releaseThreshold (G8-A: efektif sampai 4095) & < threshold
        if (rt >= PAD_FIELD_MIN[FIELD_RELEASE_THRESH] &&
            rt <= PAD_FIELD_MAX[FIELD_RELEASE_THRESH] &&
            rt < c.threshold)
        {
            c.releaseThreshold = rt;
        }

        // peakTime
        if (pt >= PAD_FIELD_MIN[FIELD_PEAK_TIME] &&
            pt <= PAD_FIELD_MAX[FIELD_PEAK_TIME])
        {
            c.peakTime = pt;
        }

        // releaseStableTime (otoritatif 1000..100000)
        if (rst >= PAD_FIELD_MIN[FIELD_RELEASE_STABLE] &&
            rst <= PAD_FIELD_MAX[FIELD_RELEASE_STABLE])
        {
            c.releaseStableTime = rst;
        }

        // maskBaseMs (G8-A: efektif sampai 65535)
        if (mask >= PAD_FIELD_MIN[FIELD_MASK_BASE_MS] &&
            mask <= PAD_FIELD_MAX[FIELD_MASK_BASE_MS])
        {
            c.maskBaseMs = mask;
        }

        // sensitivity
        if (blob[OFF_SENSITIVITY] >= PAD_FIELD_MIN[FIELD_SENSITIVITY] &&
            blob[OFF_SENSITIVITY] <= PAD_FIELD_MAX[FIELD_SENSITIVITY])
        {
            c.sensitivity = blob[OFF_SENSITIVITY];
        }

        // curve
        if (blob[OFF_CURVE] >= PAD_FIELD_MIN[FIELD_CURVE] &&
            blob[OFF_CURVE] <= PAD_FIELD_MAX[FIELD_CURVE])
        {
            c.curve = blob[OFF_CURVE];
        }

        // padType
        if (blob[OFF_PAD_TYPE] >= PAD_FIELD_MIN[FIELD_PAD_TYPE] &&
            blob[OFF_PAD_TYPE] <= PAD_FIELD_MAX[FIELD_PAD_TYPE])
        {
            c.padType = blob[OFF_PAD_TYPE];
        }

        // sampleId (G8-A: hanya slot kit valid 0..PAD_COUNT-1)
        if (blob[OFF_SAMPLE_ID] >= PAD_FIELD_MIN[FIELD_SAMPLE_ID] &&
            blob[OFF_SAMPLE_ID] <= PAD_FIELD_MAX[FIELD_SAMPLE_ID])
        {
            c.sampleId = blob[OFF_SAMPLE_ID];
        }

        // chokeGroup
        if (blob[OFF_CHOKE_GROUP] >= PAD_FIELD_MIN[FIELD_CHOKE_GROUP] &&
            blob[OFF_CHOKE_GROUP] <= PAD_FIELD_MAX[FIELD_CHOKE_GROUP])
        {
            c.chokeGroup = blob[OFF_CHOKE_GROUP];
        }
    }

    // --------------------------------------------------
    // Key builder: "kitN" for kitIndex 0..KIT_MAX
    // (KIT_MAX=4 -> keys "kit0".."kit4", no collision)
    // --------------------------------------------------

    void kitKey(char *out, uint8_t kitIndex)
    {
        // NVS key max 15 char. Safe for 0..99 with two digits.
        if (kitIndex < 10)
        {
            out[0] = 'k';
            out[1] = 'i';
            out[2] = 't';
            out[3] = (char)('0' + kitIndex);
            out[4] = '\0';
        }
        else
        {
            out[0] = 'k';
            out[1] = 'i';
            out[2] = 't';
            out[3] = (char)('0' + (kitIndex / 10));
            out[4] = (char)('0' + (kitIndex % 10));
            out[5] = '\0';
        }
    }
}

// ======================================================
// Public: Init
// ======================================================

bool persistenceInit()
{
    prefs.begin(NVS_NS, false);

    uint8_t s = prefs.getUChar(KEY_SCHEMA, 0);

    if (s != SCHEMA_VERSION)
    {
        // Schema baru/kosong: tulis schema, biarkan global default
        // (activeKit=0); data lama dianggap invalid & diabaikan
        // saat load.
        prefs.putUChar(KEY_SCHEMA, SCHEMA_VERSION);
        Serial0.println("=== NVS: new/empty schema (v1) ===");
    }
    else
    {
        Serial0.println("=== NVS: schema v1 OK ===");
    }

    return true;
}

// ======================================================
// Public: Load Global
// ======================================================

bool persistenceLoadGlobal(
    uint8_t &activeKit,
    uint8_t &volume
)
{
    bool hasActive = prefs.isKey(KEY_ACTIVE);
    bool hasVolume = prefs.isKey(KEY_VOLUME);

    activeKit = prefs.getUChar(KEY_ACTIVE, 0);
    volume    = prefs.getUChar(KEY_VOLUME, 0);

    Serial0.print("=== NVS global: activeKit=");
    Serial0.print(activeKit);
    Serial0.print(" volume=");
    Serial0.print(volume);
    Serial0.println();

    return hasActive || hasVolume;
}

// ======================================================
// Public: Load Kit (apply overrides ke PadConfig)
// ======================================================

bool persistenceLoadKit(uint8_t kitIndex)
{
    char key[7];
    kitKey(key, kitIndex);

    uint8_t blob[KIT_BLOB_BYTES];
    size_t len = prefs.getBytes(key, blob, sizeof(blob));

    if (len != sizeof(blob))
    {
        Serial0.print("=== NVS kit[");
        Serial0.print(kitIndex);
        Serial0.println("]: no valid override (skip) ===");
        return false;
    }

    if (blob[0] != KIT_BLOB_MAGIC ||
        blob[1] != KIT_BLOB_SCHEMA ||
        blob[2] != PAD_COUNT)
    {
        Serial0.print("=== NVS kit[");
        Serial0.print(kitIndex);
        Serial0.println("]: corrupt, ignore ===");
        return false;
    }

    // Apply ke config layer + runtime copy (sinkron dgn kit_loader:
    // padInit sudah jalan lebih dulu).
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        const uint8_t *p = &blob[KIT_BLOB_HEADER + i * KIT_PAD_BYTES];

        decodePad(p, getPadConfigMutable(i));
        decodePad(p, pads[i].config);
    }

    Serial0.print("=== NVS kit[");
    Serial0.print(kitIndex);
    Serial0.println("]: overrides applied ===");

    return true;
}

// ======================================================
// Public: Save Global
// ======================================================

bool persistenceSaveGlobal(
    uint8_t activeKit,
    uint8_t volume
)
{
    size_t n1 = prefs.putUChar(KEY_ACTIVE, activeKit);
    size_t n2 = prefs.putUChar(KEY_VOLUME, volume);

    Serial0.print("=== NVS save global: activeKit=");
    Serial0.print(activeKit);
    Serial0.print(" volume=");
    Serial0.print(volume);
    Serial0.println();

    return (n1 == sizeof(uint8_t) && n2 == sizeof(uint8_t));
}

// ======================================================
// Public: Save Kit (dari PadConfig saat ini)
// ======================================================

bool persistenceSaveKit(uint8_t kitIndex)
{
    uint8_t blob[KIT_BLOB_BYTES];

    blob[0] = KIT_BLOB_MAGIC;
    blob[1] = KIT_BLOB_SCHEMA;
    blob[2] = PAD_COUNT;

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        uint8_t *p = &blob[KIT_BLOB_HEADER + i * KIT_PAD_BYTES];
        encodePad(getPadConfig(i), p);
    }

    char key[7];
    kitKey(key, kitIndex);

    size_t written = prefs.putBytes(key, blob, sizeof(blob));

    Serial0.print("=== NVS save kit[");
    Serial0.print(kitIndex);
    Serial0.print("]: ");
    Serial0.print(written);
    Serial0.println(" bytes ===");

    return (written == sizeof(blob));
}

// ======================================================
// Public: Clear Kit Override
// ======================================================

bool persistenceClearKit(uint8_t kitIndex)
{
    char key[7];
    kitKey(key, kitIndex);

    bool removed = prefs.remove(key);

    Serial0.print("=== NVS clear kit[");
    Serial0.print(kitIndex);
    Serial0.println(removed ? "]: removed ===" : "]: not present ===");

    return removed;
}

// ======================================================
// Public: Save All
// ======================================================

bool persistenceSaveAll(
    uint8_t activeKit,
    uint8_t volume,
    uint8_t kitCount
)
{
    bool all = persistenceSaveGlobal(activeKit, volume);

    for (uint8_t k = 0; k < kitCount; k++)
    {
        all = persistenceSaveKit(k) && all;
    }

    Serial0.println("=== NVS save all DONE ===");
    return all;
}

// ======================================================
// Public: Has Kit Blob (migrasi)
// ======================================================

bool persistenceHasKitBlob(uint8_t kitIndex)
{
    char key[7];
    kitKey(key, kitIndex);

    uint8_t blob[KIT_BLOB_BYTES];
    size_t len = prefs.getBytes(key, blob, sizeof(blob));

    return (
        len == sizeof(blob) &&
        blob[0] == KIT_BLOB_MAGIC &&
        blob[1] == KIT_BLOB_SCHEMA &&
        blob[2] == PAD_COUNT
    );
}

// ======================================================
// Public: Load Kit Config (tanpa apply — migrasi)
// ======================================================

bool persistenceLoadKitConfig(
    uint8_t kitIndex,
    PadConfig out[PAD_COUNT]
)
{
    if (!out)
    {
        return false;
    }

    char key[7];
    kitKey(key, kitIndex);

    uint8_t blob[KIT_BLOB_BYTES];
    size_t len = prefs.getBytes(key, blob, sizeof(blob));

    if (len != sizeof(blob))
    {
        return false;
    }

    if (blob[0] != KIT_BLOB_MAGIC ||
        blob[1] != KIT_BLOB_SCHEMA ||
        blob[2] != PAD_COUNT)
    {
        return false;
    }

    // decodePad hanya menimpa field valid: out harus sudah berisi
    // nilai baseline (dari kit.json) sebelum dipanggil.
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        const uint8_t *p = &blob[KIT_BLOB_HEADER + i * KIT_PAD_BYTES];
        decodePad(p, out[i]);
    }

    Serial0.print("=== NVS kit[");
    Serial0.print(kitIndex);
    Serial0.println("]: config read for migration ===");

    return true;
}

// ======================================================
// Public: Clear All Kit Overrides (migrasi selesai)
// ======================================================

void persistenceClearAllKitOverrides()
{
    for (uint8_t k = 0; k < KIT_COUNT; k++)
    {
        char key[7];
        kitKey(key, k);

        if (prefs.remove(key))
        {
            Serial0.print("=== NVS cleared override kit[");
            Serial0.print(k);
            Serial0.println("] ===");
        }
    }
}
