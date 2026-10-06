#include "sdcard.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

namespace
{
    // ==================================================
    // SD SPI Pins
    // ==================================================

    constexpr int SD_MOSI = 38;
    constexpr int SD_MISO = 39;
    constexpr int SD_SCK  = 40;
    constexpr int SD_CS   = 41;

    // ==================================================
    // SD Init Retry Config (G19)
    // ==================================================
    //
    // Setelah crash Guru Meditation, SPI peripheral ESP32 bisa
    // meninggalkan partial state (bus mid-transaction); CS butuh
    // waktu settle. Retry dengan delay meningkat memberi waktu
    // SPI bus & SD card untuk recover.

    constexpr uint8_t  SD_INIT_RETRIES   = 3;
    constexpr uint32_t SD_RETRY_DELAY_MS = 150;

    // ==================================================
    // SD State
    // ==================================================

    SPIClass sdSPI(FSPI);

    bool sdReady = false;
}

// ======================================================
// SD Initialization
// ======================================================

void sdInit()
{
    Serial0.println();
    Serial0.println("=== SD CARD INIT ===");

    sdReady = false;

    // --------------------------------------------------
    // CS idle HIGH (wajib SEBELUM SPI.begin) supaya SD card
    // tidak masuk mode SPI yang salah saat SCK pulses selama
    // begin(). Di-set sebelum SPI.begin() dipanggil.
    // --------------------------------------------------

    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(10);  // G19: beri waktu CS untuk settle ke HIGH

    // --------------------------------------------------
    // SPI begin
    // --------------------------------------------------

    sdSPI.begin(
        SD_SCK,
        SD_MISO,
        SD_MOSI,
        SD_CS
    );

    Serial0.println("[SD] SPI begin OK");
    Serial0.print("[SD] pins: SCK=");
    Serial0.print(SD_SCK);
    Serial0.print(" MISO=");
    Serial0.print(SD_MISO);
    Serial0.print(" MOSI=");
    Serial0.print(SD_MOSI);
    Serial0.print(" CS=");
    Serial0.println(SD_CS);

    // --------------------------------------------------
    // SD begin — dengan retry (G19)
    //
    // Crash sebelumnya bisa meninggalkan bus/card partial state;
    // retry + delay memberi kesempatan bus settle. Frekuensi
    // 20 MHz; bila semua attempt gagal, log detail diagnostic.
    // --------------------------------------------------

    bool initOk = false;

    for (uint8_t attempt = 1; attempt <= SD_INIT_RETRIES; attempt++)
    {
        Serial0.print("[SD] begin attempt ");
        Serial0.print(attempt);
        Serial0.print("/");
        Serial0.println(SD_INIT_RETRIES);

        // Pastikan CS HIGH lagi sebelum setiap attempt
        // (state residual mungkin menariknya LOW).
        digitalWrite(SD_CS, HIGH);
        delay(5);

        if (SD.begin(SD_CS, sdSPI, 20000000, "/sd", 10))
        {
            initOk = true;
            break;
        }

        // Log detail kegagalan setiap attempt.
        Serial0.print("[SD] attempt ");
        Serial0.print(attempt);
        Serial0.println(" FAILED");

        if (attempt < SD_INIT_RETRIES)
        {
            // Teardown SPI sebelum retry supaya state bersih (G19:
            // tanpa end(), retry hanya mengulang state yang sama).
            sdSPI.end();
            delay(SD_RETRY_DELAY_MS * attempt);  // backoff meningkat

            // Re-set CS HIGH setelah end() karena pin state
            // mungkin berubah.
            pinMode(SD_CS, OUTPUT);
            digitalWrite(SD_CS, HIGH);
            delay(10);

            sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
            delay(5);
        }
    }

    if (!initOk)
    {
        Serial0.println("[SD] INIT FAILED (all attempts exhausted)");
        Serial0.print("[SD] freq=20MHz CS=");
        Serial0.print(SD_CS);
        Serial0.print(" SCK=");
        Serial0.print(SD_SCK);
        Serial0.print(" MISO=");
        Serial0.print(SD_MISO);
        Serial0.print(" MOSI=");
        Serial0.println(SD_MOSI);
        Serial0.println("[SD] Check: card inserted? SPI wiring? Post-crash partial state?");

        sdReady = false;
        return;
    }

    sdReady = true;

    Serial0.println("[SD] INIT OK");

    // --------------------------------------------------
    // Card Information (diagnostic)
    // --------------------------------------------------

    uint8_t cardType = SD.cardType();

    Serial0.print("[SD] cardType=");

    if (cardType == CARD_MMC)
    {
        Serial0.println("MMC");
    }
    else if (cardType == CARD_SD)
    {
        Serial0.println("SDSC");
    }
    else if (cardType == CARD_SDHC)
    {
        Serial0.println("SDHC");
    }
    else if (cardType == CARD_NONE)
    {
        Serial0.println("NONE (no card?)");
    }
    else
    {
        Serial0.print("UNKNOWN (");
        Serial0.print(cardType);
        Serial0.println(")");
    }

    uint64_t cardSize =
        SD.cardSize() / (1024ULL * 1024ULL);

    Serial0.print("[SD] size=");
    Serial0.print(cardSize);
    Serial0.println(" MB");

    Serial0.println("=== SD CARD READY ===");
}

// ======================================================
// SD Ready
// ======================================================

bool sdIsReady()
{
    return sdReady;
}

// ======================================================
// SD Task
// ======================================================

void sdTask()
{
    // --------------------------------------------------
    // Belum digunakan.
    //
    // Jangan operasi SD terus-menerus di sini; nantinya
    // audio engine yang meminta data dari SD.
    // --------------------------------------------------
}
