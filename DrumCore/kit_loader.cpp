#include "kit_loader.h"

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>

#include "pad.h"
#include "sdcard.h"
#include "psram_loader.h"

// ======================================================
// Internal State
// ======================================================

namespace
{
    // Path sample tiap slot yang sudah tervalidasi — dipakai setup()
    // untuk psramLoadKit (audio engine memetakan padIndex -> slot PSRAM).
    const char *resolvedPaths[PAD_COUNT] = {};

    uint32_t kitId = 0xFFFFFFFF;
    bool kitIdValid = false;
    char kitName[KIT_NAME_STORAGE] = "/none";
    bool kitNameValid = false;

    // Index kit yang sedang dimuat — dipakai membentuk prefix path
    // sample: /kits/kitNN/samples/<file>. Digeser oleh
    // kitLoaderBuildPath() tepat sebelum kitLoaderLoad().
    uint8_t activeKitIndex = 0;

    bool isActiveKitIndex(uint8_t kitIndex)
    {
        return kitIndex == activeKitIndex;
    }

    // Buffer path sample absolut TERVALIDASI per pad (hidup sepanjang
    // kit aktif; per-pad supaya kit switching tidak memakai String).
    // G2: folder kit bisa bernama (maks KIT_NAME_MAX), jadi path
    // absolut bisa mencapai dir(40)+"/samples/"(9)+nama(32).
    char resolvedBuf[PAD_COUNT][KIT_SAMPLE_PATH_LEN] = {};

    // Contoh prefix: "/kits/kit00/samples/"
    char kitSampleDir[KIT_SAMPLE_PATH_LEN] = "/kits/kit00/samples/";

    // Path "unassigned": pad tanpa sample (senyap). Mapping by
    // filename — JANGAN fallback ke path root config.h yang tidak
    // ada di SD. Dipakai resolvedPaths[] saat pad tak punya sample
    // valid di folder kit.
    const char emptyPath[] = "";

    void setUnassigned(uint8_t i)
    {
        resolvedBuf[i][0] = '\0';
        resolvedPaths[i]  = emptyPath;
    }

    // ==================================================
    // Validasi Nama File Sample
    // ==================================================
    //
    // Cegah path-traversal / absolute-path injection dari JSON.
    // Tolak '/', '\\', "..", nama kosong/terlalu panjang. Hanya
    // nama file sederhana: "kick.wav".

    bool isValidSampleName(const char *name)
    {
        if (!name || name[0] == '\0')
        {
            return false;
        }

        size_t n = 0;
        for (; name[n] != '\0'; n++)
        {
            char c = name[n];
            if (c == '/' || c == '\\')
            {
                return false;      // tolak separator / traversal
            }
            if (n >= 32)
            {
                return false;      // cap panjang nama
            }
        }

        // Tolak nama ".." persis (berbahaya walau tanpa separator).
        if (n == 2 && name[0] == '.' && name[1] == '.')
        {
            return false;
        }

        return true;
    }

    // ==================================================
    // Validate + Parse Bounded Integer
    // ==================================================

    bool readU8(
        const JsonVariant &v,
        uint8_t minV,
        uint8_t maxV,
        uint8_t &out
    )
    {
        if (v.isNull() || !v.is<int>())
        {
            return false;
        }

        long x = v.as<int>();
        if (x < minV || x > maxV)
        {
            return false;
        }

        out = (uint8_t)x;
        return true;
    }

    bool readU16(
        const JsonVariant &v,
        uint16_t minV,
        uint16_t maxV,
        uint16_t &out
    )
    {
        if (v.isNull() || !v.is<int>())
        {
            return false;
        }

        long x = v.as<int>();
        if (x < minV || x > maxV)
        {
            return false;
        }

        out = (uint16_t)x;
        return true;
    }

    bool readU32(
        const JsonVariant &v,
        uint32_t minV,
        uint32_t maxV,
        uint32_t &out
    )
    {
        if (v.isNull() || !v.is<unsigned long>())
        {
            return false;
        }

        unsigned long x = v.as<unsigned long>();
        if (x < minV || x > maxV)
        {
            return false;
        }

        out = (uint32_t)x;
        return true;
    }

    // ==================================================
    // Apply Satu Pad dari JSON ke PadConfig
    // ==================================================
    //
    // Field opsional: absen/tidak valid -> tetap default.

    void applyPadConfig(
        uint8_t padIndex,
        const JsonObject &obj
    )
    {
        // Tulis ke KEDUA layer: config layer + runtime copy
        // (padInit() sudah jalan duluan, jadi `pad.config` perlu
        // disinkronkan supaya override kit dipakai engine).
        PadConfig &cfg = getPadConfigMutable(padIndex);
        PadConfig &run = pads[padIndex].config;

        uint8_t  u8  = 0;
        uint16_t u16 = 0;
        uint32_t u32 = 0;

        auto applyMaskBaseMs = [&](uint32_t v)
        {
            cfg.maskBaseMs = v;
            run.maskBaseMs = v;
        };

        // midiNote
        if (readU8(
                obj["midiNote"],
                (uint8_t)PAD_FIELD_MIN[FIELD_MIDI_NOTE],
                (uint8_t)PAD_FIELD_MAX[FIELD_MIDI_NOTE],
                u8))
        {
            cfg.midiNote = u8;
            run.midiNote = u8;
        }

        // threshold (batas otoritatif config.h — G8-A)
        if (readU16(
                obj["threshold"],
                (uint16_t)PAD_FIELD_MIN[FIELD_THRESHOLD],
                (uint16_t)PAD_FIELD_MAX[FIELD_THRESHOLD],
                u16))
        {
            cfg.threshold = u16;
            run.threshold = u16;
        }

        // releaseThreshold — harus < threshold
        if (readU16(
                obj["releaseThreshold"],
                (uint16_t)PAD_FIELD_MIN[FIELD_RELEASE_THRESH],
                (uint16_t)PAD_FIELD_MAX[FIELD_RELEASE_THRESH],
                u16))
        {
            if (u16 < cfg.threshold)
            {
                cfg.releaseThreshold = u16;
                run.releaseThreshold = u16;
            }
        }

        // peakTime us
        if (readU16(
                obj["peakTime"],
                (uint16_t)PAD_FIELD_MIN[FIELD_PEAK_TIME],
                (uint16_t)PAD_FIELD_MAX[FIELD_PEAK_TIME],
                u16))
        {
            cfg.peakTime = u16;
            run.peakTime = u16;
        }

        // releaseStableTime us (otoritatif 1000..100000)
        if (readU32(
                obj["releaseStableTime"],
                PAD_FIELD_MIN[FIELD_RELEASE_STABLE],
                PAD_FIELD_MAX[FIELD_RELEASE_STABLE],
                u32))
        {
            cfg.releaseStableTime = u32;
            run.releaseStableTime = u32;
        }

        // maskBaseMs (batas otoritatif config.h — G8-A)
        if (readU32(
                obj["maskBaseMs"],
                PAD_FIELD_MIN[FIELD_MASK_BASE_MS],
                PAD_FIELD_MAX[FIELD_MASK_BASE_MS],
                u32))
        {
            applyMaskBaseMs(u32);
        }

        // sensitivity
        if (readU8(
                obj["sensitivity"],
                (uint8_t)PAD_FIELD_MIN[FIELD_SENSITIVITY],
                (uint8_t)PAD_FIELD_MAX[FIELD_SENSITIVITY],
                u8))
        {
            cfg.sensitivity = u8;
            run.sensitivity = u8;
        }

        // curve
        if (readU8(
                obj["curve"],
                (uint8_t)PAD_FIELD_MIN[FIELD_CURVE],
                (uint8_t)PAD_FIELD_MAX[FIELD_CURVE],
                u8))
        {
            cfg.curve = u8;
            run.curve = u8;
        }

        // padType (0..6 = PadType enum)
        if (readU8(
                obj["padType"],
                (uint8_t)PAD_FIELD_MIN[FIELD_PAD_TYPE],
                (uint8_t)PAD_FIELD_MAX[FIELD_PAD_TYPE],
                u8))
        {
            cfg.padType = u8;
            run.padType = u8;
        }

        // chokeGroup — metadata
        if (readU8(
                obj["chokeGroup"],
                (uint8_t)PAD_FIELD_MIN[FIELD_CHOKE_GROUP],
                (uint8_t)PAD_FIELD_MAX[FIELD_CHOKE_GROUP],
                u8))
        {
            cfg.chokeGroup = u8;
            run.chokeGroup = u8;
        }

        // sampleId (0..PAD_COUNT-1): physical PSRAM slot yang dipakai
        // saat pad ini dipukul. Absen/tidak valid -> default = padIndex
        // (backward-compat).
        if (readU8(
                obj["sampleId"],
                (uint8_t)PAD_FIELD_MIN[FIELD_SAMPLE_ID],
                (uint8_t)PAD_FIELD_MAX[FIELD_SAMPLE_ID],
                u8))
        {
            cfg.sampleId = u8;
            run.sampleId = u8;
        }
    }

    // ==================================================
    // Read Kit Meta (name + kitId) — preview-only
    // ==================================================
    //
    // Baca metadata SATU file kit.json TANPA mengubah state global
    // loader. Dipakai kitLoaderGetName()/kitLoaderGetKitId() untuk
    // permintaan nama/ID kit preview dari Wemos (UART control plane).
    //
    // Fallback: name="UNNAMED", idOut=fallbackId bila file/field
    // tidak ada. return false HANYA bila file tak terbuka / parse
    // JSON gagal (caller pakai fallback-nya sendiri).

    bool readKitMeta(
        const char *path,
        uint8_t fallbackIndex,
        char *nameBuf,
        size_t nameCap,
        uint32_t &idOut
    )
    {
        idOut = fallbackIndex;

        File f = SD.open(path, FILE_READ);

        if (!f)
        {
            return false;
        }

        // Skip UTF-8 BOM (sama seperti kitLoaderLoad).
        uint8_t peek[3];
        size_t peekN = f.read(peek, 3);

        if (peekN < 3 ||
            peek[0] != 0xEF || peek[1] != 0xBB || peek[2] != 0xBF)
        {
            f.seek(0);
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err)
        {
            return false;
        }

        if (doc["kitId"].is<unsigned long>())
        {
            idOut = (uint32_t)doc["kitId"].as<unsigned long>();
        }
        else if (doc["id"].is<unsigned long>())
        {
            idOut = (uint32_t)doc["id"].as<unsigned long>();
        }

        const char *n = doc["name"] | (const char *)nullptr;

        if (n && n[0] != '\0')
        {
            if (nameBuf && nameCap > 0)
            {
                strncpy(nameBuf, n, nameCap - 1);
                nameBuf[nameCap - 1] = '\0';
            }
        }
        else if (nameBuf && nameCap > 0)
        {
            static const char unnamedName[] = "UNNAMED";
            snprintf(
                nameBuf,
                nameCap,
                "%s",
                unnamedName
            );
        }

        return true;
    }

    // ==================================================
    // Resolve Sample File per Pad
    // ==================================================
    //
    // Source of truth MAPPING = nama file ("sample" di kit.json /
    // PAD_SOUND_SET RAM). Nama valid & file ADA di folder samples
    // kit aktif -> dipakai; kosong/tidak valid/file hilang ->
    // UNASSIGNED (pad senyap, "sample":""). TIDAK ada fallback ke
    // path root config.h: root path bukan bagian SD kit dan tidak
    // boleh menjadi mapping.

    void resolveSamples(
        const JsonArray &padsArr,
        const char * const fallbackPaths[],
        uint8_t count
    )
    {
        (void)fallbackPaths;   // deprecated (mapping by filename)

        Serial0.println("--- KIT SAMPLE RESOLVE ---");
        Serial0.print("sample dir: ");
        Serial0.println(kitSampleDir);

        for (uint8_t i = 0; i < count; i++)
        {
            if (padsArr.isNull() || !padsArr[i].is<JsonObject>())
            {
                setUnassigned(i);
                Serial0.print("PAD[");
                Serial0.print(i);
                Serial0.println("] sample: unassigned (no object)");
                continue;
            }

            const JsonObject p = padsArr[i];
            const char *name = p["sample"] | (const char *)nullptr;

            if (!name || !isValidSampleName(name))
            {
                setUnassigned(i);
                Serial0.print("PAD[");
                Serial0.print(i);
                Serial0.print("] sample: ");
                Serial0.print(name ? name : "(none)");
                Serial0.println(" INVALID -> unassigned (senyap)");
                continue;
            }

            // Bangun path absolut di dalam folder kit:
            //   /kits/kitNN/samples/<name>
            snprintf(
                resolvedBuf[i],
                sizeof(resolvedBuf[i]),
                "%s%s",
                kitSampleDir,
                name
            );

            if (SD.exists(resolvedBuf[i]))
            {
                resolvedPaths[i] = resolvedBuf[i];

                Serial0.print("PAD[");
                Serial0.print(i);
                Serial0.print("] sample: ");
                Serial0.println(resolvedBuf[i]);
            }
            else
            {
                setUnassigned(i);
                Serial0.print("PAD[");
                Serial0.print(i);
                Serial0.print("] sample NOT FOUND (");
                Serial0.print(name);
                Serial0.println(") -> unassigned (senyap)");
            }
        }
    }
}

// ======================================================
// Public: Load Kit
// ======================================================

bool kitLoaderLoad(
    const char *path,
    const char * const fallbackSamplePaths[],
    uint8_t count
)
{
    (void)fallbackSamplePaths;   // deprecated: mapping by filename

    Serial0.println("=== KIT LOADER ===");

    kitId = 0xFFFFFFFF;
    kitIdValid = false;
    kitNameValid = false;

    // G11: nama default netral "UNNAMED" — nomor slot TIDAK boleh
    // tampil di LCD (UI name-based).
    snprintf(
        kitName,
        sizeof(kitName),
        "UNNAMED"
    );

    // ----- Reset ke state aman -----
    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        setUnassigned(i);
    }

    // ----- Baca file JSON dari SD -----
    // Format FINAL: /kits/kitNN/kit.json.
    // Legacy fallback: /kits/kitNN.json (format lama, 6A-6C) —
    // tetap memuat PadConfig; sample dicari di folder samples/
    // kit baru. Tidak pernah crash.
    File f = SD.open(path, FILE_READ);

    if (!f)
    {
        char legacy[24];
        snprintf(
            legacy,
            sizeof(legacy),
            "/kits/kit%02u.json",
            (unsigned)activeKitIndex
        );

        f = SD.open(legacy, FILE_READ);

        if (f)
        {
            Serial0.print("Legacy kit format found: ");
            Serial0.println(legacy);
        }
    }

    if (!f)
    {
        Serial0.print("KIT NOT FOUND: ");
        Serial0.println(path);

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            setUnassigned(i);
        }

        Serial0.println("Using default config (pads senyap).");
        return true;  // aman, default
    }

    // Skip UTF-8 BOM (EF BB BF) bila ada — ArduinoJson tidak
    // menanganinya (hindari InvalidInput dari file editor Windows).
    uint8_t peek[3];
    size_t peekN = f.read(peek, 3);
    if (peekN < 3 ||
        peek[0] != 0xEF || peek[1] != 0xBB || peek[2] != 0xBF)
    {
        f.seek(0);   // tanpa BOM: rewind ke awal
    }
    // else: BOM sudah terkonsumsi, posisi setelah BOM.

    // Dokumen JSON (kit JSON kecil).
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err)
    {
        Serial0.print("KIT JSON PARSE ERROR: ");
        Serial0.println(err.c_str());

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            setUnassigned(i);
        }

        Serial0.println("Using default config (pads senyap).");
        return true;
    }

    // ----- Parse metadata kit -----
    // kitId = identitas internal (TASK 1). Field baru diutamakan;
    // legacy "id" tetap didukung (kit lama kompatibel).
    if (doc["kitId"].is<unsigned long>())
    {
        kitId = (uint32_t)doc["kitId"].as<unsigned long>();
        kitIdValid = true;
    }
    else if (doc["id"].is<unsigned long>())
    {
        kitId = (uint32_t)doc["id"].as<unsigned long>();
        kitIdValid = true;
    }

    const char *n = doc["name"] | (const char *)nullptr;
    if (n && n[0] != '\0')
    {
        strncpy(kitName, n, sizeof(kitName) - 1);
        kitName[sizeof(kitName) - 1] = '\0';
    }
    else
    {
        // G11: fallback netral "UNNAMED" (UI Wemos wajib name-based).
        snprintf(
            kitName,
            sizeof(kitName),
            "UNNAMED"
        );
    }

    kitNameValid = true;

    Serial0.print("Kit id=");
    Serial0.print(kitId);
    Serial0.print(" name=");
    Serial0.println(kitName);

    const JsonArray padsArr = doc["pads"];

    // ----- Resolve sample per pad (butuh SD.exists) -----
    resolveSamples(padsArr, fallbackSamplePaths, count);

    // ----- Terapkan PadConfig (opsional per-field) -----
    Serial0.println("--- KIT PADCONFIG APPLY ---");

    for (uint8_t i = 0; i < count; i++)
    {
        if (!padsArr.isNull() && padsArr[i].is<JsonObject>())
        {
            applyPadConfig(i, padsArr[i].as<JsonObject>());

            const PadConfig &c = getPadConfig(i);
            Serial0.print("PAD[");
            Serial0.print(i);
            Serial0.print("] note=");
            Serial0.print(c.midiNote);
            Serial0.print(" thr=");
            Serial0.print(c.threshold);
            Serial0.print(" rthr=");
            Serial0.print(c.releaseThreshold);
            Serial0.print(" pt=");
            Serial0.print(c.peakTime);
            Serial0.print(" rst=");
            Serial0.print(c.releaseStableTime);
            Serial0.print(" mask=");
            Serial0.print(c.maskBaseMs);
            Serial0.print(" padType=");
            Serial0.println(c.padType);
        }
        else
        {
            Serial0.print("PAD[");
            Serial0.print(i);
            Serial0.println("] no override (keep default)");
        }
    }

    Serial0.println("=== KIT LOADED ===");
    return true;
}

// ======================================================
// Public: Resolved Sample Path
// ======================================================

const char *kitLoaderResolvedSamplePath(uint8_t padIndex)
{
    if (padIndex >= PAD_COUNT)
    {
        return nullptr;
    }

    return resolvedPaths[padIndex];
}

// ======================================================
// Public: Resolved Sample Name (basename)
// ======================================================

bool kitLoaderResolvedSampleName(
    uint8_t padIndex,
    char *out,
    size_t len
)
{
    if (padIndex >= PAD_COUNT || !out || len == 0)
    {
        return false;
    }

    const char *path = kitLoaderResolvedSamplePath(padIndex);

    if (!path || path[0] == '\0')
    {
        out[0] = '\0';
        return false;
    }

    const char *base = strrchr(path, '/');

    if (base)
    {
        base = base + 1;
    }
    else
    {
        base = path;
    }

    strncpy(out, base, len - 1);
    out[len - 1] = '\0';

    return out[0] != '\0';
}

// ======================================================
// Public: KIT RUNTIME — Scan Samples Folder
// ======================================================

uint8_t kitLoaderScanSampleNames(
    char out[][SAMPLE_NAME_STORAGE],
    uint8_t maxCount
)
{
    if (!out || maxCount == 0)
    {
        return 0;
    }

    uint8_t found = 0;

    File dir = SD.open(kitSampleDir);

    if (!dir || !dir.isDirectory())
    {
        if (dir)
        {
            dir.close();
        }

        return 0;   // kit tanpa folder samples: daftar kosong (valid)
    }

    while (found < maxCount)
    {
        File e = dir.openNextFile();

        if (!e)
        {
            break;
        }

        if (!e.isDirectory())
        {
            const char *np = e.name();
            const char *base = np ? strrchr(np, '/') : nullptr;
            base = (base && base[1] != '\0') ? base + 1 : np;

            if (base && base[0] != '\0' && base[0] != '.')
            {
                size_t n = strlen(base);

                // Ekstensi .wav / .WAV (case-insensitive), nama ≤ cap.
                const bool wavSuffix =
                    n >= 4 &&
                    base[n - 4] == '.' &&
                    (base[n - 3] == 'w' || base[n - 3] == 'W') &&
                    (base[n - 2] == 'a' || base[n - 2] == 'A') &&
                    (base[n - 1] == 'v' || base[n - 1] == 'V');

                if (wavSuffix && n <= SAMPLE_NAME_MAX &&
                    isValidSampleName(base))
                {
                    strncpy(
                        out[found],
                        base,
                        SAMPLE_NAME_STORAGE - 1
                    );
                    out[found][SAMPLE_NAME_STORAGE - 1] = '\0';
                    found++;
                }
            }
        }

        e.close();
    }

    dir.close();

    // Insertion sort (abjad, deterministik untuk UX & test).
    for (uint8_t i = 1; i < found; i++)
    {
        char key[SAMPLE_NAME_STORAGE];
        strncpy(key, out[i], SAMPLE_NAME_STORAGE);
        key[SAMPLE_NAME_STORAGE - 1] = '\0';

        int8_t j = (int8_t)i;

        while (j > 0 && strcmp(out[j - 1], key) > 0)
        {
            strncpy(
                out[j],
                out[j - 1],
                SAMPLE_NAME_STORAGE
            );
            out[j][SAMPLE_NAME_STORAGE - 1] = '\0';
            j--;
        }

        strncpy(out[j], key, SAMPLE_NAME_STORAGE);
        out[j][SAMPLE_NAME_STORAGE - 1] = '\0';
    }

    return found;
}

// ======================================================
// Public: KIT RUNTIME — Validate Sample Name
// ======================================================

bool kitLoaderValidateSampleName(
    const char *name
)
{
    return isValidSampleName(name);
}

// ======================================================
// Public: KIT RUNTIME — Active Kit Sample Dir
// ======================================================

void kitLoaderGetSampleDir(
    char *out,
    size_t len
)
{
    if (out && len > 0)
    {
        snprintf(out, len, "%s", kitSampleDir);
    }
}

// ======================================================
// Public: KIT RUNTIME — Set Resolved Sample Path (RAM)
// ======================================================

void kitLoaderSetResolvedSamplePath(
    uint8_t padIndex,
    const char *absPath
)
{
    if (padIndex >= PAD_COUNT || !absPath)
    {
        return;
    }

    if (absPath[0] == '\0')
    {
        setUnassigned(padIndex);
        return;
    }

    snprintf(
        resolvedBuf[padIndex],
        sizeof(resolvedBuf[padIndex]),
        "%s",
        absPath
    );
    resolvedPaths[padIndex] = resolvedBuf[padIndex];
}

// ======================================================
// Public: Resolved Sample ID
// ======================================================
//
// sampleId per pad dari kit yang sedang aktif. Ditulis oleh
// applyPadConfig() dari field "sampleId" saat kitLoaderLoad().
// Default (kit lama / field tak ada) = padIndex, sehingga
// mapping identitas (pad x -> slot x) tetap berlaku.

uint8_t kitLoaderResolvedSampleId(uint8_t padIndex)
{
    if (padIndex >= PAD_COUNT)
    {
        return padIndex;
    }

    return getPadConfig(padIndex).sampleId;
}

// ======================================================
// Public: Resolve Kit Directory (name-based, G2)
// ======================================================
//
// Satu-satunya sumber kebenaran identitas kit = field "kitId"
// (legacy "id") di kit.json. Folder bisa di mana saja di /kits/
// (nama folder == nama kit setelah RENAME), jadi query index ->
// direktori via SCAN, bukan pola index. Fallback legacy
// /kits/kitNN/ tetap didukung.

static bool resolveKitDirWithMeta(
    uint8_t slot,
    char *out,
    size_t outLen,
    char *nameOut,
    size_t nameCap,
    uint32_t *idOut
)
{
    if (!out || outLen == 0 || slot >= KIT_COUNT)
    {
        return false;
    }

    out[0] = '\0';

    if (nameOut && nameCap > 0)
    {
        nameOut[0] = '\0';
    }

    if (idOut)
    {
        *idOut = slot;
    }

    char canonical[KIT_DIR_LEN] = {0};
    char any[KIT_DIR_LEN] = {0};
    char canonicalName[KIT_NAME_STORAGE] = {0};
    char anyName[KIT_NAME_STORAGE] = {0};
    uint32_t canonicalId = slot;
    uint32_t anyId = slot;

    File root = SD.open("/kits");

    if (root && root.isDirectory())
    {
        while (File entry = root.openNextFile())
        {
            const char *pathPtr = entry.path();
            const bool isDir = entry.isDirectory();
            char path[KIT_DIR_LEN] = {0};
            char base[KIT_NAME_STORAGE] = {0};

            if (pathPtr)
            {
                snprintf(path, sizeof(path), "%s", pathPtr);

                size_t pathLen = strlen(path);
                while (pathLen > 1 && path[pathLen - 1] == '/')
                {
                    path[--pathLen] = '\0';
                }

                const char *basePtr = strrchr(path, '/');
                basePtr = basePtr ? basePtr + 1 : path;
                snprintf(base, sizeof(base), "%s", basePtr);
            }

            entry.close();

            if (!isDir || path[0] == '\0')
            {
                continue;
            }

            char metaPath[KIT_PATH_LEN];
            snprintf(
                metaPath,
                sizeof(metaPath),
                "%s/kit.json",
                path
            );

            char metaName[KIT_NAME_STORAGE] = {0};
            uint32_t metaId = slot;

            if (readKitMeta(
                    metaPath,
                    slot,
                    metaName,
                    sizeof(metaName),
                    metaId
                ) && metaId == slot)
            {
                if (canonical[0] == '\0' &&
                    base[0] != '\0' &&
                    strcmp(metaName, base) == 0)
                {
                    snprintf(canonical, sizeof(canonical), "%s", path);
                    snprintf(
                        canonicalName,
                        sizeof(canonicalName),
                        "%s",
                        metaName
                    );
                    canonicalId = metaId;
                }

                if (any[0] == '\0')
                {
                    snprintf(any, sizeof(any), "%s", path);
                    snprintf(anyName, sizeof(anyName), "%s", metaName);
                    anyId = metaId;
                }
            }
        }
    }

    if (root)
    {
        root.close();
    }

    const char *selected = canonical[0] != '\0'
        ? canonical
        : (any[0] != '\0' ? any : nullptr);
    const char *selectedName = canonical[0] != '\0'
        ? canonicalName
        : anyName;
    const uint32_t selectedId = canonical[0] != '\0'
        ? canonicalId
        : anyId;

    if (selected)
    {
        snprintf(out, outLen, "%s", selected);

        if (nameOut && nameCap > 0)
        {
            snprintf(nameOut, nameCap, "%s", selectedName);
        }

        if (idOut)
        {
            *idOut = selectedId;
        }

        return true;
    }

    char legacyDir[KIT_DIR_LEN];
    snprintf(
        legacyDir,
        sizeof(legacyDir),
        "/kits/kit%02u",
        (unsigned)slot
    );

    char legacyJson[KIT_PATH_LEN];
    snprintf(
        legacyJson,
        sizeof(legacyJson),
        "%s/kit.json",
        legacyDir
    );

    File legacyFile = SD.open(legacyJson, FILE_READ);

    if (!legacyFile)
    {
        return false;
    }

    legacyFile.close();

    snprintf(out, outLen, "%s", legacyDir);

    if (nameOut && nameCap > 0)
    {
        uint32_t legacyId = slot;
        if (!readKitMeta(
                legacyJson,
                slot,
                nameOut,
                nameCap,
                legacyId
            ))
        {
            snprintf(nameOut, nameCap, "UNNAMED");
        }

        if (idOut)
        {
            *idOut = legacyId;
        }
    }

    return true;
}

bool kitLoaderResolveKitDir(uint8_t slot, char *out, size_t len)
{
    return resolveKitDirWithMeta(
        slot,
        out,
        len,
        nullptr,
        0,
        nullptr
    );
}

// ======================================================
// Public: Build Kit JSON Path
// ======================================================

void kitLoaderBuildPath(
    uint8_t kitIndex,
    char *out,
    size_t len
)
{
    // Format FINAL (6D) + G2: "<resolved-dir>/kit.json" — resolve
    // name-based dulu, fallback legacy "/kits/kit%02u/kit.json".
    char dir[KIT_DIR_LEN];

    if (!kitLoaderResolveKitDir(kitIndex, dir, sizeof(dir)))
    {
        snprintf(dir, sizeof(dir), "/kits/kit%02u", (unsigned)kitIndex);
    }

    snprintf(out, len, "%s/kit.json", dir);

    // Simpan index aktif + prefix direktori sample kit, supaya
    // resolveSamples() mengambil sample dari folder kit yang
    // sedang dimuat (bukan folder lain).
    activeKitIndex = kitIndex;
    kitIdValid = false;
    kitNameValid = false;
    snprintf(
        kitSampleDir,
        sizeof(kitSampleDir),
        "%s/samples/",
        dir
    );
}

// ======================================================
// Public: Build Kit Directory Path
// ======================================================

void kitLoaderBuildKitDir(
    uint8_t kitIndex,
    char *out,
    size_t len
)
{
    char dir[KIT_DIR_LEN];

    if (!kitLoaderResolveKitDir(kitIndex, dir, sizeof(dir)))
    {
        snprintf(dir, sizeof(dir), "/kits/kit%02u", (unsigned)kitIndex);
    }

    snprintf(out, len, "%s/", dir);
}

// ======================================================
// Public: Retarget Active Sample Prefix (G2)
// ======================================================
//
// Setelah folder kit AKTIF dipindah (RENAME / CREATE overwrite),
// arahkan ulang kitSampleDir + resolvedBuf ke folder baru supaya
// path sample tetap valid untuk CREATE COPY / snapshot — tanpa
// menyentuh PadConfig atau PSRAM.

void kitLoaderRetargetSampleDir(
    const char *newDir
)
{
    if (!newDir || newDir[0] == '\0')
    {
        return;
    }

    char newPrefix[KIT_SAMPLE_PATH_LEN];
    snprintf(newPrefix, sizeof(newPrefix), "%s/samples/", newDir);

    const size_t oldLen = strlen(kitSampleDir);

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        // Hanya path yang berasal dari prefix OLD yang dipindah.
        if (strncmp(resolvedBuf[i], kitSampleDir, oldLen) == 0)
        {
            const char *tail = resolvedBuf[i] + oldLen;

            char rebuilt[KIT_SAMPLE_PATH_LEN];
            snprintf(rebuilt, sizeof(rebuilt), "%s%s", newPrefix, tail);
            snprintf(resolvedBuf[i], sizeof(resolvedBuf[i]), "%s", rebuilt);
        }
    }

    snprintf(kitSampleDir, sizeof(kitSampleDir), "%s", newPrefix);
}

// ======================================================
// Public: Kit Name
// ======================================================

const char *kitLoaderGetName()
{
    return kitName;
}

// ======================================================
// Public: Set Cached Name (after RENAME on active kit)
// ======================================================

void kitLoaderSetCachedName(
    uint8_t kitIndex,
    const char *name
)
{
    if (!isActiveKitIndex(kitIndex))
    {
        return;   // bukan kit aktif: cache tidak relevan
    }

    if (!name || name[0] == '\0')
    {
        return;
    }

    strncpy(kitName, name, sizeof(kitName) - 1);
    kitName[sizeof(kitName) - 1] = '\0';
    kitNameValid = true;
}

// ======================================================
// Public: Kit Name (by index, preview-safe)
// ======================================================
//
// Nama kit untuk index tertentu tanpa mengubah state loader.
// Index aktif dari cache (tanpa baca SD); index lain dibaca dari
// lokasi ter-resolve (name-based G2, fallback legacy kitNN).
// Fallback netral "UNNAMED" selalu tersedia (G11: nomor slot
// tidak boleh bocor ke LCD).

bool kitLoaderGetKitInfo(
    uint8_t kitIndex,
    char *nameBuffer,
    size_t nameBufferSize,
    uint32_t *idOut
)
{
    if (!nameBuffer || nameBufferSize == 0)
    {
        return false;
    }

    nameBuffer[0] = '\0';
    uint32_t id = kitIndex;

    if (kitIndex >= KIT_COUNT)
    {
        snprintf(nameBuffer, nameBufferSize, "UNNAMED");

        if (idOut)
        {
            *idOut = id;
        }

        return false;
    }

    // Cache hit: kit aktif sudah punya nama valid di RAM (tanpa SD —
    // aman meski SD tidak ready).
    if (isActiveKitIndex(kitIndex) && kitIdValid && kitNameValid)
    {
        strncpy(nameBuffer, kitName, nameBufferSize - 1);
        nameBuffer[nameBufferSize - 1] = '\0';

        if (idOut)
        {
            *idOut = kitId;
        }

        return true;
    }

    // G19: bila SD tidak ready, JANGAN scan. Return false dgn buffer
    // kosong supaya pemanggil tahu resolusi gagal karena SD, bukan
    // karena kit tidak ada — mencegah "UNNAMED" palsu akibat SD failure.
    if (!sdIsReady())
    {
        // Buffer dikosongkan (bukan "UNNAMED") agar pemanggil bisa
        // membedakan "SD unavailable" dari "kit kosong".
        nameBuffer[0] = '\0';

        if (idOut)
        {
            *idOut = id;
        }

        return false;
    }

    char dir[KIT_DIR_LEN];
    char resolvedName[KIT_NAME_STORAGE] = "UNNAMED";
    const bool found = resolveKitDirWithMeta(
        kitIndex,
        dir,
        sizeof(dir),
        resolvedName,
        sizeof(resolvedName),
        &id
    );

    if (found)
    {
        strncpy(nameBuffer, resolvedName, nameBufferSize - 1);
        nameBuffer[nameBufferSize - 1] = '\0';
    }
    else
    {
        snprintf(nameBuffer, nameBufferSize, "UNNAMED");
        id = kitIndex;
    }

    if (idOut)
    {
        *idOut = id;
    }

    return found;
}

bool kitLoaderGetName(
    uint8_t kitIndex,
    char *buffer,
    size_t bufferSize
)
{
    uint32_t id = kitIndex;
    (void)kitLoaderGetKitInfo(
        kitIndex,
        buffer,
        bufferSize,
        &id
    );
    return buffer != nullptr && bufferSize > 0;
}

uint32_t kitLoaderGetKitId(
    uint8_t kitIndex
)
{
    char ignored[2] = {0};
    uint32_t id = kitIndex;

    (void)kitLoaderGetKitInfo(
        kitIndex,
        ignored,
        sizeof(ignored),
        &id
    );

    return id;
}