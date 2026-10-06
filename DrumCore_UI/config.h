#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ----- LCD I2C 16x2 -----

constexpr uint8_t LCD_ADDRESS = 0x27;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;

// ----- Rotary Encoder: D5->S1, D6->S2, D7->KEY (push button, pengganti HW-40 CLK/DT/SW) -----

constexpr uint8_t ENCODER_PIN_A = D5;
constexpr uint8_t ENCODER_PIN_B = D6;
constexpr uint8_t ENCODER_BUTTON = D7;

// ----- UART to ESP32-S3 -----

constexpr uint32_t UART_BAUD = 921600;
constexpr uint8_t UART_RX_PIN = RX;   // GPIO3 (native UART0 RX)
constexpr uint8_t UART_TX_PIN = TX;   // GPIO1 (native UART0 TX)

// ----- UART Protocol -----

constexpr uint8_t UART_SOF = 0xD3;
constexpr uint8_t UART_FRAME_SIZE = 8;
constexpr uint8_t UART_RESPONSE_BIT = 0x80;

// ----- UI Timing -----

constexpr uint16_t UI_UPDATE_MS = 20;
constexpr uint16_t PING_INTERVAL_MS = 2000;

// ----- Boot / Product UI animation timing (all non-blocking, millis-based) -----

// G22: branding LCD = "DRUM COK" (sumber teks tunggal: screens.cpp BRAND_LCD);
// nama project/firmware tetap "DrumCore".
// Boot splash: brand mengembang dari tengah ke tepi (frame 0..2), total
// BOOT_FRAME_COUNT x BOOT_FRAME_MS (~1 s), lalu boot steps (PING handshake)
// -> READY -> HOME. Non-blocking, millis-driven.
constexpr uint16_t BOOT_FRAME_MS = 350;
constexpr uint8_t BOOT_FRAME_COUNT = 3;

// Wipe kiri-ke-kanan singkat (~200 ms) saat masuk HOME/MENU/layar KIT
// action/KIT result; halus agar perangkat tak terasa lambat.
constexpr uint16_t TRANSITION_MS = 200;

// Waktu minimum tampilan boot step meski ESP32 langsung menjawab PING
// pertama; memberi kesan boot yang selesai dan disengaja.
constexpr uint16_t INIT_MIN_MS = 700;

// Dwell tiap boot step di status line (ui.cpp handleInit). MURNI
// presentasional: tidak menunda boot, tidak menahan PING handshake, tidak
// mengubah INIT_MIN_MS/timeout PING. Step hanya maju selama modul belum
// menjawab (modul cepat -> READY setelah beberapa step; modul lambat ->
// mengisi seluruh daftar). Baris ditulis ulang hanya saat index step
// berubah. Jumlah step di screens.h (BOOT_STEP_COUNT).
constexpr uint16_t BOOT_STEP_MS = 400;

// Lama "DRUM COK / READY" ditahan sebelum masuk MAIN.
constexpr uint16_t READY_HOLD_MS = 900;

// Lama layar hasil KIT LOAD/SAVE ("<kit name> / LOADED") ditahan sebelum
// kembali ke kit select screen.
constexpr uint16_t KIT_RESULT_HOLD_MS = 2000;

// Ambang long-press tombol encoder (dipakai sebagai aksi BACK global).
constexpr uint16_t ENCODER_LONG_PRESS_MS = 600;

// ----- UART Startup PING retry (non-blocking) -----

// Beri waktu task UART ESP32 siap sebelum ping pertama.
constexpr uint16_t UART_STARTUP_WAIT_MS = 500;
// Tunggu respons PING selama ini sebelum retry.
constexpr uint16_t UART_PING_RETRY_MS = 500;
// Jendela handshake startup non-blocking (~9 s). Wemos terus retry PING sampai
// task UART ESP32 siap; PING FAIL hanya dinyatakan bila seluruh jendela lewat
// tanpa respons valid. Jangan menyerah hanya karena ESP32 boot lambat.
constexpr uint16_t UART_STARTUP_TIMEOUT_MS = 9000;

// ----- Kit UI Timing -----

// Timeout refresh STATUS (bukan jalur LOAD/SAVE). Hanya memengaruhi seberapa
// cepat layar KIT_SELECT me-render ulang index saat respons status terlambat.
constexpr uint16_t KIT_CMD_TIMEOUT_MS = 6000;

// Timeout transaksi LOAD/SAVE (ui.cpp handleKitAction). DIBUAT BESAR (25 s)
// karena LOAD bersifat BLOCKING di ESP32: ESP32 menunggu voice aktif selesai
// (cap 20 s KIT_SWITCH_WAIT_MAX_MS — mis. saat crash/cymbal panjang berbunyi)
// sebelum unload/muat kit, lalu load SD+JSON+PSRAM. Jika timeout Wemos lebih
// pendek, LCD tampil "TIMEOUT" padahal ESP32 akhirnya sukses.
// 25 s > 20 s cap + load + margin.
constexpr uint16_t KIT_ACTION_TIMEOUT_MS = 25000;

constexpr uint16_t KIT_IDLE_BACK_MS = 15000;

// Progress bar LOAD/SAVE visual (murni kosmetik, millis-driven): mengatur cara
// bar NAIK agar terlihat seperti indikator loading modul drum sungguhan.
// TIDAK mengubah timeout komunikasi (KIT_ACTION_TIMEOUT_MS tetap 25000) dan
// tidak menyentuh ESP32. Bar naik cepat di awal lalu melambat, menahan di
// VISUAL_PEAK_PCT sampai respons UART tiba, lalu melompat ke 100% saat OK.
// Jika respons lebih lambat, bar tetap menahan di puncak — tidak pernah restart
// atau lompat ke 0. Jika timeout asli lewat, tampil TIMEOUT.
// Estimasi durasi nyata kit LOAD yang terukur (~5,8 s).
constexpr uint16_t KIT_VISUAL_LOAD_MS = 5800;
// SAVE lebih cepat di modul; beri kurva yang realistis tapi gesit.
constexpr uint16_t KIT_VISUAL_SAVE_MS = 4500;
// Bar menahan di sini (bukan 100%) sambil menunggu respons, agar penyelesaian
// nyata terlihat melompat ke bar penuh.
constexpr uint8_t KIT_VISUAL_PEAK_PCT = 92;

// Idle timeout sebelum layar dalam (PAD browser / PAD edit) kembali ke home.
constexpr uint16_t PAD_IDLE_BACK_MS = 15000;

#endif
