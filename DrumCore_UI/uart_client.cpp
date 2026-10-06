#include "uart_client.h"
#include "config.h"
#include <HardwareSerial.h>

// UART0 is the ONLY ESP8266 hardware UART with RX support
// (GPIO1=TX, GPIO3=RX). UART1 is TX-only and unused here.
// No Serial/Serial1 debug output to avoid any conflict with the
// UART0 link to the ESP32. Status is surfaced via the LCD only.
static HardwareSerial& UART = Serial;   // UART0 -> ESP32 link

// Parser state machine
enum class ParseState : uint8_t
{
    WAIT_SOF,
    READ_CMD,
    READ_ARG0,
    READ_ARG1,
    READ_ARG2,
    READ_ARG3,
    READ_SEQ,
    READ_CRC
};

static ParseState parseState = ParseState::WAIT_SOF;
static uint32_t lastByteMs = 0;

static uint8_t frameBuf[UART_FRAME_SIZE];
static uint8_t frameIdx = 0;

// Response ring buffer (FIFO). Multiple responses (PING, STATUS, KIT_*)
// can arrive and must not overwrite each other. Each valid frame is queued;
// uartGetResponse() pops in FIFO order.
static constexpr uint8_t RESP_QUEUE_SIZE = 8;
static UartResponse respQueue[RESP_QUEUE_SIZE];
static uint8_t respHead = 0;   // next read index
static uint8_t respTail = 0;   // next write index
static uint8_t respCount = 0;

// Sequence counter
static uint8_t seqCounter = 0;

// Ping tracking
static bool pingPending = false;
static uint32_t pingSentMs = 0;
static uint8_t lastPingSeq = 0;

// Kit info (CMD_KIT_INFO) assembly state.
// Header frame  -> kitId + nameLen (mengaktifkan asembel).
// Chunk frames  -> 3 char nama per frame, ditandai bit 0x80 di ARG0.
// Pencocokan SEQ menjaga request lama yang terlambat tidak mencemari
// asembel request baru.
static constexpr uint32_t KIT_INFO_TIMEOUT_MS = 2000;

// G15 TEMP DEBUG (hardware audit CMD_KIT_INFO) — transition-only,
// hapus setelah sumber masalah terbukti.
static constexpr bool KIT_INFO_DEBUG = true;

static bool kitInfoPending = false;   // request dikirim, header belum diterima
static bool kitInfoOk = false;        // header valid sudah diterima
static bool kitInfoReady = false;     // paket utuh sudah terkumpul
static uint8_t kitInfoReqIdx = 0;     // index yang diminta
static uint8_t kitInfoSeq = 0;        // SEQ request yang sedang diproses
static uint32_t kitInfoSentMs = 0;    // timestamp kirim request
static uint8_t kitInfoId = 0;         // kitId dari header
static uint8_t kitInfoNameLen = 0;    // panjang nama dari header
static char kitInfoName[KIT_NAME_MAX + 1];
static uint8_t kitInfoGot = 0;        // char nama yang sudah diterima
static uint8_t kitInfoNextChunk = 0;  // chunk berikutnya yang diizinkan

// Pad get (CMD_PAD_GET, G1) assembly state.
// Satu nilai field dikirim ESP32 sebagai DUA frame (lo lalu hi) dengan
// SEQ sama. Asembel selesai saat frame hi cocok dengan frame lo; SEQ
// request dicocokkan supaya request lama yang terlambat tidak mencemari.
// Dua frame = satu request (satu request aktif pada satu waktu).
static constexpr uint32_t PAD_GET_TIMEOUT_MS = 2000;

static bool padGetPending = false;    // request dikirim, belum selesai
static bool padGetHaveLo = false;     // frame lo sudah diterima
static bool padGetOk = false;         // hasil valid
static bool padGetReady = false;      // nilai utuh siap dikonsumsi
static uint8_t padGetReqPad = 0;      // pad yang diminta
static uint8_t padGetReqField = 0;    // field yang diminta
static uint8_t padGetSeq = 0;         // SEQ request yang sedang diproses
static uint32_t padGetSentMs = 0;     // timestamp kirim request
static uint8_t padGetLo = 0;          // byte rendah (frame lo)
static uint8_t padGetHi = 0;          // byte tinggi (frame hi)

// Pad sound list (CMD_PAD_SOUND, G9) assembly state.
// Satu daftar = frame header (jumlah suara) lalu per-suara: satu
// frame nama-header (index + panjang) + frame chunk (3 char/frame).
// Semua frame memakai SEQ request yang dikirim (padSoundSeq); request
// lama yang terlambat diabaikan lewat pencocokan SEQ. Timeout membatalkan
// paket -> uartPadSoundGet() mengonsumsi "gagal".
static constexpr uint32_t PAD_SOUND_TIMEOUT_MS = 2500;

// G20 TEMP DEBUG — hardware trace PAD_SOUND. Matikan (false) setelah
// sumber masalah terbukti di hardware.
static constexpr bool PAD_SOUND_DEBUG = true;

static bool padSoundPending = false;   // request dikirim, header belum lengkap
static bool padSoundHasName = false;   // sedang merakit nama aktif
static bool padSoundOk = false;        // daftar lengkap & valid
static bool padSoundReady = false;     // daftar utuh siap dikonsumsi
static uint8_t padSoundSeq = 0;        // SEQ request yang sedang diproses
static uint32_t padSoundSentMs = 0;    // timestamp kirim request
static uint8_t padSoundCount = 0;      // jumlah nama (dari header daftar)
static uint8_t padSoundIdx = 0;        // nama yang sedang dirakit
static uint8_t padSoundNameLen = 0;    // panjang nama aktif
static uint8_t padSoundGot = 0;        // char nama aktif yang sudah diterima
static uint8_t padSoundCurrent = 0xFF; // index suara pad tsb di daftar (ARG3)
static char padSoundNames[PAD_SOUND_COUNT][PAD_SOUND_NAME_MAX + 1];

// Stats
static uint32_t txCount = 0;
static uint32_t rxCount = 0;
static uint32_t crcErrors = 0;

static void responsePush(const UartResponse& r)
{
    respQueue[respTail] = r;
    respTail = (respTail + 1) % RESP_QUEUE_SIZE;

    if (respCount == RESP_QUEUE_SIZE)
    {
        // Queue full: drop the oldest entry to make room.
        respHead = (respHead + 1) % RESP_QUEUE_SIZE;
    }
    else
    {
        respCount++;
    }
}

// Remove the entry at the given absolute ring index (a value previously
// returned as head+offset). Compacts the ring into a contiguous buffer so
// head/tail stay simple.
static void responseRemoveAt(uint8_t index)
{
    static UartResponse tmp[RESP_QUEUE_SIZE];

    uint8_t out = 0;
    uint8_t i = respHead;
    for (uint8_t n = 0; n < respCount; n++)
    {
        if (i != index)
        {
            tmp[out++] = respQueue[i];
        }
        i = (i + 1) % RESP_QUEUE_SIZE;
    }

    respCount = out;
    respHead = 0;
    respTail = out;
    for (uint8_t k = 0; k < respCount; k++)
    {
        respQueue[k] = tmp[k];
    }
}

static void responseClear()
{
    respHead = 0;
    respTail = 0;
    respCount = 0;
}

static uint8_t calcCRC(
    uint8_t cmd,
    uint8_t arg0,
    uint8_t arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint8_t seq)
{
    return cmd ^ arg0 ^ arg1 ^ arg2 ^ arg3 ^ seq;
}

static void processFrame(const uint8_t* frame);   // forward-decl

// Proses frame CMD_KIT_INFO (header + chunk nama). Dipanggil dari
// processFrame sebelum frame tersebut masuk FIFO respons umum.
static void handleKitInfoFrame(
    uint8_t arg0,
    uint8_t arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint8_t seq)
{
    // Header: ARG0 = status (bit 0x80 clear)
    // Chunk : ARG0 = chunkIdx | 0x80
    const bool isChunk = (arg0 & 0x80) != 0;

    if (!isChunk)
    {
        if (!kitInfoPending || seq != kitInfoSeq)
        {
            if constexpr (KIT_INFO_DEBUG)
            {
                Serial.printf("[KIT UI] KIT_INFO HEADER REJECT idx=%u seq=%u pending=%u\n",
                              (unsigned)kitInfoReqIdx,
                              (unsigned)seq,
                              (unsigned)kitInfoPending);
            }
            return;
        }

        kitInfoPending = false;
        kitInfoOk = false;
        kitInfoReady = false;
        kitInfoNextChunk = 0;
        kitInfoGot = 0;
        kitInfoNameLen = 0;
        kitInfoId = 0;
        memset(kitInfoName, 0, sizeof(kitInfoName));

        if (arg0 == RESP_OK && arg2 > 0)
        {
            kitInfoOk = true;
            kitInfoId = arg1;
            kitInfoNameLen = arg2;
            if (kitInfoNameLen > KIT_NAME_MAX)
            {
                kitInfoNameLen = KIT_NAME_MAX;
            }

            if constexpr (KIT_INFO_DEBUG)
            {
                Serial.printf("[KIT UI] KIT_INFO HEADER idx=%u seq=%u len=%u\n",
                              (unsigned)kitInfoReqIdx,
                              (unsigned)seq,
                              (unsigned)kitInfoNameLen);
            }
        }
        else if constexpr (KIT_INFO_DEBUG)
        {
            Serial.printf("[KIT UI] KIT_INFO HEADER REJECT idx=%u seq=%u status=%u len=%u\n",
                          (unsigned)kitInfoReqIdx,
                          (unsigned)seq,
                          (unsigned)arg0,
                          (unsigned)arg2);
        }
        return;
    }

    if (!kitInfoOk || kitInfoReady || seq != kitInfoSeq)
    {
        return;
    }

    const uint8_t idx = arg0 & 0x7F;
    if (idx != kitInfoNextChunk)
    {
        return;
    }

    const uint16_t pos = (uint16_t)idx * 3U;
    if (pos >= kitInfoNameLen || pos >= sizeof(kitInfoName) - 1U)
    {
        return;
    }

    const uint8_t c[3] = { arg1, arg2, arg3 };
    char chunkData[4] = { 0 };
    uint8_t validChars = (uint8_t)(kitInfoNameLen - pos);
    if (validChars > 3)
    {
        validChars = 3;
    }

    for (uint8_t i = 0; i < validChars; i++)
    {
        uint8_t ch = c[i];
        if (ch < 0x20)
        {
            ch = ' ';
        }
        chunkData[i] = (char)ch;
        kitInfoName[pos + i] = (char)ch;
    }

    kitInfoNextChunk++;
    kitInfoGot = (uint8_t)(pos + validChars);

    if constexpr (KIT_INFO_DEBUG)
    {
        Serial.printf("[KIT UI] KIT_INFO CHUNK idx=%u chunk=%u data='%s'\n",
                      (unsigned)kitInfoReqIdx,
                      (unsigned)idx,
                      chunkData);
    }

    if (kitInfoGot >= kitInfoNameLen)
    {
        kitInfoName[kitInfoNameLen] = '\0';
        kitInfoPending = false;
        kitInfoReady = true;

        if constexpr (KIT_INFO_DEBUG)
        {
            Serial.printf("[KIT UI] KIT_INFO COMPLETE idx=%u name='%s'\n",
                          (unsigned)kitInfoReqIdx,
                          kitInfoName);
        }
    }
}

// Proses frame CMD_PAD_GET (respon). Nilai field 16-bit dikirim sebagai
// dua frame berurutan: frame lo (ARG2 = field) lalu frame hi (ARG2 =
// field | 0x80). Dipanggil dari processFrame sebelum frame masuk FIFO
// respons umum, supaya lalu lintas pad config tidak mengganggu transaksi
// KIT_*/STATUS yang sedang berjalan.
static void handlePadGetFrame(
    uint8_t arg0,   // status
    uint8_t arg1,   // pad
    uint8_t arg2,   // field (frame hi: field | 0x80)
    uint8_t arg3,   // value byte
    uint8_t seq)
{
    if (!padGetPending || seq != padGetSeq)
    {
        return;   // respon kedaluwarsa / request lama / noise
    }

    if (arg0 != RESP_OK)
    {
        // Request tertolak (mis. INVALID_PAD) -> paket gagal.
        padGetPending = false;
        padGetOk = false;
        padGetReady = true;
        return;
    }

    const bool isHigh = (arg2 & 0x80) != 0;
    const uint8_t field = arg2 & 0x7F;

    if (arg1 != padGetReqPad || field != padGetReqField)
    {
        return;   // pad/field bukan milik request ini
    }

    if (!isHigh)
    {
        padGetLo = arg3;
        padGetHaveLo = true;
        return;
    }

    if (!padGetHaveLo)
    {
        return;   // frame hi tanpa frame lo (out of order) -> ignore
    }

    padGetHi = arg3;
    padGetHaveLo = false;
    padGetOk = true;
    padGetPending = false;
    padGetReady = true;
}

// Proses frame CMD_PAD_SOUND (respon daftar suara, G9). Satu daftar:
//   header daftar : ARG0=status, ARG1=jumlah suara, ARG2=0xC7
//   nama-header   : ARG0=index suara, ARG1=panjang nama, ARG2=0
//   chunk         : ARG0=(chunkIdx|0x80), ARG1..3 = sampai 3 char
// Dipanggil dari processFrame sebelum frame masuk FIFO respons umum,
// supaya lalu lintas daftar suara tidak mengganggu transaksi lain.
static void handlePadSoundFrame(
    uint8_t arg0,
    uint8_t arg1,
    uint8_t arg2,
    uint8_t arg3,
    uint8_t seq)
{
    if (!padSoundPending || seq != padSoundSeq)
    {
        return;   // respon kedaluwarsa / request lama / noise
    }

    const bool isChunk = (arg0 & 0x80) != 0;

    if (isChunk)
    {
        // Chunk nama hanya valid saat sedang ada nama aktif.
        if (!padSoundHasName)
        {
            return;
        }

        const uint8_t idx = arg0 & 0x7F;
        const uint8_t pos = (uint8_t)(idx * 3U);

        if (pos >= padSoundNameLen)
        {
            return;
        }

        const uint8_t c[3] = { arg1, arg2, arg3 };
        char* dst = padSoundNames[padSoundIdx];

        // Jumlah karakter efektif di chunk ini (1-3, bukan selalu 3).
        // G20 FIX #2: gunakan sisa karakter nama bukan konstanta 3,
        // identik dengan pola KIT_INFO (kitInfoGot = pos + validChars).
        uint8_t validChars = (uint8_t)(padSoundNameLen - pos);
        if (validChars > 3)
        {
            validChars = 3;
        }

        for (uint8_t i = 0; i < validChars; i++)
        {
            if (pos + i >= PAD_SOUND_NAME_MAX)
            {
                break;
            }

            uint8_t ch = c[i];
            if (ch < 0x20)
            {
                ch = ' ';   // sanitasi LCD (byte kontrol aman)
            }
            dst[pos + i] = (char)ch;
        }

        // G20 FIX #2: got = posisi + karakter efektif (bukan +=3).
        padSoundGot = (uint8_t)(pos + validChars);

        if constexpr (PAD_SOUND_DEBUG)
        {
            Serial.printf("[SOUND UI] RX CHUNK idx=%u chunk=%u got=%u/%u\n",
                          (unsigned)padSoundIdx,
                          (unsigned)idx,
                          (unsigned)padSoundGot,
                          (unsigned)padSoundNameLen);
        }

        if (padSoundGot >= padSoundNameLen)
        {
            if (padSoundNameLen < PAD_SOUND_NAME_MAX)
            {
                padSoundNames[padSoundIdx][padSoundNameLen] = '\0';
            }
            else
            {
                padSoundNames[padSoundIdx][PAD_SOUND_NAME_MAX] = '\0';
            }
            padSoundHasName = false;

            if constexpr (PAD_SOUND_DEBUG)
            {
                Serial.printf("[SOUND UI] COMPLETE idx=%u name='%s'\n",
                              (unsigned)padSoundIdx,
                              padSoundNames[padSoundIdx]);
            }

            padSoundIdx++;

            if (padSoundIdx >= padSoundCount)
            {
                padSoundOk = true;
                padSoundPending = false;
                padSoundReady = true;

                if constexpr (PAD_SOUND_DEBUG)
                {
                    Serial.printf("[SOUND UI] SESSION COMPLETE count=%u\n",
                                  (unsigned)padSoundIdx);
                }
            }
        }
        return;
    }

    // ---- Header (bukan chunk) ----

    // Header daftar ditandai ARG2 == 0xC7 (tanda khusus; kontras
    // dengan nama-header yang memakai ARG2 = 0).
    if (arg2 == 0xC7)
    {
        // Reset daftar. Hanya valid bila belum ada nama mulai dirakit.
        padSoundCount = arg1;
        if (padSoundCount > PAD_SOUND_COUNT)
        {
            padSoundCount = PAD_SOUND_COUNT;
        }
        padSoundIdx = 0;
        padSoundHasName = false;
        padSoundCurrent = arg3;   // index suara pad tsb (0xFF = tak ada)
        memset(padSoundNames, 0, sizeof(padSoundNames));

        if constexpr (PAD_SOUND_DEBUG)
        {
            Serial.printf("[SOUND UI] RX HEADER count=%u current=%u\n",
                          (unsigned)padSoundCount,
                          (unsigned)padSoundCurrent);
        }

        // G20 FIX #1: count == 0 berarti daftar kosong — sesi selesai
        // sekarang, tanpa menunggu nama-header / chunk yang tidak ada.
        // Tanpa fix ini padSoundPending tetap true dan sistem timeout
        // 2500ms padahal ESP32 sudah merespons dengan benar.
        if (padSoundCount == 0)
        {
            padSoundOk = true;
            padSoundPending = false;
            padSoundReady = true;

            if constexpr (PAD_SOUND_DEBUG)
            {
                Serial.printf("[SOUND UI] SESSION COMPLETE count=0 (empty)\n");
            }
        }
        return;
    }

    // ---- Nama-header ----
    if (padSoundIdx >= padSoundCount)
    {
        return;   // semua nama sudah lengkap; frame ekstra diabaikan
    }
    if (arg0 != padSoundIdx)
    {
        return;   // index tidak berurutan -> daftar rusak, tunggu timeout
    }

    padSoundNameLen = arg1;
    if (padSoundNameLen > PAD_SOUND_NAME_MAX)
    {
        padSoundNameLen = PAD_SOUND_NAME_MAX;
    }
    padSoundGot = 0;
    padSoundHasName = true;

    if constexpr (PAD_SOUND_DEBUG)
    {
        Serial.printf("[SOUND UI] RX NAME HEADER idx=%u len=%u\n",
                      (unsigned)padSoundIdx,
                      (unsigned)padSoundNameLen);
    }

    // Nama kosong (suara tidak terresolve): tanpa chunk, langsung
    // lanjut ke suara berikutnya.
    if (padSoundNameLen == 0)
    {
        padSoundHasName = false;
        padSoundIdx++;

        if (padSoundIdx >= padSoundCount)
        {
            padSoundOk = true;
            padSoundPending = false;
            padSoundReady = true;

            if constexpr (PAD_SOUND_DEBUG)
            {
                Serial.printf("[SOUND UI] SESSION COMPLETE count=%u\n",
                              (unsigned)padSoundIdx);
            }
        }
    }
}

static void processFrame(const uint8_t* frame)
{
    uint8_t sof   = frame[0];
    uint8_t cmd   = frame[1];
    uint8_t arg0  = frame[2];
    uint8_t arg1  = frame[3];
    uint8_t arg2  = frame[4];
    uint8_t arg3  = frame[5];
    uint8_t seq   = frame[6];
    uint8_t crc   = frame[7];

    if (sof != UART_SOF)
    {
        return;
    }

    uint8_t expectedCRC = calcCRC(cmd, arg0, arg1, arg2, arg3, seq);

    if (crc != expectedCRC)
    {
        crcErrors++;
        return;
    }

    // Kit-info frames (header + chunk nama) dirakit khusus; tidak
    // masuk ke FIFO respons umum supaya tidak mengganggu transaksi
    // KIT_NEXT/PREV/LOAD/SAVE yang sedang berjalan.
    if (cmd == (CMD_KIT_INFO | UART_RESPONSE_BIT))
    {
        handleKitInfoFrame(arg0, arg1, arg2, arg3, seq);
        return;
    }

    // CMD_PAD_GET respon (nilai field 16-bit, dua frame per nilai)
    // dirakit khusus di luar FIFO respons umum (lihat handlePadGetFrame).
    if (cmd == (CMD_PAD_GET | UART_RESPONSE_BIT))
    {
        handlePadGetFrame(arg0, arg1, arg2, arg3, seq);
        return;
    }

    // CMD_PAD_SOUND respon (daftar suara kit aktif, G9) dirakit
    // khusus di luar FIFO respons umum (lihat handlePadSoundFrame).
    if (cmd == (CMD_PAD_SOUND | UART_RESPONSE_BIT))
    {
        handlePadSoundFrame(arg0, arg1, arg2, arg3, seq);
        return;
    }

    // Response frame: CMD is the request CMD with UART_RESPONSE_BIT set
    // (e.g. PING 0x01 -> response CMD 0x81).
    // Field layout (ESP32 sendResponse): SOF, CMD|0x80, ARG0=STATUS,
    // ARG1=a1, ARG2=a2, ARG3=a3, SEQ, CRC.
    //   f[2]=status, f[3]=arg0, f[4]=arg1, f[5]=arg2, f[6]=seq, f[1]=cmd.
    UartResponse r;
    r.status = arg0;          // f[2] status
    r.cmd    = cmd;           // f[1] cmd
    r.seq    = seq;           // f[6] seq
    r.arg0   = arg1;          // f[3] ARG1=a1 (e.g. kit index)
    r.arg1   = arg2;          // f[4] ARG2=a2
    r.valid  = true;

    responsePush(r);
    rxCount++;
}

static uint8_t sendFrameSeq(
    uint8_t cmd,
    uint32_t arg0,
    uint32_t arg1,
    uint32_t arg2,
    uint32_t arg3,
    uint8_t seq)
{
    uint8_t a0 = (uint8_t)(arg0 & 0xFF);
    uint8_t a1 = (uint8_t)(arg1 & 0xFF);
    uint8_t a2 = (uint8_t)(arg2 & 0xFF);
    uint8_t a3 = (uint8_t)(arg3 & 0xFF);

    uint8_t crc = calcCRC(cmd, a0, a1, a2, a3, seq);

    uint8_t frame[UART_FRAME_SIZE];
    frame[0] = UART_SOF;
    frame[1] = cmd;
    frame[2] = a0;
    frame[3] = a1;
    frame[4] = a2;
    frame[5] = a3;
    frame[6] = seq;
    frame[7] = crc;

    UART.write(frame, UART_FRAME_SIZE);
    txCount++;
    return seq;
}

static uint8_t sendFrame(
    uint8_t cmd,
    uint32_t arg0 = 0,
    uint32_t arg1 = 0,
    uint32_t arg2 = 0,
    uint32_t arg3 = 0)
{
    return sendFrameSeq(cmd, arg0, arg1, arg2, arg3, seqCounter++);
}

void uartClientInit()
{
    UART.begin(UART_BAUD);

    parseState = ParseState::WAIT_SOF;
    frameIdx = 0;
    seqCounter = 0;
    responseClear();

    pingPending = false;
    pingSentMs = 0;
    lastPingSeq = 0;

    kitInfoPending = false;
    kitInfoOk = false;
    kitInfoReady = false;
    kitInfoReqIdx = 0;
    kitInfoSeq = 0;
    kitInfoSentMs = 0;
    kitInfoId = 0;
    kitInfoNameLen = 0;
    kitInfoGot = 0;
    kitInfoNextChunk = 0;
    memset(kitInfoName, 0, sizeof(kitInfoName));

    padGetPending = false;
    padGetHaveLo = false;
    padGetOk = false;
    padGetReady = false;
    padGetReqPad = 0;
    padGetReqField = 0;
    padGetSeq = 0;
    padGetSentMs = 0;
    padGetLo = 0;
    padGetHi = 0;

    padSoundPending = false;
    padSoundHasName = false;
    padSoundOk = false;
    padSoundReady = false;
    padSoundSeq = 0;
    padSoundSentMs = 0;
    padSoundCount = 0;
    padSoundIdx = 0;
    padSoundNameLen = 0;
    padSoundGot = 0;
    padSoundCurrent = 0xFF;
    memset(padSoundNames, 0, sizeof(padSoundNames));
}

void uartClientUpdate()
{
    uint32_t now = millis();

    // Timeout kit-info: header atau chunk tidak kunjung lengkap
    // (ESP32 sibuk / frame dropped). Paket dibatalkan; UI boleh
    // meminta ulang tanpa menghasilkan paket ready palsu.
    if ((kitInfoPending || (kitInfoOk && !kitInfoReady)) &&
        (now - kitInfoSentMs >= KIT_INFO_TIMEOUT_MS))
    {
        const uint8_t reqIdx = kitInfoReqIdx;
        const uint8_t seq = kitInfoSeq;
        kitInfoPending = false;
        kitInfoOk = false;
        kitInfoReady = false;
        kitInfoNameLen = 0;
        kitInfoGot = 0;
        kitInfoNextChunk = 0;
        kitInfoId = 0;
        memset(kitInfoName, 0, sizeof(kitInfoName));

        if constexpr (KIT_INFO_DEBUG)
        {
            Serial.printf("[KIT UI] KIT_INFO TIMEOUT idx=%u seq=%u\n",
                          (unsigned)reqIdx,
                          (unsigned)seq);
        }
    }

    // Timeout pad-get: frame lo/hi tidak kunjung lengkap (ESP32
    // sibuk / frame dropped). Paket dibatalkan; UI boleh meminta ulang.
    // uartPadGetGet() lalu mengonsumsi "gagal".
    if (padGetPending && (now - padGetSentMs >= PAD_GET_TIMEOUT_MS))
    {
        padGetPending = false;
        padGetOk = false;
        padGetReady = true;
    }

    // Timeout pad-sound: daftar suara tidak kunjung lengkap (ESP32
    // sibuk / frame dropped). Paket dibatalkan; UI boleh meminta ulang.
    // uartPadSoundGet() lalu mengonsumsi "gagal".
    if (padSoundPending && (now - padSoundSentMs >= PAD_SOUND_TIMEOUT_MS))
    {
        padSoundPending = false;
        padSoundOk = false;
        padSoundReady = true;

        if constexpr (PAD_SOUND_DEBUG)
        {
            Serial.printf("[SOUND UI] TIMEOUT seq=%u count=%u got=%u/%u\n",
                          (unsigned)padSoundSeq,
                          (unsigned)padSoundCount,
                          (unsigned)padSoundIdx,
                          (unsigned)padSoundCount);
        }
    }

    while (UART.available())
    {
        uint8_t b = UART.read();

        if (parseState != ParseState::WAIT_SOF)
        {
            if (now - lastByteMs > 10)
            {
                parseState = ParseState::WAIT_SOF;
                frameIdx = 0;
            }
        }
        lastByteMs = now;

        switch (parseState)
        {
            case ParseState::WAIT_SOF:
                if (b == UART_SOF)
                {
                    frameBuf[0] = b;
                    frameIdx = 1;
                    parseState = ParseState::READ_CMD;
                }
                break;

            case ParseState::READ_CMD:
                frameBuf[1] = b;
                parseState = ParseState::READ_ARG0;
                break;

            case ParseState::READ_ARG0:
                frameBuf[2] = b;
                parseState = ParseState::READ_ARG1;
                break;

            case ParseState::READ_ARG1:
                frameBuf[3] = b;
                parseState = ParseState::READ_ARG2;
                break;

            case ParseState::READ_ARG2:
                frameBuf[4] = b;
                parseState = ParseState::READ_ARG3;
                break;

            case ParseState::READ_ARG3:
                frameBuf[5] = b;
                parseState = ParseState::READ_SEQ;
                break;

            case ParseState::READ_SEQ:
                frameBuf[6] = b;
                parseState = ParseState::READ_CRC;
                break;

            case ParseState::READ_CRC:
                frameBuf[7] = b;
                processFrame(frameBuf);
                parseState = ParseState::WAIT_SOF;
                frameIdx = 0;
                break;
        }
    }

    if (pingPending && (now - pingSentMs >= PING_TIMEOUT_MS))
    {
        pingPending = false;
    }
}

uint8_t uartSendCommand(
    uint8_t cmd,
    uint32_t arg0,
    uint32_t arg1,
    uint32_t arg2,
    uint32_t arg3)
{
    return sendFrame(cmd, arg0, arg1, arg2, arg3);
}

bool uartGetResponse(UartResponse& out)
{
    if (respCount == 0)
    {
        return false;
    }

    out = respQueue[respHead];
    respHead = (respHead + 1) % RESP_QUEUE_SIZE;
    respCount--;
    return true;
}

int8_t uartFindResponse(uint8_t respCmd, uint8_t seq)
{
    uint8_t index = respHead;
    for (uint8_t n = 0; n < respCount; n++)
    {
        if (respQueue[index].cmd == respCmd &&
            respQueue[index].seq == seq)
        {
            return (int8_t)index;
        }
        index = (index + 1) % RESP_QUEUE_SIZE;
    }
    return -1;
}

bool uartRemoveAt(int8_t absIndex, UartResponse& out)
{
    if (absIndex < 0 || respCount == 0)
    {
        return false;
    }
    uint8_t idx = (uint8_t)absIndex;
    // Verify the index is within the valid range of the ring
    uint8_t check = respHead;
    for (uint8_t n = 0; n < respCount; n++)
    {
        if (check == idx)
        {
            out = respQueue[idx];
            responseRemoveAt(idx);
            return true;
        }
        check = (check + 1) % RESP_QUEUE_SIZE;
    }
    return false;
}

uint8_t uartSendPing()
{
    uint8_t seq = sendFrame(CMD_PING);
    pingPending = true;
    pingSentMs = millis();
    lastPingSeq = seq;
    return seq;
}

UartPingResult uartPollPing()
{
    if (!pingPending)
    {
        return UartPingResult::NONE;
    }

    uint32_t now = millis();

    if (now - pingSentMs >= PING_TIMEOUT_MS)
    {
        pingPending = false;
        return UartPingResult::FAIL;
    }

    // Scan the queue for a PING response. Command responses (KIT/STATUS...)
    // are left intact so uartGetResponse() still receives them. Only a PING
    // response is consumed, and only when its SEQ/STATUS match.
    uint8_t index = respHead;
    for (uint8_t n = 0; n < respCount; n++)
    {
        if (respQueue[index].cmd == (CMD_PING | UART_RESPONSE_BIT))
        {
            UartResponse pingResp = respQueue[index];
            responseRemoveAt(index);
            pingPending = false;

            if (pingResp.seq   == lastPingSeq &&
                pingResp.status == RESP_OK)
            {
                return UartPingResult::OK;
            }
            return UartPingResult::FAIL;
        }
        index = (index + 1) % RESP_QUEUE_SIZE;
    }

    return UartPingResult::NONE;
}

// ============================================================
// Kit info (CMD_KIT_INFO)
// ============================================================

void uartKitInfoRequest(uint8_t kitIndex)
{
    // Selalu kirim request baru (SEQ baru). Header/chunk dari request
    // lama yang terlambat diabaikan lewat pencocokan SEQ di atas.
    kitInfoPending = true;
    kitInfoOk = false;
    kitInfoReady = false;
    kitInfoReqIdx = kitIndex;
    kitInfoId = 0;
    kitInfoNameLen = 0;
    kitInfoGot = 0;
    kitInfoNextChunk = 0;
    memset(kitInfoName, 0, sizeof(kitInfoName));

    kitInfoSeq = sendFrame(CMD_KIT_INFO, kitIndex);
    kitInfoSentMs = millis();

    if constexpr (KIT_INFO_DEBUG)
    {
        Serial.printf("[KIT UI] request KIT_INFO idx=%u seq=%u\n",
                      (unsigned)kitIndex, (unsigned)kitInfoSeq);
    }
}

bool uartKitInfoPending()
{
    return kitInfoPending;
}

// G14: sesi KIT_INFO sibuk = header belum (pending) ATAU asembel chunk
// masih jalan (ok) ATAU paket utuh menunggu dikonsumsi (ready). Guard
// ini menutup celah "header sudah tiba tapi chunk terpecah drain": dulu
// request baru bisa lewat saat pending==false && ok==true lalu membuang
// rakitan yang sedang berjalan.
bool uartKitInfoBusy()
{
    return kitInfoPending || kitInfoOk || kitInfoReady;
}

bool uartKitInfoAvailable()
{
    return kitInfoReady && kitInfoOk;
}

bool uartKitInfoGet(
    uint8_t& kitIndex,
    uint8_t& kitId,
    char* name,
    uint8_t nameMax)
{
    if (!kitInfoReady)
    {
        return false;
    }

    const bool success = kitInfoOk &&
                         kitInfoNameLen > 0 &&
                         name != nullptr &&
                         nameMax > 0;
    const uint8_t reqIdx = kitInfoReqIdx;
    const uint8_t id = kitInfoId;
    uint8_t n = kitInfoNameLen;

    kitInfoReady = false;
    kitInfoPending = false;
    kitInfoOk = false;
    kitInfoNextChunk = 0;
    kitInfoGot = 0;
    kitInfoNameLen = 0;

    if (!success)
    {
        return false;
    }

    kitIndex = reqIdx;
    kitId = id;

    if (n >= nameMax)
    {
        n = nameMax - 1;
    }
    for (uint8_t i = 0; i < n; i++)
    {
        name[i] = kitInfoName[i];
    }
    name[n] = '\0';

    return true;
}

// ============================================================
// Kit rename (CMD_KIT_RENAME)
// ============================================================

// Kirim header + chunk nama (mekanisme sama utk RENAME, CREATE
// bernama, dan PAD_SOUND_SET). Semua frame memakai SEQ header
// supaya respon ESP32 (satu frame status) bisa dicocokkan via
// uartFindResponse(cmd, seq). maxLen = batas nama request tsb
// (KIT_NAME_MAX utk rename/create, PAD_SOUND_NAME_MAX utk sample).
static uint8_t sendNameChunked(
    uint8_t cmd,
    uint32_t headerArg0,
    const char* name,
    uint8_t maxLen)
{
    if (!name)
    {
        return 0;
    }

    uint8_t len = 0;
    while (name[len] != '\0' && len < maxLen)
    {
        len++;
    }

    if (len == 0)
    {
        return 0;
    }

    uint8_t seq = seqCounter++;
    sendFrameSeq(cmd, headerArg0, len, 0, 0, seq);

    uint8_t chunk = 0;
    while (chunk * 3 < len)
    {
        uint8_t a1 = name[chunk * 3 + 0];
        uint8_t a2 = (chunk * 3 + 1 < len) ? name[chunk * 3 + 1] : 0;
        uint8_t a3 = (chunk * 3 + 2 < len) ? name[chunk * 3 + 2] : 0;

        sendFrameSeq(
            cmd,
            (uint8_t)(chunk | 0x80),
            a1,
            a2,
            a3,
            seq);
        chunk++;
    }

    return seq;
}

uint8_t uartSendRenameRequest(uint8_t kitIndex, const char* name)
{
    // ARG0 = slot kit (index subjek rename).
    return sendNameChunked(CMD_KIT_RENAME, kitIndex, name, KIT_NAME_MAX);
}

// ============================================================
// Kit create dengan nama (G4)
// ============================================================

uint8_t uartSendCreateNamedRequest(uint8_t cmd, const char* name)
{
    // ARG0 header = 0 (cadangan; slot dipilih ESP32 secara internal).
    return sendNameChunked(cmd, 0, name, KIT_NAME_MAX);
}

// ============================================================
// Pad get (CMD_PAD_GET, G1)
// ============================================================

uint8_t uartPadGetRequest(uint8_t padIndex, uint8_t fieldId)
{
    // Mulai request baru (SEQ baru). Frame respon lama yang terlambat
    // diabaikan lewat pencocokan SEQ di handlePadGetFrame.
    padGetPending = true;
    padGetHaveLo = false;
    padGetOk = false;
    padGetReady = false;
    padGetReqPad = padIndex;
    padGetReqField = fieldId;
    padGetLo = 0;
    padGetHi = 0;

    padGetSeq = sendFrame(CMD_PAD_GET, padIndex, fieldId);
    padGetSentMs = millis();
    return padGetSeq;
}

bool uartPadGetAvailable()
{
    return padGetReady;
}

bool uartPadGetGet(
    uint8_t& padIndex,
    uint8_t& fieldId,
    uint16_t& value)
{
    if (!padGetReady)
    {
        return false;
    }

    // Consume (satu sukses = satu pengambilan).
    padGetReady = false;
    padGetPending = false;

    if (!padGetOk)
    {
        return false;
    }

    padIndex = padGetReqPad;
    fieldId = padGetReqField;
    value = (uint16_t)(padGetLo | ((uint16_t)padGetHi << 8));
    return true;
}

// ============================================================
// Pad set (CMD_PAD_SET)
// ============================================================

uint8_t uartPadSetRequest(
    uint8_t padIndex,
    uint8_t fieldId,
    uint16_t value)
{
    return sendFrame(
        CMD_PAD_SET,
        padIndex,
        fieldId,
        (uint8_t)(value & 0xFF),
        (uint8_t)(value >> 8));
}

// ============================================================
// Pad sound list (CMD_PAD_SOUND, G9)
// ============================================================

uint8_t uartPadSoundRequest(uint8_t padIndex)
{
    // Mulai request baru (SEQ baru). Frame respon lama yang terlambat
    // diabaikan lewat pencocokan SEQ di handlePadSoundFrame.
    padSoundPending = true;
    padSoundHasName = false;
    padSoundOk = false;
    padSoundReady = false;
    padSoundCount = 0;
    padSoundIdx = 0;
    padSoundNameLen = 0;
    padSoundGot = 0;
    padSoundCurrent = 0xFF;
    memset(padSoundNames, 0, sizeof(padSoundNames));

    padSoundSeq = sendFrame(CMD_PAD_SOUND, padIndex);
    padSoundSentMs = millis();

    if constexpr (PAD_SOUND_DEBUG)
    {
        Serial.printf("[SOUND UI] REQUEST pad=%u seq=%u\n",
                      (unsigned)padIndex, (unsigned)padSoundSeq);
    }

    return padSoundSeq;
}

bool uartPadSoundAvailable()
{
    return padSoundReady;
}

bool uartPadSoundGet(
    char names[PAD_SOUND_COUNT][PAD_SOUND_NAME_MAX + 1],
    uint8_t& currentIndex)
{
    if (!padSoundReady)
    {
        return false;
    }

    // Consume (satu sukses = satu pengambilan).
    padSoundReady = false;
    padSoundPending = false;

    if (!padSoundOk)
    {
        return false;
    }

    for (uint8_t i = 0; i < PAD_SOUND_COUNT; i++)
    {
        memcpy(names[i], padSoundNames[i], PAD_SOUND_NAME_MAX + 1);
    }

    currentIndex = padSoundCurrent;
    return true;
}

// ============================================================
// Pad sound set (CMD_PAD_SOUND_SET)
// ============================================================

uint8_t uartPadSoundSetRequest(uint8_t padIndex, const char* name)
{
    // ARG0 header = index pad; panjang = nama file (maks sample name).
    return sendNameChunked(
        CMD_PAD_SOUND_SET,
        padIndex,
        name,
        PAD_SOUND_NAME_MAX);
}
