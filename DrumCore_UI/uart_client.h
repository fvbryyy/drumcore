#ifndef UART_CLIENT_H
#define UART_CLIENT_H

#include <Arduino.h>

// UART Commands (must match ESP32-S3 Phase 6C)
enum UartCommand : uint8_t
{
    CMD_PING           = 0x01,
    CMD_REQUEST_STATUS = 0x02,
    CMD_KIT_NEXT       = 0x10,
    CMD_KIT_PREV       = 0x11,
    CMD_KIT_LOAD       = 0x12,
    CMD_KIT_SAVE       = 0x13,
    CMD_KIT_INFO       = 0x14,
    CMD_KIT_CREATE_EMPTY = 0x15,   // TASK 3
    CMD_KIT_CREATE_COPY  = 0x16,   // TASK 3
    CMD_KIT_RENAME       = 0x17,   // TASK 3 (header + chunk nama)
    CMD_KIT_DELETE       = 0x18,   // TASK 3
    CMD_KIT_CREATE_EMPTY_NAMED = 0x19,   // TASK 4 (header + chunk nama)
    CMD_KIT_CREATE_COPY_NAMED  = 0x1A,   // TASK 4 (header + chunk nama)
    CMD_PAD_SET        = 0x20,
    CMD_PAD_GET        = 0x21,
    CMD_PAD_SOUND      = 0x22,   // daftar WAV kit aktif (scan folder)
    CMD_PAD_SOUND_SET  = 0x23,   // KIT RUNTIME: assign WAV ke pad (chunked)
    CMD_GLOBAL_SAVE    = 0x30,
    CMD_CALIBRATE      = 0x40,
    CMD_FULL_SYNC      = 0x50
};

// UART Response Status (must match ESP32-S3 Phase 6C)
enum UartStatus : uint8_t
{
    RESP_OK              = 0x00,
    RESP_INVALID_COMMAND = 0x01,
    RESP_INVALID_PAD     = 0x02,
    RESP_INVALID_VALUE   = 0x03,
    RESP_INVALID_KIT     = 0x04,
    RESP_BUSY            = 0x05,
    RESP_CRC_ERROR       = 0x06,
    RESP_NAME_EXISTS     = 0x07,
    // G24.15: nilai baru di field status yang sudah ada (ARG0 respons).
    // Frame/CRC/SEQ/daftar command tidak berubah. Dipasangkan dengan
    // STATUS_NO_SPACE (DrumCore/uart_protocol.h) = 0x08.
    RESP_NO_SPACE        = 0x08
};

// ------------------------------------------------------------
// Pad field IDs (CMD_PAD_SET / CMD_PAD_GET) — SALINAN PROTOKOL.
// Wajib sinkron dengan DrumCore/config.h (FIELD_MIDI_NOTE..FIELD_COUNT).
// Ini HANYA id protokol; batas NILAI ada di config.h + logika editor
// ui.cpp, bukan duplikasi di sini.
// ------------------------------------------------------------
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

// Response frame structure
struct UartResponse
{
    uint8_t status;
    uint8_t cmd;
    uint8_t seq;
    uint32_t arg0;
    uint32_t arg1;
    bool valid;
};

constexpr uint32_t PING_TIMEOUT_MS = 300;

// Result of a PING handshake, polled non-blocking from uiUpdate().
enum class UartPingResult : uint8_t
{
    NONE,   // no ping in flight / result not yet available
    OK,     // valid response: CMD|0x80, matching SEQ, status OK
    FAIL    // timeout or mismatched response
};

void uartClientInit();

void uartClientUpdate();

uint8_t uartSendCommand(
    uint8_t cmd,
    uint32_t arg0 = 0,
    uint32_t arg1 = 0,
    uint32_t arg2 = 0,
    uint32_t arg3 = 0
);

bool uartGetResponse(UartResponse& out);

int8_t uartFindResponse(uint8_t respCmd, uint8_t seq);

bool uartRemoveAt(int8_t absIndex, UartResponse& out);

uint8_t uartSendPing();

UartPingResult uartPollPing();

// ------------------------------------------------------------
// Kit info (CMD_KIT_INFO, TASK 1)
// ------------------------------------------------------------
//
// Wemos meminta nama + kitId sebuah kit (aktif atau preview).
// ESP32 membalas satu frame header (kitId + nameLen) lalu
// deretan frame chunk nama (3 char/frame). Asembel terjadi di
// dalam uartClientUpdate() — api ini hanya inisiasi + konsumsi.

// Panjang nama maksimum dari ESP32 (sama dengan protokol 0x14).
constexpr uint8_t KIT_NAME_MAX = 28;

// Minta info kit untuk `kitIndex`. Non-blocking; request baru
// selalu dikirim (response lama yang tidak cocok SEQ diabaikan).
// Caller bermain aman dengan memanggil hanya pada event diskrit.
void uartKitInfoRequest(uint8_t kitIndex);

// true jika ada request CMD_KIT_INFO yang masih in-flight (header/
// chunk belum lengkap atau belum di-consume). Dipakai UI supaya
// tidak mengirim request baru tiap frame saat satu request masih
// berlangsung (G13: guard anti-livelock).
bool uartKitInfoPending();

// true selama ada AKTIVITAS KIT_INFO apa pun: request menunggu header
// (pending), chunk sedang dirakit (ok=true setelah header), ATAU paket
// utuh menunggu dikonsumsi (ready). G14: *kitInfoPending* menjadi
// false segera setelah header diterima, padahal chunk belum tentu
// selesai di-drain pada tick yang sama — guard hanya-pending dapat
// membiarkan request baru lewat lalu menghancurkan rakitan chunk yang
// masih berjalan. Sesi dianggap sibuk sampai di-consume / timeout.
bool uartKitInfoBusy();

// true jika satu paket kit-info utuh sudah tiba dan belum dikonsumsi.
bool uartKitInfoAvailable();

// Ambil (consume) satu paket kit-info. Mengisi `kitIndex` (index
// yang diminta), `kitId`, dan `name` (null-terminated, max 28,
// disanitasi aman untuk LCD). return false bila paket belum lengkap,
// timeout, atau tidak ada paket. Satu sukses sah = satu pemanggilan.
bool uartKitInfoGet(
    uint8_t& kitIndex,
    uint8_t& kitId,
    char* name,
    uint8_t nameMax);

// ------------------------------------------------------------
// Kit rename (CMD_KIT_RENAME, TASK 3)
// ------------------------------------------------------------
//
// Kirim request rename kit: satu frame header (ARG0 = kit index,
// ARG1 = panjang nama) lalu deretan frame chunk nama (3 char per
// frame, ARG0 = chunkIdx | 0x80). Semua frame memakai SEQ sama,
// sehingga respon ESP32 (satu frame status) bisa dicocokkan via
// uartFindResponse(CMD_KIT_RENAME | UART_RESPONSE_BIT, seq).
// Return SEQ header; 0 jika nama kosong/telah dipotong 28.
uint8_t uartSendRenameRequest(
    uint8_t kitIndex,
    const char* name);

// ------------------------------------------------------------
// Kit create dengan nama (CMD_KIT_CREATE_EMPTY_NAMED / _COPY_NAMED,
// TASK 4 / G4)
// ------------------------------------------------------------
//
// Kirim request create bernama: satu frame header (ARG0 = 0,
// ARG1 = panjang nama) lalu deretan frame chunk nama (3 char per
// frame, ARG0 = chunkIdx | 0x80). Semua frame memakai SEQ sama —
// sama persis mekanisme rename. `cmd` = CMD_KIT_CREATE_EMPTY_NAMED
// atau CMD_KIT_CREATE_COPY_NAMED. Return SEQ header; 0 jika nama
// kosong/telah dipotong KIT_NAME_MAX.
uint8_t uartSendCreateNamedRequest(
    uint8_t cmd,
    const char* name);

// ------------------------------------------------------------
// Pad get (CMD_PAD_GET, G1) — nilai field 16-bit
// ------------------------------------------------------------
//
// ESP32 membalas SATU nilai field dengan DUA frame berturut-turut
// (SEQ sama) supaya nilai >255 tidak terpotong di frame 8-byte:
//   frame lo : ARG1=pad, ARG2=field,      ARG3=byte rendah
//   frame hi : ARG1=pad, ARG2=field|0x80, ARG3=byte tinggi
// Asembel dua frame berlangsung di dalam uartClientUpdate(); api
// ini hanya inisiasi + konsumsi satu nilai per satu request.
// Format identik dengan dump pad CMD_FULL_SYNC.

// Minta satu nilai field pad. Non-blocking; panggil hanya pada
// event diskrit (satu request aktif pada satu waktu).
uint8_t uartPadGetRequest(uint8_t padIndex, uint8_t fieldId);

// true jika satu nilai pad-get utuh sudah tiba dan belum dikonsumsi.
bool uartPadGetAvailable();

// Ambil (consume) satu nilai pad-get. Mengisi `padIndex`, `fieldId`,
// dan `value` (16-bit utuh, tidak terpotong). return false bila paket
// gagal/timeout atau tidak ada paket. Satu sukses sah = satu pemanggilan.
bool uartPadGetGet(
    uint8_t& padIndex,
    uint8_t& fieldId,
    uint16_t& value);

// ------------------------------------------------------------
// Pad set (CMD_PAD_SET, G1 backend) — tulis satu field pad
// ------------------------------------------------------------
//
// ESP32 membalas SATU frame status normal (CMD|RESP_FLAG, status di
// ARG0). Return SEQ request agar caller bisa mencocokkan via
// uartFindResponse(CMD_PAD_SET | UART_RESPONSE_BIT, seq) di dalam
// FIFO respons umum. value 16-bit dikirim ARG2=lo, ARG3=hi (sama
// persis dengan encoding PAD_GET / FULL_SYNC).
uint8_t uartPadSetRequest(
    uint8_t padIndex,
    uint8_t fieldId,
    uint16_t value);

// ------------------------------------------------------------
// Pad sound list (CMD_PAD_SOUND, KIT RUNTIME) — daftar WAV kit aktif
// ------------------------------------------------------------
//
// ESP32 membalas DAFTAR SUKARA = scan folder samples kit AKTIF
// (sorted; mapping by FILENAME). Request membawa ARG0 = index pad
// yang ingin dilihat (utk ARG3 = "current index"). Mekanisme chunk
// KIT_INFO plus satu frame nama-header per suara:
//   header      : ARG0=status, ARG1=jumlah ter-scan, ARG2=0xC7
//                 (tanda daftar), ARG3=index suara pad tsb di daftar
//                 (0xFF bila tidak ada / unassigned)
//   nama-header : ARG0=index suara (tanpa 0x80), ARG1=panjang nama
//   chunk       : ARG0=(chunkIdx|0x80), ARG1..3 = hingga 3 char nama
// Asembel nama berlangsung di dalam uartClientUpdate(); api ini hanya
// inisiasi + konsumsi satu daftar per satu request.

// Panjang nama file suara maksimum (sama dengan SAMPLE_NAME_MAX backend).
constexpr uint8_t PAD_SOUND_NAME_MAX = 32;
// Jumlah nama maksimum dalam satu daftar (cap PSRAM_MAX_SAMPLES).
constexpr uint8_t PAD_SOUND_COUNT = 12;

// Minta daftar suara kit aktif utk pad `padIndex`. Non-blocking;
// panggil hanya pada event diskrit (satu request aktif per waktu).
uint8_t uartPadSoundRequest(uint8_t padIndex);

// true jika satu paket daftar suara utuh sudah tiba / gagal & belum
// dikonsumsi.
bool uartPadSoundAvailable();

// Ambil (consume) satu paket daftar suara. Mengisi `names`
// (PAD_SOUND_COUNT x null-terminated, sanitasi aman LCD) dan
// `currentIndex` (index suara pad tsb di daftar; 0xFF bila tidak
// ada). return false bila paket gagal/timeout / tidak ada paket.
// Satu sukses sah = satu pemanggilan.
bool uartPadSoundGet(
    char names[PAD_SOUND_COUNT][PAD_SOUND_NAME_MAX + 1],
    uint8_t& currentIndex);

// ------------------------------------------------------------
// Pad sound set (CMD_PAD_SOUND_SET, KIT RUNTIME) — assign WAV
// ------------------------------------------------------------
//
// Assign WAV dari folder samples kit AKTIF ke sebuah pad (RAM-only
// sampai KIT SAVE). Request chunked identik RENAME/CREATE_NAMED:
//   header: ARG0=index pad, ARG1=panjang nama file
//   chunk : ARG0=(chunkIdx|0x80), ARG1..3 = hingga 3 char nama
// Semua frame memakai SEQ header. ESP32 membalas SATU frame status
// normal -> dicocokkan via uartFindResponse(CMD_PAD_SOUND_SET |
// UART_RESPONSE_BIT, seq). Return SEQ; 0 jika nama kosong / melebihi
// PAD_SOUND_NAME_MAX.
uint8_t uartPadSoundSetRequest(
    uint8_t padIndex,
    const char* name);

#endif
