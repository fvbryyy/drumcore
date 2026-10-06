#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==========================
// LCD I2C 16x2
// ==========================

constexpr uint8_t LCD_ADDRESS = 0x27;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;

// ==========================
// Rotary Encoder (KEY / S1 / S2; replaces HW-40 CLK/DT/SW)
//   D5 -> S1, D6 -> S2, D7 -> KEY (push button)
// ==========================

constexpr uint8_t ENCODER_PIN_A = D5;
constexpr uint8_t ENCODER_PIN_B = D6;
constexpr uint8_t ENCODER_BUTTON = D7;

// ==========================
// UART to ESP32-S3
// ==========================

constexpr uint32_t UART_BAUD = 921600;
constexpr uint8_t UART_RX_PIN = RX;   // GPIO3 (native UART0 RX)
constexpr uint8_t UART_TX_PIN = TX;   // GPIO1 (native UART0 TX)

// ==========================
// UART Protocol
// ==========================

constexpr uint8_t UART_SOF = 0xD3;
constexpr uint8_t UART_FRAME_SIZE = 8;
constexpr uint8_t UART_RESPONSE_BIT = 0x80;

// ==========================
// UI Timing
// ==========================

constexpr uint16_t UI_UPDATE_MS = 20;
constexpr uint16_t PING_INTERVAL_MS = 2000;

// ==========================
// Boot / Product UI animation timing (all non-blocking, millis-based)
// ==========================

// G22: branding LCD = "DRUM COK". Nama project/firmware tetap
// "DrumCore" (sumber teks tunggal ada di screens.cpp BRAND_LCD).
//
// Boot splash: a spot-to-edge expand of the brand.
//   frame 0:     DRUM COK      (plain)
//   frame 1:      -DRUM COK-    (rule accent starts)
//   frame 2:     --DRUM COK--   (rule accent widens)
// The whole splash is BOOT_FRAME_COUNT x BOOT_FRAME_MS (~1 s); the real
// boot steps (PING handshake) and READY state follow, then HOME. The
// full boot reads: expand -> boot steps -> READY -> HOME.
// Non-blocking, millis-driven.
constexpr uint16_t BOOT_FRAME_MS = 350;
constexpr uint8_t BOOT_FRAME_COUNT = 3;

// Brief left-to-right wipe used when entering HOME, MENU menus, the KIT
// action screen and the KIT result screen. Short (~200 ms) and subtle so the
// device never feels slow.
constexpr uint16_t TRANSITION_MS = 200;

// Minimum time the boot step display is visible even when the ESP32
// answers the very first PING. Gives the boot a finished, deliberate feel.
constexpr uint16_t INIT_MIN_MS = 700;

// Dwell per boot step shown on the status line (ui.cpp handleInit).
// PURELY presentational: it does NOT delay boot, does NOT hold back the
// PING handshake, and never changes INIT_MIN_MS or any PING timeout. A
// step only advances while the module is still not answering, so a fast
// module reaches READY after a few steps while a slow one fills the whole
// list. No blinking dots: the line is written only when the step index
// actually changes. Step count lives in screens.h (BOOT_STEP_COUNT).
constexpr uint16_t BOOT_STEP_MS = 400;

// How long "DRUM COK / READY" is held before entering MAIN.
constexpr uint16_t READY_HOLD_MS = 900;

// How long a KIT LOAD/SAVE result screen ("<kit name> / LOADED") is
// held before returning to the kit select screen.
constexpr uint16_t KIT_RESULT_HOLD_MS = 2000;

// Encoder button long-press threshold (used as the global BACK action).
constexpr uint16_t ENCODER_LONG_PRESS_MS = 600;

// ==========================
// UART Startup PING retry (non-blocking)
// ==========================

// Give the ESP32 UART task time to be ready before the first ping.
constexpr uint16_t UART_STARTUP_WAIT_MS = 500;
// Wait this long for a PING response before retrying.
constexpr uint16_t UART_PING_RETRY_MS = 500;
// Total non-blocking startup handshake window (~9s). Wemos keeps retrying PING
// until the ESP32 UART task is ready; only when this whole window elapses with
// no valid response does it declare PING FAIL. Never give up just because the
// ESP32 boots slowly.
constexpr uint16_t UART_STARTUP_TIMEOUT_MS = 9000;

// ==========================
// Kit UI Timing
// ==========================

// Timeout untuk refresh STATUS (bukan jalur LOAD/SAVE).
// Hanya memengaruhi seberapa cepat layar KIT_SELECT merender
// ulang index saat respons status terlambat. Tetap pendek.
constexpr uint16_t KIT_CMD_TIMEOUT_MS = 6000;

// Timeout transaksi LOAD/SAVE (ui.cpp handleKitAction).
// DIBUAT BESAR (25 s) karena LOAD adalah BLOCKING di ESP32:
//   - ESP32 menunggu voice aktif selesai (cap 20 detik
//     KIT_SWITCH_WAIT_MAX_MS) sebelum unload/muat kit,
//     terlebih saat crash/cymbal panjang sedang berbunyi.
//   - lalu SD+JSON+PSRAM load.
// Kalau timeout Wemos lebih pendek, LCD tampil "TIMEOUT"
// walau ESP32 akhirnya sukses. 25 s > 20 s cap + load + margin.
constexpr uint16_t KIT_ACTION_TIMEOUT_MS = 25000;

constexpr uint16_t KIT_IDLE_BACK_MS = 15000;

// ------------------------------------------------------------
// Visual LOAD/SAVE progress (purely cosmetic, millis-driven).
//
// These describe how the on-screen progress bar RAMPS so it looks like a
// real drum-module loading indicator. They do NOT change the communication
// timeout (KIT_ACTION_TIMEOUT_MS remains 25000) and never touch the ESP32.
//
// The bar climbs quickly at first, then tapers, holds around VISUAL_PEAK_PCT
// until the UART response arrives, and snaps to 100% on OK. If the response
// is slower than usual it simply holds at the peak - it never restarts and
// never jumps back to 0. If the real timeout passes, TIMEOUT is shown.
// ------------------------------------------------------------
// Estimated real-world duration measured for a kit LOAD (~5.8 s).
constexpr uint16_t KIT_VISUAL_LOAD_MS = 5800;
// SAVE is faster on the module; give it a realistic but snappy curve.
constexpr uint16_t KIT_VISUAL_SAVE_MS = 4500;
// The bar holds here (not 100%) while waiting for the response, so a real
// completion visibly jumps to a full bar.
constexpr uint8_t KIT_VISUAL_PEAK_PCT = 92;

// Idle timeout before a deep screen (PAD browser / PAD edit) returns home.
constexpr uint16_t PAD_IDLE_BACK_MS = 15000;

#endif
