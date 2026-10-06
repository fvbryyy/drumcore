#include <Arduino.h>

// Phase 1: FreeRTOS task management dari setup()/loop()

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "hardware.h"
#include "pad.h"
#include "trigger.h"
#include "midi.h"
#include "audio.h"
#include "velocity.h"
#include "sdcard.h"
#include "psram_loader.h"
#include "kit_loader.h"
#include "kit_manager.h"
#include "persistence.h"
#include "uart_protocol.h"
#include "adc_calibration.h"

namespace
{
    uint32_t lastDiagPrintTime = 0;
    constexpr uint32_t DIAG_PRINT_INTERVAL_MS = 100;
}

void setup()
{
    Serial0.begin(115200);
    delay(500);

    hardwareInit();
    padInit();
    triggerInit();
    midiInit();
    sdInit();

    // Velocity LUT: build sekali di init (zero-float setelahnya),
    // SEBELUM audioInit() supaya LUT siap sebelum hit pertama.

    buildVelocityLut();

    // ----- NVS: init + baca global (activeKit) -----
    // Baca SEBELUM load base kit JSON supaya kit terakhir bisa
    // dipilih. NVS kosong/rusak -> activeKit=0.
    persistenceInit();

    uint8_t activeKit = 0;
    uint8_t volume    = 0;
    persistenceLoadGlobal(activeKit, volume);

    // ----- Migrasi NVS per-kit override -> kit.json (sekali) -----
    // Sejak KIT RUNTIME, NVS hanya menyimpan global (active/volume);
    // blob per-kit legacy di-merge ke kit.json lalu dihapus. Dipanggil
    // SEBELUM load kit aktif supaya hasil merge ikut dibaca.
    kitManagerMigrateNvsOverrides();

    // ----- Boot fallback aman -----
    // NVS active bisa menunjuk kit yang sudah dihapus/rusak: jatuh ke
    // kit factory (kit00) + tulis ulang global supaya restart
    // berikutnya boot ke kit yang benar-benar ada.
    char kitJsonPath[KIT_PATH_LEN];
    char activeDir[KIT_DIR_LEN];

    if (!kitLoaderResolveKitDir(activeKit, activeDir, sizeof(activeDir)))
    {
        Serial0.print("Active kit NOT resolvable (slot ");
        Serial0.print(activeKit);
        Serial0.println(") -> fallback to factory kit.");

        activeKit = KIT_FACTORY_INDEX;
        persistenceSaveGlobal(activeKit, volume);
    }

    kitLoaderBuildPath(activeKit, kitJsonPath, sizeof(kitJsonPath));

    // ----- Load base kit JSON dari activeKit -----
    // Kit JSON menentukan file sample tiap pad + PadConfig. Kalau
    // tidak ada / corrupt, loader fallback aman ke default config.h
    // (pads senyap, tanpa fallback root).
    kitLoaderLoad(
        kitJsonPath,
        PAD_SAMPLE_PATHS,
        PAD_COUNT
    );

    // ----- Load sample paths / PSRAM (dari activeKit) -----
    if (psramSetup())
    {
        // Mapping by FILENAME: slot PSRAM == pad index (identity,
        // sinkron dgn audio engine). sampleIds identity sehingga
        // slotForPad[i] = i: nama file (mapping RAM) sumber efektif,
        // bukan index.
        const char *kitPaths[PAD_COUNT];
        uint8_t sampleIds[PAD_COUNT];

        for (uint8_t i = 0; i < PAD_COUNT; i++)
        {
            kitPaths[i] = kitLoaderResolvedSamplePath(i);
            sampleIds[i] = i;   // identity (slot == pad index)
        }

        psramLoadKit(
            kitPaths,
            sampleIds,
            PAD_COUNT
        );
        psramPrintStatus();
    }

    audioInit();

    // ----- UART control plane init -----
    // Inisialisasi hardware + parser. Task UART (prio 5, Core 1)
    // dimulai bersama task lain; UART = control plane, TIDAK
    // menyentuh realtime drum engine.
    uartProtocolInit(activeKit, volume);

    // ADC Calibration instrumentation (compile-out bila false).
    // Inisialisasi SEBELUM task dimulai supaya buffer bersih.
#if DEBUG_ADC_CALIBRATION
    adcCalibrationInit();
#endif

    // Phase 1: audio engine + trigger + MIDI berjalan di FreeRTOS
    // task (bukan superloop):
    //   AudioTask   Core 0 / priority 24
    //   TriggerTask Core 1 / priority 23
    //   MidiTask    Core 1 / priority 8
    //   UartTask    Core 1 / priority  5  (control plane)

    audioTaskStart();
    triggerTaskStart();
    midiTaskStart();
    uartTaskStart();

    delay(500);

    Serial0.print("=== DrumCore v");
    Serial0.print(FW_VERSION);
    Serial0.println(" READY ===");
}

void diagPrintADC()
{
    if constexpr (!DEBUG_ADC)
    {
        return;
    }

    const uint32_t now = millis();

    if ((now - lastDiagPrintTime) < DIAG_PRINT_INTERVAL_MS)
    {
        return;
    }

    lastDiagPrintTime = now;

    for (uint8_t i = 0; i < PAD_COUNT; i++)
    {
        Serial0.print("P");
        Serial0.print(i);
        Serial0.print("=");
        Serial0.print(pads[i].currentValue);
        Serial0.print(" ");
    }

    Serial0.println();
}

void loop()
{
    // Phase 1: loop() TIDAK lagi memproses drum. Trigger scanning,
    // audio mixing, dan MIDI berjalan di FreeRTOS task. loop() hanya
    // menjalankan diagnostic non-real-time (Core 1) — tidak boleh ada
    // blocking operation yang mengganggu task real-time di Core 0.

    audioTwoVoiceDebugTask();
    audioMixAuditTask();
    triggerProfileReport();
    playabilityReport();
    diagPrintADC();
    adcScanProfileReport();

#if DEBUG_ADC_CALIBRATION
    adcCalibrationTask();
#endif

    vTaskDelay(pdMS_TO_TICKS(1));
}