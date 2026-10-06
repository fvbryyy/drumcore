#include "uart_protocol.h"

#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "pad.h"
#include "kit_loader.h"
#include "kit_manager.h"
#include "persistence.h"
#include "psram_loader.h"
#include "audio.h"
#include "sdcard.h"

// ======================================================
// Internal State
// ======================================================

namespace
{
    // UART1 hardware
    HardwareSerial UartPort(1);

    // Control-plane runtime state (dari NVS saat boot, diubah
    // oleh command UART). Bukan realtime path.
    uint8_t uartActiveKit  = 0;
    uint8_t uartPreviewKit = 0;   // preview index, does NOT trigger kit load
    uint8_t uartVolume     = 0;

    // Task handle + stack
    //
    // G19: UART_TASK_STACK dinaikkan dari 4096 ke 8192.
    // Audit stack menunjukkan call chain CMD_KIT_LOAD
    // (performKitSwitch → kitLoaderBuildPath →
    // resolveKitDirWithMeta → readKitMeta → deserializeJson
    // → kitLoaderLoad → psramLoadKit) menghabiskan estimasi
    // 3200-3800 byte — margin 300-900 byte dari 4096, terlalu
    // tipis untuk ArduinoJson + SD stack alignment. Stack
    // canary watchpoint terbukti dipicu dari CMD_KIT_LOAD.
    // 8192 memberikan ~4096 byte headroom yang rasional.
    TaskHandle_t uartTaskHandle = nullptr;
    constexpr uint32_t UART_TASK_STACK = 8192;   // G19: was 4096, overflow proven
    constexpr uint32_t KIT_INFO_TASK_STACK = 6144;
    constexpr uint8_t  UART_TASK_PRIORITY = 5;   // Core 1, di bawah midi(8)

    TaskHandle_t kitInfoTaskHandle = nullptr;
    volatile bool kitInfoRequestPending = false;
    volatile bool kitInfoResultReady = false;
    uint8_t kitInfoRequestIndex = 0;
    uint8_t kitInfoRequestSeq = 0;
    char kitInfoResultName[KIT_NAME_STORAGE] = {0};
    uint32_t kitInfoResultId = 0;

    // Kit switch wait timing (bukan "naikkan timeout 600ms"):
    // floor tetap 600ms untuk kasus normal; sisanya adaptif
    // mengikuti sisa durasi voice terpanjang (crash/cymbal).
    constexpr uint32_t KIT_SWITCH_WAIT_FLOOR_MS   = 600;
    constexpr uint32_t KIT_SWITCH_WAIT_MARGIN_MS  = 100;
    constexpr uint32_t KIT_SWITCH_WAIT_MAX_MS     = 20000;

    // ==================================================
    // CMD_KIT_RENAME chunk collector (TASK 3)
    // ==================================================
    //
    // Nama (maks KIT_NAME_MAX) datang sebagai header + chunk
    // 3 char/frame (sama mekanisme KIT_INFO). State dibuat per
    // request: header baru menimbulkan rename state baru, chunk
    // dengan seq berbeda diabaikan (noise/sisa request lama).

    bool   renamePending = false;
    uint8_t renameSeq    = 0;
    uint8_t renameSlot   = 0;
    uint8_t renameLen    = 0;
    uint8_t renameGot    = 0;
    char   renameName[KIT_NAME_MAX + 1] = {0};

    // CREATE bernama (G4): sesi transfer nama TERSENDIRI dari
    // RENAME (tidak boleh saling tumpang-tindih). Header membawa
    // panjang nama; chunk 3 char/frame mengisi createName.
    bool    createPending = false;
    uint8_t createSeq     = 0;
    uint8_t createLen     = 0;
    uint8_t createGot     = 0;
    char    createName[KIT_NAME_MAX + 1] = {0};

    // PAD_SOUND_SET (KIT RUNTIME): sesi transfer nama file sample
    // (maks SAMPLE_NAME_MAX) — TERSENDIRI dari RENAME/CREATE.
    bool    psPending = false;
    uint8_t psSeq     = 0;
    uint8_t psPad     = 0;
    uint8_t psLen     = 0;
    uint8_t psGot     = 0;
    char    psName[SAMPLE_NAME_MAX + 1] = {0};

    // Map KitOpResult (kit_manager) ke status protokol UART.
    uint8_t kitResultToStatus(KitOpResult r)
    {
        switch (r)
        {
            case KitOpResult::OK:            return STATUS_OK;
            case KitOpResult::INVALID_INDEX: return STATUS_INVALID_KIT;
            case KitOpResult::NOT_FOUND:     return STATUS_INVALID_KIT;
            case KitOpResult::INVALID_NAME:  return STATUS_INVALID_VALUE;
            case KitOpResult::BUSY:          return STATUS_BUSY;
            case KitOpResult::PROTECTED:     return STATUS_INVALID_KIT;
            case KitOpResult::COLLISION:     return STATUS_NAME_EXISTS;
            // G24.15:bedakan "tidak ada slot kosong" dari BUSY.
            case KitOpResult::NO_SPACE:      return STATUS_NO_SPACE;
            default:                         return STATUS_BUSY;
        }
    }

    // ==================================================
    // Frame helpers
    // ==================================================

    // Debug: log CMD_KIT_INFO assembly ke Serial0 (UART0 debug).
    // Default OFF — flip ke true hanya saat debugging KIT_INFO.
    static constexpr bool KIT_INFO_DEBUG = false;

    // Debug: log PAD -> SOUND assignment ke Serial0 (UART0 debug).
    // Trace di control plane (UartTask), bukan realtime audio path.
    // Menunjukkan REMAP ONLY vs LOAD NEW SAMPLE per operasi.
    // Default OFF — flip ke true hanya saat debugging PAD_SOUND_SET.
    static constexpr bool DEBUG_SOUND_SET = false;

    inline uint8_t frameCrc(
        uint8_t cmd,
        uint8_t a0,
        uint8_t a1,
        uint8_t a2,
        uint8_t a3,
        uint8_t seq
    )
    {
        // CRC 8-bit = XOR(CMD,ARG0..ARG3,SEQ)
        return (uint8_t)(cmd ^ a0 ^ a1 ^ a2 ^ a3 ^ seq);
    }

    bool sendFrame(
        uint8_t cmd,
        uint8_t a0,
        uint8_t a1,
        uint8_t a2,
        uint8_t a3,
        uint8_t seq
    )
    {
        uint8_t f[UART_FRAME_LEN];
        f[0] = UART_SOF;
        f[1] = cmd;
        f[2] = a0;
        f[3] = a1;
        f[4] = a2;
        f[5] = a3;
        f[6] = seq;
        f[7] = frameCrc(cmd, a0, a1, a2, a3, seq);

        size_t n = UartPort.write(f, UART_FRAME_LEN);

        if constexpr (DEBUG_UART)
        {
            Serial0.print("[UART] TX n=");
            Serial0.print(n);
            Serial0.print(" CMD=0x");
            Serial0.print(cmd, HEX);
            Serial0.print(" SEQ=");
            Serial0.println(seq);
        }

        return (n == UART_FRAME_LEN);
    }

    // Respon: CMD | RESP_FLAG, status di ARG0
    void sendResponse(
        uint8_t reqCmd,
        uint8_t status,
        uint8_t a1,
        uint8_t a2,
        uint8_t a3,
        uint8_t seq
    )
    {
        sendFrame(
            (uint8_t)(reqCmd | UART_RESP_FLAG),
            status,
            a1,
            a2,
            a3,
            seq
        );
    }

    // ==================================================
    // Kit-info name chunks (CMD_KIT_INFO, TASK 1)
    // ==================================================
    //
    // Kirim nama kit sebagai frame chunk berurutan (3 char per
    // frame; frame header menandakan mulai). Setiap frame:
    //   [0] SOF  [1] CMD|RESP_FLAG  [2] chunkIdx|0x80
    //   [3..5] char0..char2  [6] SEQ  [7] CRC
    // Max 28 char -> 10 chunk. Header mendahului via ARG0 tanpa
    // bit 0x80; chunk ditandai bit 0x80 di ARG0.
    void sendKitInfoNameFrames(
        const char *name,
        uint8_t seq
    )
    {
        uint8_t nameLen = 0;
        while (name[nameLen] != '\0' && nameLen < 28)
        {
            nameLen++;
        }

        uint8_t chunk = 0;
        while (chunk * 3 < nameLen)
        {
            uint8_t c0 = name[chunk * 3 + 0];
            uint8_t c1 = (chunk * 3 + 1 < nameLen)
                ? name[chunk * 3 + 1]
                : 0;
            uint8_t c2 = (chunk * 3 + 2 < nameLen)
                ? name[chunk * 3 + 2]
                : 0;

            sendFrame(
                (uint8_t)(CMD_KIT_INFO | UART_RESP_FLAG),
                (uint8_t)(chunk | 0x80),  // ARG0: chunk index + flag
                c0,
                c1,
                c2,
                seq
            );
            chunk++;
        }
    }

    bool performKitInfoLookup(
        uint8_t kitIndex,
        char *name,
        size_t nameSize,
        uint32_t *idOut
    )
    {
        if constexpr (KIT_INFO_DEBUG)
        {
            if (kitIndex != uartActiveKit)
            {
                Serial0.printf(
                    "[KIT INFO ESP] PREVIEW RESOLVE idx=%u\n",
                    (unsigned)kitIndex
                );
            }

            Serial0.printf(
                "[KIT INFO ESP] stack-safe lookup idx=%u\n",
                (unsigned)kitIndex
            );
        }

        const bool found = kitLoaderGetKitInfo(
            kitIndex,
            name,
            nameSize,
            idOut
        );

        if constexpr (KIT_INFO_DEBUG)
        {
            if (found)
            {
                Serial0.printf(
                    "[KIT INFO ESP] RESOLVED idx=%u kitId=%lu name='%s'\n",
                    (unsigned)kitIndex,
                    (unsigned long)(idOut ? *idOut : kitIndex),
                    name
                );
            }
            else if (!sdIsReady())
            {
                // G19: Bedakan SD failure dari kit memang tidak ada.
                // UNNAMED dari SD failure bukan bug cache/nama.
                Serial0.printf(
                    "[KIT INFO ESP] RESOLVE idx=%u -> SD NOT READY (name unresolvable)\n",
                    (unsigned)kitIndex
                );
                // Fallback ke "UNNAMED" untuk display agar Wemos
                // tetap menampilkan sesuatu (bukan layar kosong).
                if (nameSize > 0)
                {
                    snprintf(name, nameSize, "UNNAMED");
                }
            }
            else
            {
                Serial0.printf(
                    "[KIT INFO ESP] RESOLVE idx=%u -> UNNAMED\n",
                    (unsigned)kitIndex
                );
            }
        }
        else
        {
            // Debug off: pastikan nama tetap fallback "UNNAMED"
            // bila found=false (agar display tidak kosong).
            if (!found && nameSize > 0 && name[0] == '\0')
            {
                snprintf(name, nameSize, "UNNAMED");
            }
        }

        return found;
    }

    void kitInfoTaskLoop(void *)
    {
        for (;;)
        {
            if (!kitInfoRequestPending)
            {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            const uint8_t index = kitInfoRequestIndex;
            char name[KIT_NAME_STORAGE] = {0};
            uint32_t id = index;

            performKitInfoLookup(index, name, sizeof(name), &id);

            strncpy(
                kitInfoResultName,
                name,
                sizeof(kitInfoResultName) - 1
            );
            kitInfoResultName[sizeof(kitInfoResultName) - 1] = '\0';
            kitInfoResultId = id;
            kitInfoRequestPending = false;
            kitInfoResultReady = true;
        }
    }

    void requestKitInfo(uint8_t index, uint8_t seq)
    {
        (void)seq;

        kitInfoResultReady = false;
        kitInfoResultId = index;
        kitInfoRequestIndex = index;
        kitInfoRequestSeq = seq;
        kitInfoRequestPending = true;

        if (!kitInfoTaskHandle)
        {
            char name[KIT_NAME_STORAGE] = {0};
            uint32_t id = index;
            performKitInfoLookup(index, name, sizeof(name), &id);
            strncpy(
                kitInfoResultName,
                name,
                sizeof(kitInfoResultName) - 1
            );
            kitInfoResultName[sizeof(kitInfoResultName) - 1] = '\0';
            kitInfoResultId = id;
            kitInfoRequestPending = false;
            kitInfoResultReady = true;
            return;
        }

        while (!kitInfoResultReady)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    void handleKitInfo(uint8_t index, uint8_t seq)
    {
        if (index > UART_MAX_KIT)
        {
            if constexpr (KIT_INFO_DEBUG)
            {
                Serial0.printf(
                    "[KIT INFO ESP] RX request idx=%u seq=%u -> INVALID\n",
                    (unsigned)index,
                    (unsigned)seq
                );
                Serial0.printf(
                    "[KIT INFO ESP] RESOLVE idx=%u -> UNNAMED\n",
                    (unsigned)index
                );
            }

            sendResponse(
                CMD_KIT_INFO,
                STATUS_INVALID_KIT,
                0,
                0,
                0,
                seq
            );
            return;
        }

        if constexpr (KIT_INFO_DEBUG)
        {
            Serial0.printf(
                "[KIT INFO ESP] RX request idx=%u seq=%u\n",
                (unsigned)index,
                (unsigned)seq
            );
        }

        requestKitInfo(index, seq);

        uint8_t nameLen = 0;
        while (kitInfoResultName[nameLen] != '\0' &&
               nameLen < KIT_NAME_MAX)
        {
            nameLen++;
        }

        sendResponse(
            CMD_KIT_INFO,
            STATUS_OK,
            (uint8_t)kitInfoResultId,
            nameLen,
            0,
            seq
        );

        sendKitInfoNameFrames(kitInfoResultName, seq);

        if constexpr (KIT_INFO_DEBUG)
        {
            Serial0.printf(
                "[KIT INFO ESP] TX response seq=%u\n",
                (unsigned)seq
            );
        }
    }

    // ==================================================
    // Pad field access (bounds-checked)
    // ==================================================

    // Batas nilai field DIAMBIL dari satu sumber kebenaran:
    // PAD_FIELD_MIN/MAX di config.h (G8-A). Transport UART hanya
    // membawa 16-bit, jadi maksimum otoritatif di-clamp ke 0xFFFF
    // di lapisan ini saja (frame tetap 8-byte; format tidak berubah).
    uint32_t padFieldUartMax(uint8_t fieldId)
    {
        uint32_t m = PAD_FIELD_MAX[fieldId];
        return (m > 0xFFFFu) ? 0xFFFFu : m;
    }

    bool setPadField(
        PadConfig &cfg,
        PadConfig &run,
        uint8_t fieldId,
        uint16_t value
    )
    {
        if (fieldId >= FIELD_COUNT)
        {
            return false;
        }

        const uint32_t minV = PAD_FIELD_MIN[fieldId];
        const uint32_t maxV = padFieldUartMax(fieldId);

        if (value < minV || value > maxV)
        {
            return false;
        }

        switch (fieldId)
        {
            case FIELD_MIDI_NOTE:
                cfg.midiNote = (uint8_t)value;
                run.midiNote = (uint8_t)value;
                break;

            case FIELD_THRESHOLD:
                cfg.threshold = value;
                run.threshold = value;
                break;

            case FIELD_RELEASE_THRESH:
                // harus < threshold
                if (value >= cfg.threshold)
                {
                    return false;
                }
                cfg.releaseThreshold = value;
                run.releaseThreshold = value;
                break;

            case FIELD_PEAK_TIME:
                cfg.peakTime = value;
                run.peakTime = value;
                break;

            case FIELD_RELEASE_STABLE:
                cfg.releaseStableTime = value;
                run.releaseStableTime = value;
                break;

            case FIELD_MASK_BASE_MS:
                cfg.maskBaseMs = value;
                run.maskBaseMs = value;
                break;

            case FIELD_SENSITIVITY:
                cfg.sensitivity = (uint8_t)value;
                run.sensitivity = (uint8_t)value;
                break;

            case FIELD_CURVE:
                cfg.curve = (uint8_t)value;
                run.curve = (uint8_t)value;
                break;

            case FIELD_PAD_TYPE:
                cfg.padType = (uint8_t)value;
                run.padType = (uint8_t)value;
                break;

            case FIELD_SAMPLE_ID:
                cfg.sampleId = (uint8_t)value;
                run.sampleId = (uint8_t)value;
                break;

            case FIELD_CHOKE_GROUP:
                cfg.chokeGroup = (uint8_t)value;
                run.chokeGroup = (uint8_t)value;
                break;

            default:
                return false;
        }

        return true;
    }

    uint16_t getPadField(const PadConfig &cfg, uint8_t fieldId)
    {
        switch (fieldId)
        {
            case FIELD_MIDI_NOTE:       return cfg.midiNote;
            case FIELD_THRESHOLD:       return cfg.threshold;
            case FIELD_RELEASE_THRESH:  return cfg.releaseThreshold;
            case FIELD_PEAK_TIME:       return cfg.peakTime;
            case FIELD_RELEASE_STABLE:  return (uint16_t)cfg.releaseStableTime;
            case FIELD_MASK_BASE_MS:    return (uint16_t)cfg.maskBaseMs;
            case FIELD_SENSITIVITY:     return cfg.sensitivity;
            case FIELD_CURVE:           return cfg.curve;
            case FIELD_PAD_TYPE:        return cfg.padType;
            case FIELD_SAMPLE_ID:       return cfg.sampleId;
            case FIELD_CHOKE_GROUP:     return cfg.chokeGroup;
            default:                    return 0;
        }
    }

    // ==================================================
    // Thread-safe pad config update
    //
    // TriggerTask (Core 1, prio 23) membaca pad.config saat
    // scan/hit. Update config dari UART task memakai critical
    // section singkat (µs). Karena tiap field atomic + lock
    // writer, aman tanpa mengubah scheduler/priority.
    // ==================================================

    portMUX_TYPE uartSpin = portMUX_INITIALIZER_UNLOCKED;

    bool uartSetPadField(uint8_t padIndex, uint8_t fieldId, uint16_t value)
    {
        if (padIndex >= PAD_COUNT || fieldId >= FIELD_COUNT)
        {
            return false;
        }

        bool ok = false;

        portENTER_CRITICAL(&uartSpin);
        {
            PadConfig &cfg = getPadConfigMutable(padIndex);
            PadConfig &run = pads[padIndex].config;
            ok = setPadField(cfg, run, fieldId, value);
        }
        portEXIT_CRITICAL(&uartSpin);

        return ok;
    }

    // ==================================================
    // Sample registry resync (G8-B)
    //
    // PAD_SET sampleId mengubah mapping pad->slot. Arsitektur
    // yang ada membuat SEMUA field PAD_SET langsung runtime-
    // effective (menulis copy runtime `run`); sampleId tidak
    // terkecuali: dibangun ulang registry pad->slot dari config
    // efektif SEKARANG. PSRAM tidak di-unload/reload (file 12
    // slot sudah dimuat); hanya tabel mapping 12-byte yang
    // di-update. Audio membaca slotForPad per-hit; tiap entry
    // uint8 ditulis atomik, jadi pad yang kena hit saat remap
    // memakai slot lama ATAU baru (dua-duanya valid & termuat) —
    // tidak perlu lock tambahan, arsitektur task tidak berubah.
    // PERSISTENSI tetap via CMD_KIT_SAVE (model sama dgn field
    // lain: PAD_SET = RAM, KIT_SAVE = NVS, reload = NVS+JSON).
    // ==================================================

    void uartApplySampleIdRegistry()
    {
        uint8_t ids[PAD_COUNT];

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            ids[i] = getPadConfig(i).sampleId;
        }

        psramRebuildSlotRegistry(ids);
    }

    // ==================================================
    // Kit Switch (control op — BUKAN realtime path)
    //
    // Audio engine memegang pointer PSRAM (voicePtr) selama
    // voice aktif. Sebelum unload/load kit, TUNGGU sampai
    // tidak ada voice aktif (audioActiveVoiceCount()==0)
    // agar tidak membebaskan PSRAM yang sedang dibaca.
    // Setelah unload, psramGetData()=nullptr -> startVoice
    // bails aman. Seluruh op blocking di task UART (prio 5),
    // tidak memblok Trigger/Audio.
    // ==================================================

    uint8_t performKitSwitch(uint8_t newKit, uint8_t seq)
    {
        (void)seq;

        if (newKit > UART_MAX_KIT)
        {
            return STATUS_INVALID_KIT;
        }

        if (newKit == uartActiveKit)
        {
            return STATUS_OK;   // sudah aktif; tidak perlu reload
        }

        // ==================================================
        // 1. Gate hit acceptance SEKARANG, sebelum apa pun.
        //    Hit baru dibuang; voice yang sudah aktif tetap
        //    selesai NORMAL (tidak dipotong). Hanya atomic
        //    flag, tidak ada blocking di realtime path.
        // ==================================================

        audioSetHitAccept(false);

        // ==================================================
        // 2. Tunggu voice aktif selesai. Karena hit baru sudah
        //    di-gate, count akan turun ke 0 tanpa diisi lagi.
        //    Deadline adaptif = sisa durasi voice terpanjang
        //    + margin (crash/cymbal bisa berbunyi ~detik).
        // ==================================================

        const uint32_t waitStartMs = millis();

        // Dari gate-off ke sini, TIDAK ada lagi voice baru
        // (hit ring di-discard). Baseline sisa durasi voice
        // terpanjang diambil SEKALI di awal, dan deadline
        // dibuat FIXED.
        //
        // PENTING — jangan recompute deadline dengan remainUs
        // tiap iterasi: remainUs = sisa byte / rate, yang
        // KEHILANGAN nilainya seiring playback. Deadline yang
        // ikut mengecil membuat loop menyerah (BUSY) kira-kira
        // di SETENGAH durasi voice — persis gejala "busy saat
        // crash/cymbal dimainkan". Deadline DISINI berbasis
        // baseline penuh (panjang sample asli) + margin,
        // sehingga switch menunggu voice selesai sungguhan.

        const uint32_t remainUs0 =
            audioActiveVoiceMaxRemainingUs();
        const uint32_t remainMs0 =
            (remainUs0 + 999u) / 1000u;

        uint32_t deadlineMs =
            waitStartMs + KIT_SWITCH_WAIT_FLOOR_MS +
            remainMs0 + KIT_SWITCH_WAIT_MARGIN_MS;

        if ((deadlineMs - waitStartMs) >
            KIT_SWITCH_WAIT_MAX_MS)
        {
            deadlineMs =
                waitStartMs + KIT_SWITCH_WAIT_MAX_MS;
        }

        for (;;)
        {
            if (audioActiveVoiceCount() == 0)
            {
                break;
            }

            const uint32_t nowMs = millis();

            if ((int32_t)(nowMs - deadlineMs) >= 0)
            {
                if constexpr (DEBUG_UART)
                {
                    Serial0.println(
                        "[UART] kit switch busy (voices active)"
                    );
                }

                // Buka kembali penerimaan hit SEBELUM return,
                // supaya BUG apa pun di sini tidak meninggalkan
                // drum dalam keadaan bisu.
                audioSetHitAccept(true);

                return STATUS_BUSY;
            }

            vTaskDelay(pdMS_TO_TICKS(5));
        }

        // ==================================================
        // 3. Snapshot state aktif untuk rollback yang aman.
        //    Kit aktif lama (PadConfig + kit index) tidak boleh
        //    rusak jika load gagal (safety SD/JSON/PSRAM).
        //
        // G19: cfgSnapshot + runSnapshot dipindahkan ke static
        // untuk menghilangkan 720 byte dari stack frame
        // performKitSwitch (12 × PadConfig × 2 array).
        // AMAN: performKitSwitch hanya dipanggil dari dispatch()
        // yang berjalan di satu thread (UartTask) — non-reentrant
        // by design, static tidak menambah risiko.
        // ==================================================

        static PadConfig cfgSnapshot[PAD_COUNT];
        static PadConfig runSnapshot[PAD_COUNT];

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            cfgSnapshot[i] = getPadConfig(i);
            runSnapshot[i] = pads[i].config;
        }

        const uint8_t oldKit = uartActiveKit;

        // ==================================================
        // 4. Load kit baru (JSON base -> PSRAM).
        // ==================================================

        char path[KIT_PATH_LEN];
        kitLoaderBuildPath(newKit, path, sizeof(path));

        // Mutate PadConfig ke kit baru (dianggap "best effort";
        // dijejaki kit_loader selalu nullable-fallback).
        kitLoaderLoad(path, PAD_SAMPLE_PATHS, PAD_COUNT);

        bool loadOk = false;

        if (sdIsReady())
        {
            if (psramSetup())
            {
                // KIT RUNTIME: mapping by filename -> slot == pad
                // index (identity). NVS per-kit sudah dimigrasi &
                // dibuang; sampleIds identity sehingga registry
                // slotForPad[i] = i.
                const char *kitPaths[PAD_COUNT];
                uint8_t sampleIds[PAD_COUNT];

                for (uint8_t i = 0; i < PAD_COUNT; i++)
                {
                    kitPaths[i] =
                        kitLoaderResolvedSamplePath(i);
                    sampleIds[i] = i;   // identity
                }

                // Return-nya WAJIB diperiksa: psramLoadKit()
                // memakai desain free-then-alloc, jadi kalau
                // ps_malloc() gagal PSRAM sudah kosong dan kita
                // HARUS reload kit lama (lihat blok rollback).
                loadOk = psramLoadKit(kitPaths, sampleIds, PAD_COUNT);
            }
        }

        // ==================================================
        // 5. Commit atau rollback.
        // ==================================================

        if (loadOk)
        {
            // Satu-satunya titik uartActiveKit berubah:
            // hanya setelah SEMUA operasi switch berhasil.
            uartActiveKit = newKit;

            // Persist global (TEST G): restart berikutnya boot ke
            // kit yang sama. Best-effort (gagal NVS tidak
            // menggagalkan switch). Volume tidak pernah berubah.
            persistenceSaveGlobal(uartActiveKit, uartVolume);

            if constexpr (DEBUG_UART)
            {
                Serial0.print("[UART] kit switch -> ");
                Serial0.println(newKit);
            }
        }
        else
        {
            if constexpr (DEBUG_UART)
            {
                Serial0.println(
                    "[UART] kit switch FAILED - restoring old kit"
                );
            }

            // Rollback kit_loader internal (index/nama/paths)
            // + PadConfig ke kondisi sebelum switch.
            char oldPath[KIT_PATH_LEN];
            kitLoaderBuildPath(oldKit, oldPath, sizeof(oldPath));
            kitLoaderLoad(oldPath, PAD_SAMPLE_PATHS, PAD_COUNT);

            for (uint8_t i = 0; i < PAD_COUNT; i++)
            {
                getPadConfigMutable(i) = cfgSnapshot[i];
                pads[i].config        = runSnapshot[i];
            }

            // ---------------------------------------------
            // G23: PSRAM TIDAK otomatis utuh setelah load gagal.
            //
            // psramLoadKit() memakai desain free-then-alloc
            // (satu blok, tanpa double buffer): blok kit lama
            // DAN slot runtime G21 dibebaskan SEBELUM
            // ps_malloc() blok baru. Kalau alokasi itu gagal,
            // PSRAM sudah KOSONG sementara uartActiveKit masih
            // menunjuk kit lama -> drum bisu total.
            //
            // Konsekuensi: hanya mengembalikan
            // uartActiveKit/PadConfig TIDAK cukup; kit lama
            // harus DIMUAT ULANG supaya "rollback" benar-benar
            // berarti "kit A valid lagi, sample A tersedia
            // lagi, mapping A tetap".
            //
            // Aman karena kita masih di dalam regions yang sama:
            // hit gate masih OFF dan audioActiveVoiceCount()==0
            // sudah dibuktikan di atas, jadi tidak ada voice yang
            // membaca buffer yang dibebaskan.
            // ---------------------------------------------
            bool restored = false;

            if (sdIsReady())
            {
                const char *oldPaths[PAD_COUNT];
                uint8_t oldIds[PAD_COUNT];

                for (uint8_t i = 0; i < PAD_COUNT; i++)
                {
                    oldPaths[i] = kitLoaderResolvedSamplePath(i);
                    oldIds[i] = i;   // identity
                }

                restored = psramLoadKit(oldPaths, oldIds, PAD_COUNT);
            }

            if (restored)
            {
                uartActiveKit = oldKit;
            }
            else
            {
                // Kit lama juga gagal dimuat. uartActiveKit sengaja
                // TIDAK diubah (tetap menunjuk kit lama = intent
                // pengguna), tapi PSRAM kosong sehingga drum bisu
                // sampai KIT LOAD berikutnya sukses. Keadaan ini
                // harus terlihat, bukan disembunyikan.
                Serial0.println(
                    "[UART] ROLLBACK FAILED - PSRAM empty, no kit active"
                );

                psramPrintStatus();
            }
        }

        // ==================================================
        // 6. Buang sisa hit lama dari ring, BARU aktifkan
        //    kembali penerimaan hit (acceptance F).
        // ==================================================

        for (;;)
        {
            if (audioHitRingEmpty())
            {
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1));
        }

        audioSetHitAccept(true);

        return loadOk ? STATUS_OK : STATUS_BUSY;
    }

    // ==================================================
    // PAD -> SOUND runtime remap (G21)
    // ==================================================
    //
    // PAD_SOUND_SET HANYA mengubah mapping pad -> slot PSRAM. Jalur
    // ini tidak pernah memuat ulang kit:
    //
    //   file SUDAH di PSRAM
    //     -> psramFindSlotByPath()   lookup registry, memory-only
    //     -> psramSetPadSlot()       satu store uint8 di slotForPad
    //     -> SELESAI (tanpa SD, tanpa malloc, tanpa gate)
    //
    //   file BARU di folder samples kit aktif
    //     -> psramLoadSingleSample() baca SATU file, alokasi PSRAM
    //        sendiri, daftar slot baru
    //     -> psramSetPadSlot()       remap ke slot baru
    //     -> 11 sample lain tidak disentuh sama sekali
    //
    // Kenapa TIDAK ada hit-gate + tunggu-voice seperti KIT SWITCH:
    //   - jalur remap hanya menulis 1 byte di slotForPad; tidak ada
    //     buffer PSRAM yang ditulis atau di-free, jadi voice yang
    //     sedang bunyi tidak mungkin memakai pointer rusak;
    //   - jalur file baru hanya MENGALOKASI buffer baru (slot punya
    //     alokasi sendiri) — tidak ada pointer PCM lama yang perlu
    //     di-rebase atau di-free;
    //   - voice aktif dibiarkan selesai normal; tidak ada hit hilang.
    // PSRAM kit tetap di-free HANYA pada KIT SWITCH
    // (performKitSwitch), yang masih memakai gate + tunggu voice.
    //
    // Gagal = tidak ada yang di-commit (mapping RAM maupun slot
    // tetap seperti semula), jadi tidak ada rollback yang perlu
    // dijalankan. Persistensi tetap lewat CMD_KIT_SAVE (nama file
    // di kit.json) — fungsi ini tidak pernah menulis SD.
    // ==================================================

    __attribute__((noinline)) uint8_t performSampleAssign(
        uint8_t padIndex,
        const char *absPath
    )
    {
        if (padIndex >= PAD_COUNT)
        {
            return STATUS_INVALID_VALUE;
        }

        // Nama file saja untuk trace (tanpa alokasi).
        const char *fileName = absPath ? strrchr(absPath, '/') : nullptr;
        fileName = (fileName && fileName[1] != '\0')
            ? fileName + 1
            : absPath;

        if constexpr (DEBUG_SOUND_SET)
        {
            Serial0.printf(
                "[SOUND SET] pad=%u file=%s\n",
                (unsigned)padIndex,
                fileName ? fileName : "(none)"
            );
        }

        // Idempoten: mapping sudah persis sama -> OK tanpa lookup,
        // tanpa SD, tanpa load. (PAD01 -> KICK.wav dua kali)
        const char *cur = kitLoaderResolvedSamplePath(padIndex);

        if (cur && absPath && strcmp(cur, absPath) == 0)
        {
            if constexpr (DEBUG_SOUND_SET)
            {
                Serial0.println(
                    "[SOUND SET] UNCHANGED (idempotent, no work)");
            }

            return STATUS_OK;
        }

        // UNASSIGN: pad menjadi senyap. Sample yang pernah dipakai
        // pad ini TIDAK di-free — pad lain / voice aktif boleh masih
        // memakainya, dan membiarkan slot hidup adalah perilaku
        // aman minimum yang diminta.
        if (!absPath || absPath[0] == '\0')
        {
            psramSetPadSlot(padIndex, PSRAM_SLOT_NONE);
            kitLoaderSetResolvedSamplePath(padIndex, "");

            if constexpr (DEBUG_SOUND_SET)
            {
                Serial0.println(
                    "[SOUND SET] UNASSIGN (pad senyap, sample PSRAM tetap hidup)");
            }

            return STATUS_OK;
        }

        uint32_t remapStartUs = 0;
        uint32_t loadStartUs = 0;

        if constexpr (DEBUG_SOUND_SET)
        {
            remapStartUs = micros();
        }

        // ---- Skenario A: sample sudah loaded -> REMAP ONLY ----
        const uint8_t cachedSlot = psramFindSlotByPath(absPath);

        if (cachedSlot != PSRAM_SLOT_NONE)
        {
            // Slot dulu, baru path: mapping audio berubah duluan,
            // bookkeeping persistensi menyusul (keduanya ~us).
            psramSetPadSlot(padIndex, cachedSlot);
            kitLoaderSetResolvedSamplePath(padIndex, absPath);

            if constexpr (DEBUG_SOUND_SET)
            {
                Serial0.printf(
                    "[SOUND SET] cached slot=%u\n",
                    (unsigned)cachedSlot
                );
                Serial0.printf(
                    "[SOUND SET] REMAP ONLY elapsed=%luus\n",
                    (unsigned)(micros() - remapStartUs)
                );
            }

            return STATUS_OK;
        }

        // ---- Skenario B: file baru -> load SATU file ----
        //
        // Tidak ada unload/reload 11 sample lain, tidak ada
        // performKitSwitch, tidak ada full kit load.
        // psramIsLoaded() berarti kit sudah termuat, jadi PSRAM
        // sudah pasti siap (dijamin jalur full kit load).
        if (!sdIsReady() || !psramIsLoaded())
        {
            if constexpr (DEBUG_SOUND_SET)
            {
                Serial0.println(
                    "[SOUND SET] FAILED (SD/PSRAM belum siap)");
            }

            return STATUS_BUSY;
        }

        if constexpr (DEBUG_SOUND_SET)
        {
            loadStartUs = micros();
        }

        const uint8_t newSlot = psramLoadSingleSample(absPath);

        if (newSlot == PSRAM_SLOT_NONE)
        {
            if constexpr (DEBUG_SOUND_SET)
            {
                Serial0.println(
                    "[SOUND SET] LOAD NEW SAMPLE FAILED (mapping tidak berubah)");
            }

            return STATUS_BUSY;
        }

        psramSetPadSlot(padIndex, newSlot);
        kitLoaderSetResolvedSamplePath(padIndex, absPath);

        if constexpr (DEBUG_SOUND_SET)
        {
            Serial0.printf(
                "[SOUND SET] LOAD NEW SAMPLE slot=%u elapsed=%luus\n",
                (unsigned)newSlot,
                (unsigned)(micros() - loadStartUs)
            );
            Serial0.printf(
                "[SOUND SET] REMAP ONLY elapsed=%luus\n",
                (unsigned)(micros() - remapStartUs)
            );
        }

        return STATUS_OK;
    }

    // ==================================================
    // Parser state
    // ==================================================

    enum ParserState
    {
        PARSER_SEEK_SOF,
        PARSER_COLLECT
    };

    ParserState parserState = PARSER_SEEK_SOF;
    uint8_t frame[UART_FRAME_LEN];
    uint8_t frameIdx = 0;

    // ==================================================
    // Command dispatch
    // ==================================================

    void dispatch(const uint8_t *f)
    {
        uint8_t cmd  = f[1];
        uint8_t a0   = f[2];
        uint8_t a1   = f[3];
        uint8_t a2   = f[4];
        uint8_t a3   = f[5];
        uint8_t seq  = f[6];

        if constexpr (DEBUG_UART)
        {
            Serial0.print("[UART] CMD=0x");
            Serial0.print(cmd, HEX);
            Serial0.print(" a0=");
            Serial0.print(a0);
            Serial0.print(" a1=");
            Serial0.print(a1);
            Serial0.print(" a2=");
            Serial0.print(a2);
            Serial0.print(" a3=");
            Serial0.println(a3);
        }

        switch (cmd)
        {
            // ---- Ping ----
            case CMD_PING:
                sendResponse(cmd, STATUS_OK, 1, 0, 0, seq);
                break;

            // ---- Status ----
            case CMD_REQUEST_STATUS:
                sendResponse(
                    cmd,
                    STATUS_OK,
                    uartActiveKit,       // ARG1 = activeKit
                    PAD_COUNT,           // ARG2 = padCount
                    uartVolume,          // ARG3 = volume
                    seq
                );
                break;

            // ---- Kit ----
            //
            // NEXT/PREV: preview only — update uartPreviewKit,
            // do NOT call performKitSwitch(). The active kit and
            // PSRAM/NVS remain unchanged until LOAD is issued.
            //
            // LOAD: the ONLY command that triggers a full kit
            // switch (JSON + PadConfig + PSRAM + NVS + activeKit).

            case CMD_KIT_NEXT: {
                if (uartPreviewKit < UART_MAX_KIT)
                {
                    uartPreviewKit++;
                }
                // Always respond STATUS_OK so Wemos updates its display.
                sendResponse(cmd, STATUS_OK, uartPreviewKit, 0, 0, seq);
                break;
            }

            case CMD_KIT_PREV: {
                if (uartPreviewKit > 0)
                {
                    uartPreviewKit--;
                }
                sendResponse(cmd, STATUS_OK, uartPreviewKit, 0, 0, seq);
                break;
            }

            case CMD_KIT_LOAD: {
                // G19: Watermark sebelum operasi berat
                // (hanya saat DEBUG_UART=true).
                if constexpr (DEBUG_UART)
                {
                    UBaseType_t wm =
                        uxTaskGetStackHighWaterMark(nullptr);
                    Serial0.print("[UART TASK] CMD_KIT_LOAD BEGIN idx=");
                    Serial0.print(a0);
                    Serial0.print(" stack_free=");
                    Serial0.println(wm * sizeof(StackType_t));
                }

                uint8_t st = performKitSwitch(a0, seq);

                if constexpr (DEBUG_UART)
                {
                    UBaseType_t wm =
                        uxTaskGetStackHighWaterMark(nullptr);
                    Serial0.print("[UART TASK] CMD_KIT_LOAD END st=");
                    Serial0.print(st);
                    Serial0.print(" stack_watermark=");
                    Serial0.println(wm * sizeof(StackType_t));
                }

                if (st == STATUS_OK)
                {
                    uartPreviewKit = uartActiveKit;  // sync preview
                }
                sendResponse(cmd, st, uartActiveKit, 0, 0, seq);
                break;
            }

            case CMD_KIT_SAVE:
                // KIT RUNTIME: save = serialize config EFEKTIF RAM
                // (PadConfig + mapping sample by filename) ke
                // kit.json kit aktif (atomic). NVS per-kit TIDAK
                // lagi ditulis (kit.json = source of truth).
                {
                    KitOpResult kr = kitManagerSaveActive();
                    if (kr == KitOpResult::OK)
                    {
                        sendResponse(cmd, STATUS_OK, uartActiveKit, 0, 0, seq);
                    }
                    else
                    {
                        sendResponse(cmd, STATUS_BUSY, uartActiveKit, 0, 0, seq);
                    }
                }
                break;

            // ---- Kit Management (TASK 3) ----
            //
            // CREATE EMPTY/COPY/DELETE: ARG0 = slot tujuan/subjek.
            // RENAME: header (ARG0=slot, ARG1=len) + chunk data.
            // Semua respon = satu frame normal, status di ARG0.

            case CMD_KIT_CREATE_EMPTY: {
                KitOpResult r = kitManagerCreateEmpty(a0);
                sendResponse(cmd, kitResultToStatus(r), a0, 0, 0, seq);
                break;
            }

            case CMD_KIT_CREATE_COPY: {
                KitOpResult r = kitManagerCreateCopy(a0);
                sendResponse(cmd, kitResultToStatus(r), a0, 0, 0, seq);
                break;
            }

            case CMD_KIT_DELETE: {
                KitOpResult r = kitManagerDelete(a0);
                sendResponse(cmd, kitResultToStatus(r), a0, 0, 0, seq);
                break;
            }

            case CMD_KIT_RENAME: {
                if (a0 & 0x80)
                {
                    // ---- Data chunk ----
                    if (!renamePending || seq != renameSeq)
                    {
                        break;   // sisa request lama / noise
                    }

                    uint8_t idx = (uint8_t)(a0 & 0x7F) * 3;

                    // Batasi copy per-char ke dalam panjang nama &
                    // kapasitas buffer (chunk terakhir bisa parsial).
                    if (idx < renameLen)
                    {
                        renameName[idx] = (char)a1;
                        if (idx + 1 < renameLen)
                        {
                            renameName[idx + 1] = (char)a2;
                        }
                        if (idx + 2 < renameLen)
                        {
                            renameName[idx + 2] = (char)a3;
                        }
                    }

                    renameGot += 3;

                    if (renameGot >= renameLen)
                    {
                        renameName[renameLen] = '\0';
                        renamePending = false;

                        KitOpResult r =
                            kitManagerRename(renameSlot, renameName);

                        sendResponse(
                            cmd,
                            kitResultToStatus(r),
                            renameSlot,
                            0,
                            0,
                            renameSeq
                        );
                    }
                }
                else
                {
                    // ---- Header: mulai request rename ----
                    if (a0 > UART_MAX_KIT || a1 == 0 || a1 > KIT_NAME_MAX)
                    {
                        renamePending = false;
                        sendResponse(cmd, STATUS_INVALID_VALUE, a0, 0, 0, seq);
                        break;
                    }

                    renamePending = true;
                    renameSeq     = seq;
                    renameSlot    = a0;
                    renameLen     = a1;
                    renameGot     = 0;
                    memset(renameName, 0, sizeof(renameName));
                }
                break;
            }

            // ---- CREATE EMPTY/COPY bernama (G4, chunked) ----
            //
            // Header: ARG0 = 0 (cadangan), ARG1 = panjang nama.
            // Chunk : ARG0 = (chunkIdx | 0x80), ARG1..3 = hingga
            //         3 char nama (mekanisme sama seperti RENAME).
            // ESP32 memilih slot kosong otomatis; konsep slot tidak
            // diekspos ke user. Respon = satu frame status (ARG1 =
            // slot terisi — internal/instrumentasi).
            case CMD_KIT_CREATE_EMPTY_NAMED:
            case CMD_KIT_CREATE_COPY_NAMED: {
                const uint8_t cCmd = cmd;

                if (a0 & 0x80)
                {
                    // ---- Data chunk ----
                    if (!createPending || seq != createSeq)
                    {
                        break;   // sisa request lama / noise
                    }

                    uint8_t idx = (uint8_t)(a0 & 0x7F) * 3;

                    if (idx < createLen)
                    {
                        createName[idx] = (char)a1;
                        if (idx + 1 < createLen)
                        {
                            createName[idx + 1] = (char)a2;
                        }
                        if (idx + 2 < createLen)
                        {
                            createName[idx + 2] = (char)a3;
                        }
                    }

                    createGot += 3;

                    if (createGot >= createLen)
                    {
                        createName[createLen] = '\0';
                        createPending = false;

                        KitOpResult r = (cCmd == CMD_KIT_CREATE_EMPTY_NAMED)
                            ? kitManagerCreateEmptyNamed(createName)
                            : kitManagerCreateCopyNamed(createName);

                        sendResponse(
                            cCmd,
                            kitResultToStatus(r),
                            0,   // slot: dipilih internal (lihat nama)
                            0,
                            0,
                            createSeq
                        );
                    }
                }
                else
                {
                    // ---- Header: mulai sesi create bernama ----
                    if (a1 == 0 || a1 > KIT_NAME_MAX)
                    {
                        createPending = false;
                        sendResponse(cCmd, STATUS_INVALID_VALUE, 0, 0, 0, seq);
                        break;
                    }

                    createPending = true;
                    createSeq     = seq;
                    createLen     = a1;
                    createGot     = 0;
                    memset(createName, 0, sizeof(createName));
                }
                break;
            }

            case CMD_KIT_INFO:
                handleKitInfo(a0, seq);
                break;

            // ---- Pad ----
            case CMD_PAD_SET: {
                if (a0 >= PAD_COUNT)
                {
                    sendResponse(cmd, STATUS_INVALID_PAD, a0, 0, 0, seq);
                    break;
                }
                uint16_t value = (uint16_t)(a2 | (a3 << 8));
                if (uartSetPadField(a0, a1, value))
                {
                    // G8-B: sampleId PAD_SET = runtime-effective
                    // SEKARANG (registry pad->slot disinkronkan),
                    // konsisten dgn semua field PAD_SET lain.
                    if (a1 == FIELD_SAMPLE_ID)
                    {
                        uartApplySampleIdRegistry();
                    }
                    sendResponse(cmd, STATUS_OK, a0, a1, 0, seq);
                }
                else
                {
                    sendResponse(cmd, STATUS_INVALID_VALUE, a0, a1, 0, seq);
                }
                break;
            }

            case CMD_PAD_GET: {
                if (a0 >= PAD_COUNT)
                {
                    sendResponse(cmd, STATUS_INVALID_PAD, a0, 0, 0, seq);
                    break;
                }
                if (a1 >= FIELD_COUNT)
                {
                    sendResponse(cmd, STATUS_INVALID_VALUE, a0, a1, 0, seq);
                    break;
                }
                // G1 fix: nilai field 16-bit dikirim sebagai DUA frame
                // berurutan dengan SEQ sama, supaya tidak ada byte yang
                // terpotong dalam frame 8-byte (4 arg):
                //   frame lo : ARG1=pad, ARG2=field,        ARG3=byte rendah
                //   frame hi : ARG1=pad, ARG2=field|0x80,   ARG3=byte tinggi
                // Format sama dengan dump pad CMD_FULL_SYNC (di bawah),
                // dan posisi pad/field sama dengan respon CMD_PAD_SET.
                uint16_t value = getPadField(getPadConfig(a0), a1);
                sendResponse(
                    cmd,
                    STATUS_OK,
                    a0, a1,
                    (uint8_t)(value & 0xFF),
                    seq
                );
                sendResponse(
                    cmd,
                    STATUS_OK,
                    a0, (uint8_t)(a1 | 0x80),
                    (uint8_t)(value >> 8),
                    seq
                );
                break;
            }

            // ---- Pad sound list (KIT RUNTIME) ----
            //
            // Read-only, control plane. Daftar suara = scan folder
            // samples kit AKTIF (sorted, cap PAD_COUNT) — mapping by
            // FILENAME. Request ARG0 = index pad yang ingin dilihat
            // (utk menghitung "current index" di ARG3).
            // Respon:
            //   frame header     : ARG0=STATUS_OK (ARG0=status),
            //                      ARG1=jumlah ter-scan, ARG2=0xC7,
            //                      ARG3=index suara pad tsb di daftar
            //                      (0xFF bila tidak ada / unassigned).
            //   frame nama-header: ARG0=index suara (tanpa 0x80),
            //                      ARG1=panjang nama, ARG2=0, ARG3=0.
            //   frame chunk      : ARG0=(chunkIdx|0x80), ARG1..3=nama.
            // TIDAK mengubah state apa pun (kit/PSRAM/NVS).
            case CMD_PAD_SOUND: {
                if (a0 >= PAD_COUNT)
                {
                    sendResponse(cmd, STATUS_INVALID_PAD, a0, 0, 0, seq);
                    break;
                }

                // G20 TRACE: log request masuk (transition-only).
                {
                    char sdir[KIT_SAMPLE_PATH_LEN];
                    kitLoaderGetSampleDir(sdir, sizeof(sdir));
                    Serial0.printf("[SOUND ESP] RX pad=%u seq=%u\n",
                                   (unsigned)a0, (unsigned)seq);
                    Serial0.printf("[SOUND ESP] SAMPLE DIR '%s'\n", sdir);
                }

                char names[PAD_COUNT][SAMPLE_NAME_STORAGE];
                const uint8_t count =
                    kitLoaderScanSampleNames(names, PAD_COUNT);

                // G20 TRACE: log scan result.
                Serial0.printf("[SOUND ESP] SCAN COUNT=%u\n",
                               (unsigned)count);

                // Current index = posisi sample pad (mapping RAM by
                // filename) di daftar scan; 0xFF bila tidak ada.
                uint8_t currentIndex = 0xFF;
                char padName[SAMPLE_NAME_STORAGE];

                if (kitLoaderResolvedSampleName(
                        a0, padName, sizeof(padName)))
                {
                    for (uint8_t s = 0; s < count; s++)
                    {
                        if (strcmp(names[s], padName) == 0)
                        {
                            currentIndex = s;
                            break;
                        }
                    }
                }

                // G20 TRACE: log header yang akan dikirim.
                Serial0.printf("[SOUND ESP] TX HEADER count=%u current=%u\n",
                               (unsigned)count, (unsigned)currentIndex);

                sendResponse(
                    cmd,
                    STATUS_OK,
                    count,
                    0xC7,        // penanda daftar
                    currentIndex,
                    seq
                );

                for (uint8_t s = 0; s < count; s++)
                {
                    uint8_t nameLen = 0;

                    while (names[s][nameLen] != '\0' &&
                           nameLen < SAMPLE_NAME_MAX)
                    {
                        nameLen++;
                    }

                    // G20 TRACE: log setiap nama yang dikirim.
                    Serial0.printf("[SOUND ESP] TX NAME idx=%u name='%s'\n",
                                   (unsigned)s, names[s]);

                    sendFrame(
                        (uint8_t)(cmd | UART_RESP_FLAG),
                        s, nameLen, 0, 0,
                        seq
                    );

                    uint8_t chunk = 0;

                    while (chunk * 3 < nameLen)
                    {
                        uint8_t c0 = names[s][chunk * 3 + 0];
                        uint8_t c1 = (chunk * 3 + 1 < nameLen)
                            ? names[s][chunk * 3 + 1] : 0;
                        uint8_t c2 = (chunk * 3 + 2 < nameLen)
                            ? names[s][chunk * 3 + 2] : 0;

                        sendFrame(
                            (uint8_t)(cmd | UART_RESP_FLAG),
                            (uint8_t)(chunk | 0x80),
                            c0, c1, c2,
                            seq
                        );
                        chunk++;
                    }
                }
                break;
            }

            // ---- Pad sound set (KIT RUNTIME, chunked) ----
            //
            // Assign WAV dari folder samples kit AKTIF ke sebuah pad.
            // RAM-only sampai CMD_KIT_SAVE. Mekanisme nama chunked
            // identik CMD_KIT_RENAME:
            //   header: ARG0=index pad, ARG1=panjang nama (≤
            //           SAMPLE_NAME_MAX).      chunk: ARG0=(idx|0x80).
            // Validasi: nama valid (kitLoaderValidateSampleName) +
            // file ADA di folder samples (#include kit_loader API).
            // Sukses = performSampleAssign (gate+tunggu+reload).
            // Respon = satu frame status (seq = seq REQUEST header).
            case CMD_PAD_SOUND_SET: {
                if (a0 & 0x80)
                {
                    // ---- Data chunk ----
                    if (!psPending || seq != psSeq)
                    {
                        break;   // sisa request lama / noise
                    }

                    uint8_t idx = (uint8_t)(a0 & 0x7F) * 3;

                    if (idx < psLen)
                    {
                        psName[idx] = (char)a1;

                        if (idx + 1 < psLen)
                        {
                            psName[idx + 1] = (char)a2;
                        }

                        if (idx + 2 < psLen)
                        {
                            psName[idx + 2] = (char)a3;
                        }
                    }

                    psGot += 3;

                    if (psGot >= psLen)
                    {
                        psName[psLen] = '\0';
                        psPending = false;

                        uint8_t st = STATUS_INVALID_VALUE;

                        if (kitLoaderValidateSampleName(psName))
                        {
                            char sdir[KIT_SAMPLE_PATH_LEN];
                            kitLoaderGetSampleDir(
                                sdir, sizeof(sdir));

                            char abs[KIT_SAMPLE_PATH_LEN];
                            snprintf(
                                abs,
                                sizeof(abs),
                                "%s%s",
                                sdir,
                                psName
                            );

                            // Pastikan langkah terakhir aman: path
                            // absolut di dalam folder samples, file
                            // wajib ADA atau assign ditolak (tidak
                            // memperkenalkan mapping yang tak
                            // ter-loadable).
                            if (SD.exists(abs))
                            {
                                st = performSampleAssign(psPad, abs);
                            }
                        }

                        sendResponse(
                            cmd,
                            st,
                            psPad,
                            0,
                            0,
                            psSeq
                        );
                    }
                }
                else
                {
                    // ---- Header: mulai sesi assign ----
                    if (a0 >= PAD_COUNT ||
                        a1 == 0 || a1 > SAMPLE_NAME_MAX)
                    {
                        psPending = false;
                        sendResponse(
                            cmd,
                            STATUS_INVALID_VALUE,
                            a0,
                            0,
                            0,
                            seq
                        );
                        break;
                    }

                    psPending = true;
                    psSeq     = seq;
                    psPad     = a0;
                    psLen     = a1;
                    psGot     = 0;
                    memset(psName, 0, sizeof(psName));
                }
                break;
            }

            // ---- Global ----
            case CMD_GLOBAL_SAVE:
                if (persistenceSaveGlobal(uartActiveKit, uartVolume))
                {
                    sendResponse(cmd, STATUS_OK, uartActiveKit, uartVolume, 0, seq);
                }
                else
                {
                    sendResponse(cmd, STATUS_BUSY, uartActiveKit, 0, 0, seq);
                }
                break;

            // ---- Calibrate (reserved, no-op real function) ----
            case CMD_CALIBRATE:
                sendResponse(cmd, STATUS_OK, 0, 0, 0, seq);
                break;

            // ---- Full Sync ----
            case CMD_FULL_SYNC: {
                // Header: status + activeKit + padCount
                sendResponse(
                    CMD_REQUEST_STATUS,
                    STATUS_OK,
                    uartActiveKit,
                    PAD_COUNT,
                    uartVolume,
                    seq
                );

                // Kirim nama kit (dipecah jadi chunk 4-byte; max 4 char
                // per frame, max ~7 frame). Flag master di byte pertama.
                const char *name = kitLoaderGetName();
                uint8_t nameLen = 0;
                while (name[nameLen] != '\0' && nameLen < 28)
                {
                    nameLen++;
                }

                uint8_t chunk = 0;
                while (chunk * 4 < nameLen)
                {
                    uint8_t c0 = name[chunk * 4 + 0];
                    uint8_t c1 = (chunk * 4 + 1 < nameLen) ? name[chunk * 4 + 1] : 0;
                    uint8_t c2 = (chunk * 4 + 2 < nameLen) ? name[chunk * 4 + 2] : 0;
                    uint8_t c3 = (chunk * 4 + 3 < nameLen) ? name[chunk * 4 + 3] : 0;
                    sendFrame(
                        (uint8_t)(CMD_FULL_SYNC | UART_RESP_FLAG),
                        (uint8_t)(chunk | 0x80),  // ARG0: chunk index + flag
                        c0, c1, c2,
                        seq
                    );
                    chunk++;
                }

                // Dump seluruh PadConfig user field per pad. Nilai 16-bit
                // dikirim sebagai DUA frame per field (lo + hi), format
                // identik dengan respon CMD_PAD_GET (lihat di atas).
                for (uint8_t p = 0; p < PAD_COUNT; p++)
                {
                    for (uint8_t fld = 0; fld < FIELD_COUNT; fld++)
                    {
                        uint16_t value = getPadField(getPadConfig(p), fld);
                        sendFrame(
                            (uint8_t)(CMD_PAD_GET | UART_RESP_FLAG),
                            STATUS_OK,
                            p, fld,
                            (uint8_t)(value & 0xFF),
                            seq
                        );
                        sendFrame(
                            (uint8_t)(CMD_PAD_GET | UART_RESP_FLAG),
                            STATUS_OK,
                            p, (uint8_t)(fld | 0x80),
                            (uint8_t)(value >> 8),
                            seq
                        );
                    }
                }
                break;
            }

            default:
                sendResponse(cmd, STATUS_INVALID_COMMAND, 0, 0, 0, seq);
                break;
        }
    }

    // ==================================================
    // RX parser (state machine, non-blocking)
    // ==================================================

    void uartServiceRx()
    {
        while (UartPort.available() > 0)
        {
            uint8_t byte = (uint8_t)UartPort.read();

            switch (parserState)
            {
                case PARSER_SEEK_SOF:
                    if (byte == UART_SOF)
                    {
                        frame[0] = byte;
                        frameIdx = 1;
                        parserState = PARSER_COLLECT;
                    }
                    break;

                case PARSER_COLLECT:
                    frame[frameIdx++] = byte;

                    if (frameIdx >= UART_FRAME_LEN)
                    {
                        // Validasi CRC
                        uint8_t crc = frameCrc(
                            frame[1], frame[2], frame[3],
                            frame[4], frame[5], frame[6]
                        );

                        if (crc == frame[7])
                        {
                            parserState = PARSER_SEEK_SOF;
                            dispatch(frame);
                        }
                        else
                        {
                            if constexpr (DEBUG_UART)
                            {
                                Serial0.println("[UART] CRC error, resync");
                            }
                            // Balas CRC error dgn CMD yang sempat
                            // terbaca (kalau ada), lalu cari SOF lagi.
                            uint8_t badCmd = frame[1];
                            uint8_t badSeq = frame[6];
                            sendFrame(
                                (uint8_t)(badCmd | UART_RESP_FLAG),
                                STATUS_CRC_ERROR, 0, 0, 0,
                                badSeq
                            );
                            parserState = PARSER_SEEK_SOF;
                        }
                    }
                    break;
            }
        }
    }

    // ==================================================
    // UART Task Loop
    // ==================================================

    void uartTaskLoop(void *)
    {
        // G19: Stack watermark interval — report saat debug.
        // Di release build (DEBUG_UART=false) ini di-compile out.
        uint32_t lastWatermarkMs = 0;
        constexpr uint32_t WATERMARK_INTERVAL_MS = 10000;  // 10s

        for (;;)
        {
            if constexpr (DEBUG_UART)
            {
                const uint32_t nowMs = millis();
                if ((nowMs - lastWatermarkMs) >= WATERMARK_INTERVAL_MS)
                {
                    lastWatermarkMs = nowMs;
                    UBaseType_t wm =
                        uxTaskGetStackHighWaterMark(nullptr);
                    Serial0.print("[UART TASK] stack watermark=");
                    Serial0.print(wm * sizeof(StackType_t));
                    Serial0.println(" bytes free");
                }
            }

            uartServiceRx();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ======================================================
// Public: Init
// ======================================================

void uartProtocolInit(uint8_t activeKit, uint8_t volume)
{
    uartActiveKit  = activeKit;
    uartPreviewKit = activeKit;   // preview starts at loaded kit
    uartVolume     = volume;

    UartPort.begin(
        UART1_BAUD,
        SERIAL_8N1,
        UART1_RX_PIN,
        UART1_TX_PIN
    );

    if constexpr (DEBUG_UART)
    {
        Serial0.print("=== UART1 init: baud=");
        Serial0.print(UART1_BAUD);
        Serial0.print(" RX=");
        Serial0.print(UART1_RX_PIN);
        Serial0.print(" TX=");
        Serial0.print(UART1_TX_PIN);
        Serial0.print(" activeKit=");
        Serial0.println(uartActiveKit);
    }

    parserState = PARSER_SEEK_SOF;
    frameIdx = 0;
}

// ======================================================
// Public: Start Task
// ======================================================

void uartTaskStart()
{
    if (!kitInfoTaskHandle)
    {
        xTaskCreatePinnedToCore(
            kitInfoTaskLoop,
            "KitInfoTask",
            KIT_INFO_TASK_STACK,
            nullptr,
            UART_TASK_PRIORITY,
            &kitInfoTaskHandle,
            1
        );
    }

    xTaskCreatePinnedToCore(
        uartTaskLoop,
        "UartTask",
        UART_TASK_STACK,
        nullptr,
        UART_TASK_PRIORITY,
        &uartTaskHandle,
        1
    );

    if constexpr (DEBUG_UART)
    {
        Serial0.println("=== UART task started (Core 1, prio 5) ===");
    }
}

// ======================================================
// Public: Safe Kit Switch (dipakai kit_manager)
// ======================================================

uint8_t uartPerformKitSwitch(uint8_t newKit)
{
    return performKitSwitch(newKit, 0);
}

// ======================================================
// Public: Active Kit Slot (dipakai kit_manager)
// ======================================================

uint8_t uartGetActiveKit()
{
    return uartActiveKit;
}
