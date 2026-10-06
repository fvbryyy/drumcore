#include "kit_manager.h"

#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>

#include "pad.h"
#include "kit_loader.h"
#include "persistence.h"
#include "uart_protocol.h"

// ======================================================
// Internal State & Helpers
// ======================================================

namespace
{
    // --------------------------------------------------
    // Path builders (control plane; tidak menyentuh state
    // loader — path direktori bukan index aktif)
    //
    // G2: folder kit MENGIKUTI nama kit ("/kits/ROCK LIVE/"),
    // jadi path dibangun dari dua sumber:
    //   - dirResolved()  : lokasi yang SEDANG ADA di SD (via
    //     resolver kitLoaderResolveKitDir, fallback legacy
    //     /kits/kitNN). Dipakai untuk READ / EXISTS / RENAME.
    //   - dirFromName()  : folder berdasar nama (untuk tulis
    //     kit baru / overwrite CREATE).
    // --------------------------------------------------

    void dirFromName(const char *name, char *out, size_t len)
    {
        snprintf(out, len, "/kits/%s", name ? name : "");
    }

    void jsonFromName(const char *name, char *out, size_t len)
    {
        snprintf(out, len, "/kits/%s/kit.json", name ? name : "");
    }

    void dirResolved(uint8_t slot, char *out, size_t len)
    {
        if (!kitLoaderResolveKitDir(slot, out, len))
        {
            snprintf(out, len, "/kits/kit%02u", (unsigned)slot);
        }
    }

    void jsonResolved(uint8_t slot, char *out, size_t len)
    {
        char d[KIT_DIR_LEN];
        dirResolved(slot, d, sizeof(d));
        snprintf(out, len, "%s/kit.json", d);
    }

    void samplesDirResolved(uint8_t slot, char *out, size_t len)
    {
        char d[KIT_DIR_LEN];
        dirResolved(slot, d, sizeof(d));
        snprintf(out, len, "%s/samples", d);
    }

    void sampleAbsResolved(uint8_t slot, const char *base, char *out, size_t len)
    {
        char s[KIT_DIR_LEN];
        samplesDirResolved(slot, s, sizeof(s));
        snprintf(out, len, "%s/%s", s, base);
    }

    // --------------------------------------------------
    // Atomic JSON write: tulis ke ".tmp" lalu pindahkan.
    // Kalau file tujuan dihapus sebelum rename (target
    // FAT tidak boleh ada), operasi yang gagal di tengah
    // SELALU dibersihkan (.tmp dihapus) sehingga file lama
    // tetap utuh sampai sukses penuh.
    // --------------------------------------------------

    bool writeJsonAtomic(const char *path, JsonDocument &doc)
    {
        char tmp[KIT_PATH_LEN + 8];
        char bak[KIT_PATH_LEN + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        snprintf(bak, sizeof(bak), "%s.bak", path);

        if (SD.exists(tmp))
        {
            SD.remove(tmp);
        }

        File f = SD.open(tmp, FILE_WRITE);

        if (!f)
        {
            return false;
        }

        size_t n = serializeJson(doc, f);
        f.close();

        if (n == 0)
        {
            SD.remove(tmp);
            return false;
        }

        // Simpan file lama ke .bak dulu. Dengan begitu kalau rename
        // ke path final gagal, file lama bisa dipulihkan — "gagal
        // operasi = kit tidak berubah" selalu terjaga (atomic best-
        // effort di FAT yang tidak mengizinkan rename-overwrite).
        if (SD.exists(bak))
        {
            SD.remove(bak);
        }

        if (SD.exists(path))
        {
            if (!SD.rename(path, bak))
            {
                SD.remove(tmp);
                return false;
            }
        }

        if (!SD.rename(tmp, path))
        {
            // Rollback: pulihkan file lama, buang temporer.
            if (SD.exists(bak))
            {
                SD.rename(bak, path);
            }
            SD.remove(tmp);
            return false;
        }

        SD.remove(bak);
        return true;
    }

    // --------------------------------------------------
    // Per-pad snapshot (config + nama file sample)
    // --------------------------------------------------

    struct KitPadSource
    {
        char sample[40];
        PadConfig cfg;
    };

    // Baseline trigger stable (default padInit): threshold=300,
    // releaseThreshold=250, peakTime=1200, releaseStableTime=3000,
    // sensitivity=100, curve=0, padType=GENERIC, chokeGroup=0.
    // Sample & note & mask dari tabel config.h.
    void defaultPadSource(uint8_t padIndex, KitPadSource &s)
    {
        s.cfg.pin                = PAD_PINS[padIndex];
        s.cfg.midiNote           = PAD_MIDI_NOTES[padIndex];
        s.cfg.threshold          = 300;
        s.cfg.releaseThreshold   = 250;
        s.cfg.peakTime           = 1200;
        s.cfg.releaseStableTime  = 3000;
        s.cfg.maskBaseMs         = PAD_MASK_MS[padIndex];
        s.cfg.sensitivity        = 100;
        s.cfg.curve              = 0;
        s.cfg.padType            = PAD_TYPE_GENERIC;
        s.cfg.sampleId           = padIndex;
        s.cfg.chokeGroup         = 0;

        const char *p = PAD_SAMPLE_PATHS[padIndex];
        const char *base = strrchr(p, '/');

        snprintf(s.sample, sizeof(s.sample), "%s", base ? base + 1 : p);
    }

    // Apply satu pad dari JSON ke source (mirror kit_loader,
    // ditulis ke buffer lokal supaya tidak ada side effect).
    bool resultU8(const JsonVariant &v, uint8_t minV, uint8_t maxV, uint8_t &out)
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

    bool resultU16(const JsonVariant &v, uint16_t minV, uint16_t maxV, uint16_t &out)
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

    bool resultU32(const JsonVariant &v, uint32_t minV, uint32_t maxV, uint32_t &out)
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

    void applyPadSource(const JsonObject &obj, KitPadSource &s)
    {
        uint8_t  u8  = 0;
        uint16_t u16 = 0;
        uint32_t u32 = 0;

        // Batas memakai PAD_FIELD_MIN/MAX (config.h — sumber
        // kebenaran G8-A), sama dgn UART & kit_loader.
        if (resultU8(
                obj["midiNote"],
                (uint8_t)PAD_FIELD_MIN[FIELD_MIDI_NOTE],
                (uint8_t)PAD_FIELD_MAX[FIELD_MIDI_NOTE],
                u8))
        {
            s.cfg.midiNote = u8;
        }

        if (resultU16(
                obj["threshold"],
                (uint16_t)PAD_FIELD_MIN[FIELD_THRESHOLD],
                (uint16_t)PAD_FIELD_MAX[FIELD_THRESHOLD],
                u16))
        {
            s.cfg.threshold = u16;
        }

        if (resultU16(
                obj["releaseThreshold"],
                (uint16_t)PAD_FIELD_MIN[FIELD_RELEASE_THRESH],
                (uint16_t)PAD_FIELD_MAX[FIELD_RELEASE_THRESH],
                u16))
        {
            if (u16 < s.cfg.threshold)
            {
                s.cfg.releaseThreshold = u16;
            }
        }

        if (resultU16(
                obj["peakTime"],
                (uint16_t)PAD_FIELD_MIN[FIELD_PEAK_TIME],
                (uint16_t)PAD_FIELD_MAX[FIELD_PEAK_TIME],
                u16))
        {
            s.cfg.peakTime = u16;
        }

        if (resultU32(
                obj["releaseStableTime"],
                PAD_FIELD_MIN[FIELD_RELEASE_STABLE],
                PAD_FIELD_MAX[FIELD_RELEASE_STABLE],
                u32))
        {
            s.cfg.releaseStableTime = u32;
        }

        if (resultU16(
                obj["maskBaseMs"],
                (uint16_t)PAD_FIELD_MIN[FIELD_MASK_BASE_MS],
                (uint16_t)PAD_FIELD_MAX[FIELD_MASK_BASE_MS],
                u16))
        {
            s.cfg.maskBaseMs = u16;
        }

        if (resultU8(
                obj["sensitivity"],
                (uint8_t)PAD_FIELD_MIN[FIELD_SENSITIVITY],
                (uint8_t)PAD_FIELD_MAX[FIELD_SENSITIVITY],
                u8))
        {
            s.cfg.sensitivity = u8;
        }

        if (resultU8(
                obj["curve"],
                (uint8_t)PAD_FIELD_MIN[FIELD_CURVE],
                (uint8_t)PAD_FIELD_MAX[FIELD_CURVE],
                u8))
        {
            s.cfg.curve = u8;
        }

        if (resultU8(
                obj["padType"],
                (uint8_t)PAD_FIELD_MIN[FIELD_PAD_TYPE],
                (uint8_t)PAD_FIELD_MAX[FIELD_PAD_TYPE],
                u8))
        {
            s.cfg.padType = u8;
        }

        if (resultU8(
                obj["chokeGroup"],
                (uint8_t)PAD_FIELD_MIN[FIELD_CHOKE_GROUP],
                (uint8_t)PAD_FIELD_MAX[FIELD_CHOKE_GROUP],
                u8))
        {
            s.cfg.chokeGroup = u8;
        }

        if (resultU8(
                obj["sampleId"],
                (uint8_t)PAD_FIELD_MIN[FIELD_SAMPLE_ID],
                (uint8_t)PAD_FIELD_MAX[FIELD_SAMPLE_ID],
                u8))
        {
            s.cfg.sampleId = u8;
        }

        // sample: nama file sederhana (path-traversal aman).
        const char *sam = obj["sample"] | (const char *)nullptr;

        if (sam && sam[0] != '\0')
        {
            size_t n = 0;
            bool ok = true;

            for (; sam[n] != '\0' && n < 32; n++)
            {
                if (sam[n] == '/' || sam[n] == '\\')
                {
                    ok = false;
                    break;
                }
            }

            if (ok && n > 0 && !(n == 2 && sam[0] == '.' && sam[1] == '.'))
            {
                strncpy(s.sample, sam, sizeof(s.sample) - 1);
                s.sample[sizeof(s.sample) - 1] = '\0';
            }
        }
    }

    // --------------------------------------------------
    // Load source per-pad dari kit.json slot (untuk CREATE
    // EMPTY: sumber factory kit00). Isi default dulu, lalu
    // override dari JSON. return false jika file/parse gagal.
    // --------------------------------------------------

    bool loadKitSource(uint8_t slot, KitPadSource out[PAD_COUNT])
    {
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            defaultPadSource(i, out[i]);
        }

        char path[KIT_PATH_LEN];
        jsonResolved(slot, path, sizeof(path));

        File f = SD.open(path, FILE_READ);

        if (!f)
        {
            return false;
        }

        // Skip UTF-8 BOM (sama seperti kit_loader).
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

        const JsonArray padsArr = doc["pads"];

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            if (!padsArr.isNull() && padsArr[i].is<JsonObject>())
            {
                applyPadSource(padsArr[i].as<JsonObject>(), out[i]);
            }
        }

        return true;
    }

    // --------------------------------------------------
    // Write kit.json dari sumber per-pad
    // --------------------------------------------------

    bool writeJsonFromSources(
        uint8_t slot,
        const char *name,
        const KitPadSource *src,
        const char *jsonPath
    )
    {
        JsonDocument doc;
        doc["kitId"] = slot;
        doc["id"]    = slot;
        doc["name"]  = name;

        JsonArray padsArr = doc["pads"].to<JsonArray>();

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            JsonObject p = padsArr.add<JsonObject>();

            p["sample"]            = src[i].sample;
            p["midiNote"]          = src[i].cfg.midiNote;
            p["threshold"]         = src[i].cfg.threshold;
            p["releaseThreshold"]  = src[i].cfg.releaseThreshold;
            p["peakTime"]          = src[i].cfg.peakTime;
            p["releaseStableTime"] = src[i].cfg.releaseStableTime;
            p["maskBaseMs"]        = src[i].cfg.maskBaseMs;
            p["sensitivity"]       = src[i].cfg.sensitivity;
            p["curve"]             = src[i].cfg.curve;
            p["padType"]           = src[i].cfg.padType;
            p["sampleId"]          = src[i].cfg.sampleId;
            p["chokeGroup"]        = src[i].cfg.chokeGroup;
        }

        return writeJsonAtomic(jsonPath, doc);
    }

    // --------------------------------------------------
    // Sample copy (best-effort; toleran file hilang)
    // --------------------------------------------------

    bool copyFile(const char *src, const char *dst)
    {
        File sf = SD.open(src, FILE_READ);

        if (!sf)
        {
            return false;
        }

        File df = SD.open(dst, FILE_WRITE);

        if (!df)
        {
            sf.close();
            return false;
        }

        uint8_t buf[1024];
        bool ok = true;

        while (sf.available())
        {
            size_t n = sf.read(buf, sizeof(buf));

            if (n == 0)
            {
                break;
            }

            if (df.write(buf, n) != n)
            {
                ok = false;
                break;
            }
        }

        sf.close();
        df.close();
        return ok;
    }

    void basenameOf(const char *path, char *out, size_t len)
    {
        const char *base = path ? strrchr(path, '/') : nullptr;

        snprintf(
            out,
            len,
            "%s",
            (base && base[1] != '\0') ? base + 1 : (path ? path : "")
        );
    }

    // Salin SEEKALI file ke direktori samples tujuan.
    // return true bila berhasil; false hanya jika file sumber
    // ADA tapi gagal disalin (file hilang = skip diam-diam).
    bool copySampleToDir(const char *dstSamplesDir, const char *srcAbs)
    {
        if (!srcAbs || srcAbs[0] == '\0')
        {
            return true;
        }

        if (!SD.exists(srcAbs))
        {
            return true;   // sumber tidak ada: biarkan fallback saat load
        }

        char base[40];
        basenameOf(srcAbs, base, sizeof(base));

        char dst[KIT_SAMPLE_PATH_LEN];
        snprintf(dst, sizeof(dst), "%s/%s", dstSamplesDir, base);

        return copyFile(srcAbs, dst);
    }

    // Pastikan direktori kit + samples ada (untuk folder
    // berdasar nama yang mungkin belum punya folder fisik,
    // mis. hasil DELETE).
    bool ensureKitDirsFromName(const char *name)
    {
        char d[KIT_NAME_STORAGE + 16];
        snprintf(d, sizeof(d), "/kits/%s", name ? name : "");

        if (!SD.exists(d))
        {
            if (!SD.mkdir(d))
            {
                return false;
            }
        }

        snprintf(d, sizeof(d), "/kits/%s/samples", name ? name : "");

        if (!SD.exists(d))
        {
            if (!SD.mkdir(d))
            {
                return false;
            }
        }

        return true;
    }

    // --------------------------------------------------
    // Copy / delete / move SELURUH pohon direktori
    // (G2 folder move + G3 delete with GC).
    // G12: Arduino-ESP32 File::name() = basename SAJA (pathToFileName);
    // full VFS path hanya via File::path(). Operasi SD.* memakai path().
    // --------------------------------------------------

    bool copyTree(const char *srcDir, const char *dstDir)
    {
        if (!SD.mkdir(dstDir))
        {
            if (!SD.exists(dstDir))
            {
                return false;
            }
        }

        File dir = SD.open(srcDir);

        if (!dir)
        {
            return false;
        }

        bool ok = true;

        while (ok)
        {
            File e = dir.openNextFile();

            if (!e)
            {
                break;
            }

            const char *np = e.path();
            const char *base = np ? strrchr(np, '/') : nullptr;
            base = (base && base[1] != '\0') ? base + 1 : np;

            if (base && strcmp(base, ".") != 0 && strcmp(base, "..") != 0)
            {
                if (e.isDirectory())
                {
                    char nd[KIT_SAMPLE_PATH_LEN];
                    snprintf(nd, sizeof(nd), "%s/%s", dstDir, base);
                    ok = copyTree(np, nd) && ok;
                }
                else
                {
                    char nd[KIT_SAMPLE_PATH_LEN];
                    snprintf(nd, sizeof(nd), "%s/%s", dstDir, base);
                    ok = copyFile(np, nd) && ok;
                }
            }

            e.close();
        }

        dir.close();
        return ok;
    }

    bool removeTree(const char *path)
    {
        File d = SD.open(path);

        if (!d)
        {
            return true;   // sudah tidak ada
        }

        if (!d.isDirectory())
        {
            d.close();
            return SD.remove(path);
        }

        bool ok = true;

        while (ok)
        {
            File e = d.openNextFile();

            if (!e)
            {
                break;
            }

            const char *np = e.path();

            if (e.isDirectory())
            {
                ok = removeTree(np) && ok;
            }
            else
            {
                ok = SD.remove(np) && ok;
            }

            e.close();
        }

        d.close();

        if (ok)
        {
            ok = SD.rmdir(path);
        }

        return ok;
    }

    // Pindah pohon kit srcDir -> dstDir (G2). Coba rename FAT
    // asli dulu (atomic, cepat — FatFs mendukung rename direktori);
    // kalau gagal, fallback copy+delete dengan rollback parsial.
    bool moveKitTree(const char *srcDir, const char *dstDir)
    {
        if (!srcDir || !dstDir ||
            srcDir[0] == '\0' || dstDir[0] == '\0')
        {
            return false;
        }

        if (strcmp(srcDir, dstDir) == 0)
        {
            return true;
        }

        if (SD.exists(dstDir))
        {
            return false;   // target sudah ada -> collision
        }

        if (SD.rename(srcDir, dstDir))
        {
            return SD.exists(dstDir) && !SD.exists(srcDir);
        }

        // Fallback: salin seluruh pohon, lalu hapus sumber.
        if (!copyTree(srcDir, dstDir))
        {
            removeTree(dstDir);   // buang parsial; sumber utuh
            return false;
        }

        // Hapus sumber (best-effort; isi sudah benar di dst).
        if (SD.exists(srcDir))
        {
            removeTree(srcDir);
        }

        return SD.exists(dstDir);
    }

    // --------------------------------------------------
    // CREATE (EMPTY / COPY) G2: tulis kit di folder NAMA.
    // --------------------------------------------------
    //
    // - newDir ("/kits/<name>") adalah destinasi SUDAH MENGIKUTI
    //   nama kit (bukan index). Kalau slot sudah punya folder di
    //   lokasi lain (oldDir), folder lama dibersihkan setelah
    //   sukses supaya filesystem bersih (G3-style GC).
    // - Collision: newDir sudah terisi kit milik slot LAIN.
    // - JSON ditulis atomik dulu (authoritative); gagal = tidak
    //   berubah. Sampel disalin best-effort.
    // - Bila slot == kit aktif, prefix sample loader diarahkan
    //   ulang supaya copy/struktur selanjutnya konsisten.

    KitOpResult writeKitLifecycle(
        uint8_t slot,
        const char *name,
        const KitPadSource *src,
        const char * const srcSamples[PAD_COUNT]
    )
    {
        char newDir[KIT_DIR_LEN];
        dirFromName(name, newDir, sizeof(newDir));

        char oldDir[KIT_DIR_LEN] = {0};
        kitLoaderResolveKitDir(slot, oldDir, sizeof(oldDir));

        // Collision: folder nama tujuan sudah dipakai kit LAIN.
        if (SD.exists(newDir) && strcmp(newDir, oldDir) != 0)
        {
            return KitOpResult::COLLISION;
        }

        if (!ensureKitDirsFromName(name))
        {
            return KitOpResult::BUSY;
        }

        // JSON dulu (authoritative). Gagal di sini = kit tidak berubah.
        char jsonPath[KIT_PATH_LEN];
        jsonFromName(name, jsonPath, sizeof(jsonPath));

        if (!writeJsonFromSources(slot, name, src, jsonPath))
        {
            return KitOpResult::BUSY;
        }

        // Best-effort: salin sampel sumber ke folder baru.
        if (srcSamples)
        {
            char sdir[KIT_DIR_LEN + 16];
            snprintf(sdir, sizeof(sdir), "%s/samples", newDir);

            for (uint8_t i = 0; i < PAD_COUNT; i++)
            {
                copySampleToDir(sdir, srcSamples[i]);
            }
        }

        // Bersihkan folder lama bila kit pindah lokasi (GC).
        if (oldDir[0] != '\0' && strcmp(oldDir, newDir) != 0)
        {
            removeTree(oldDir);
        }

        // Sapu override NVS lama slot tujuan (best-effort). Kegagalan
        // di sini TIDAK menggagalkan CREATE (kit sudah valid).
        persistenceClearKit(slot);

        // Bila yang ditulis adalah kit yang sedang dimuat, arahkan
        // ulang prefix sample (folder kemungkinan berpindah).
        if (slot == kitManagerActiveSlot())
        {
            kitLoaderRetargetSampleDir(newDir);
        }

        return KitOpResult::OK;
    }
}

// ======================================================
// Public
// ======================================================

// --- CREATE cores (dipakai legacy slot-based & named auto-slot) ---
//
// Isi judge (KIT RUNTIME):
//   EMPTY = baseline aman murni (defaultPadSource) + mapping sample
//           kosong ("sample":"" — kit SENYAP sampai user assign via
//           PAD→SOUND) + folder samples/ KOSONG (tidak ada clone
//           WAV factory kit00) + TIDAK otomatis aktif.
//   COPY  = snapshot kit AKTIF (config efektif RAM + mapping sample
//           by filename) ke folder baru duplikat file — kit hasil
//           INDEPENDEN. Keduanya menulis lewat writeKitLifecycle
//           (folder = nama, collision-check, GC folder lama, NVS
//           override dibersihkan, retarget bila slot aktif).

static KitOpResult createKitEmpty(uint8_t slot, const char *name)
{
    // Baseline aman MURNI dari tabel config.h (bukan kit00). Tidak
    // pernah membaca/menyalin factory kit00.
    KitPadSource src[PAD_COUNT];

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        defaultPadSource(i, src[i]);
        src[i].sample[0] = '\0';   // mapping kosong: pad senyap
    }

    // srcSamples = nullptr -> tidak ada file disalin; samples/
    // dibuat kosong oleh ensureKitDirsFromName.
    return writeKitLifecycle(slot, name, src, nullptr);
}

static KitOpResult createKitCopy(uint8_t slot, const char *name)
{
    // Snapshot kit AKTIF (config efektif RAM). Sample name = nama
    // file resolved (mapping by filename); pad unassigned ikut
    // tidak punya sample (bukan fallback default root).
    KitPadSource src[PAD_COUNT];

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        src[i].cfg = getPadConfig(i);

        if (kitLoaderResolvedSampleName(i, src[i].sample, sizeof(src[i].sample)))
        {
            // resolved: sample ter-assign, nama file tersimpan.
        }
        else
        {
            src[i].sample[0] = '\0';   // unassigned (senyap)
        }
    }

    // Sampel sumber = resolved path sample kit AKTIF.
    const char *srcSamples[PAD_COUNT];

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        const char *p = kitLoaderResolvedSamplePath(i);
        srcSamples[i] = (p && p[0] != '\0') ? p : nullptr;
    }

    return writeKitLifecycle(slot, name, src, srcSamples);
}

bool kitManagerSlotOccupied(uint8_t slot)
{
    if (slot >= KIT_COUNT)
    {
        return false;
    }

    char dir[KIT_DIR_LEN];
    return kitLoaderResolveKitDir(slot, dir, sizeof(dir));
}

uint8_t kitManagerActiveSlot()
{
    return uartGetActiveKit();
}

KitOpResult kitManagerCreateEmpty(uint8_t slot)
{
    if (slot >= KIT_COUNT)
    {
        return KitOpResult::INVALID_INDEX;
    }

    if (slot == KIT_FACTORY_INDEX)
    {
        return KitOpResult::PROTECTED;
    }

    char name[KIT_NAME_STORAGE];
    snprintf(name, sizeof(name), "KIT %02u", (unsigned)slot);

    return createKitEmpty(slot, name);
}

KitOpResult kitManagerCreateCopy(uint8_t slot)
{
    if (slot >= KIT_COUNT)
    {
        return KitOpResult::INVALID_INDEX;
    }

    if (slot == KIT_FACTORY_INDEX)
    {
        return KitOpResult::PROTECTED;
    }

    const char *activeName = kitLoaderGetName();
    char name[KIT_NAME_STORAGE];

    if (activeName &&
        activeName[0] != '\0' &&
        strcmp(activeName, "/none") != 0)
    {
        strncpy(name, activeName, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    else
    {
        snprintf(name, sizeof(name), "KIT %02u", (unsigned)slot);
    }

    return createKitCopy(slot, name);
}

uint8_t kitManagerFindFreeSlot()
{
    for (uint8_t s = 1; s < KIT_COUNT; s++)
    {
        if (!kitManagerSlotOccupied(s))
        {
            return s;
        }
    }

    return KIT_COUNT;   // semua slot user terisi
}

KitOpResult kitManagerCreateEmptyNamed(const char *name)
{
    char clean[KIT_NAME_STORAGE];

    if (!kitManagerValidateName(name, clean, sizeof(clean)))
    {
        return KitOpResult::INVALID_NAME;
    }

    uint8_t slot = kitManagerFindFreeSlot();

    if (slot >= KIT_COUNT)
    {
        // G24.15: "tidak ada slot user kosong" BUKAN kegagalan SD dan
        // BUKAN backend yang sibuk. Nilai sendiri supaya UI bisa
        // jujur bilang NO SPACE, bukan generic BUSY.
        return KitOpResult::NO_SPACE;
    }

    return createKitEmpty(slot, clean);
}

KitOpResult kitManagerCreateCopyNamed(const char *name)
{
    char clean[KIT_NAME_STORAGE];

    if (!kitManagerValidateName(name, clean, sizeof(clean)))
    {
        return KitOpResult::INVALID_NAME;
    }

    uint8_t slot = kitManagerFindFreeSlot();

    if (slot >= KIT_COUNT)
    {
        return KitOpResult::NO_SPACE;
    }

    return createKitCopy(slot, clean);
}

KitOpResult kitManagerRename(uint8_t slot, const char *name)
{
    if (slot >= KIT_COUNT)
    {
        return KitOpResult::INVALID_INDEX;
    }

    char clean[KIT_NAME_STORAGE];

    if (!kitManagerValidateName(name, clean, sizeof(clean)))
    {
        return KitOpResult::INVALID_NAME;
    }

    // Lokasi kit yang SEDANG ADA (name-based atau legacy).
    char oldDir[KIT_DIR_LEN];

    if (!kitLoaderResolveKitDir(slot, oldDir, sizeof(oldDir)))
    {
        return KitOpResult::NOT_FOUND;
    }

    char path[KIT_PATH_LEN];
    snprintf(path, sizeof(path), "%s/kit.json", oldDir);

    File f = SD.open(path, FILE_READ);

    if (!f)
    {
        return KitOpResult::NOT_FOUND;
    }

    // Skip UTF-8 BOM.
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
        return KitOpResult::NOT_FOUND;
    }

    const char *oldName = doc["name"] | (const char *)"";

    char newDir[KIT_DIR_LEN];
    dirFromName(clean, newDir, sizeof(newDir));

    // ------------------------------------------------------
    // Collision (G2): folder nama baru sudah dipakai kit LAIN.
    // Karena folder = nama kit, ini berarti nama sudah dimiliki
    // kit lain. Kecuali newDir == oldDir (rename ke nama yang
    // sudah sama -> hanya update isi).
    // ------------------------------------------------------
    if (SD.exists(newDir) && strcmp(newDir, oldDir) != 0)
    {
        return KitOpResult::COLLISION;
    }

    doc["name"] = clean;

    // 1. Nama baru ditulis ke JSON dulu (authoritative).
    //    Gagal = kit tidak berubah (writeJsonAtomic bersih).
    if (!writeJsonAtomic(path, doc))
    {
        return KitOpResult::BUSY;
    }

    // 2. Pindahkan FOLDER kit ke nama baru (G2). Kalau gagal,
    //    kembalikan nama JSON lama supaya "gagal = tidak berubah".
    if (strcmp(newDir, oldDir) != 0)
    {
        if (!moveKitTree(oldDir, newDir))
        {
            // Rollback nama (best-effort). moveKitTree menjamin
            // sumber utuh saat rename/copy gagal, jadi path lama
            // masih bisa ditulis.
            doc["name"] = oldName;
            writeJsonAtomic(path, doc);
            return KitOpResult::BUSY;
        }
    }

    // 3. Cache nama aktif + arahkan ulang prefix sample bila yang
    //    di-rename adalah kit yang sedang dimuat (folder pindah).
    if (slot == kitManagerActiveSlot())
    {
        kitLoaderRetargetSampleDir(newDir);
    }

    kitLoaderSetCachedName(slot, clean);

    return KitOpResult::OK;
}

KitOpResult kitManagerDelete(uint8_t slot)
{
    if (slot >= KIT_COUNT)
    {
        return KitOpResult::INVALID_INDEX;
    }

    if (slot == KIT_FACTORY_INDEX)
    {
        return KitOpResult::PROTECTED;
    }

    char dir[KIT_DIR_LEN];
    const bool hasDir = kitLoaderResolveKitDir(slot, dir, sizeof(dir));

    // Jika yang dihapus adalah kit aktif, pindah RAM dulu ke
    // kit00 lewat jalur LOAD yang aman (gate + tunggu voice).
    if (slot == uartGetActiveKit())
    {
        if (uartPerformKitSwitch(KIT_FACTORY_INDEX) != STATUS_OK)
        {
            return KitOpResult::BUSY;
        }
    }

    if (hasDir)
    {
        // G3: hapus SELURUH pohon — samples/*, samples/, kit.json,
        // hingga folder itu sendiri (GC penuh, tidak ada orphan).
        if (!removeTree(dir))
        {
            return KitOpResult::BUSY;
        }
    }
    else
    {
        // Slot sudah tidak ter-resolve (mis. kit.json hilang duluan
        // atau hanya sisa legacy). Bersihkan sisa folder index &
        // flat json agar tidak ada orphan.
        char legacyDir[KIT_DIR_LEN];
        snprintf(legacyDir, sizeof(legacyDir), "/kits/kit%02u", (unsigned)slot);

        if (SD.exists(legacyDir))
        {
            if (!removeTree(legacyDir))
            {
                return KitOpResult::BUSY;
            }
        }

        char legacyFlat[24];
        snprintf(legacyFlat, sizeof(legacyFlat), "/kits/kit%02u.json", (unsigned)slot);

        if (SD.exists(legacyFlat))
        {
            if (!SD.remove(legacyFlat))
            {
                return KitOpResult::BUSY;
            }
        }
    }

    // Sapu override NVS kit tsb (best-effort).
    persistenceClearKit(slot);

    // Boot-kit (NVS active) jangan sampai menunjuk slot yang
    // dihapus (kasus NVS active != RAM active setelah LOAD via
    // UART yang tidak mem-persist NVS).
    uint8_t savedActive = 0;
    uint8_t savedVolume = 0;

    if (persistenceLoadGlobal(savedActive, savedVolume))
    {
        if (savedActive == slot)
        {
            persistenceSaveGlobal(KIT_FACTORY_INDEX, savedVolume);
        }
    }

    return KitOpResult::OK;
}

KitOpResult kitManagerLoad(uint8_t slot)
{
    if (slot >= KIT_COUNT)
    {
        return KitOpResult::INVALID_INDEX;
    }

    uint8_t st = uartPerformKitSwitch(slot);

    if (st == STATUS_OK)
    {
        return KitOpResult::OK;
    }

    if (st == STATUS_BUSY)
    {
        return KitOpResult::BUSY;
    }

    return KitOpResult::INVALID_INDEX;
}

KitOpResult kitManagerSaveActive()
{
    // Kit SAVE: serialisasi konfigurasi EFEKTIF RAM kit AKTIF ke
    // kit.json-nya (atomic). kit.json = satu-satunya source of
    // truth setelah ini; NVS per-kit TIDAK lagi ditulis.
    const uint8_t active = uartGetActiveKit();

    // Nama aktif dari cache loader (paling akurat — ikut rename).
    char name[KIT_NAME_STORAGE];
    kitLoaderGetName(active, name, sizeof(name));

    // Lokasi kit aktif di SD (dir-resolved/legacy fallback).
    char dir[KIT_DIR_LEN];

    if (!kitLoaderResolveKitDir(active, dir, sizeof(dir)))
    {
        snprintf(dir, sizeof(dir), "/kits/kit%02u", (unsigned)active);
    }

    char jsonPath[KIT_PATH_LEN];
    snprintf(jsonPath, sizeof(jsonPath), "%s/kit.json", dir);

    if (SD.exists(dir) && !SD.exists(jsonPath))
    {
        // Kit folder ada tapi belum punya JSON (state rusak/legacy
        // kitNN). Jangan menulis ke folder tanpa JSON — biarkan
        // resolver tetap konsisten.
        return KitOpResult::BUSY;
    }

    KitPadSource src[PAD_COUNT];

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        src[i].cfg = getPadConfig(i);

        if (kitLoaderResolvedSampleName(i, src[i].sample, sizeof(src[i].sample)))
        {
            // resolved: nama file tersimpan (mapping by filename).
        }
        else
        {
            src[i].sample[0] = '\0';   // unassigned (senyap)
        }
    }

    if (!writeJsonFromSources(active, name, src, jsonPath))
    {
        return KitOpResult::BUSY;
    }

    return KitOpResult::OK;
}

// ======================================================
// Public: Kit Migrate NVS Overrides -> kit.json
// ======================================================

bool kitManagerMigrateNvsOverrides()
{
    // Migrasi sekali waktu (boot) dari blob NVS per-kit legacy
    // (kit0..kit4) ke kit.json masing-masing. Blob TIDAK pernah
    // lagi ditulis sejak KIT RUNTIME (kit.json = source of truth).
    //
    // Untuk tiap slot yang punya blob valid:
    //   - slot harus punya kit.json (resolvasi + file ada). Bila
    //     tidak ada (kit dihapus / rusak), override dibuang saja
    //     bersama blob — TIDAK membuat kit baru dari NVS.
    //   - merge: load isi kit.json sbg baseline, timpa field yang
    //     VALID menurut decodePad (mengikuti chart PAD_FIELD_MIN/
    //     MAX), tulis balik atomik. Mapping sample (nama file)
    //     TIDAK disentuh blob (blob tidak menyimpan sample).
    //
    // Best-effort total: kegagalan per-slot tidak menggagalkan
    // slot lain / keseluruhan boot. Semua blob dihapus di akhir.

    for (uint8_t slot = 0; slot < KIT_COUNT; slot++)
    {
        if (!persistenceHasKitBlob(slot))
        {
            continue;
        }

        char dir[KIT_DIR_LEN];
        bool hasDir = kitLoaderResolveKitDir(slot, dir, sizeof(dir));

        if (!hasDir)
        {
            snprintf(dir, sizeof(dir), "/kits/kit%02u", (unsigned)slot);
        }

        char jsonPath[KIT_PATH_LEN];
        snprintf(jsonPath, sizeof(jsonPath), "%s/kit.json", dir);

        if (!SD.exists(jsonPath))
        {
            // kit.json tidak ada: tidak ada baseline untuk merge.
            // Blob tetap dibuang di akhir (override mati bersama
            // kit yang sudah tidak ada).
            continue;
        }

        KitPadSource src[PAD_COUNT];

        if (loadKitSource(slot, src))
        {
            PadConfig cfg[PAD_COUNT];

            for (uint8_t i = 0; i < PAD_COUNT; i++)
            {
                cfg[i] = src[i].cfg;   // baseline dari kit.json
            }

            if (persistenceLoadKitConfig(slot, cfg))
            {
                for (uint8_t i = 0; i < PAD_COUNT; i++)
                {
                    src[i].cfg = cfg[i];
                }
            }

            // Nama dibaca langsung dari kit.json (bukan cache loader,
            // supaya side-effect free & aman dipanggil saat boot
            // sebelum kit aktif di-build).
            char name[KIT_NAME_STORAGE] = "UNNAMED";

            {
                File nf = SD.open(jsonPath, FILE_READ);

                if (nf)
                {
                    uint8_t peekBuf[3];
                    size_t pn = nf.read(peekBuf, 3);

                    if (pn < 3 ||
                        peekBuf[0] != 0xEF ||
                        peekBuf[1] != 0xBB ||
                        peekBuf[2] != 0xBF)
                    {
                        nf.seek(0);
                    }

                    JsonDocument ndoc;

                    if (!deserializeJson(ndoc, nf))
                    {
                        const char *nn = ndoc["name"] | (const char *)nullptr;

                        if (nn && nn[0] != '\0')
                        {
                            strncpy(name, nn, sizeof(name) - 1);
                            name[sizeof(name) - 1] = '\0';
                        }
                    }

                    nf.close();
                }
            }

            if (writeJsonFromSources(slot, name, src, jsonPath))
            {
                Serial0.print("=== MIGRATE NVS -> kit.json slot ");
                Serial0.print(slot);
                Serial0.println(" OK ===");
            }
            else
            {
                Serial0.print("=== MIGRATE NVS -> kit.json slot ");
                Serial0.print(slot);
                Serial0.println(" WRITE FAILED (best-effort) ===");
            }
        }
    }

    // Semua blob legacy dibuang — selesai (idempoten di boot
    // berikutnya: tidak ada blob tersisa untuk diproses).
    persistenceClearAllKitOverrides();

    return true;
}

bool kitManagerValidateName(
    const char *name,
    char *clean,
    size_t cleanCap
)
{
    if (!name || !clean || cleanCap == 0)
    {
        return false;
    }

    // Hitung panjang tertulis, potong di KIT_NAME_MAX saat scan.
    size_t n = 0;

    for (; name[n] != '\0' && n < KIT_NAME_MAX; n++)
    {
        char c = name[n];

        if (c == '/' || c == '\\')
        {
            return false;   // separator / traversal
        }

        if (c == '"' || c == '*' || c == ':' ||
            c == '<' || c == '>' || c == '?' || c == '|')
        {
            return false;   // karakter ilegal FAT (folder = nama)
        }

        if (c < 0x20)
        {
            return false;   // control character
        }
    }

    if (name[n] != '\0')
    {
        return false;       // lebih dari KIT_NAME_MAX
    }

    // Trim spasi kiri.
    size_t start = 0;

    while (start < n && name[start] == ' ')
    {
        start++;
    }

    // Trim spasi kanan.
    while (n > start && name[n - 1] == ' ')
    {
        n--;
    }

    if (start >= n)
    {
        return false;       // kosong setelah trim
    }

    // Nama mustahil "." dan ".." (folder traversal / FAT).
    if (n - start == 1 && name[start] == '.')
    {
        return false;
    }

    if (n - start == 2 && name[start] == '.' && name[start + 1] == '.')
    {
        return false;
    }

    // Titik di ujung (setelah trim spasi): FAT memotong titik akhir
    // nama — folder hasil != string "name", jadi tolak.
    if (name[n - 1] == '.')
    {
        return false;
    }

    size_t out = 0;

    for (size_t i = start; i < n && out + 1 < cleanCap; i++)
    {
        clean[out++] = name[i];
    }

    clean[out] = '\0';
    return out > 0;
}