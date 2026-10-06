#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <Arduino.h>
#include "config.h"

// ======================================================
// UART Control Plane (Phase 6C)
// ======================================================
//
// Komunikasi non-blocking ESP32-S3 <-> Wemos D1 Mini via
// UART1 (GPIO1 TX / GPIO3 RX, 921600 baud). HANYA layer
// kontrol — TIDAK menyentuh realtime drum engine.
//
// Frame fixed 8 byte:
//   [0] SOF=0xD3
//   [1] CMD       (bit7 = respon: 0=req, 1=resp)
//   [2] ARG0
//   [3] ARG1
//   [4] ARG2
//   [5] ARG3
//   [6] SEQ
//   [7] CRC       = XOR(CMD,ARG0..ARG3,SEQ)
//
// CRC: checksum 8-bit XOR dari byte 1..6.
// Parser state-machine, tanpa String / dynamic allocation.

// ======================================================
// Protocol Constants
// ======================================================

constexpr uint8_t UART_SOF         = 0xD3;
constexpr uint8_t UART_FRAME_LEN   = 8;
constexpr uint8_t UART_RESP_FLAG   = 0x80;

// ---- Command IDs (request) ----
constexpr uint8_t CMD_PING            = 0x01;
constexpr uint8_t CMD_REQUEST_STATUS  = 0x02;
constexpr uint8_t CMD_KIT_NEXT        = 0x10;
constexpr uint8_t CMD_KIT_PREV        = 0x11;
constexpr uint8_t CMD_KIT_LOAD        = 0x12;
constexpr uint8_t CMD_KIT_SAVE        = 0x13;
constexpr uint8_t CMD_KIT_INFO        = 0x14;
constexpr uint8_t CMD_KIT_CREATE_EMPTY = 0x15;   // TASK 3
constexpr uint8_t CMD_KIT_CREATE_COPY  = 0x16;   // TASK 3
constexpr uint8_t CMD_KIT_RENAME       = 0x17;   // TASK 3 (chunked)
constexpr uint8_t CMD_KIT_DELETE       = 0x18;   // TASK 3
constexpr uint8_t CMD_KIT_CREATE_EMPTY_NAMED = 0x19;   // TASK 4 (chunked)
constexpr uint8_t CMD_KIT_CREATE_COPY_NAMED  = 0x1A;   // TASK 4 (chunked)
constexpr uint8_t CMD_PAD_SET         = 0x20;
constexpr uint8_t CMD_PAD_GET         = 0x21;
constexpr uint8_t CMD_PAD_SOUND       = 0x22;   // G9: daftar suara kit aktif (read-only)
constexpr uint8_t CMD_PAD_SOUND_SET   = 0x23;   // KIT RUNTIME: assign WAV kit ke pad (chunked)
constexpr uint8_t CMD_GLOBAL_SAVE     = 0x30;
constexpr uint8_t CMD_CALIBRATE       = 0x40;
constexpr uint8_t CMD_FULL_SYNC       = 0x50;

// CMD_KIT_RENAME request encoding (frame 8-byte standar):
//   frame header : ARG0 = slot kit, ARG1 = panjang nama,
//                  ARG2 = 0, ARG3 = 0.
//   frame data   : ARG0 = (chunkIdx | 0x80), ARG1..3 = sampai
//                  3 char nama (maks KIT_NAME_MAX char ->> 10
//                  chunk). Sama dengan mekanisme chunk KIT_INFO.
// Respon = satu frame normal dgn status di ARG0.
//
// CMD_KIT_CREATE_EMPTY_NAMED / CMD_KIT_CREATE_COPY_NAMED (G4):
//   Encoding identik dengan RENAME, tetapi nama dipakai utk CREATE
//   (EMPTY / COPY CURRENT). ARG0 header = 0 (cadangan), ARG1 =
//   panjang nama. ESP32 memilih slot kosong otomatis (konsep slot
//   TIDAK diekspos ke user). Respon = satu frame status:
//     ARG1 = slot terisi (internal / instrumentasi; UI mengabaikan).
//   Status: OK / INVALID_VALUE (nama invalid) / NAME_EXISTS (nama
//   sudah dipakai kit lain) / BUSY (semua slot penuh).
//   Loop frame 8-byte, CRC, SEQ, SOF tidak berubah.

// CMD_PAD_SOUND (G9 / KIT RUNTIME): daftar suara (file WAV) di folder
// samples kit AKTIF — source of truth MAPPING = nama file. Request membawa
// ARG0 = index pad (yang ingin dilihat). TIDAK mengubah kit/NVS/PSRAM;
// frame 8-byte standar.
// Respon (semua frame memakai SEQ request; dirakit khusus di klien,
// tidak lewat FIFO respons umum — identik dengan CMD_KIT_INFO):
//   frame header     : ARG0 = STATUS_OK, ARG1 = jumlah suara ter-scan,
//                      ARG2 = 0xC7 (penanda "daftar"), ARG3 = index suara
//                      yang sedang dipakai pad tsb (0xFF bila tidak ada /
//                      pad unassigned).
//   frame nama-header: ARG0 = index suara (tanpa bit 0x80),
//                      ARG1 = panjang nama, ARG2 = 0, ARG3 = 0.
//   frame chunk      : ARG0 = (chunkIdx | 0x80), ARG1..3 = hingga
//                      3 char nama. Mekanisme identik KIT_INFO.
// Daftar = scan folder samples kit aktif (cap PAD_COUNT, urut abjad).
//
// CMD_PAD_SOUND_SET (0x23, KIT RUNTIME): assign WAV dari folder samples
// kit AKTIF ke sebuah pad (RAM-only sampai KIT SAVE). Request chunked
// (mekanisme identik CMD_KIT_RENAME / CREATE_NAMED):
//   frame header     : ARG0 = index pad, ARG1 = panjang nama file.
//   frame data       : ARG0 = (chunkIdx | 0x80), ARG1..3 = hingga 3 char.
// Respon = satu frame status normal. Nama wajib valid (≤ SAMPLE_NAME_MAX,
// tanpa '/', '\\', "..") dan file harus ADA di folder samples kit aktif.
// Sukses = mapping RAM pad->filename diperbarui + PSRAM disinkronkan
// (gate + tunggu voice + reload). TIDAK menulis SD.
// ---- Status codes (ARG0 respon) ----
constexpr uint8_t STATUS_OK              = 0x00;
constexpr uint8_t STATUS_INVALID_COMMAND = 0x01;
constexpr uint8_t STATUS_INVALID_PAD     = 0x02;
constexpr uint8_t STATUS_INVALID_VALUE   = 0x03;
constexpr uint8_t STATUS_INVALID_KIT     = 0x04;
constexpr uint8_t STATUS_BUSY            = 0x05;
constexpr uint8_t STATUS_CRC_ERROR       = 0x06;
    constexpr uint8_t STATUS_NAME_EXISTS     = 0x07;
    // G24.15: value BARU di field status yang sudah ada. Frame,
    // panjang frame, SOF, CRC, SEQ, dan daftar command TIDAK
    // berubah — hanya satu arti tambahan pada ARG0 respons, supaya
    // "semua slot kituser terisi" tidak lagi dilaptop jadi BUSY.
    constexpr uint8_t STATUS_NO_SPACE        = 0x08;

// ---- Pad field IDs (CMD_PAD_SET / CMD_PAD_GET) ----
// SUMBER KEBENARAN: config.h (FIELD_MIDI_NOTE..FIELD_COUNT) bersama
// PAD_FIELD_MIN/MAX. Jangan definisi ulang di sini — cukup pakai
// yang dari config.h.
// Documented set (salinan klien Wemos: DrumCore_UI/uart_client.h):
//   FIELD_MIDI_NOTE    = 0
//   FIELD_THRESHOLD    = 1
//   FIELD_RELEASE_THRESH = 2
//   FIELD_PEAK_TIME    = 3
//   FIELD_RELEASE_STABLE = 4
//   FIELD_MASK_BASE_MS = 5
//   FIELD_SENSITIVITY  = 6
//   FIELD_CURVE        = 7
//   FIELD_PAD_TYPE     = 8
//   FIELD_SAMPLE_ID    = 9
//   FIELD_CHOKE_GROUP  = 10
//   FIELD_COUNT        = 11

// ======================================================
// Public API
// ======================================================

// Inisialisasi UART1 + parser. Panggil SETELAH persistence/
// kit ini (perlu currentActiveKit/volume awal). Sebelum
// uartTaskStart(). Non-blocking.
void uartProtocolInit(
    uint8_t activeKit,
    uint8_t volume
);

// Mulai FreeRTOS UART task (control plane, Core 1, prio rendah).
void uartTaskStart();

// Jalankan kit-switch penuh yang aman (gate hit -> tunggu voice
// -> JSON/PSRAM -> rollback) LALU update uartActiveKit. Public
// supaya dipakai kit_manager (LOAD / DELETE fallback). Status =
// STATUS_OK / STATUS_BUSY / STATUS_INVALID_KIT.
uint8_t uartPerformKitSwitch(
    uint8_t newKit
);

// Slot kit yang sedang aktif di RAM (setelah boot / LOAD).
uint8_t uartGetActiveKit();

#endif // UART_PROTOCOL_H
