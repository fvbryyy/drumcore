#include "psram_loader.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "sdcard.h"

// ======================================================
// Internal State
// ======================================================

namespace
{
    // ======================================================
    // G25: TIDAK ADA LAGI SATU BLOK PSRAM KONTIGU BESAR
    // ======================================================
    //
    // SEBELUMNYA kit memuat seluruh PCM ke SATU blok
    // ps_malloc(totalPCMBytes) ~5.5 MB (samples[i].data =
    // psramBuffer + offset). Satu KIT LOAD butuh satu blok kontigu
    // sebesar SELURUH kit — free-list yang terpecah langsung gagal:
    //     free=8135KB largest=5119KB requested=5453KB
    //     -> PSRAM ALLOC FAILED / cause=FRAGMENTED
    //
    // SEKARANG tiap sample unik dialokasikan sebagai blok PSRAM-nya
    // SENDIRI (slotOwnBuf[slot]). Total kit tidak harus muat dalam
    // satu blok: cukup free TOTAL, karena tiap permintaan hanya
    // sebesar satu sample.
    //
    // Konsekuensi untuk audio: TIDAK ADA. Audio tidak pernah melihat
    // "blok kit" — startVoice() hanya memakai psramGetData(slot) /
    // psramGetDataSize(slot) yang sudah per-slot. audio.cpp, mixer,
    // I2S/DMA, voice allocator, trigger, velocity, priority, dan GPIO
    // TIDAK disentuh sama sekali.
    //
    // slotOwnBuf[] kini SATU-SATUNYA tabel kepemilikan blok PCM di
    // PSRAM (slot kit 0..11 maupun runtime 12..15). Tidak ada blok
    // PCM kedua yang tersembunyi di luar tabel ini.

    // Total byte PCM kit yang hidup di PSRAM (dijumlah dari blok
    // milik slot kit).
    size_t psramTotalAllocated = 0;

    SampleInfo samples[PSRAM_MAX_SAMPLES];
    uint8_t sampleCount = 0;
    bool kitLoaded = false;

    // G25: pengganti sentinel "psramBuffer != nullptr".
    // true  = KIT LOAD terakhir menghasilkan >= 1 blok PCM di PSRAM.
    // false = kit kosong (semua pad unassigned) atau gagal. Dipakai
    // sebagai precondition psramLoadSingleSample().
    bool kitHasPcm = false;

    // ==================================================
    // Pad → PSRAM Slot Registry (dibangun saat commit)
    // ==================================================
    // slotForPad[p] = physical PSRAM slot (0..PAD_COUNT-1) yang
    // memuat sample untuk pad p, ATAU p sendiri bila kit belum
    // dimuat / source slot invalid.
    //
    // Lookup: O(1), memory-only. Dibangun di control plane (saat kit
    // commit / unload), hanya dibaca AudioTask (realtime path).
    // Tanpa SD/malloc/String. Default = identitas (backward-compat).

    uint8_t slotForPad[PAD_COUNT] = {};

    void initSlotForPadIdentity()
    {
        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            slotForPad[i] = i;
        }
    }

    // ==================================================
    // Sample Registry (G21): path file → physical PSRAM slot
    // ==================================================
    //
    // Satu-satunya sumber "file ini sudah ada di PSRAM atau belum".
    // slotPath[s] kosong (== '\0') berarti slot s bebas untuk file baru.
    //
    // Diisi ulang setiap KIT LOAD commit (12 slot kit) dan bertambah
    // satu entri tiap satu file runtime dimuat. Dibaca hanya di
    // control plane (psramFindSlotByPath), TIDAK pernah disentuh
    // AudioTask.
    //
    // Path yang dipakai adalah path absolut hasil resolve (mis.
    // "/kits/Acoustic/samples/SNARE.wav"), bukan basename: dua file
    // bernama sama di folder kit berbeda tidak pernah tertukar.

    char slotPath[PSRAM_MAX_SAMPLES][KIT_SAMPLE_PATH_LEN] = {};

    // ==================================================
    // G25: TABEL KEPEMILIKAN BLOK PCM (SATU-SATUNYA)
    // ==================================================
    //
    // SEBELUMNYA slotOwnBuf[] hanya untuk slot RUNTIME (12..15);
    // PCM kit 12 slot pertama "ikut" satu blok kit tunggal di luar
    // tabel — ada DUA cara memegang PSRAM (tabel + variabel global).
    //
    // SEKARANG tiap slot yang punya PCM punya blok sendiri di sini.
    // slotOwnBuf[s] == nullptr berarti slot s tidak memiliki blok
    // (belum terisi, alias, atau invalid).
    //
    // Alias (dua slot menunjuk file sama) SENGAJA tidak punya blok
    // sendiri: slot alias memakai pointer yang sama dgn slot pemilik
    // dan slotOwnBuf-nya null, sehingga blok hanya dibebaskan SATU
    // KALI oleh releaseAll() — tanpa risiko double free.
    //
    // slotOwnBytes[] menyimpan ukuran blok yang benar-benar
    // dialokasikan (untuk audit leak & laporan alokasi), karena
    // samples[s].dataSize bisa sudah tidak valid saat failure path.
/* clang-format off */
    uint8_t  *slotOwnBuf[PSRAM_MAX_SAMPLES]   = {};
    uint32_t  slotOwnBytes[PSRAM_MAX_SAMPLES] = {};
/* clang-format on */

    void clearSampleRegistry()
    {
        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            slotPath[s][0] = '\0';
        }
    }
    // ==================================================
    // G23 + G25: PSRAM heap instrumentation
    // ==================================================
    //
    // "PSRAM ALLOC FAILED!" lama menutupi DUA kegagalan dgn remedy
    // berbeda:
    //   OOM        free total < requested -> kit memang tidak muat;
    //              kurangi sampel / turunkan PSRAM_MAX_SAMPLE_SIZE.
    //   FRAGMENTED free total >= requested TAPI largest contiguous
    //              block < requested -> ruang cukup tapi terpecah.
    //
    // G25: permintaan terbesar kini hanya sebesar SATU sample (bukan
    // total kit), jadi FRAGMENTED praktis hilang. Klasifikasi tetap
    // dipertahankan supaya kegagalan terkonfirmasi (mis. satu sample
    // > largest block) terbaca jujur sebagai FRAGMENTED, bukan OOM.
    //
    // ps_malloc() = heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
    // MALLOC_CAP_8BIT), jadi capability yang benar untuk mengukur
    // adalah MALLOC_CAP_SPIRAM (bukan MALLOC_CAP_8BIT campur).
    // Dicetak di 6 titik: sebelum/sesudah unload, sebelum/sesudah
    // alokasi blok PCM, saat gagal, dan saat PSRAM STATUS diminta —
    // tidak per audio frame, hanya handful per KIT LOAD.

    constexpr bool DEBUG_PSRAM_HEAP = true;

    void printHeap(const char *tag, size_t requested = 0)
    {
        if constexpr (!DEBUG_PSRAM_HEAP)
        {
            (void)tag;
            (void)requested;
            return;
        }

        const size_t freeSpi =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largestSpi =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        const size_t freeInt =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        Serial0.print("[PSRAM] ");
        Serial0.print(tag);
        Serial0.print(" free=");
        Serial0.print(freeSpi / 1024);
        Serial0.print("KB largest=");
        Serial0.print(largestSpi / 1024);
        Serial0.print("KB");

        if (requested > 0)
        {
            Serial0.print(" requested=");
            Serial0.print(requested / 1024);
            Serial0.print("KB");
        }

        Serial0.print(" internalFree=");
        Serial0.print(freeInt / 1024);
        Serial0.println("KB");
    }

    // ==================================================
    // G25: hitung ringkasan kepemilikan PSRAM
    // ==================================================
    //
    // Dipakai printAllocFailure(), releaseAll(), dan printKitLayout()
    // supaya semua angka berasal dari SATU sumber: tabel slotOwnBuf[].

    struct PcmOwnership
    {
        uint8_t blocks;         // jumlah slot dengan blok sendiri
        uint32_t bytes;         // total byte PCM hidup
        uint8_t kitBlocks;      // blok milik slot kit (0..11)
        uint8_t runtimeBlocks;  // blok milik slot runtime (12..15)
    };

    PcmOwnership pcmOwnership()
    {
        PcmOwnership o = {0, 0, 0, 0};

        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (!slotOwnBuf[s])
            {
                continue;
            }

            o.blocks++;
            o.bytes += slotOwnBytes[s];

            if (s < PSRAM_KIT_SLOT_COUNT)
            {
                o.kitBlocks++;
            }
            else
            {
                o.runtimeBlocks++;
            }
        }

        return o;
    }

    // Klasifikasikan kegagalan alokasi: ruang terpecah vs ruang memang
    // kurang.
    //
    // G25: `requested` kini ukuran SATU sample yang gagal (bukan total
    // kit). `allocatedSoFar` = byte yang sudah teralokasi untuk kit baru
    // sebelum request ini gagal.
    void printAllocFailure(
        size_t requested,
        size_t totalPCMBytes,
        size_t allocatedSoFar
    )
    {
        const size_t freeSpi =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largestSpi =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        Serial0.print("[PSRAM] cause=");

        if (largestSpi < requested && freeSpi >= requested)
        {
            Serial0.println("FRAGMENTED");
            Serial0.print("[PSRAM] free total cukup, largest contiguous block ");
            Serial0.print(largestSpi / 1024);
            Serial0.print("KB < requested ");
            Serial0.print(requested / 1024);
            Serial0.print("KB (satu sample, bukan total kit)");
        }
        else
        {
            Serial0.println("OOM");
            Serial0.print("[PSRAM] free total ");
            Serial0.print(freeSpi / 1024);
            Serial0.print("KB < requested ");
            Serial0.print(requested / 1024);
            Serial0.println("KB");
        }

        Serial0.print("[PSRAM] kit total=");
        Serial0.print(totalPCMBytes / 1024);
        Serial0.print("KB allocated=");
        Serial0.print(allocatedSoFar / 1024);
        Serial0.println("KB (blok yang sudah jadi dibuang) ");

        if constexpr (DEBUG_PSRAM_HEAP)
        {
            const PcmOwnership o = pcmOwnership();

            Serial0.print("[PSRAM] audit ownedSlots=");
            Serial0.print(o.blocks);
            Serial0.print("/");
            Serial0.print(PSRAM_MAX_SAMPLES);
            Serial0.print(" kitBlocks=");
            Serial0.print(o.kitBlocks);
            Serial0.print(" runtimeBlocks=");
            Serial0.print(o.runtimeBlocks);
            Serial0.print(" pcmInPSRAM=");
            Serial0.print(o.bytes / 1024);
            Serial0.print("KB ownedRemaining=");

            // ownedRemaining == o.blocks: blok kit baru yang sudah
            // jadi SUDAH dilepas oleh releaseStaged() dan belum
            // pernah masuk tabel, jadi tabel harus 0 entry.
            Serial0.println(o.blocks);
        }
    }

    // ==================================================
    // G25: DEBUG LAYOUT PSRAM
    // ==================================================
    //
    // Host test tidak bisa membuktikan isi free-list sungguhan, jadi
    // firmware wajib melaporkan kebenaran runtime-nya sendiri:
    // jumlah sample ter-load, total PCM + jumlah blok, jumlah slot /
    // owned buffer, free PSRAM + largest contiguous block, mapping
    // pad -> slot, dan hasil SETIAP alokasi blok (OK / GAGAL).
    // Semua di control plane, handful baris per KIT LOAD, tidak
    // pernah per audio frame.
    //
    // clang-format off
    constexpr bool DEBUG_PSRAM_LAYOUT = true;
    // clang-format on

    // Hasil satu permintaan blok PCM.
    void printAllocStep(
        uint8_t order,
        uint8_t slot,
        const char *path,
        size_t requested,
        bool ok
    )
    {
        if constexpr (!DEBUG_PSRAM_LAYOUT)
        {
            (void)order; (void)slot; (void)path;
            (void)requested; (void)ok;
            return;
        }

        Serial0.print("[PSRAM] alloc #");
        Serial0.print(order);
        Serial0.print(" slot=");
        Serial0.print(slot);
        Serial0.print(" req=");
        Serial0.print(requested / 1024);
        Serial0.print("KB -> ");

        if (ok)
        {
            Serial0.print("OK ");
        }
        else
        {
            Serial0.print("FAILED ");
        }

        Serial0.print(path ? path : "(unassigned)");
        Serial0.println();
    }

    // Ringkasan layout setelah kit di-commit (atau setelah gagal —
    // supaya kegagalan terbaca dari satu blok log).
    void printKitLayout(
        const char *phase,
        const char * const samplePaths[],
        uint8_t count,
        size_t totalPCMBytes
    )
    {
        if constexpr (!DEBUG_PSRAM_LAYOUT)
        {
            (void)phase; (void)samplePaths; (void)count;
            (void)totalPCMBytes;
            return;
        }

        const PcmOwnership o = pcmOwnership();

        uint8_t loaded = 0;      // sample valid (termasuk alias)
        uint8_t unique = 0;      // blok PCM dialokasikan
        uint32_t uniqueBytes = 0;

        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (!samples[s].valid)
            {
                continue;
            }

            loaded++;

            if (slotOwnBuf[s])
            {
                unique++;
                uniqueBytes += slotOwnBytes[s];
            }
        }

        const size_t freeSpi =
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largestSpi =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        Serial0.print("[PSRAM] layout ");
        Serial0.print(phase);
        Serial0.print(": samplesLoaded=");
        Serial0.print(loaded);
        Serial0.print("/");
        Serial0.print(count);
        Serial0.print(" blocks=");
        Serial0.print(unique);
        Serial0.print(" ownedSlots=");
        Serial0.print(o.blocks);
        Serial0.print("/");
        Serial0.print(PSRAM_MAX_SAMPLES);
        Serial0.print(" totalPCM=");
        Serial0.print(totalPCMBytes / 1024);
        Serial0.print("KB uniquePCM=");
        Serial0.print(uniqueBytes / 1024);
        Serial0.print("KB pcmInPSRAM=");
        Serial0.print(o.bytes / 1024);
        Serial0.print("KB free=");
        Serial0.print(freeSpi / 1024);
        Serial0.print("KB largest=");
        Serial0.print(largestSpi / 1024);
        Serial0.println("KB");

        for (uint8_t p = 0; p < PAD_COUNT; p++)
        {
            const uint8_t s = slotForPad[p];

            Serial0.print("[PSRAM] pad");
            Serial0.print(p + 1);
            Serial0.print(" -> slot ");

            if (s >= PSRAM_MAX_SAMPLES)
            {
                Serial0.println("NONE");
                continue;
            }

            Serial0.print(s);
            Serial0.print(" ");
            Serial0.print(slotOwnBytes[s] / 1024);
            Serial0.print("KB ");

            if (s < count && samplePaths && samplePaths[s])
            {
                const char *base = strrchr(samplePaths[s], '/');
                Serial0.print(base ? base + 1 : samplePaths[s]);
            }
            else
            {
                Serial0.print("INVALID");
            }

            Serial0.println();
        }
    }

    // Pasang registry dari daftar path kit (dipanggil di commit
    // psramLoadKit). Slot tidak valid (unassigned / file hilang /
    // WAV rusak) sengaja dibiarkan kosong supaya bisa dipakai file
    // runtime nanti.
    void commitKitRegistry(
        const char * const samplePaths[],
        uint8_t count
    )
    {
        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (s < count &&
                samples[s].valid &&
                samplePaths[s] &&
                samplePaths[s][0] != '\0')
            {
                snprintf(
                    slotPath[s],
                    sizeof(slotPath[s]),
                    "%s",
                    samplePaths[s]
                );
            }
            else
            {
                slotPath[s][0] = '\0';
            }
        }
    }

    // Slot PSRAM pertama yang belum dipakai file apa pun (slot kit
    // kosong diprioritaskan supaya kit yang semula penuh bisa
    // menampung file baru tanpa menambah alokasi).
    // PSRAM_SLOT_NONE bila registry penuh.
    uint8_t firstFreeRegistrySlot()
    {
        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (slotPath[s][0] == '\0')
            {
                return s;
            }
        }

        return PSRAM_SLOT_NONE;
    }

    // Jumlah slot yang punya blok PCM sendiri. Dipakai audit leak:
    // setelah releaseAll() HARUS 0; setelah satu KIT LOAD harus ==
    // jumlah sample unik yang valid.
    uint8_t ownedSlotCount()
    {
        return pcmOwnership().blocks;
    }

    // ==================================================
    // G25: pembatalan commit — lepas blok PCM yang SUDAH
    // dialokasikan untuk kit baru tapi belum jadi milik slot.
    // ==================================================
    //
    // Blok staging hidup di stack psramLoadKit dan BELUM pernah masuk
    // tabel slotOwnBuf[], jadi ownership-nya belum terlihat dari luar.
    // Kalau satu blok gagal dialokasikan (atau satu WAV gagal dibaca
    // dari SD), SEMUA blok staging harus dilepas — kalau tidak, PSRAM
    // bocor setiap kali KIT LOAD gagal.
    //
    // Ini SATU-SATUNYA free point untuk blok staging (dipanggil dari
    // failure path alokasi DAN baca SD). Blok yang sudah di-commit
    // tidak pernah lewat sini. Di-iterate per SLOT (bukan per indeks
    // alokasi) supaya blok yang sama tak mungkin dibebaskan dua kali.
    void releaseStaged(uint8_t *stagedBuf[])
    {
        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (stagedBuf[s])
            {
                free(stagedBuf[s]);
                stagedBuf[s] = nullptr;
            }
        }
    }

    // Urutkan slot menurut ukuran DESCENDING (insertion sort pada
    // array kecil; control plane, non-realtime).
    //
    // MENGAPA descending: permintaan terbesar dilayani saat free-list
    // masih punya blok terbesar — mengurangi hole besar tak terpakai,
    // sehingga blok kecil setelahnya lebih mudah muat. Maks 16 elemen,
    // O(n^2) sekali (~120 perbandingan) tidak terasa.
    void sortSlotsBySizeDesc(
        uint8_t *order,
        uint8_t count,
        const uint32_t *sizes
    )
    {
        for (uint8_t i = 1; i < count; i++)
        {
            const uint8_t key = order[i];

            int8_t j = (int8_t)i - 1;

            while (j >= 0 &&
                   sizes[order[j]] < sizes[key])
            {
                order[j + 1] = order[j];
                j--;
            }

            order[j + 1] = key;
        }
    }

    // ==================================================
    // G23: SATU-SATUNYA pemilik pelepasan resource PSRAM
    // ==================================================
    //
    // SEBELUMNYA pelepasan dipecah dan bergantung invariant IMPLISIT:
    // psramUnloadKit() membebaskan psramBuffer, dan slotOwnBuf G21 ikut
    // ter-free "karena kebetulan dipanggil dari psramLoadKit() tepat
    // sebelum alokasi besar". Tidak ada satu fungsi pun yang menyatakan
    // "ini semua resource kit" — pemanggil unload lain bisa membebaskan
    // blok kit tapi menyisakan slot runtime G21 (atau sebaliknya).
    //
    // SEKARANG (G25) SATU fungsi memiliki SELURUH resource:
    //   1. slotOwnBuf[0..PSRAM_MAX_SAMPLES-1] — SETIAP blok PCM di
    //      PSRAM, baik blok kit (0..11) maupun runtime G21 (12..15).
    //      Tidak ada blok PCM di luar tabel ini.
    //   2. slotPath[], slotForPad[], samples[], sampleCount,
    //      kitLoaded, kitHasPcm, psramTotalAllocated
    //
    // Slot ALIAS tidak punya blok sendiri (slotOwnBuf-nya null), jadi
    // blok asli hanya dibebaskan satu kali — tanpa double free.
    //
    // PRECONDITION: hanya dipanggil dari jalur yang SUDAH voice-drained
    // (performKitSwitch: hit gate off lalu tunggu
    // audioActiveVoiceCount() == 0). Tidak ada delay; tidak ada free
    // buffer yang sedang dibaca voice.

    void releaseAll(const char *why)
    {
        printHeap("before unload");

        // (1) SELURUH blok PCM milik slot 0..15 (bukan hanya runtime).
        //     free() di bawah = SATU-SATUNYA tempat slotOwnBuf dilepas.
        const PcmOwnership before = pcmOwnership();

        size_t freedRuntime = 0;
        size_t freedKit = 0;
        uint8_t freedSlots = 0;

        for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
        {
            if (slotOwnBuf[s])
            {
                if (s < PSRAM_KIT_SLOT_COUNT)
                {
                    freedKit += slotOwnBytes[s];
                }
                else
                {
                    freedRuntime += slotOwnBytes[s];
                }

                freedSlots++;
                free(slotOwnBuf[s]);
                slotOwnBuf[s] = nullptr;
                slotOwnBytes[s] = 0;
            }
        }

        psramTotalAllocated = 0;
        kitHasPcm = false;
        sampleCount = 0;
        kitLoaded = false;

        // (2) Metadata + registry. samples[] dinolkan SETELAH semua free
        //     supaya tak ada iterasi yang melihat memori ter-free.
        initSlotForPadIdentity();
        clearSampleRegistry();

        for (uint8_t i = 0; i < PSRAM_MAX_SAMPLES; i++)
        {
            samples[i] = SampleInfo{};
        }

        printHeap("after unload");

        if constexpr (DEBUG_PSRAM_HEAP)
        {
            Serial0.print("[PSRAM] released (");
            Serial0.print(why);
            Serial0.print(") blocks=");
            Serial0.print(freedSlots);
            Serial0.print(" kit=");
            Serial0.print(freedKit / 1024);
            Serial0.print("KB (");
            Serial0.print(before.kitBlocks);
            Serial0.print(" slot) runtime=");
            Serial0.print(freedRuntime / 1024);
            Serial0.print("KB (");
            Serial0.print(before.runtimeBlocks);
            Serial0.print(" slot) ownedRemaining=");
            Serial0.println(ownedSlotCount());
        }

        Serial0.println("Kit unloaded from PSRAM");
    }

    // ==================================================
    // Build Pad → Slot Registry — SATU-SATUNYA aturan pemetaan
    // ==================================================
    //
    // slotForPad[p] = physical PSRAM slot yang memuat file untuk pad p,
    // diturunkan dari sampleIds. Source slot x valid → pad p dipetakan
    // ke slot x; invalid → fallback ke slot p (backward-compat).
    // Duplikat source natural: dua pad memakai slot fisik yang SAMA
    // (pointer berbagi karena alias / path identik), tanpa duplikasi
    // PSRAM.
    //
    // Dipanggil psramLoadKit() (commit) DAN psramRebuildSlotRegistry()
    // (PAD_SET sampleId, G8-B) — aturan validasi & fallback tidak pernah
    // menyimpang. Control plane non-realtime; entry uint8 atomik.
    // ==================================================

    void buildSlotRegistry(const uint8_t *ids, uint8_t count)
    {
        for (uint8_t p = 0; p < count && p < PAD_COUNT; p++)
        {
            uint8_t srcSlot = ids[p];

            if (srcSlot < PAD_COUNT && srcSlot < sampleCount &&
                samples[srcSlot].valid)
            {
                slotForPad[p] = srcSlot;
            }
            else
            {
                slotForPad[p] = p;   // fallback: slot identitas pad
            }
        }
    }

    // ==================================================
    // SD -> PSRAM LOADER (benchmark + chunked read)
    // ==================================================
    //
    // Benchmark timing loading WAV (Pass 1 / Pass 3 / total).
    // DEBUG_SD_LOAD=false mematikan seluruh log timing (dan kode
    // penghitungannya) bila Serial0 jadi bottleneck.
    constexpr bool DEBUG_SD_LOAD = true;

    // Read buffer TUNGGAL (reusable, internal RAM -> DMA-friendly untuk
    // driver SDSPI; menghindari bounce lambat saat target langsung ke
    // PSRAM). Satu buffer untuk semua WAV, bukan per-WAV. Internal RAM
    // ~32 KB pada ESP32-S3 aman.
    constexpr size_t SD_READ_BUFFER_SIZE = 32 * 1024;
    uint8_t sdReadBuffer[SD_READ_BUFFER_SIZE] = {};

    // ==================================================
    // WAV Header Parser
    // ==================================================
    //
    // Mencari "data" chunk secara robust (beberapa WAV punya chunk
    // tambahan di antara "fmt " dan "data").

    struct WavHeader
    {
        uint32_t dataSize;
        uint32_t dataOffset;
        uint16_t audioFormat;
        uint16_t channels;
        uint32_t sampleRate;
        uint16_t bitsPerSample;
        bool valid;
    };

    WavHeader parseWavHeader(File &f)
    {
        WavHeader hdr = {};
        hdr.valid = false;

        // Baca 128 byte pertama (cukup untuk header)
        uint8_t header[128];

        size_t bytesRead = f.read(header, sizeof(header));

        if (bytesRead < 44)
        {
            return hdr;
        }

        // Cek "RIFF" + "WAVE"
        if (header[0] != 'R' || header[1] != 'I' ||
            header[2] != 'F' || header[3] != 'F')
        {
            return hdr;
        }

        if (header[8] != 'W' || header[9] != 'A' ||
            header[10] != 'V' || header[11] != 'E')
        {
            return hdr;
        }

        // Parse fmt chunk
        if (header[12] == 'f' && header[13] == 'm' &&
            header[14] == 't' && header[15] == ' ')
        {
            hdr.audioFormat =
                header[20] | (header[21] << 8);
            hdr.channels =
                header[22] | (header[23] << 8);
            hdr.sampleRate =
                header[24] | (header[25] << 8) |
                (header[26] << 16) | (header[27] << 24);
            hdr.bitsPerSample =
                header[34] | (header[35] << 8);
        }
        else
        {
            return hdr;
        }

        // Scan untuk "data" chunk
        for (uint16_t i = 12; i < 120; i++)
        {
            if (header[i]     == 'd' && header[i + 1] == 'a' &&
                header[i + 2] == 't' && header[i + 3] == 'a')
            {
                hdr.dataOffset = i + 8;
                hdr.dataSize =
                    header[i + 4] |
                    (header[i + 5] << 8) |
                    (header[i + 6] << 16) |
                    (header[i + 7] << 24);
                hdr.valid = true;
                break;
            }
        }

        return hdr;
    }
}

// ======================================================
// Public: Init PSRAM
// ======================================================

bool psramSetup()
{
    Serial0.println("=== PSRAM INIT ===");

    size_t psramSize = ESP.getPsramSize();

    if (psramSize == 0)
    {
        Serial0.println("PSRAM NOT FOUND!");
        Serial0.println("Firmware needs PSRAM for low latency.");
        return false;
    }

    Serial0.print("PSRAM Size: ");
    Serial0.print(psramSize / 1024);
    Serial0.println(" KB");

    Serial0.println("=== PSRAM READY ===");

    return true;
}

// ======================================================
// Public: Load Kit
// ======================================================
//
// G25: KIT LOAD TIDAK LAGI BUTUH SATU BLOK KONTIGU ~5.5 MB.
//   SEBELUM : validate → free semua → ps_malloc(totalPCM) → baca
//             semua → commit (satu permintaan sebesar SELURUH kit).
//   SEKARANG: validate → free semua → ps_malloc per sample (blok per
//             sample unik, urutan size DESC) → baca semua → commit
//             (permintaan terbesar = satu sample).
// Free-list yang terpecah tidak lagi menggagalkan KIT LOAD: ruang
// cukup selama free TOTAL cukup.
//
// Sifat lain yang DIPERTAHANKAN persis:
//   - audio gate + voice drain tetap di pemanggil (performKitSwitch)
//   - free-all sebelum alokasi (bukan double buffer: 2 kit = ~10.5 MB
//     > 8 MB PSRAM)
//   - tidak ada kit setengah jadi: commit hanya kalau SEMUA WAV
//     terbaca; kalau gagal, semua blok staging dilepas dan state
//     dikosongkan
//   - alias (path duplikat) tetap berbagi satu blok

bool psramLoadKit(
    const char * const samplePaths[],
    const uint8_t sampleIds[],
    uint8_t count
)
{
    // PRECONDITION (dijamin caller / performKitSwitch):
    //   - hit gate OFF, active voice == 0, tidak ada voice yang
    //     membaca blok PCM lama.
    // Blok PCM lama AMAN dilepas (menghindari "kit lama + kit baru"
    // bersamaan di PSRAM). Fungsi ini tidak mengubah trigger/audio
    // timing.

    Serial0.println("=== LOADING KIT TO PSRAM ===");

    uint32_t totalStartUs = 0;
    uint32_t pass1StartUs = 0;
    uint32_t pass1Ms = 0;

    if constexpr (DEBUG_SD_LOAD)
    {
        totalStartUs = micros();
        pass1StartUs = micros();
    }

    if (count > PSRAM_MAX_SAMPLES)
    {
        count = PSRAM_MAX_SAMPLES;
    }

    // -----------------------------------------------------------
    // Reset slot registry ke identitas (backward-compat) SEBELUM kit
    // switch dimulai. Bila load gagal, state ini tetap aman. Diperbarui
    // di commit bila kit berhasil dimuat (khususnya bila sampleId pad
    // != padIndex karena remapping).
    // -----------------------------------------------------------

    initSlotForPadIdentity();

    // Staging metadata kit baru — di-commit ke samples[] hanya bila
    // SELURUH WAV berhasil dimuat (tidak pernah kit setengah jadi).
    SampleInfo newSamples[PSRAM_MAX_SAMPLES] = {};

    // ----------------------------------------------
    // Pass 1: hitung total size & validasi.
    // Duplicate path detection: path sama -> reuse sample
    // yang sudah di-load.
    // ----------------------------------------------

    uint32_t totalPCMBytes = 0;
    uint32_t headerOffsets[PSRAM_MAX_SAMPLES];
    uint32_t fileSizes[PSRAM_MAX_SAMPLES];
    int8_t aliasOf[PSRAM_MAX_SAMPLES];
    memset(aliasOf, -1, sizeof(aliasOf));

    for (uint8_t i = 0; i < count; i++)
    {
        // Cek duplicate: sudah load path yang sama?
        for (uint8_t j = 0; j < i; j++)
        {
            if (strcmp(samplePaths[i], samplePaths[j]) == 0)
            {
                aliasOf[i] = j;
                Serial0.print("ALIAS: ");
                Serial0.print(samplePaths[i]);
                Serial0.print(" -> slot [");
                Serial0.print(j);
                Serial0.println("]");
                break;
            }
        }

        if (aliasOf[i] >= 0)
        {
            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        // Path kosong = pad UNASSIGNED (senyap). Bukan error: slot
        // sengaja tanpa sample, tidak menambah totalPCMBytes.
        if (!samplePaths[i] || samplePaths[i][0] == '\0')
        {
            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        File f = SD.open(samplePaths[i], FILE_READ);

        if (!f)
        {
            Serial0.print("NOT FOUND: ");
            Serial0.println(samplePaths[i]);

            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        WavHeader hdr = parseWavHeader(f);

        if (!hdr.valid || hdr.audioFormat != 1)
        {
            Serial0.print("INVALID WAV: ");
            Serial0.println(samplePaths[i]);
            f.close();
            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        if (hdr.bitsPerSample != 16)
        {
            Serial0.print("NOT 16-BIT: ");
            Serial0.println(samplePaths[i]);
            f.close();
            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        if (hdr.sampleRate != 48000)
        {
            Serial0.print("WARN SR: ");
            Serial0.print(samplePaths[i]);
            Serial0.print(" is ");
            Serial0.print(hdr.sampleRate);
            Serial0.print("Hz, expected 48000Hz\n");
        }

        if (hdr.channels != 1 && hdr.channels != 2)
        {
            Serial0.print("INVALID CH: ");
            Serial0.print(samplePaths[i]);
            Serial0.print(" ch=");
            Serial0.println(hdr.channels);
            f.close();
            newSamples[i].valid = false;
            headerOffsets[i] = 0;
            fileSizes[i] = 0;
            continue;
        }

        headerOffsets[i] = hdr.dataOffset;
        fileSizes[i] = f.size();
        totalPCMBytes += hdr.dataSize;

        newSamples[i].sampleRate = hdr.sampleRate;
        newSamples[i].channels = hdr.channels;
        newSamples[i].bitsPerSample = hdr.bitsPerSample;
        newSamples[i].dataSize = hdr.dataSize;
        newSamples[i].valid = true;

        Serial0.print("OK: ");
        Serial0.print(samplePaths[i]);
        Serial0.print(" ch=");
        Serial0.print(hdr.channels);
        Serial0.print(" rate=");
        Serial0.print(hdr.sampleRate);
        Serial0.print(" bits=");
        Serial0.print(hdr.bitsPerSample);
        Serial0.print(" PCM=");
        Serial0.print(hdr.dataSize);
        Serial0.println(" bytes");

        f.close();
    }

    if constexpr (DEBUG_SD_LOAD)
    {
        pass1Ms = (micros() - pass1StartUs) / 1000u;
    }

    if (totalPCMBytes == 0)
    {
        // Kit tanpa PCM (semua pad unassigned): commit kit EMPTY.
        // Seluruh slot invalid (senyap); psramGetData()=nullptr ->
        // startVoice bails aman. Kit "tanpa suara" = state valid.
        Serial0.println("No PCM (kit senyap/unassigned) -> commit EMPTY.");

        // G23: void, tapi tetap satu-satunya release point.
        releaseAll("empty-kit");

        sampleCount = count;
        kitLoaded = true;
        kitHasPcm = false;   // tidak ada blok PCM di PSRAM

        memcpy(samples, newSamples, sizeof(samples));
        buildSlotRegistry(sampleIds, count);

        // Registry G21: kit senyap -> semua slot kosong (tidak ada file
        // untuk PAD -> SOUND tanpa load baru).
        clearSampleRegistry();

        printKitLayout("empty", samplePaths, count, 0);

        Serial0.println("=== LOADING KIT TO PSRAM (empty) ===");
        return true;
    }

    // ----------------------------------------------
    // Pass 2 (G25): Lepas SELURUH resource kit lama (SEMUA blok
    // PCM per-slot: blok kit 0..11 + slot runtime G21 12..15),
    // BARU alokasi blok PCM PER SAMPLE untuk kit baru.
    //
    // Tetap free-all-sebelum-alloc (bukan double buffer: 2 kit =
    // ~10.5 MB > 8 MB PSRAM). Konsekuensinya: kalau alokasi gagal,
    // kit lama SUDAH tidak ada dan pemanggil (performKitSwitch) wajib
    // memuat ulang kit lama — lihat blok rollback di uart_protocol.cpp.
    //
    // BLOK PER SAMPLE karena inilah akar masalah G25: free-list PSRAM
    // terpecah (satu permintaan 5453 KB butuh satu blok kontigu 5453
    // KB, padahal free total 8135 KB tersusun dari blok terbesar
    // 5119 KB). Dengan blok per sample, permintaan terbesar hanya
    // sebesar satu sample.
    // ----------------------------------------------

    Serial0.print("Total PCM: ");
    Serial0.print(totalPCMBytes / 1024);
    Serial0.println(" KB");

    // G23: release point tunggal, mencakup SELURUH blok PCM.
    releaseAll("pre-alloc");

    // G23: ukur heap SETELAH semua resource kit lama benar-benar
    // bebas. Inilah angka yang membedakan OOM dari FRAGMENTED.
    printHeap("before kit alloc", totalPCMBytes);

    // Kumpulkan slot yang butuh blok (unik saja; alias berbagi
    // blok dengan slot pemiliknya lalu).
    uint8_t  allocOrder[PSRAM_MAX_SAMPLES];
    uint8_t  allocCount = 0;
    uint8_t *stagedBuf[PSRAM_MAX_SAMPLES] = {};
    uint32_t stagedBytes[PSRAM_MAX_SAMPLES] = {};

    for (uint8_t i = 0; i < count; i++)
    {
        if (aliasOf[i] >= 0 || !newSamples[i].valid)
        {
            continue;
        }

        stagedBytes[i] = newSamples[i].dataSize;
        allocOrder[allocCount++] = i;
    }

    // Ukuran terbesar didahulukan: permintaan besar dilayani selagi
    // free-list masih punya hole terbesar.
    sortSlotsBySizeDesc(allocOrder, allocCount, stagedBytes);

    size_t allocatedSoFar = 0;
    uint8_t blocksMade = 0;

    for (uint8_t k = 0; k < allocCount; k++)
    {
        const uint8_t s = allocOrder[k];
        const size_t req = stagedBytes[s];

        uint8_t *buf = (uint8_t *)ps_malloc(req);

        printAllocStep(k, s, samplePaths[s], req, buf != nullptr);

        if (!buf)
        {
            Serial0.println("PSRAM ALLOC FAILED!");
            Serial0.println("PSRAM kit kosong (state konsisten).");

            // Buang SEMUA blok staging kit ini supaya tidak bocor,
            // lalu kosongkan state.
            releaseStaged(stagedBuf);

            printHeap("kit alloc FAILED", totalPCMBytes);
            printAllocFailure(req, totalPCMBytes, allocatedSoFar);

            return false;
        }

        stagedBuf[s] = buf;
        allocatedSoFar += req;
        blocksMade++;
    }

    printHeap("kit alloc OK", totalPCMBytes);

    psramTotalAllocated = allocatedSoFar;
    kitHasPcm = true;

    if constexpr (DEBUG_PSRAM_LAYOUT)
    {
        Serial0.print("[PSRAM] blocks allocated=");
        Serial0.print(blocksMade);
        Serial0.print(" bytes=");
        Serial0.print(allocatedSoFar / 1024);
        Serial0.print("KB (largest request=");

        // Permintaan terbesar = sample terbesar.
        size_t biggestReq = 0;

        for (uint8_t k = 0; k < allocCount; k++)
        {
            if (stagedBytes[allocOrder[k]] > biggestReq)
            {
                biggestReq = stagedBytes[allocOrder[k]];
            }
        }

        Serial0.print(biggestReq / 1024);
        Serial0.println("KB, bukan total kit)");
    }

    uint32_t pass3StartUs = 0;

    if constexpr (DEBUG_SD_LOAD)
    {
        pass3StartUs = micros();
    }

    // ----------------------------------------------
    // Pass 3: baca PCM ke blok per-sample (chunked, DMA-friendly).
    // Satu WAV gagal dibaca -> allLoaded=false -> TIDAK ada commit,
    // semua blok staging dibuang.
    // ----------------------------------------------

    bool allLoaded = true;

    for (uint8_t i = 0; i < count; i++)
    {
        // Alias: reuse pointer dari original (satu blok, dua slot)
        if (aliasOf[i] >= 0)
        {
            newSamples[i] = newSamples[aliasOf[i]];
            continue;
        }

        if (!newSamples[i].valid)
        {
            newSamples[i].data = nullptr;
            continue;
        }

        File f = SD.open(samplePaths[i], FILE_READ);

        if (!f)
        {
            Serial0.print("NOT FOUND: ");
            Serial0.println(samplePaths[i]);
            newSamples[i].data = nullptr;
            newSamples[i].valid = false;
            allLoaded = false;
            continue;
        }

        // Skip header
        if (!f.seek(headerOffsets[i]))
        {
            Serial0.print("SEEK FAIL: ");
            Serial0.println(samplePaths[i]);
            f.close();
            newSamples[i].data = nullptr;
            newSamples[i].valid = false;
            allLoaded = false;
            continue;
        }

        uint32_t wavStartUs = 0;

        if constexpr (DEBUG_SD_LOAD)
        {
            wavStartUs = micros();
        }

        // Baca PCM ber-chunk: SD -> sdReadBuffer (internal RAM,
        // DMA-friendly untuk driver SDSPI) -> memcpy ke blok PSRAM
        // milik slot ini. Satu reusable buffer, bukan per-WAV. Format
        // tetap raw PCM 16-bit — tidak ada transformasi audio.
        uint8_t *writePtr = stagedBuf[i];
        size_t remainingBytes = newSamples[i].dataSize;
        bool wavOk = true;

        while (remainingBytes > 0)
        {
            const size_t chunk =
                (remainingBytes < SD_READ_BUFFER_SIZE)
                    ? remainingBytes
                    : SD_READ_BUFFER_SIZE;

            const size_t n = f.read(
                sdReadBuffer,
                chunk
            );

            if (n != chunk)
            {
                f.close();

                Serial0.print("READ INCOMPLETE: ");
                Serial0.println(samplePaths[i]);
                newSamples[i].data = nullptr;
                newSamples[i].valid = false;
                allLoaded = false;
                wavOk = false;
                break;
            }

            memcpy(writePtr, sdReadBuffer, chunk);

            writePtr += chunk;
            remainingBytes -= chunk;
        }

        if (wavOk)
        {
            f.close();

            newSamples[i].data = stagedBuf[i];
        }

        if constexpr (DEBUG_SD_LOAD)
        {
            if (wavOk)
            {
                const uint32_t wavMs =
                    (micros() - wavStartUs) / 1000u;
                const uint32_t wavKB =
                    newSamples[i].dataSize / 1024u;

                Serial0.print("LOADED: ");
                Serial0.print(samplePaths[i]);
                Serial0.print(": ");
                Serial0.print(wavMs);
                Serial0.print(" ms (");
                Serial0.print(wavKB);
                Serial0.println(" KB)");
            }
        }
    }

    uint32_t pass3Ms = 0;
    uint32_t totalMs = 0;

    if constexpr (DEBUG_SD_LOAD)
    {
        pass3Ms = (micros() - pass3StartUs) / 1000u;
        totalMs = (micros() - totalStartUs) / 1000u;

        Serial0.println("--- SD LOAD TIMING ---");
        Serial0.print("PASS1: ");
        Serial0.println(pass1Ms);
        Serial0.print("PASS3: ");
        Serial0.println(pass3Ms);
        Serial0.print("TOTAL: ");
        Serial0.println(totalMs);
    }

    // ----------------------------------------------
    // Commit / Fail-state. Tidak pernah ada kit setengah jadi:
    //   - semua sample sukses -> commit kit baru
    //   - satu saja gagal     -> state dikosongkan, konsisten
    // ----------------------------------------------

    if (!allLoaded)
    {
        Serial0.println("WAV LOAD FAILED (incomplete)!");
        Serial0.println("Pembatalan commit; state PSRAM dikosongkan.");

        // Blok PCM kit lama sudah dibebaskan di Pass 2 dan samples[]
        // sudah dikosongkan (tidak ada pointer ke memori ter-free).
        //
        // G25: blok kit BARU masih hidup di staging (belum masuk tabel
        // slotOwnBuf), jadi dilepas lewat releaseStaged() — satu-satunya
        // free point untuk blok staging. Setelah itu state direset
        // (kitHasPcm=false, psramTotalAllocated=0, sampleCount=0,
        // kitLoaded=false).
        //
        // slotOwnBuf[] sudah kosong (releaseAll di Pass 2) dan TIDAK
        // pernah disentuh di jalur ini — tanpa ownership leak maupun
        // pointer stale.
        releaseStaged(stagedBuf);

        psramTotalAllocated = 0;
        kitHasPcm = false;
        sampleCount = 0;
        kitLoaded = false;

        // Registry tetap identitas (backward-compat). Bila PSRAM kosong,
        // psramGetData()=nullptr -> startVoice bails aman.
        initSlotForPadIdentity();

        printKitLayout("FAILED", samplePaths, count, 0);

        return false;
    }

    // ==============================================
    // Commit: blok PCM di-finalisasi ke tabel kepemilikan.
    // ==============================================
    //
    // Sampai titik ini stagedBuf[] TIDAK terlihat dari luar (belum di
    // slotOwnBuf), jadi tidak ada voice/pemanggil lain yang bisa
    // membaca PCM yang belum final. Sekarang tiap blok dipindahkan ke
    // slotOwnBuf[slot] + slotOwnBytes.
    //
    // Slot ALIAS TIDAK disentuh: pointer-nya sudah berbagi dgn slot
    // pemilik (Pass 3) dan slotOwnBuf-nya tetap nullptr, jadi blok
    // asli hanya dibebaskan satu kali oleh releaseAll().
    //
    // releaseAll() sudah dipanggil di Pass 2, jadi SELURUH tabel
    // slotOwnBuf dijamin kosong di sini — tanpa overwrite blok yang
    // masih hidup (anti leak-by-overwrite, G23).

    for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)
    {
        if (stagedBuf[s])
        {
            slotOwnBuf[s] = stagedBuf[s];
            slotOwnBytes[s] = stagedBytes[s];
            stagedBuf[s] = nullptr;   // ownership berpindah
        }
    }

    // Semua sample sukses: commit metadata kit baru.
    sampleCount = count;
    kitLoaded = true;

    memcpy(samples, newSamples, sizeof(samples));

    // Bangun registry pad->slot dari sampleIds kit baru (aturan tunggal
    // buildSlotRegistry — lihat komentar helper di atas).
    buildSlotRegistry(sampleIds, count);

    // Bangun registry G21 (path file -> slot) dari path kit baru, supaya
    // PAD -> SOUND berikutnya hanya lookup memori (file yang sudah
    // dimuat tidak dibaca ulang dari SD).
    commitKitRegistry(samplePaths, count);

    printKitLayout("OK", samplePaths, count, totalPCMBytes);

    Serial0.print("Kit loaded: ");
    Serial0.print(totalPCMBytes / 1024);
    Serial0.print(" KB into PSRAM (");
    Serial0.print(psramTotalAllocated / 1024);
    Serial0.print(" KB total, ");
    Serial0.print(ownedSlotCount());
    Serial0.println(" blocks)");
    Serial0.println("============================");

    return true;
}

// ======================================================
// Public: Unload Kit
// ======================================================

void psramUnloadKit()
{
    // G23 + G25: seluruh resource PSRAM dilepas di sini — SETIAP blok
    // PCM per-slot (kit DAN runtime G21). Lihat releaseAll() untuk
    // daftar lengkap dan precondition voice-drained.
    releaseAll("unload");
}

// ======================================================
// Public: Data Access
// ======================================================

const uint8_t *psramGetData(uint8_t index)
{
    if (index >= sampleCount) return nullptr;
    if (!samples[index].valid) return nullptr;
    return samples[index].data;
}

uint32_t psramGetDataSize(uint8_t index)
{
    if (index >= sampleCount) return 0;
    if (!samples[index].valid) return 0;
    return samples[index].dataSize;
}

const SampleInfo &psramGetInfo(uint8_t index)
{
    return samples[index];
}

bool psramIsLoaded()
{
    return kitLoaded;
}

// ======================================================
// Public: Pad → Slot Registry
// ======================================================
//
// Lookup O(1), memory-only. Dibangun di control plane (saat kit
// commit), hanya dibaca AudioTask. Default identitas (backward-compat).

uint8_t psramSlotForPad(uint8_t padIndex)
{
    if (padIndex >= PAD_COUNT)
    {
        return padIndex;      // out-of-range: fallback identitas
    }

    return slotForPad[padIndex];
}

// ======================================================
// Public: Rebuild Pad → Slot Registry (G8-B)
// ======================================================
//
// Remap langsung pada registry (tanpa unload/reload PSRAM) dari
// sampleId EFFECTIVE saat ini. Dipanggil dari control plane saat
// PAD_SET sampleId berhasil — aturan sama dgn commit kit load
// (buildSlotRegistry). Tanpa SD/malloc/blocking. Audio membaca
// slotForPad per-hit (Core 0); tiap entry uint8 ditulis atomik ->
// aman tanpa lock tambahan.
// ======================================================

void psramRebuildSlotRegistry(
    const uint8_t sampleIds[]
)
{
    // Registry hanya bermakna saat kit termuat; kalau belum, skip
    // (audio bails aman) — unload sudah mereset ke identitas.
    //
    // G25: sentinel "kit punya PCM di PSRAM" (sebelumnya `psramBuffer
    // != nullptr`). Yang dicek: apakah KIT LOAD terakhir menghasilkan
    // >= 1 blok PCM.
    if (!kitHasPcm || !kitLoaded)
    {
        return;
    }

    buildSlotRegistry(sampleIds, PAD_COUNT);
}

// ======================================================
// Public: Sample Registry — path → PSRAM slot (G21)
// ======================================================
//
// Control-plane lookup, memory-only (tanpa SD/malloc/file):
// menentukan apakah PAD -> SOUND cukup REMAP (file sudah termuat)
// atau perlu LOAD SATU FILE (file baru). Hanya slot yang PCM-nya
// benar-benar ada yang dikembalikan — pemanggil bisa langsung
// memakainya untuk playback.

uint8_t psramFindSlotByPath(const char *absPath)
{
    if (!absPath || absPath[0] == '\0')
    {
        return PSRAM_SLOT_NONE;
    }

    for (uint8_t s = 0;
         s < sampleCount && s < PSRAM_MAX_SAMPLES;
         s++)
    {
        if (samples[s].valid &&
            slotPath[s][0] != '\0' &&
            strcmp(slotPath[s], absPath) == 0)
        {
            return s;
        }
    }

    return PSRAM_SLOT_NONE;
}

// ======================================================
// Public: Set Pad → Slot runtime (G21)
// ======================================================
//
// Satu-satunya operasi realtime-remap: tulis SATU entri uint8 pada
// fixed array slotForPad (bentuk yang dibaca AudioTask via
// fireHit → psramSlotForPad). Store byte-aligned bersifat atomic di
// ESP32-S3 — AudioTask yang menembak pad bersamaan memakai slot LAMA
// atau BARU (keduanya valid & PCM termuat), tidak pernah
// setengah-updating dan tidak pernah membaca pointer yang belum
// ter-commit. Tanpa lock/SD/String/alokasi.
//
// Slot PSRAM_SLOT_NONE = pad senyap (startVoice menolak lewat bound
// check, jadi data sample lain tidak tersentuh).

void psramSetPadSlot(
    uint8_t padIndex,
    uint8_t slot
)
{
    if (padIndex >= PAD_COUNT)
    {
        return;   // no-op: jangan menulis di luar tabel
    }

    slotForPad[padIndex] = slot;
}

// ======================================================
// Public: Load SATU file baru ke slot kosong (G21)
// ======================================================
//
// Berbeda dari psramLoadKit(), fungsi ini TIDAK menyentuh sample
// yang sudah termuat: tidak membebaskan blok PCM slot lain, tidak
// me-rebase pointer PCM sample lain — hanya membaca file yang
// diminta, satu kali, chunked.
//
// G25: SELURUH slot PSRAM kini punya blok sendiri (tidak ada lagi
// "blok kit tunggal" yang harus dijaga utuh), jadi slot hasil memakai
// alokasi PSRAM independen seperti slot kit. Blok lain yang sedang
// dibaca voice tetap utuh — pemanggil tidak perlu menahan voice
// karena tidak ada pointer yang berubah lifetime di sini.
//
// Semua penolakan terjadi SEBELUM commit: file hilang, WAV tidak
// valid, PSRAM penuh, atau slot penuh → tidak ada state yang berubah
// (mapping pemanggil tetap aman).

uint8_t psramLoadSingleSample(
    const char *absPath
)
{
    if (!absPath || absPath[0] == '\0')
    {
        return PSRAM_SLOT_NONE;
    }

    // Idempoten: file sudah ada di PSRAM -> jangan baca SD lagi.
    const uint8_t existing = psramFindSlotByPath(absPath);

    if (existing != PSRAM_SLOT_NONE)
    {
        return existing;
    }

    // Registry hanya bermakna kalau ada kit aktif (slot kit sebagai
    // konteks). Tanpa kit, remap tidak punya tempat hidup.
    //
    // G25: `kitHasPcm` menggantikan sentinel `psramBuffer != nullptr`:
    // true hanya kalau KIT LOAD terakhir menghasilkan >= 1 blok PCM.
    // Kit senyap (semua pad unassigned) tetap menolak.
    if (!kitHasPcm || !kitLoaded)
    {
        return PSRAM_SLOT_NONE;
    }

    // G23: path lebih panjang dari slotPath[] akan terpotong snprintf,
    // sehingga psramFindSlotByPath() tak pernah cocok lagi -> tiap
    // PAD -> SOUND mengulang alokasi sampai registry penuh. Tolak di
    // gerbang (seperti file hilang / WAV rusak): tidak ada yang
    // di-commit.
    if (strlen(absPath) >= sizeof(slotPath[0]))
    {
        Serial0.print("[PSRAM] single load PATH TOO LONG (");
        Serial0.print(strlen(absPath));
        Serial0.print(" >= ");
        Serial0.print(sizeof(slotPath[0]));
        Serial0.print("): ");
        Serial0.println(absPath);
        return PSRAM_SLOT_NONE;
    }

    if (!sdIsReady())
    {
        Serial0.println("[PSRAM] single load: SD not ready");
        return PSRAM_SLOT_NONE;
    }

    const uint8_t slot = firstFreeRegistrySlot();

    if (slot == PSRAM_SLOT_NONE)
    {
        Serial0.println("[PSRAM] single load: registry full");
        return PSRAM_SLOT_NONE;
    }

    // G23: JARING pengaman ownership. firstFreeRegistrySlot() hanya
    // melihat slotPath[], jadi slot dgn slotOwnBuf != nullptr yang
    // slotPath-nya kosong akan TERTIMPA di commit dan buffer lamanya
    // hilang tanpa free (leak by overwrite).
    //
    // Buffer TIDAK dibebaskan di sini: jalur ini tanpa hit-gate /
    // voice-drain (lihat performSampleAssign), jadi free buffer yang
    // mungkin dibaca voice = dangling pointer ke AudioTask. Lebih
    // aman menolak; KIT LOAD (sudah voice-drained) yang membebaskan
    // semuanya lewat releaseAll().
    if (slotOwnBuf[slot])
    {
        Serial0.print("[PSRAM] single load: slot ");
        Serial0.print(slot);
        Serial0.println(" punya orphan buffer (tidak ditimpa)");
        return PSRAM_SLOT_NONE;
    }

    const uint32_t loadStartUs = micros();

    File f = SD.open(absPath, FILE_READ);

    if (!f)
    {
        Serial0.print("[PSRAM] single load NOT FOUND: ");
        Serial0.println(absPath);
        return PSRAM_SLOT_NONE;
    }

    const WavHeader hdr = parseWavHeader(f);

    if (!hdr.valid ||
        hdr.audioFormat != 1 ||
        hdr.bitsPerSample != 16 ||
        (hdr.channels != 1 && hdr.channels != 2) ||
        hdr.dataSize == 0 ||
        hdr.dataSize > PSRAM_MAX_SAMPLE_SIZE)
    {
        Serial0.print("[PSRAM] single load INVALID WAV: ");
        Serial0.println(absPath);
        f.close();
        return PSRAM_SLOT_NONE;
    }

    uint8_t *buf = (uint8_t *)ps_malloc(hdr.dataSize);

    if (!buf)
    {
        Serial0.println("[PSRAM] single load ALLOC FAILED");
        f.close();
        return PSRAM_SLOT_NONE;
    }

    if (!f.seek(hdr.dataOffset))
    {
        Serial0.print("[PSRAM] single load SEEK FAIL: ");
        Serial0.println(absPath);
        free(buf);
        f.close();
        return PSRAM_SLOT_NONE;
    }

    // SD -> sdReadBuffer (internal RAM, DMA-friendly) -> buf PSRAM.
    // Format identik dengan pass 3 psramLoadKit.
    uint8_t *writePtr = buf;
    size_t remaining = hdr.dataSize;
    bool readOk = true;

    while (remaining > 0)
    {
        const size_t chunk =
            (remaining < SD_READ_BUFFER_SIZE)
                ? remaining
                : SD_READ_BUFFER_SIZE;

        if (f.read(sdReadBuffer, chunk) != chunk)
        {
            Serial0.print("[PSRAM] single load READ INCOMPLETE: ");
            Serial0.println(absPath);
            readOk = false;
            break;
        }

        memcpy(writePtr, sdReadBuffer, chunk);

        writePtr += chunk;
        remaining -= chunk;
    }

    f.close();

    if (!readOk)
    {
        free(buf);       // belum ada yang di-commit: state utuh
        return PSRAM_SLOT_NONE;
    }

    // -----------------------------------------------------------
    // Commit. Satu titik, setelah seluruh PCM sudah di PSRAM.
    // -----------------------------------------------------------

    samples[slot].data          = buf;
    samples[slot].dataSize      = hdr.dataSize;
    samples[slot].sampleRate    = hdr.sampleRate;
    samples[slot].channels      = hdr.channels;
    samples[slot].bitsPerSample = hdr.bitsPerSample;
    samples[slot].valid         = true;

    slotOwnBuf[slot] = buf;

    snprintf(
        slotPath[slot],
        sizeof(slotPath[slot]),
        "%s",
        absPath
    );

    if (slot >= sampleCount)
    {
        sampleCount = (uint8_t)(slot + 1);
    }

    Serial0.print("[PSRAM] single load OK slot=");
    Serial0.print(slot);
    Serial0.print(" (");
    Serial0.print(hdr.dataSize / 1024);
    Serial0.print(" KB) elapsed=");
    Serial0.print((micros() - loadStartUs) / 1000u);
    Serial0.println(" ms");

    return slot;
}

void psramPrintStatus()
{
    Serial0.println("=== PSRAM STATUS ===");

    Serial0.print("Kit loaded: ");
    Serial0.println(kitLoaded ? "YES" : "NO");

    Serial0.print("Kit has PCM blocks: ");
    Serial0.println(kitHasPcm ? "YES" : "NO");

    Serial0.print("Samples: ");
    Serial0.println(sampleCount);

    // G25: ringkasan layout blok per-sample.
    const PcmOwnership o = pcmOwnership();

    printHeap("status");

    Serial0.print("[PSRAM] ownedBuffers=");
    Serial0.print(o.blocks);
    Serial0.print("/");
    Serial0.print(PSRAM_MAX_SAMPLES);
    Serial0.print(" (kit=");
    Serial0.print(o.kitBlocks);
    Serial0.print(" runtime=");
    Serial0.print(o.runtimeBlocks);
    Serial0.print(") pcmInPSRAM=");
    Serial0.print(o.bytes / 1024);
    Serial0.println("KB");

    Serial0.print("[PSRAM] kitPCM=");
    Serial0.print(psramTotalAllocated / 1024);
    Serial0.println("KB");

    uint8_t registryUsed = 0;

    for (uint8_t i = 0; i < sampleCount; i++)
    {
        Serial0.print("  [");
        Serial0.print(i);
        Serial0.print("] ");

        if (samples[i].valid)
        {
            Serial0.print(samples[i].dataSize / 1024);
            Serial0.print("KB @0x");
            Serial0.print((uint32_t)samples[i].data, HEX);
        }
        else
        {
            Serial0.print("INVALID");
        }

        // G25: status kepemilikan blok — "own" (blok sendiri) vs
        // "alias" (berbagi pointer) vs "none" (tanpa PCM).
        if (slotOwnBuf[i])
        {
            Serial0.print(" own=");
            Serial0.print(slotOwnBytes[i] / 1024);
            Serial0.print("KB");
        }
        else if (samples[i].valid)
        {
            Serial0.print(" alias");
        }
        else
        {
            Serial0.print(" none");
        }

        // Registry G21: path file yang memakai slot ini.
        if (slotPath[i][0] != '\0')
        {
            registryUsed++;

            const char *base = strrchr(slotPath[i], '/');
            Serial0.print(" <- ");
            Serial0.println(base ? base + 1 : slotPath[i]);
        }
        else
        {
            Serial0.println();
        }
    }

    Serial0.print("Registry: ");
    Serial0.print(registryUsed);
    Serial0.print('/');
    Serial0.println(PSRAM_MAX_SAMPLES);

    Serial0.println("  pad -> slot:");
    for (uint8_t p = 0; p < PAD_COUNT; p++)
    {
        Serial0.print("   PAD");
        Serial0.print(p + 1);
        Serial0.print(" -> slot ");
        Serial0.println(slotForPad[p]);
    }

    Serial0.println("====================");
}