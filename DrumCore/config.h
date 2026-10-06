#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ======================================================
// Firmware Configuration
// ======================================================

constexpr char FW_VERSION[] = "0.4.0";

constexpr uint32_t SERIAL_BAUD = 115200;

// ======================================================
// Audio Engine
// ======================================================

constexpr uint8_t VOICE_COUNT = 32;

// ======================================================
// Peripheral Fixed Pins (REFERENSI — jangan diubah)
// ======================================================
//
// SD SPI:   MOSI=38  MISO=39  SCK=40  CS=41
// I2S:      BCLK=42  LRCK=21  DOUT=2
// USB MIDI: GPIO19 / GPIO20 (native USB, jangan dipakai)
// UART0:    GPIO43 (TX) / GPIO44 (RX) — Serial0 debug

constexpr uint8_t SD_MOSI_PIN = 38;
constexpr uint8_t SD_MISO_PIN = 39;
constexpr uint8_t SD_SCK_PIN  = 40;
constexpr uint8_t SD_CS_PIN   = 41;

constexpr uint8_t I2S_BCLK_PIN = 42;
constexpr uint8_t I2S_LRCK_PIN = 21;
constexpr uint8_t I2S_DOUT_PIN = 2;

// ======================================================
// Pad Configuration
// ======================================================

constexpr uint8_t PAD_COUNT = 12;

constexpr uint8_t PAD_PINS[PAD_COUNT] =
{
    4, 5, 6,      // PAD01-03 : kecil
    17, 8, 9,     // PAD04-06 : besar
    10, 11, 12,   // PAD07-09 : besar
    7, 15, 16     // PAD10-12 : kecil
};

// ======================================================
// Default MIDI Note Mapping (GM)
// ======================================================

constexpr uint8_t PAD_MIDI_NOTES[PAD_COUNT] =
{
    36,  // PAD 0  Bass Drum (Kick)
    38,  // PAD 1  Snare
    40,  // PAD 2  Electric Snare / Rim
    45,  // PAD 3  Low Tom
    47,  // PAD 4  Mid-Low Tom
    48,  // PAD 5  Mid Tom
    50,  // PAD 6  High Tom
    41,  // PAD 7  Low Floor Tom
    43,  // PAD 8  High Floor Tom
    49,  // PAD 9  Crash Cymbal
    51,  // PAD 10 Ride Cymbal
    56   // PAD 11 Cowbell
};

// ======================================================
// Sample Paths (12 pad = 12 sample unik)
// ======================================================
//
// Semua file WAV harus:
//   - 48000 Hz sample rate
//   - 16-bit PCM
//   - Mono atau stereo
//   - Header standar 44 byte

constexpr const char* PAD_SAMPLE_PATHS[PAD_COUNT] =
{
    "/KICK.wav",
    "/SNARE.wav",
    "/CHH.wav",
    "/OHH.wav",
    "/TOM1.wav",
    "/TOM2.wav",
    "/FLOOR.wav",
    "/LCRASH.wav",
    "/RCRASH.wav",
    "/SPLASH.wav",
    "/RIDE.wav",
    "/BEL.wav"
};

// ======================================================
// PSRAM Configuration
// ======================================================
//
// ESP32-S3 punya 8MB PSRAM (QSPI).
// 12 samples × ~256KB = ~3MB. Sisa untuk kit cache.
//
// G21 — Domain slot sample.
//   slot 0 .. PSRAM_KIT_SLOT_COUNT-1 : dipakai KIT LOAD (slot == index
//       pad saat kit dimuat; identitas, backward-compat).
//   slot PSRAM_KIT_SLOT_COUNT .. PSRAM_MAX_SAMPLES-1 : slot RUNTIME
//       untuk file yang BELUM ada di PSRAM (PAD -> SOUND memilih file
//       baru). Tiap slot punya alokasi PSRAM sendiri — menambah sample
//       runtime tidak perlu me-rebase pointer sample kit / free buffer
//       yang sedang dibaca voice.
// Audio realtime TIDAK berubah bentuk: psramSlotForPad() tetap fixed
// array uint8, O(1), tanpa SD/malloc/String; yang berubah hanya NILAI
// slot yang boleh dipakai (0..PSRAM_MAX_SAMPLES-1).
//
// G25 — Kepemilikan blok PCM.
//   Dahulu PCM 12 slot kit berada di SATU blok ps_malloc(total kit)
//   ~5.5 MB; slotOwnBuf[] hanya untuk slot runtime. Satu KIT LOAD butuh
//   satu blok kontigu sebesar seluruh kit — free-list yang terpecah
//   (free total cukup, largest < requested) gagal.
//   Sekarang SETIAP slot yang punya PCM punya blok sendiri di
//   slotOwnBuf[] (kit maupun runtime). Total kit tidak harus muat dalam
//   satu blok; hanya free TOTAL yang perlu cukup. Konsekuensi ke audio:
//   nol (psramGetData/size sudah per-slot).
constexpr uint8_t PSRAM_KIT_SLOT_COUNT = PAD_COUNT;      // 12 slot kit
constexpr uint8_t PSRAM_EXTRA_SLOT_COUNT = 4;             // slot runtime
constexpr size_t PSRAM_MAX_SAMPLES =
    PSRAM_KIT_SLOT_COUNT + PSRAM_EXTRA_SLOT_COUNT;        // 16
constexpr size_t PSRAM_MAX_SAMPLE_SIZE = 4 * 1024 * 1024; // 4MB max per sample
constexpr size_t PSRAM_KIT_SLOTS = 2;  // 2 kit di-cache

// Sentinel "tidak ada slot" untuk mapping pad -> slot (pad di-UNASSIGN
// = senyap). startVoice() menolaknya lewat bound check sehingga pad
// membisu tanpa menyentuh data sample lain.
constexpr uint8_t PSRAM_SLOT_NONE = 0xFF;

// ======================================================
// External Trigger
// ======================================================

constexpr uint8_t EXT_TRIGGER_PIN       = 13;
constexpr uint8_t EXT_TRIGGER_MIDI_NOTE = 35;

// ======================================================
// Hi-Hat Switch
// ======================================================

constexpr uint8_t HIHAT_SWITCH_PIN  = 14;
constexpr uint8_t HIHAT_OPEN_NOTE   = 46;
constexpr uint8_t HIHAT_CLOSED_NOTE = 42;

// ======================================================
// Per-Pad Retrigger Mask (ms)
// ======================================================
// Beda per jenis pad untuk feel natural:
//   - Snare/tom: 15ms (=15000us) — buzz roll aman
//   - Kick:      30ms (=30000us) — hindari double trigger
//   - Cymbal:    17ms (=17000us)
//
// Phase 3C: nilai konservatif awal (ms), setara KICK=30000us,
// SNARE/TOM=15000us, CYMBAL=17000us. Disimpan dalam ms karena
// adjustMaskTime() dan state machine memakai satuan ms (runtime
// x1000 -> us). Penyetelan ulang cukup di tabel ini; trigger engine
// tidak diubah.

constexpr uint32_t PAD_MASK_MS[PAD_COUNT] =
{
    30,  // PAD 0  Kick       = 30000us
    15,  // PAD 1  Snare      = 15000us
    15,  // PAD 2  Rim/E.Snr  = 15000us
    15,  // PAD 3  Low Tom    = 15000us
    15,  // PAD 4  Mid-Low Tom= 15000us
    15,  // PAD 5  Mid Tom    = 15000us
    15,  // PAD 6  High Tom   = 15000us
    15,  // PAD 7  Low Floor  = 15000us
    15,  // PAD 8  High Floor = 15000us
    17,  // PAD 9  Crash      = 17000us
    17,  // PAD 10 Ride       = 17000us
    15   // PAD 11 Cowbell    = 15000us
};

// ======================================================
// Pad Field Bounds (SINGLE SOURCE OF TRUTH — G8-A)
// ======================================================
//
// Satu-satunya sumber kebenaran batas nilai field PadConfig
// (CMD_PAD_SET / CMD_PAD_GET). Dipakai control plane
// (uart_protocol.cpp), persistence (NVS), kit_loader (JSON SD), dan
// kit_manager (CREATE source). JANGAN hardcode angka batas di file
// lain — kalau batas berubah, ubah hanya di sini.
//
// Field ID harus sinkron dengan setField/getPadField di
// uart_protocol.cpp (dan klien Wemos di DrumCore_UI/uart_client.h —
// salinan protokol, bukan salinan batas).
//
// CATATAN TRANSPORT: frame UART membawa nilai 16-bit (PAD_GET dikirim
// dua frame lo/hi). Batas di bawah ini adalah batas NILAI per field;
// lapisan UART hanya me-clamp maksimumnya ke 0xFFFF (keterbatasan
// frame, bukan duplikasi batas). releaseStableTime (uint32) otoritatif
// sampai 100000, tapi lewat UART hanya sampai 65535.

constexpr uint8_t FIELD_MIDI_NOTE        = 0;
constexpr uint8_t FIELD_THRESHOLD        = 1;
constexpr uint8_t FIELD_RELEASE_THRESH   = 2;
constexpr uint8_t FIELD_PEAK_TIME        = 3;
constexpr uint8_t FIELD_RELEASE_STABLE   = 4;
constexpr uint8_t FIELD_MASK_BASE_MS     = 5;
constexpr uint8_t FIELD_SENSITIVITY      = 6;
constexpr uint8_t FIELD_CURVE            = 7;
constexpr uint8_t FIELD_PAD_TYPE         = 8;
constexpr uint8_t FIELD_SAMPLE_ID        = 9;
constexpr uint8_t FIELD_CHOKE_GROUP      = 10;
constexpr uint8_t FIELD_COUNT            = 11;

// Batas bawah — dipertahankan dari filosofi validasi lama
// (nilai di bawah ini = invalid, dibiarkan memakai nilai lama).
constexpr uint32_t PAD_FIELD_MIN[FIELD_COUNT] =
{
    0,      // FIELD_MIDI_NOTE
    10,     // FIELD_THRESHOLD
    50,     // FIELD_RELEASE_THRESH
    200,    // FIELD_PEAK_TIME
    1000,   // FIELD_RELEASE_STABLE
    1,      // FIELD_MASK_BASE_MS
    1,      // FIELD_SENSITIVITY
    0,      // FIELD_CURVE
    0,      // FIELD_PAD_TYPE
    0,      // FIELD_SAMPLE_ID
    0       // FIELD_CHOKE_GROUP
};

// Batas atas — efektif (D1: threshold/releaseThreshold dinaikkan
// 1023 -> 4095, maskBaseMs 500 -> 65535; sampleId disatukan ke
// 0..PAD_COUNT-1, hanya slot kit yang valid).
constexpr uint32_t PAD_FIELD_MAX[FIELD_COUNT] =
{
    127,                 // FIELD_MIDI_NOTE
    4095,                // FIELD_THRESHOLD
    4095,                // FIELD_RELEASE_THRESH
    10000,               // FIELD_PEAK_TIME
    100000,              // FIELD_RELEASE_STABLE (uint32; UART cap 16-bit)
    65535,               // FIELD_MASK_BASE_MS
    255,                 // FIELD_SENSITIVITY
    255,                 // FIELD_CURVE
    6,                   // FIELD_PAD_TYPE (PadType enum max)
    (PAD_COUNT - 1),     // FIELD_SAMPLE_ID (hanya slot kit yang valid)
    255                  // FIELD_CHOKE_GROUP
};

// ======================================================
// UART Control Plane (Phase 6C)
// ======================================================
//
// UART1 = kontrol plane Wemos D1 Mini (non-blocking).
// TIDAK menyentuh PAD_PINS / trigger / audio realtime.
//
// Pin bebas (tidak dipakai peripheral lain):
//   SD=38,39,40,41  I2S=42,21,2  PAD=4..12,15,16,17
//   EXT=13  HIHAT=14  USB MIDI=19,20  UART0=43,44
//   -> UART1 pakai GPIO1 (TX) / GPIO3 (RX).

constexpr uint8_t UART1_TX_PIN = 1;
constexpr uint8_t UART1_RX_PIN = 3;
constexpr uint32_t UART1_BAUD = 921600;

constexpr uint8_t UART_MAX_KIT = 9;   // kit 0..9 (10 kits)

// ======================================================
// Kit Management (TASK 3 — control plane)
// ======================================================
//
// 5 slot kit (0..4). Kit 0 = factory default, dilindungi dari DELETE
// (juga tujuan fallback saat active kit di-delete). CREATE hanya
// boleh menulis slot 1..4.

constexpr uint8_t KIT_COUNT         = UART_MAX_KIT + 1; // 5
constexpr uint8_t KIT_FACTORY_INDEX = 0;                // protected
constexpr uint8_t KIT_NAME_MAX      = 28;               // panjang nama maks
constexpr uint8_t KIT_NAME_STORAGE  = 32;               // buffer NUL-terminated

// Nama file sample per pad (field "sample" di kit.json & protokol
// PAD_SOUND_SET). Cap identik aturan isValidSampleName (kit_loader):
// maks SAMPLE_NAME_MAX char, storage +1 utk NUL.
constexpr uint8_t SAMPLE_NAME_MAX    = 32;
constexpr uint8_t SAMPLE_NAME_STORAGE = 33;

// Path buffers untuk kit name-based (G2): folder kit MENGIKUTI nama
// kit (maks KIT_NAME_MAX char). Ukuran dihitung dari:
//   dir  = "/kits/" (6) + KIT_NAME_MAX (28) + NUL = 35 <= 40
//   json = dir (39) + "/kit.json" (9) + NUL = 49 <= 56
//   smpl = dir (39) + "/samples/" (9) + nama sample (32) + NUL
constexpr uint8_t KIT_DIR_LEN           = 40;
constexpr uint8_t KIT_PATH_LEN          = 56;
constexpr uint8_t KIT_SAMPLE_PATH_LEN   = 96;

// ======================================================
// Debug Configuration
// ======================================================

constexpr bool DEBUG_ADC     = false;
constexpr bool DEBUG_TRIGGER = false;

// UART control plane debug (RX frames / command log). Default
// OFF — release tidak spam Serial saat drum dimainkan.
// Nyalakan ke `true` hanya saat development/debug build.
constexpr bool DEBUG_UART    = false;
// Mode instrumentasi ADC Calibration/Debug — non-intrusif.
// Set 1 untuk mengaktifkan auto-capture tool, 0 untuk compile-out total.
#define DEBUG_ADC_CALIBRATION 1
constexpr bool PROFILE_TRIGGER_LATENCY = false;
constexpr bool PROFILE_ADC_SCAN = false;
constexpr bool PROFILE_ADC_STALENESS = false;

// 2-voice cutout audit instrumentation (counters + 20-event ring log)
constexpr bool PROFILE_2VOICE_DEBUG = false;

// ======================================================
// DIAGNOSTIC ROLLING TEST (infrastruktur debug)
// ======================================================
//
// 1 = aktifkan diagnostic rolling test di trigger engine
//    (event ring SPSC non-blocking + counter + laporan).
// 0 = nonaktif — default RELEASE BUILD, tanpa output diagnostic
//    di Serial.
//
// Infrastruktur dipertahankan (bukan dihapus) supaya bisa diaktifkan
// lagi untuk DEBUG BUILD; di 0 seluruh kode compile-out sehingga nol
// biaya di release. TIDAK mengubah ADC/trigger algorithm/threshold/
// mask/audio.

#ifndef DEBUG_ROLLING_TEST
#define DEBUG_ROLLING_TEST 0
#endif

// ======================================================
// PLAYABILITY DIAGNOSTIC (infrastruktur debug)
// ======================================================
//
// 1 = aktifkan counter playability ringan + laporan periodik
//     1 detik (TRIGGER/AUDIO/PREFETCH).
// 0 = nonaktif — default RELEASE BUILD, nol output Serial.
//
// Di 0 seluruh counter & laporan compile-out sehingga nol biaya
// runtime. Hanya counter non-invasif (tanpa print per-hit); laporan
// dijaga di Core 1 (loop()) dan TIDAK pernah di real-time path Core 0.
//
// TIDAK mengubah ADC/trigger algorithm/threshold/mask/audio.
// Hanya observasi.

#ifndef DEBUG_PLAYABILITY
#define DEBUG_PLAYABILITY 0
#endif

// Mix-pipeline audit: ring per-mix/per-onset/per-write untuk melacak
// titik di mana attack multi-pad tertunda
constexpr bool PROFILE_MIX_AUDIT = false;

#endif // CONFIG_H