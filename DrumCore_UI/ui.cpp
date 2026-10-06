#include "ui.h"
#include "config.h"
#include "display.h"
#include "encoder.h"
#include "menu.h"
#include "screens.h"
#include "uart_client.h"
#include <cstdio>
#include <cstring>

// Batas atas index kit; mencerminkan ESP32 UART_MAX_KIT (4 = kit 0..4).
// Didefinisikan lokal karena UART_MAX_KIT ada di DrumCore config.h yang tak
// tersedia di build Wemos/ESP8266. Penomoran slot konsisten dengan backend:
// internal 0 = factory kit (protected, "KIT 01"), internal 1..4 = slot user
// writable ("KIT 02".."KIT 05").
static constexpr uint8_t UI_MAX_KIT      = 4;
static constexpr uint8_t UI_FACTORY_KIT  = 0;
static constexpr uint8_t UI_MIN_WRITABLE = 1;

static UIState uiState = UIState::BOOT;
static uint32_t lastUpdateMs = 0;
static uint32_t lastPingMs = 0;

// ----- BOOT / INIT / READY / CHECK_CONNECTION -----

enum class PingPhase : uint8_t
{
    WAIT_START,   // give the ESP32 UART task time to boot
    AWAIT_RESP,   // a PING is in flight
    DONE_OK       // PING confirmed, only waiting out INIT_MIN_MS
};

static bool bootSplashDone = false;
static uint32_t bootStartMs = 0;
static uint8_t lastBootFrame = 0xFF;

static PingPhase pingPhase = PingPhase::WAIT_START;
static uint32_t pingLastSentMs = 0;
static uint32_t initStartMs = 0;

// G22: baris status boot hanya ditulis ulang saat STEP-nya berubah
// (0xFF = belum pernah). Tidak ada lagi animasi titik per-loop.
static uint8_t lastInitStep = 0xFF;

static uint32_t readyShownMs = 0;

// ----- ACTIVE KIT (kit yang sedang dimuat DrumCore) -----
// ACTIVE KIT hanya diupdate oleh sumber terpercaya: respons STATUS (masuk home
// screen), respons sukses LOAD (ARG0 = kit aktif baru), dan DELETE kit aktif
// (backend menjamin fallback ke factory kit, lihat kit_manager.cpp).
// CREATE / SAVE / RENAME tak pernah mengubah active kit.

static uint8_t activeKit = 0;
static bool activeKitKnown = false;

static bool mainStatusPending = false;
static uint32_t mainStatusStartMs = 0;
static uint8_t mainStatusSeq = 0;

// ----- Kit name cache (CMD_KIT_INFO, keyed by requested index) -----
// Selalu menjawab tepat "kit mana index X", entah X kit aktif atau pilihan
// browsing.

// G14: trace sementara propagasi nama (transition-only). Matikan
// (false) setelah verifikasi hardware selesai.
static constexpr bool KIT_NAME_DEBUG = true;

static char kitNameBuf[KIT_NAME_MAX + 1] = "";
static uint8_t namedKitIndex = 0xFF;

static const char* kitNameFor(uint8_t idx)
{
    return (namedKitIndex == idx && kitNameBuf[0] != '\0')
        ? kitNameBuf
        : nullptr;
}

static void ensureKitNameRequest(uint8_t idx)
{
    if (namedKitIndex == idx && kitNameBuf[0] != '\0')
    {
        return;
    }

    // G13+G14: satu sesi KIT_INFO (request->header->chunk->consume) dianggap
    // sibuk sampai benar-benar selesai. Dulu guard hanya 'kitInfoPending' —
    // header langsung melepas pending padahal chunk masih dirakit, jadi request
    // baru bisa lewat dan membuang rakitan yang berjalan (livelock bila drain
    // UART terpecah). Kini setiap aktivitas KIT_INFO memblokir request baru;
    // retry hanya setelah consume sukses atau timeout.
    if (uartKitInfoBusy())
    {
        return;
    }

    uartKitInfoRequest(idx);
}

static void invalidateKitName()
{
    namedKitIndex = 0xFF;
    kitNameBuf[0] = '\0';
}

// Dipanggil tiap UI update. Mengonsumsi satu paket KIT_INFO yang lengkap dan
// return true bila paket itu milik `idx` (pencocokan seq di uart_client
// mencegah request basi mendarat di sini).
static bool pollKitName(uint8_t idx)
{
    uint8_t ridx = 0;
    uint8_t id = 0;
    char name[KIT_NAME_MAX + 1];

    if (!uartKitInfoGet(ridx, id, name, sizeof(name)))
    {
        return false;
    }

    if constexpr (KIT_NAME_DEBUG)
    {
        Serial.printf("[KIT UI] KIT_INFO CONSUME idx=%u name='%s'\n",
                      (unsigned)ridx, name);
    }

    invalidateKitName();
    namedKitIndex = ridx;
    memset(kitNameBuf, 0, sizeof(kitNameBuf));
    snprintf(kitNameBuf, sizeof(kitNameBuf), "%s", name);

    if constexpr (KIT_NAME_DEBUG)
    {
        Serial.printf("[KIT UI] KIT NAME CACHE idx=%u name='%s'\n",
                      (unsigned)namedKitIndex, kitNameBuf);
    }

    return (ridx == idx);
}

// ----- KIT TRANSACTION BOOKKEEPING -----

enum class KitAction : uint8_t
{
    NONE,
    LOAD,
    SAVE,
    // G4: create dengan nama user (auto-slot) — satu-satunya jalur CREATE yang
    // masih hidup. Varian tanpa nama (CMD_KIT_CREATE_EMPTY/COPY) hanya dipakai
    // layar SELECT SLOT yang sudah mati dan dibuang G24.
    CREATE_EMPTY_NAMED,
    CREATE_COPY_NAMED,
    DELETE,
    RENAME
};

static KitAction kitActiveAction = KitAction::NONE;

// Kit yang dioperasikan transaksi INI (progress + label hasil). Terpisah dari
// activeKit: CREATE tidak menjadikan kit baru sebagai kit aktif.
static uint8_t kitSubject = 0;

static const char* kitActionLabel(KitAction a)
{
    switch (a)
    {
        case KitAction::LOAD:                 return "LOADING";
        case KitAction::SAVE:                 return "SAVING";
        case KitAction::CREATE_EMPTY_NAMED:
        case KitAction::CREATE_COPY_NAMED:    return "CREATING";
        case KitAction::DELETE:               return "DELETING";
        case KitAction::RENAME:               return "RENAME";
        default:                              return "";
    }
}

static uint8_t kitActionCmd(KitAction a)
{
    switch (a)
    {
        case KitAction::LOAD:                 return CMD_KIT_LOAD;
        case KitAction::SAVE:                 return CMD_KIT_SAVE;
        case KitAction::CREATE_EMPTY_NAMED:   return CMD_KIT_CREATE_EMPTY_NAMED;
        case KitAction::CREATE_COPY_NAMED:    return CMD_KIT_CREATE_COPY_NAMED;
        case KitAction::DELETE:               return CMD_KIT_DELETE;
        case KitAction::RENAME:               return CMD_KIT_RENAME;
        default:                              return 0;
    }
}

static const char* kitActionFailText(KitAction a)
{
    switch (a)
    {
        case KitAction::LOAD:                 return "LOAD FAILED";
        case KitAction::SAVE:                 return "SAVE FAILED";
        case KitAction::CREATE_EMPTY_NAMED:
        case KitAction::CREATE_COPY_NAMED:    return "CREATE FAILED";
        case KitAction::DELETE:               return "DELETE FAILED";
        case KitAction::RENAME:               return "RENAME FAILED";
        default:                              return "FAILED";
    }
}

// CREATE-bernama (G4): konsep slot tak terlihat user — progress dan hasil ditampilkan tanpa angka kit.
static bool isNamedCreate(KitAction a)
{
    return a == KitAction::CREATE_EMPTY_NAMED ||
           a == KitAction::CREATE_COPY_NAMED;
}

// Aksi berbasis nama: respon NAME_EXISTS / INVALID_VALUE relevan.
static bool isNameAction(KitAction a)
{
    return isNamedCreate(a) || a == KitAction::RENAME;
}

static bool kitActionIsLoad(KitAction a)
{
    return a == KitAction::LOAD;
}

static bool kitActionSent = false;
static bool kitActionPending = false;
static uint32_t kitActionStartMs = 0;
static uint8_t kitActionSeq = 0;

static uint32_t kitActionResultMs = 0;
static uint8_t kitActionResultKind = 0;
// 0 = none  1 = ok (hasil generic)  3 = failed  4 = timeout
static bool kitActionNameExists = false;
// RENAME / CREATE-bernama ditolak karena nama sudah dipakai kit lain (status RESP_NAME_EXISTS, 0x07).
static bool kitActionNameInvalid = false;
// Nama yang dikirim ditolak backend karena invalid (RESP_INVALID_VALUE, 0x03)
// — dipakai utk RENAME dan CREATE-bernama.
static bool kitActionBusy = false;
// G24.15: semua slot kit user (1..4) sudah terisi (RESP_NO_SPACE, 0x08). Ini
// BUKAN "backend sibuk" — user harus diberi tahu tak ada ruang, bukan melihat
// BUSY yang menyesatkan.
static bool kitActionNoSpace = false;
// Backend sibuk / SD / NVS gagal (RESP_BUSY, 0x05) — ditampilkan sebagai kelas
// error terpisah (G11 §11) agar tak disembunyikan ke "<VERB> FAILED" generik.

// G11: snapshot nama subjek saat action diluncurkan. Berbeda dari cache
// KIT_INFO (yang boleh berubah selama menunggu respons), hasil layar selalu
// name-based: nama inilah yang tampil. Untuk RENAME, isinya nama LAMA (nama
// baru di renameBuf).
static char kitSubjectName[KIT_NAME_MAX + 1] = "";

static uint8_t lastActionPercent = 0xFF;

// ----- SELECTION STATE -----
// SELECTED KIT (kitSelectPurpose + selectedKit) adalah target LOAD/RENAME/
// DELETE. Ini seleksi LOCAL-ONLY: memutar browser TAK PERNAH mengirim
// KIT_NEXT/KIT_PREV, jadi tak ada respons UART basi yang bisa menggeser
// pilihan. Nama kit diambil terpisah via CMD_KIT_INFO(index) yang read-only.
// G24: targetSlot (destinasi CREATE) dihapus. CREATE memakai KIT_CREATE_NAME
// dan ESP32 memilih slot kosong sendiri, jadi nomor slot tak pernah tampil
// ke user.

static KitSelectPurpose kitSelectPurpose = KitSelectPurpose::LOAD;
static uint8_t selectedKit = 0;

static uint8_t kitSelMinFor(KitSelectPurpose p)
{
    // Factory kit (0) hanya ditawarkan sebagai target LOAD. CREATE/RENAME/
    // DELETE hanya beroperasi di slot user writable (1..4).
    return (p == KitSelectPurpose::LOAD) ? 0 : UI_MIN_WRITABLE;
}

// ----- CONFIRMATION STATE (KIT_CONFIRM — YES/NO) -----
// Konfirmasi tak pernah auto-execute. CLICK pada pilihan: YES -> jalankan aksi
// pending; NO -> kembali ke browser (pilihan dipertahankan). LONG PRESS /
// idle -> batal kembali ke browser.

struct KitConfirmState
{
    KitAction action;   // LOAD / CREATE_*_NAMED / DELETE
    uint8_t   kit;      // subject kit
    UIState   origin;   // KIT_SELECT (cancel target)
};

static KitConfirmState confirmState =
{ KitAction::NONE, 0, UIState::KIT_SELECT };
static uint8_t confirmChoice = 0;   // 0 = YES, 1 = NO

// G11: snapshot nama subjek saat CONFIRM tampil, agar layar konfirmasi tetap
// name-based walau cache nama berubah.
static char confirmName[KIT_NAME_MAX + 1] = "";

static const char* confirmVerb(KitAction a)
{
    switch (a)
    {
        case KitAction::LOAD:         return "LOAD";
        case KitAction::CREATE_EMPTY_NAMED:
        case KitAction::CREATE_COPY_NAMED:  return "CREATE";
        case KitAction::DELETE:       return "DELETE";
        default:                      return "";
    }
}

// ----- CREATE METHOD (dipilih sebelum target slot, dipertahankan sampai transaksi diluncurkan) -----

static KitCreateMethod createMethod = KitCreateMethod::EMPTY;

// ----- KIT RENAME EDITOR (TASK 3) -----

// G22: SPACE adalah karakter NAMA yang sah dan harus bisa dipilih sebagai
// karakter biasa. Nilai yang ditulis ke buffer adalah ' ' (0x20) — BUKAN
// underscore '_' dan BUKAN string literal "SPACE". Backend
// (kitManagerValidateName) sudah menerima spasi di tengah nama dan tetap
// menolak spasi di ujung, separator path, control character, serta nama kosong
// setelah trim. Validasi, protocol, dan chunk UART tidak berubah karena
// karakter ini.
static constexpr char KIT_RENAME_SPACE = ' ';

// Ring karakter editor. SPACE menempati slot 0 tepat sebelum 'A': dari 'A'
// satu langkah mundur sudah menghasilkan spasi asli, dan sel kosong mendapat
// SPACE sebagai langkah pertamanya.
static const char KIT_RENAME_ALPHABET[] =
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";

static constexpr uint8_t KIT_RENAME_MAX = 14;
static constexpr uint8_t KIT_CREATE_NAME_MAX = KIT_NAME_MAX;   // G4

// Satu buffer editor dipakai RENAME (14) dan CREATE bernama (28).
static char renameBuf[KIT_NAME_MAX + 1] = "";
static uint8_t renameCursor = 0;

// Posisi karakter di ring, atau -1 bila di luar ring (mis. '\0' pada sel kosong di belakang nama lama).
static int8_t kitRenameIndexOf(char c)
{
    const int8_t len = (int8_t)(sizeof(KIT_RENAME_ALPHABET) - 1);

    for (int8_t i = 0; i < len; i++)
    {
        if (KIT_RENAME_ALPHABET[i] == c)
        {
            return i;
        }
    }

    return -1;
}

// Satu langkah putar pada satu sel editor: sel berisi karakter ring ->
// maju/mundur satu slot (wrap); sel kosong (di luar ring) -> langkah pertama
// menghasilkan SPACE, sehingga nama ber-spasi ("ROCK LIVE", "KENDANG JAWA",
// "KIT TEST 01") bisa diketik tanpa memutar melewati seluruh alfabet.
static char kitRenameCharAfter(char cur, int8_t dir)
{
    const int8_t len = (int8_t)(sizeof(KIT_RENAME_ALPHABET) - 1);
    const int8_t base = kitRenameIndexOf(cur);

    if (base < 0)
    {
        return KIT_RENAME_SPACE;
    }

    int8_t next = base + dir;

    if (next >= len)
    {
        next = 0;
    }
    else if (next < 0)
    {
        next = len - 1;
    }

    return KIT_RENAME_ALPHABET[next];
}

// Tutup editor: pangkas spasi kanan. Nama kosong setelah trim berarti BATAL, bukan nama berisi spasi.
static bool kitRenameFinalize()
{
    size_t n = strlen(renameBuf);

    while (n > 0 && renameBuf[n - 1] == KIT_RENAME_SPACE)
    {
        n--;
    }

    if (n == 0)
    {
        return false;
    }

    renameBuf[n] = '\0';
    return true;
}

// ----- PAD UI STATE / SHARED IDLE TIMESTAMP -----

static uint32_t padIdleMs = 0;
static uint32_t kitNavIdleMs = 0;

// ----- SHARED KIT NAME EDITOR (G24.15) -----
// Mesin editor di bawah memanggil enter* yang didefinisikan jauh di bawah
// file; prototype-nya dideklarasikan di sini (blok deklarasi umum di bawah
// tidak memuat prototype baru ini).
static void enterKitSelect(KitSelectPurpose purpose, bool resetSelection);
static void enterKitCreate();
static void enterKitConfirm(KitAction action, uint8_t kit, UIState origin);
static void enterKitAction(KitAction action, uint8_t kit);

//
//
// RENAME dan CREATE memakai SATU mesin editor; tidak ada lagi "editor versi
// kedua": buffer, cursor, ring karakter, spasi, penghapusan, dan aturan
// konfirmasi semuanya hidup di sini. Yang berbeda antar mode hanya tiga
// parameter: nameEditMax (panjang maks: RENAME 14, CREATE 28), blank (sumber
// nama: lama/baru), nameEditExit (tujuan saat editor ditutup).
// Aturan navigasi — IDENTIK untuk RENAME dan CREATE:
//   putar -> ganti karakter pada sel cursor (sel kosong '\0' dan spasi di ring
//            yang sama; satu langkah MUNDUR dari 'A' = spasi, jadi memutar ke
//            spasi = menghapus karakter / backspace)
//   klik  -> maju satu sel; di sel terakhir = SELESAI
//   tahan -> batal, kembali ke layar sebelumnya
//   diam  -> batal (KIT_IDLE_BACK_MS)
// G24.15 — bug yang diperbaiki: CREATE lama memakai heuristik "restEmpty" yang
// menutup editor begitu cursor mendarat di sel spasi. Akibatnya (1) spasi di
// tengah nama mustahil diketik, (2) nama terpotong diam-diam tanpa disadari
// user, (3) screen dan aturan navigasi beda dari RENAME. Heuristik itu dihapus;
// CREATE kini memakai aturan RENAME.

enum class NameEditorExit : uint8_t
{
    RENAME,   // selesai -> enterKitAction(RENAME, selectedKit)
    CREATE    // selesai -> enterKitConfirm(CREATE_*, 0, KIT_CREATE_NAME)
};

static uint8_t nameEditMax = KIT_RENAME_MAX;
static NameEditorExit nameEditExit = NameEditorExit::RENAME;

static void nameEditorRedraw()
{
    screenShowNameEditor(renameBuf, renameCursor);
}

// Batal: kembali ke layar asal mode ini, bukan ke MENU.
static void nameEditorCancel()
{
    if (nameEditExit == NameEditorExit::CREATE)
    {
        enterKitCreate();
    }
    else
    {
        enterKitSelect(kitSelectPurpose, false);
    }
}

static void enterNameEditor(
    uint8_t maxLen,
    bool blank,
    NameEditorExit exitTo)
{
    nameEditMax = maxLen;
    nameEditExit = exitTo;

    if (blank)
    {
        // Sel kosong = '\0', persis model RENAME untuk nama kosong;
        // kitRenameCharAfter('\0', *) mengembalikan spasi, jadi sel kosong
        // bisa diisi tanpa memutar melewati separuh alfabet.
        memset(renameBuf, 0, nameEditMax + 1);
    }
    else
    {
        const char* cur = kitNameFor(selectedKit);

        if (cur && cur[0] != '\0')
        {
            strncpy(renameBuf, cur, nameEditMax);
            renameBuf[nameEditMax] = '\0';
        }
        else
        {
            // G11: jangan pernah prefill "KIT %02u" — nomor slot dilarang
            // tampil di layar. Editor mulai kosong.
            memset(renameBuf, 0, nameEditMax + 1);
        }
    }

    renameCursor = 0;

    kitNavIdleMs = millis();

    encoderFlush();

    nameEditorRedraw();
}

static void handleNameEditor()
{
    uint32_t now = millis();

    // Tahan = batal.
    if (encoderLongPressed())
    {
        encoderFlush();
        nameEditorCancel();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        kitNavIdleMs = now;

        // Memutar ke spasi = menghapus karakter pada sel itu (spasi di ujung
        // dipangkas kitRenameFinalize, spasi di tengah dipertahankan).
        renameBuf[renameCursor] = kitRenameCharAfter(
            renameBuf[renameCursor],
            (delta > 0) ? 1 : -1);

        nameEditorRedraw();
        return;
    }

    if (encoderPressed())
    {
        kitNavIdleMs = now;
        encoderFlush();

        // Maju sampai sel TERAKHIR; klik di sana = selesai.
        if (renameCursor < nameEditMax - 1)
        {
            renameCursor++;
            nameEditorRedraw();
            return;
        }

        // Selesai: pangkas spasi kanan; kosong = batal.
        if (!kitRenameFinalize())
        {
            nameEditorCancel();
            return;
        }

        if (nameEditExit == NameEditorExit::CREATE)
        {
            KitAction act = (createMethod == KitCreateMethod::EMPTY)
                ? KitAction::CREATE_EMPTY_NAMED
                : KitAction::CREATE_COPY_NAMED;

            enterKitConfirm(act, 0, UIState::KIT_CREATE_NAME);
        }
        else
        {
            enterKitAction(KitAction::RENAME, selectedKit);
        }

        return;
    }

    if (now - kitNavIdleMs >= KIT_IDLE_BACK_MS)
    {
        encoderFlush();
        nameEditorCancel();
    }
}

// ----- PAD EDIT FIELD MODEL (G9) -----
// Satu lapisan konfigurasi editor numerik: mapping UI -> field protokol +
// rentang DISPLAY. Rentang ini adalah rentang transport 16-bit; batas
// otoritatif (PAD_FIELD_MIN/MAX) tetap di config.h (ESP32) dan backend tetap
// memvalidasi (mis. RELEASE < THRESHOLD -> RESP_INVALID_VALUE). Editor tidak
// melewati rentang ini sehingga user tak bisa memasukkan kombinasi yang
// jelas-jelas mustahil.
// Catatan rentang: RELEASE 4094 (bukan 4095) karena invariant release <
// threshold dan threshold <= 4095; RELEASE STABLE 65535 (bukan 100000) karena
// batas transport frame UART 16-bit — nilai otoritatif 100000 tak bisa dikirim
// lewat PAD_SET/PAD_GET (lihat G8 report — KNOWN LIMITATIONS).

struct PadValueField
{
    uint8_t      fieldId;
    const char*  label;
    uint16_t     min;
    uint16_t     max;
};

static const PadValueField PAD_VALUE_FIELDS[] =
{
    { FIELD_MIDI_NOTE,        "MIDI NOTE",        0,    127 },
    { FIELD_THRESHOLD,        "THRESHOLD",      100,   4095 },
    { FIELD_RELEASE_THRESH,   "RELEASE",         50,   4094 },
    { FIELD_PEAK_TIME,        "PEAK TIME",      200,  10000 },
    { FIELD_RELEASE_STABLE,   "RELEASE STABLE", 1000,  65535 },
    { FIELD_MASK_BASE_MS,     "MASK",             1,  65535 }
};

// Timeout per transaksi pad (GET/SET), jauh di bawah timeout KIT.
constexpr uint16_t PAD_VALUE_CMD_TIMEOUT_MS = 2500;

// Berapa lama layar hasil pad ditahan sebelum kembali ke sub-list.
constexpr uint16_t PAD_RESULT_HOLD_MS = 1500;

// ----- PAD VALUE EDITOR (G9) -----
// Alur (wajib per G9, jangan pernah mengasumsikan PAD_SET sukses): GET nilai
// saat ini -> display -> user edit -> PAD_SET -> respon -> GET nilai efektif
// -> update UI. Respons selain OK/timeout menampilkan kegagalan jujur
// (INVALID/BUSY/TIMEOUT) dan TIDAK pernah berpura-pura nilai berubah.

enum class PadValuePhase : uint8_t
{
    READ,     // tunggu nilai saat ini (PAD_GET)
    EDIT,     // tampil + edit
    WRITE,    // tunggu respon PAD_SET
    READBACK, // tunggu nilai efektif (PAD_GET pasca-sukses)
    RESULT    // hasil tampil, menunggu auto-return
};

static PadValuePhase padValuePhase = PadValuePhase::READ;
static uint8_t padValueFieldIdx = 0;      // index di PAD_VALUE_FIELDS
static uint8_t padValuePad = 0;           // pad yang sedang diedit
static uint16_t padValueDisplay = 0;      // nilai di layar / sedang diedit
static uint16_t padValueApplied = 0;      // nilai yang dikirim ke backend
static uint32_t padValueStampMs = 0;      // penanda fase
static uint8_t padValueSeq = 0;           // SEQ request aktif
static uint8_t padValueResultKind = 0;    // 1=ok, 3=failed, 4=timeout
static bool padValueBusy = false;         // respon RESP_BUSY
static bool padValueNameInvalid = false;  // respon RESP_INVALID_VALUE

// Akselerasi putar (G9): putaran cepat dalam jendela 1.2 s menaikkan step
// 1 -> 10 -> 100 tanpa pernah melompat liar (diterapkan lewat perubahan delta
// encoder yang sudah di-clamp ±8 per poll).
static uint32_t padValueSpinMs = 0;
static uint16_t padValueSpinAccum = 0;

// ----- PAD SOUND (daftar WAV kit aktif, KIT RUNTIME) -----
// Daftar suara = scan folder samples kit AKTIF (sorted; mapping by FILENAME,
// bukan index). Ditarik SEGAR setiap masuk (tanpa cache yang bisa basi), plus
// penjaga generasi padCacheGen yang dinaikkan saat kit aktif berubah (LOAD
// sukses / DELETE aktif fallback) agar daftar kit lama tak pernah tampil
// sebagai daftar kit baru.
// Alur: request daftar (CMD_PAD_SOUND, ARG0=pad) -> BROWSE (cursor awal =
// currentIndex dari header daftar) -> OK kirim CMD_PAD_SOUND_SET (nama file
// terpilih) -> respons status -> hasil.

enum class PadSoundPhase : uint8_t
{
    LOAD,     // fetch daftar + current index
    BROWSE,   // pilih suara (index list == nama file)
    WRITE,    // tunggu respon PAD_SOUND_SET
    RESULT    // hasil tampil, menunggu auto-return
};

static PadSoundPhase padSoundPhase = PadSoundPhase::LOAD;
static char padSoundNames[PAD_SOUND_COUNT][PAD_SOUND_NAME_MAX + 1];
static bool padSoundListLoaded = false;   // daftar valid terkumpul
static uint8_t padSoundCount = 0;         // jumlah nama pada daftar
static uint8_t padSoundCursor = 0;        // index list yang dipilih
static uint8_t padSoundCurrent = 0;       // index suara pad saat ini
static uint32_t padSoundStampMs = 0;
static uint8_t padSoundSeq = 0;           // SEQ PAD_SOUND_SET aktif
static uint8_t padSoundResultKind = 0;    // 1=ok, 3=failed, 4=timeout
static bool padSoundBusy = false;
static uint8_t padSoundGenAtRequest = 0;  // padCacheGen saat daftar diminta

// Generasi cache PAD; dinaikkan setiap kit aktif berubah.
static uint8_t padCacheGen = 0;

// ----- Forward declarations -----

static void handleBoot();
static void handleInit();
static void handleReady();
static void handleCheckConn();
static void handleMain();
static void handleMenu();
static void handleKit();
static void handleKitSelect();
static void handleKitCreate();
static void handleKitCreateName();
static void handleKitConfirm();
static void handleKitRename();
static void handleKitAction();
static void handlePadSelect();
static void handlePadEdit();
static void handlePadSubList(uint8_t listId);
static void handlePadSound();
static void handlePadValue();

static void enterMain();
static void enterMenu();
static void enterKit();
static void enterKitSelect(KitSelectPurpose purpose, bool resetSelection);
static void enterKitCreate();
static void enterKitCreateName(bool resetName);
static void enterKitConfirm(KitAction action, uint8_t kit, UIState origin);
static void redrawConfirm();
static void enterKitRename();
static void enterKitAction(KitAction action, uint8_t kit);
static void enterPadSelect();
static void enterPadEdit();
static void enterPadMapping();
static void enterPadTrigger();
static void enterPadSound();
static void enterPadValue(uint8_t fieldIdx);
static void enterPadValueReturn();
static void enterReady();
static void enterCheckConnection();
static void renderKitActionResult();
static void showActionResultName();

// ----- BOOT -----

static void handleBoot()
{
    uint32_t now = millis();

    if (!bootSplashDone)
    {
        bootSplashDone = true;
        bootStartMs = now;
        lastBootFrame = 0xFF;
    }

    uint8_t frame =
        (uint8_t)((now - bootStartMs) / BOOT_FRAME_MS);

    if (frame >= BOOT_FRAME_COUNT)
    {
        encoderFlush();
        bootSplashDone = false;

        uiState = UIState::INIT;
        initStartMs = now;
        pingPhase = PingPhase::WAIT_START;
        pingLastSentMs = 0;
        lastInitStep = 0xFF;

        screenShowBootStep(0);

        return;
    }

    if (frame != lastBootFrame)
    {
        lastBootFrame = frame;
        screenShowBoot(frame);
    }
}

// ----- INIT -----

static void handleInit()
{
    uint32_t now = millis();

    // G22: progress boot ditampilkan sebagai STEP, bukan titik-titik berkedip.
    // Step hanya maju selama handshake PING yang SEDANG berjalan; ini murni
    // presentasi — tidak menambah waktu boot, tidak menahan PING, dan tidak
    // mengubah urutan inisialisasi. PING OK -> langsung lompat ke layar READY,
    // berapa pun step yang sempat tampil.
    uint8_t step = (uint8_t)((now - initStartMs) / BOOT_STEP_MS);

    if (step >= BOOT_STEP_COUNT)
    {
        step = (uint8_t)(BOOT_STEP_COUNT - 1);
    }

    if (step != lastInitStep)
    {
        lastInitStep = step;
        screenShowBootStep(step);
    }

    switch (pingPhase)
    {
        case PingPhase::WAIT_START:
        {
            if (now - initStartMs >= UART_STARTUP_TIMEOUT_MS)
            {
                enterCheckConnection();
                break;
            }

            if (now - initStartMs >= UART_STARTUP_WAIT_MS)
            {
                uartSendPing();
                pingLastSentMs = now;
                pingPhase = PingPhase::AWAIT_RESP;
            }
            break;
        }

        case PingPhase::AWAIT_RESP:
        {
            UartPingResult result = uartPollPing();

            if (result == UartPingResult::OK)
            {
                if (now - initStartMs >= INIT_MIN_MS)
                {
                    enterReady();
                }
                else
                {
                    pingPhase = PingPhase::DONE_OK;
                }
                break;
            }

            if (now - pingLastSentMs >= UART_PING_RETRY_MS)
            {
                if (now - initStartMs >= UART_STARTUP_TIMEOUT_MS)
                {
                    enterCheckConnection();
                }
                else
                {
                    uartSendPing();
                    pingLastSentMs = now;
                }
            }
            break;
        }

        case PingPhase::DONE_OK:
        {
            if (now - initStartMs >= INIT_MIN_MS)
            {
                enterReady();
            }
            break;
        }
    }
}

// ----- READY -----

static void enterReady()
{
    uiState = UIState::READY;
    readyShownMs = millis();
    encoderFlush();

    screenShowReady();
}

static void handleReady()
{
    if (millis() - readyShownMs >= READY_HOLD_MS)
    {
        enterMain();
    }
}

// ----- CHECK CONNECTION -----

static void enterCheckConnection()
{
    uiState = UIState::CHECK_CONN;
    pingPhase = PingPhase::AWAIT_RESP;
    pingLastSentMs = 0;

    encoderFlush();

    screenShowCheckConn();
}

static void handleCheckConn()
{
    uint32_t now = millis();

    if (uartPollPing() == UartPingResult::OK)
    {
        enterReady();
        return;
    }

    if (pingLastSentMs == 0 ||
        now - pingLastSentMs >= UART_PING_RETRY_MS)
    {
        uartSendPing();
        pingLastSentMs = now;
    }
}

// ----- MAIN (home status screen — ACTIVE KIT) -----

static void enterMain()
{
    uiState = UIState::MAIN;

    encoderFlush();

    mainStatusPending = true;
    mainStatusSeq = uartSendCommand(CMD_REQUEST_STATUS);
    mainStatusStartMs = millis();

    displayTransitionBegin();

    screenShowMainHome(activeKit, activeKitKnown, kitNameFor(activeKit));
}

static void handleMain()
{
    uint32_t now = millis();

    // G13: consume hasil KIT_INFO yang sudah lengkap DULU, baru request
    // berikutnya (kalau masih dibutuhkan). Urutan lama (request dulu)
    // menghancurkan paket saat masih livelock.
    if (pollKitName(activeKit))
    {
        if constexpr (KIT_NAME_DEBUG)
        {
            Serial.printf("[KIT UI] render activeKit=%u name='%s'\n",
                          (unsigned)activeKit, kitNameBuf);
        }
        screenShowMainHome(activeKit, activeKitKnown, kitNameFor(activeKit));
    }

    ensureKitNameRequest(activeKit);

    if (mainStatusPending)
    {
        int8_t idx = uartFindResponse(
            CMD_REQUEST_STATUS | UART_RESPONSE_BIT,
            mainStatusSeq);

        if (idx >= 0)
        {
            UartResponse resp;

            if (uartRemoveAt(idx, resp))
            {
                mainStatusPending = false;

                if (resp.status == RESP_OK)
                {
                    activeKit = (uint8_t)(resp.arg0 & 0xFF);
                    if (activeKit > UI_MAX_KIT)
                    {
                        activeKit = UI_MAX_KIT;
                    }
                    activeKitKnown = true;

                    ensureKitNameRequest(activeKit);
                }

                screenShowMainHome(activeKit, activeKitKnown, kitNameFor(activeKit));
            }
        }
        else if (now - mainStatusStartMs >= KIT_CMD_TIMEOUT_MS)
        {
            mainStatusPending = false;
        }
    }

    if (encoderPressed())
    {
        encoderFlush();
        enterMenu();
        return;
    }

    if (encoderLongPressed())
    {
        encoderFlush();
    }

    if (displayTransitionActive())
    {
        screenShowMainHome(activeKit, activeKitKnown, kitNameFor(activeKit));
    }
}

// ----- MENU (top level: KIT / PAD) -----

static void enterMenu()
{
    uiState = UIState::MENU;

    encoderFlush();

    displayTransitionBegin();

    screenShowMenuList(menuGetTopIndex());
}

static void handleMenu()
{
    if (encoderLongPressed())
    {
        encoderFlush();
        enterMain();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        menuNavTop(delta);
        screenShowMenuList(menuGetTopIndex());
        return;
    }

    if (encoderPressed())
    {
        encoderFlush();

        // G24: hanya dua entri — KIT dan PAD.
        switch (menuGetTopIndex())
        {
            case 0: // KIT
                enterKit();
                break;

            case 1: // PAD
                enterPadSelect();
                break;

            default:
                // Defensif: index di luar daftar (mis. cursor rusak) -> kembali
                // ke home, tidak pernah menggantung di layar.
                enterMain();
                break;
        }
    }

    if (displayTransitionActive())
    {
        screenShowMenuList(menuGetTopIndex());
    }
}

// ----- KIT (action list: LOAD / CREATE / RENAME / DELETE / SAVE / BACK) -----
// Anak langsung MENU -> KIT. LOAD/RENAME/DELETE menuju SELECT KIT; CREATE
// menuju method -> name editor (ESP32 memilih slot); SAVE langsung bekerja
// pada kit AKTIF.

static void enterKit()
{
    uiState = UIState::KIT;

    menuKitSubReset();
    kitNavIdleMs = millis();

    encoderFlush();

    screenShowKitMenu(menuKitSubGetIndex());
}

static void handleKit()
{
    uint32_t now = millis();

    // Long press = back to the parent menu.
    if (encoderLongPressed())
    {
        encoderFlush();
        enterMenu();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        kitNavIdleMs = now;

        menuKitSubNavigate(delta);

        screenShowKitMenu(menuKitSubGetIndex());
        return;
    }

    if (encoderPressed())
    {
        kitNavIdleMs = now;
        encoderFlush();

        uint8_t idx = menuKitSubGetIndex();

        if (idx == menuKitSubGetBackIndex())
        {
            enterMenu();
        }
        else if (idx == 0) // LOAD
        {
            enterKitSelect(KitSelectPurpose::LOAD, true);
        }
        else if (idx == 1) // CREATE
        {
            enterKitCreate();
        }
        else if (idx == 2) // RENAME
        {
            enterKitSelect(KitSelectPurpose::RENAME, true);
        }
        else if (idx == 3) // DELETE
        {
            enterKitSelect(KitSelectPurpose::DELETE, true);
        }
        else if (idx == 4) // SAVE (saves the ACTIVE kit, no confirm)
        {
            enterKitAction(KitAction::SAVE, activeKit);
        }

        return;
    }

    // Idle back to home.
    if (now - kitNavIdleMs >= KIT_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- KIT SELECT (SELECT KIT browser) -----
// Seleksi local-only: rotasi menggerakkan kursor layar tanpa round-trip
// KIT_NEXT/KIT_PREV, jadi pilihan TAK PERNAH bisa digeser respons UART
// basi/terlambat. Nama kit diambil terpisah via CMD_KIT_INFO(index) yang
// read-only.
// CLICK tergantung alasan masuk: LOAD -> KIT_CONFIRM ("LOAD <nama>?" YES/NO);
// RENAME -> editor KIT_RENAME; DELETE -> KIT_CONFIRM ("DELETE <nama>?" YES/NO).
// LONG PRESS -> kembali ke daftar aksi KIT.

static void enterKitSelect(
    KitSelectPurpose purpose,
    bool resetSelection)
{
    uiState = UIState::KIT_SELECT;

    kitSelectPurpose = purpose;

    if (resetSelection)
    {
        selectedKit = activeKit;

        uint8_t min = kitSelMinFor(purpose);
        if (selectedKit < min)
        {
            selectedKit = min;
        }
    }

    kitNavIdleMs = millis();

    encoderFlush();

    ensureKitNameRequest(selectedKit);
    screenShowKitSelect(selectedKit, kitNameFor(selectedKit));
}

static void handleKitSelect()
{
    uint32_t now = millis();

    // G13: consume hasil yang sudah lengkap dulu, baru request berikutnya bila
    // masih dibutuhkan (anti-livelock).
    if (pollKitName(selectedKit))
    {
        screenShowKitSelect(selectedKit, kitNameFor(selectedKit));
    }

    ensureKitNameRequest(selectedKit);

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        kitNavIdleMs = now;

        uint8_t min = kitSelMinFor(kitSelectPurpose);
        int8_t nk = (int8_t)selectedKit + ((delta > 0) ? 1 : -1);

        if (nk < (int8_t)min)
        {
            nk = (int8_t)UI_MAX_KIT;
        }
        else if (nk > (int8_t)UI_MAX_KIT)
        {
            nk = (int8_t)min;
        }

        selectedKit = (uint8_t)nk;

        ensureKitNameRequest(selectedKit);
        screenShowKitSelect(selectedKit, kitNameFor(selectedKit));
        return;
    }

    if (encoderPressed())
    {
        kitNavIdleMs = now;
        encoderFlush();

        switch (kitSelectPurpose)
        {
            case KitSelectPurpose::LOAD:
                enterKitConfirm(KitAction::LOAD, selectedKit, UIState::KIT_SELECT);
                break;

            case KitSelectPurpose::RENAME:
                // G11: jangan buka editor dengan nama kosong (atau fallback
                // angka slot). Tunggu nama subjek termuat di cache KIT_INFO
                // dulu — LCD wajib name-based (§3/§6).
                if (kitNameFor(selectedKit) &&
                    kitNameFor(selectedKit)[0] != '\0')
                {
                    enterKitRename();
                }
                break;

            case KitSelectPurpose::DELETE:
                enterKitConfirm(KitAction::DELETE, selectedKit, UIState::KIT_SELECT);
                break;
        }

        return;
    }

    if (encoderLongPressed())
    {
        encoderFlush();
        enterKit();
        return;
    }

    if (now - kitNavIdleMs >= KIT_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- KIT CREATE (method: EMPTY / COPY CURRENT / BACK) -----
// Tujuannya HANYA memilih method; slot target dipilih ESP32 sendiri (G4).

static void enterKitCreate()
{
    uiState = UIState::KIT_CREATE;

    menuKitCreateReset();
    kitNavIdleMs = millis();

    encoderFlush();

    screenShowKitCreate(menuKitCreateGetIndex());
}

static void handleKitCreate()
{
    uint32_t now = millis();

    if (encoderLongPressed())
    {
        encoderFlush();
        enterKit();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        kitNavIdleMs = now;

        menuKitCreateNavigate(delta);

        screenShowKitCreate(menuKitCreateGetIndex());
        return;
    }

    if (encoderPressed())
    {
        kitNavIdleMs = now;
        encoderFlush();

        uint8_t idx = menuKitCreateGetIndex();

        if (idx == menuKitCreateGetBackIndex())
        {
            enterKit();
        }
        else if (idx == 0) // EMPTY
        {
            createMethod = KitCreateMethod::EMPTY;
            enterKitCreateName(true);
        }
        else if (idx == 1) // COPY CURRENT
        {
            createMethod = KitCreateMethod::COPY_CURRENT;
            enterKitCreateName(true);
        }

        return;
    }

    if (now - kitNavIdleMs >= KIT_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- CREATE WITH NAME (G4) — memakai SHARED NAME EDITOR (G24.15) -----
// User mengetik nama kit langsung — konsep slot TIDAK pernah terlihat (ESP32
// memilih slot kosong sendiri). G24.15: logika editor TIDAK lagi hidup di
// sini; fungsi di bawah hanya "adapter" tipis yang menulis state/parameter
// lalu melepas seluruh pekerjaan ke handleNameEditor() yang sama dengan
// RENAME. Tidak ada editor cursor/buffer/karakter versi kedua.

static void enterKitCreateName(bool resetName)
{
    uiState = UIState::KIT_CREATE_NAME;

    enterNameEditor(KIT_CREATE_NAME_MAX, resetName, NameEditorExit::CREATE);
}

static void handleKitCreateName()
{
    handleNameEditor();
}

static void enterKitConfirm(
    KitAction action,
    uint8_t kit,
    UIState origin)
{
    uiState = UIState::KIT_CONFIRM;

    confirmState.action = action;
    confirmState.kit = kit;
    confirmState.origin = origin;
    confirmChoice = 0;

    // G11: snapshot nama subjek (bukan angka slot) untuk layar konfirmasi.
    // Cache boleh berubah selama layar tampil.
    const char* nm = kitNameFor(kit);
    if (nm && nm[0] != '\0')
    {
        snprintf(confirmName, sizeof(confirmName), "%s", nm);
    }
    else
    {
        confirmName[0] = '\0';
    }

    kitNavIdleMs = millis();

    encoderFlush();

    redrawConfirm();
}

// Redraw KIT_CONFIRM: tanpa angka kit — name-based (G11 §3). Nama belum
// termuat -> baris verb-only "LOAD?"/"DELETE?".
static void redrawConfirm()
{
    if (isNamedCreate(confirmState.action) || confirmName[0] == '\0')
    {
        screenShowKitConfirmName(confirmVerb(confirmState.action), confirmChoice);
    }
    else
    {
        screenShowKitConfirm(
            confirmVerb(confirmState.action),
            confirmName,
            confirmChoice);
    }
}

// Kembali dari KIT_CONFIRM sesuai asal (cancel/NO). Untuk CREATE bernama
// kembali ke editor nama dengan nama yang dipertahankan.
static void confirmCancelBack()
{
    if (confirmState.origin == UIState::KIT_CREATE_NAME)
    {
        // Nama dgn cursor dipertahankan untuk koreksi.
        enterKitCreateName(false);
    }
    else
    {
        enterKitSelect(kitSelectPurpose, false);
    }
}

static void handleKitConfirm()
{
    uint32_t now = millis();

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        kitNavIdleMs = now;

        confirmChoice = (confirmChoice == 0) ? 1 : 0;

        redrawConfirm();
        return;
    }

    if (encoderPressed())
    {
        kitNavIdleMs = now;
        encoderFlush();

        if (confirmChoice == 0) // YES -> execute
        {
            enterKitAction(confirmState.action, confirmState.kit);
        }
        else // NO -> back, selection preserved
        {
            confirmCancelBack();
        }

        return;
    }

    if (encoderLongPressed())
    {
        encoderFlush();
        confirmCancelBack();
        return;
    }

    // Idle -> CANCEL (never auto-execute a confirmation).
    if (now - kitNavIdleMs >= KIT_IDLE_BACK_MS)
    {
        encoderFlush();
        confirmCancelBack();
    }
}

// ----- KIT RENAME EDITOR — adapter ke SHARED NAME EDITOR (G24.15) -----
// RENAME adalah baseline yang sudah terbukti normal, jadi ia yang
// mendefinisikan shared engine; CREATE memakai mesin yang sama dengan max 28
// (lihat enterKitCreateName). Perbedaan RENAME vs CREATE hanya tiga parameter:
// max length (14 vs 28), sumber nama (lama vs baru), dan asal kembali.

static void enterKitRename()
{
    uiState = UIState::KIT_RENAME;

    enterNameEditor(KIT_RENAME_MAX, false, NameEditorExit::RENAME);
}

static void handleKitRename()
{
    handleNameEditor();
}

// ----- KIT ACTION (LOAD / SAVE / CREATE / DELETE / RENAME) -----

static void enterKitAction(KitAction action, uint8_t kit)
{
    uiState = UIState::KIT_ACTION;

    kitActiveAction = action;
    kitSubject = kit;

    kitActionSent = false;
    kitActionPending = false;
    kitActionResultKind = 0;
    kitActionNameExists = false;
    kitActionNameInvalid = false;
    kitActionBusy = false;
    kitActionNoSpace = false;
    lastActionPercent = 0xFF;

    encoderFlush();

    // G11: seluruh aksi memakai layout verb-only (nomor slot tak boleh tampil
    // di layar progress). Nama subjek di-snapshot untuk layar hasil name-based.
    const char* nm = kitNameFor(kit);
    if (nm && nm[0] != '\0')
    {
        snprintf(kitSubjectName, sizeof(kitSubjectName), "%s", nm);
    }
    else
    {
        kitSubjectName[0] = '\0';
    }

    screenDrawKitActionTitled(kitActionLabel(action));
}

static uint8_t kitVisualPercent(
    uint32_t elapsed,
    uint16_t visualMs)
{
    uint32_t full = visualMs;

    if (elapsed >= full)
    {
        return KIT_VISUAL_PEAK_PCT;
    }

    uint32_t t = elapsed * 100u / full;

    if (t < 20u)
    {
        return (uint8_t)(t * 38u / 20u);
    }

    if (t < 55u)
    {
        return 38u + (uint8_t)((t - 20u) * 32u / 35u);
    }

    if (t < 85u)
    {
        return 70u + (uint8_t)((t - 55u) * 14u / 30u);
    }

    return 84u + (uint8_t)((t - 85u) *
        (uint32_t)(KIT_VISUAL_PEAK_PCT - 84) / 15u);
}

static void renderKitActionResult()
{
    // G11: layar hasil name-based — judul = snapshot nama subjek (atau nama
    // BARU setelah RENAME sukses); status = kelas error.
    switch (kitActionResultKind)
    {
        case 1:  // ok
            if (isNamedCreate(kitActiveAction))
            {
                screenShowActionResult("KIT CREATED", "OK");
            }
            else if (kitActiveAction == KitAction::RENAME)
            {
                screenShowKitResultNameBar(renameBuf);
            }
            else
            {
                screenShowKitResultNameBar(kitSubjectName);
            }
            break;

        case 4:  // timeout
            if (isNamedCreate(kitActiveAction))
            {
                screenShowActionResult("KIT CREATED", "TIMEOUT");
            }
            else
            {
                screenShowKitResultName(kitSubjectName, "TIMEOUT");
            }
            break;

        case 5:  // nama sudah dipakai kit lain
            showActionResultName();
            break;

        default: // failed
            if (kitActionNameExists)
            {
                showActionResultName();
            }
            else if (kitActionNameInvalid)
            {
                if (isNamedCreate(kitActiveAction))
                {
                    screenShowActionResult("KIT CREATED", "NAME INVALID");
                }
                else
                {
                    screenShowKitResultName(kitSubjectName, "NAME INVALID");
                }
            }
            else if (kitActionNoSpace)
            {
                // G24.15: ruang kit habis — katakan jujur dan spesifik; jangan
                // pernah menyamar sebagai BUSY.
                if (isNamedCreate(kitActiveAction))
                {
                    screenShowActionResult("NO SPACE", "ALL KITS FULL");
                }
                else
                {
                    screenShowKitResultName(kitSubjectName, "NO SPACE");
                }
            }
            else if (kitActionBusy)
            {
                // G11 §11: BUSY tidak layak diubah jadi FAILED generik.
                if (isNamedCreate(kitActiveAction))
                {
                    screenShowActionResult("KIT CREATED", "BUSY");
                }
                else
                {
                    screenShowKitResultName(kitSubjectName, "BUSY");
                }
            }
            else
            {
                if (isNamedCreate(kitActiveAction))
                {
                    screenShowActionResult(
                        "KIT CREATED",
                        "CREATE FAILED");
                }
                else
                {
                    screenShowKitResultName(
                        kitSubjectName,
                        kitActionFailText(kitActiveAction));
                }
            }
            break;
    }
}

// Nama ditolak karena sudah dipakai kit lain — tampil tanpa angka kit
// (name-based, G11): judul = nama subjek, status = kelas error.
static void showActionResultName()
{
    if (isNamedCreate(kitActiveAction))
    {
        screenShowActionResult("KIT CREATED", "NAME IN USE");
    }
    else
    {
        screenShowKitResultName(kitSubjectName, "NAME IN USE");
    }
}

static void handleKitAction()
{
    uint32_t now = millis();

    // Kirim command sekali.

    if (!kitActionSent)
    {
        kitActionSent = true;
        kitActionPending = true;
        kitActionStartMs = now;

        switch (kitActiveAction)
        {
            case KitAction::LOAD:
                kitActionSeq =
                    uartSendCommand(CMD_KIT_LOAD, kitSubject);
                break;

            case KitAction::SAVE:
                kitActionSeq =
                    uartSendCommand(CMD_KIT_SAVE);
                break;

            case KitAction::CREATE_EMPTY_NAMED:
                kitActionSeq =
                    uartSendCreateNamedRequest(
                        CMD_KIT_CREATE_EMPTY_NAMED,
                        renameBuf);
                break;

            case KitAction::CREATE_COPY_NAMED:
                kitActionSeq =
                    uartSendCreateNamedRequest(
                        CMD_KIT_CREATE_COPY_NAMED,
                        renameBuf);
                break;

            case KitAction::DELETE:
                kitActionSeq =
                    uartSendCommand(CMD_KIT_DELETE, kitSubject);
                break;

            case KitAction::RENAME:
                kitActionSeq =
                    uartSendRenameRequest(kitSubject, renameBuf);
                break;

            default:
                kitActionSeq = 0;
                break;
        }

        return;
    }

    // Tunggu respons yang cocok (dengan animasi progress).

    if (kitActionPending)
    {
        uint32_t elapsed = now - kitActionStartMs;
        uint16_t visualMs = kitActionIsLoad(kitActiveAction)
            ? KIT_VISUAL_LOAD_MS
            : KIT_VISUAL_SAVE_MS;

        uint8_t percent = kitVisualPercent(elapsed, visualMs);

        uint8_t newCells = (uint8_t)(((uint16_t)percent * 12u * 5u / 100u) / 5u);
        uint8_t lastCells = (lastActionPercent == 0xFF)
            ? 0xFF
            : (uint8_t)(((uint16_t)lastActionPercent * 12u * 5u / 100u) / 5u);

        if (newCells != lastCells)
        {
            lastActionPercent = percent;
            screenUpdateKitProgress(percent);
        }

        uint8_t expectedResp =
            kitActionCmd(kitActiveAction) | UART_RESPONSE_BIT;

        int8_t idx =
            uartFindResponse(expectedResp, kitActionSeq);

        if (idx >= 0)
        {
            UartResponse resp;

            if (uartRemoveAt(idx, resp))
            {
                kitActionPending = false;

                if (resp.status == RESP_OK)
                {
                    kitActionResultKind = 1;

                    switch (kitActiveAction)
                    {
                        case KitAction::LOAD:
                            activeKit = (uint8_t)(resp.arg0 & 0xFF);
                            if (activeKit > UI_MAX_KIT)
                            {
                                activeKit = UI_MAX_KIT;
                            }
                            activeKitKnown = true;

                            // Daftar/suara pad TIDAK boleh basi setelah pindah kit.
                            padCacheGen++;
                            break;

                        case KitAction::DELETE:
                            // Backend menjamin fallback ke kit factory ketika kit AKTIF dihapus (kit_manager.cpp).
                            if (kitSubject == activeKit)
                            {
                                activeKit = UI_FACTORY_KIT;
                                padCacheGen++;
                            }
                            break;

                        default:
                            // CREATE / SAVE / RENAME tidak mengubah ACTIVE KIT.
                            break;
                    }

                    // Name cache di-refresh agar setiap perubahan nama/isi terlihat di layar berikutnya.
                    invalidateKitName();
                }
                else
                {
                    kitActionResultKind = 3;

                    // G24.15: NO SPACE diperiksa lebih dulu — saat semua slot
                    // user terisi, backend tak pernah mengirim BUSY, jadi tak
                    // ada tumpang tindih.
                    kitActionNoSpace = (resp.status == RESP_NO_SPACE);

                    kitActionBusy =
                        (resp.status == RESP_BUSY) && !kitActionNoSpace;

                    kitActionNameExists =
                        isNameAction(kitActiveAction) &&
                        (resp.status == RESP_NAME_EXISTS);

                    kitActionNameInvalid =
                        isNameAction(kitActiveAction) &&
                        (resp.status == RESP_INVALID_VALUE);
                }

                kitActionResultMs = now;

                displayTransitionBegin();

                renderKitActionResult();

                return;
            }
        }
        else if (now - kitActionStartMs >= KIT_ACTION_TIMEOUT_MS)
        {
            kitActionPending = false;

            kitActionResultKind = 4;   // timeout
            kitActionResultMs = now;

            displayTransitionBegin();

            renderKitActionResult();

            return;
        }

        return;
    }

    // Tahan hasil.

    if (encoderPressed())
    {
        encoderFlush();
        enterKit();
        return;
    }

    if (displayTransitionActive())
    {
        renderKitActionResult();
    }

    if (now - kitActionResultMs >= KIT_RESULT_HOLD_MS)
    {
        enterKit();
    }
}

// ----- PAD BROWSER (PAD 01 .. PAD 12) -----

static void enterPadSelect()
{
    uiState = UIState::PAD;

    padIdleMs = millis();

    encoderFlush();

    screenShowPadSelect(menuGetPadIndex());
}

static void handlePadSelect()
{
    uint32_t now = millis();

    if (encoderLongPressed())
    {
        encoderFlush();
        enterMenu();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        padIdleMs = now;

        menuNavPad(delta);

        screenShowPadSelect(menuGetPadIndex());
        return;
    }

    if (encoderPressed())
    {
        encoderFlush();
        enterPadEdit();
        return;
    }

    if (now - padIdleMs >= PAD_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- PAD EDIT (per-pad parameter groups) -----

static void enterPadEdit()
{
    uiState = UIState::PAD_EDIT;

    menuPadEditReset();
    padIdleMs = millis();

    encoderFlush();

    screenShowPadEdit(menuGetPadIndex(), menuGetPadEditIndex());
}

static void handlePadEdit()
{
    uint32_t now = millis();

    if (encoderLongPressed())
    {
        encoderFlush();
        enterPadSelect();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        padIdleMs = now;

        menuNavPadEdit(delta);

        screenShowPadEdit(menuGetPadIndex(), menuGetPadEditIndex());
        return;
    }

    if (encoderPressed())
    {
        encoderFlush();

        uint8_t idx = menuGetPadEditIndex();

        if (idx == menuGetPadEditBackIndex())
        {
            enterPadSelect();
        }
        else if (idx == 0) // SOUND
        {
            enterPadSound();
        }
        else if (idx == 1) // MAPPING
        {
            enterPadMapping();
        }
        else if (idx == 2) // TRIGGER
        {
            enterPadTrigger();
        }

        return;
    }

    if (now - padIdleMs >= PAD_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- PAD SUB-LISTS (MAPPING / TRIGGER parameter groups) -----

static void enterPadMapping()
{
    uiState = UIState::PAD_MAPPING;

    menuSubListReset(MENU_LIST_PAD_MAPPING);
    padIdleMs = millis();

    encoderFlush();

    screenShowPadSubList(
        menuGetPadIndex(),
        MENU_LIST_PAD_MAPPING,
        menuSubListGetIndex(MENU_LIST_PAD_MAPPING));
}

static void enterPadTrigger()
{
    uiState = UIState::PAD_TRIGGER;

    menuSubListReset(MENU_LIST_PAD_TRIGGER);
    padIdleMs = millis();

    encoderFlush();

    screenShowPadSubList(
        menuGetPadIndex(),
        MENU_LIST_PAD_TRIGGER,
        menuSubListGetIndex(MENU_LIST_PAD_TRIGGER));
}

static void handlePadSubList(uint8_t listId)
{
    uint32_t now = millis();

    if (encoderLongPressed())
    {
        encoderFlush();
        enterPadEdit();
        return;
    }

    int8_t delta = encoderGetDelta();

    if (delta != 0)
    {
        padIdleMs = now;

        menuSubListNavigate(listId, delta);

        screenShowPadSubList(
            menuGetPadIndex(),
            listId,
            menuSubListGetIndex(listId));
        return;
    }

    if (encoderPressed())
    {
        padIdleMs = now;
        encoderFlush();

        uint8_t idx = menuSubListGetIndex(listId);

        if (idx == menuSubListGetBackIndex(listId))
        {
            enterPadEdit();
            return;
        }

        if (listId == MENU_LIST_PAD_MAPPING)
        {
            // MIDI NOTE satu-satunya item fungsional.
            if (idx == 0)
            {
                enterPadValue(0);
            }
        }
        else if (listId == MENU_LIST_PAD_TRIGGER)
        {
            // TRIGGER list meniru slot TRIGGER_FIELD_IDX di PAD_VALUE_FIELDS[1..5] secara berurutan.
            static const uint8_t TRIGGER_FIELD_IDX[] = { 1, 2, 3, 4, 5 };

            if (idx < sizeof(TRIGGER_FIELD_IDX) / sizeof(TRIGGER_FIELD_IDX[0]))
            {
                enterPadValue(TRIGGER_FIELD_IDX[idx]);
            }
        }

        return;
    }

    if (now - padIdleMs >= PAD_IDLE_BACK_MS)
    {
        encoderFlush();
        enterMain();
    }
}

// ----- PAD SOUND (daftar suara kit aktif, G9) -----
// padSoundResultKind: 1 = OK, 3 = gagal set (BUSY/INVALID), 4 = TIMEOUT,
// 5 = kit aktif tak punya file WAV sama sekali. G24: kind 5 dipisah dari
// "INVALID" agar folder kosong dilaporkan jujur sebagai NO SAMPLES, bukan
// error generik.

static void showPadSoundResult()
{
    char title[8];
    snprintf(title, sizeof(title), "PAD %02u", menuGetPadIndex() + 1);

    switch (padSoundResultKind)
    {
        case 1:
            screenShowActionResult(title, "SOUND OK");
            break;
        case 4:
            screenShowActionResult(title, "TIMEOUT");
            break;
        case 5:
            screenShowActionResult(title, "NO SAMPLES");
            break;
        default:
            screenShowActionResult(title, padSoundBusy ? "BUSY" : "INVALID");
            break;
    }
}

static void enterPadSound()
{
    uiState = UIState::PAD_SOUND;

    padSoundPhase = PadSoundPhase::LOAD;
    padSoundListLoaded = false;
    padSoundCount = 0;
    padSoundCursor = 0;
    padSoundCurrent = 0;
    padSoundResultKind = 0;
    padSoundBusy = false;
    padSoundGenAtRequest = padCacheGen;
    padIdleMs = millis();

    encoderFlush();

    screenShowPadSoundLoading(menuGetPadIndex());

    // Satu request: daftar WAV kit aktif utk pad ini. Current index suara pad
    // dikirim ESP32 di header daftar (ARG3) — tak perlu PAD_GET
    // FIELD_SAMPLE_ID lagi (mapping by filename).
    uartPadSoundRequest(menuGetPadIndex());
}

static void handlePadSound()
{
    uint32_t now = millis();

    // LOAD: kumpulkan daftar (+ current index dari header).
    if (padSoundPhase == PadSoundPhase::LOAD)
    {
        // Kit aktif berubah di tengah pembacaan -> daftar lama tak sah; minta
        // ulang agar daftar selalu dari kit saat ini.
        if (padCacheGen != padSoundGenAtRequest)
        {
            padSoundGenAtRequest = padCacheGen;
            padSoundListLoaded = false;
            padSoundCount = 0;
            uartPadSoundRequest(menuGetPadIndex());
        }

        if (!padSoundListLoaded)
        {
            if (uartPadSoundAvailable())
            {
                uint8_t currentIndex = 0xFF;

                if (uartPadSoundGet(padSoundNames, currentIndex))
                {
                    padSoundListLoaded = true;
                    padSoundCurrent = currentIndex;
                }
                else
                {
                    // Daftar timeout/gagal -> kegagalan jujur.
                    padSoundResultKind = 4;
                    padSoundPhase = PadSoundPhase::RESULT;
                    padSoundStampMs = now;
                    displayTransitionBegin();
                    showPadSoundResult();
                    return;
                }
            }
        }

        if (padSoundListLoaded)
        {
            // Jumlah entri = yang ter-scan (folder samples), bukan konstanta
            // pad. Nama "kosong" di luar rentang dikosongkan agar tak pernah
            // tampil.
            uint8_t i = 0;

            while (padSoundNames[i][0] != '\0' && i < PAD_SOUND_COUNT)
            {
                i++;
            }

            padSoundCount = i;

            if (padSoundCount == 0)
            {
                // Kit aktif tak punya WAV di folder samples — laporkan apa
                // adanya, jangan pretend success.
                padSoundResultKind = 5;
                padSoundBusy = false;
                padSoundPhase = PadSoundPhase::RESULT;
                padSoundStampMs = now;
                displayTransitionBegin();
                showPadSoundResult();
                return;
            }

            padSoundCursor = (padSoundCurrent < padSoundCount)
                ? padSoundCurrent : 0;
            padSoundPhase = PadSoundPhase::BROWSE;
            padIdleMs = now;

            screenShowPadSound(
                menuGetPadIndex(),
                padSoundNames[padSoundCursor]);
            return;
        }

        if (now - padIdleMs >= PAD_IDLE_BACK_MS)
        {
            encoderFlush();
            enterMain();
        }
        return;
    }

    // BROWSE: pilih suara (index list == nama file).
    if (padSoundPhase == PadSoundPhase::BROWSE)
    {
        if (encoderLongPressed())
        {
            encoderFlush();
            enterPadEdit();
            return;
        }

        int8_t delta = encoderGetDelta();

        if (delta != 0)
        {
            padIdleMs = now;

            int16_t nc = (int16_t)padSoundCursor + ((delta > 0) ? 1 : -1);

            if (nc < 0)
            {
                nc = (int16_t)padSoundCount - 1;
            }
            else if (nc >= (int16_t)padSoundCount)
            {
                nc = 0;
            }

            padSoundCursor = (uint8_t)nc;

            screenShowPadSound(
                menuGetPadIndex(),
                padSoundNames[padSoundCursor]);
            return;
        }

        if (encoderPressed())
        {
            encoderFlush();

            padSoundPhase = PadSoundPhase::WRITE;
            padSoundSeq = uartPadSoundSetRequest(
                menuGetPadIndex(),
                padSoundNames[padSoundCursor]);
            padSoundStampMs = now;
            padIdleMs = now;

            screenShowPadSetting(menuGetPadIndex());
            return;
        }

        if (now - padIdleMs >= PAD_IDLE_BACK_MS)
        {
            encoderFlush();
            enterMain();
        }
        return;
    }

    // WRITE: tunggu respons PAD_SOUND_SET (satu frame status).
    if (padSoundPhase == PadSoundPhase::WRITE)
    {
        int8_t idx = uartFindResponse(
            CMD_PAD_SOUND_SET | UART_RESPONSE_BIT,
            padSoundSeq);

        if (idx >= 0)
        {
            UartResponse resp;

            if (uartRemoveAt(idx, resp))
            {
                if (resp.status == RESP_OK)
                {
                    padSoundResultKind = 1;
                }
                else
                {
                    padSoundResultKind = 3;
                    padSoundBusy = (resp.status == RESP_BUSY);
                }

                padSoundPhase = PadSoundPhase::RESULT;
                padSoundStampMs = now;
                displayTransitionBegin();
                showPadSoundResult();
            }
            return;
        }

        if (now - padSoundStampMs >= PAD_VALUE_CMD_TIMEOUT_MS)
        {
            padSoundResultKind = 4;
            padSoundPhase = PadSoundPhase::RESULT;
            padSoundStampMs = now;
            displayTransitionBegin();
            showPadSoundResult();
        }
        return;
    }

    // RESULT: tahan singkat, auto-kembali ke PAD_EDIT.
    if (encoderPressed())
    {
        encoderFlush();
        enterPadEdit();
        return;
    }

    if (displayTransitionActive())
    {
        showPadSoundResult();
    }

    if (now - padSoundStampMs >= PAD_RESULT_HOLD_MS)
    {
        encoderFlush();
        enterPadEdit();
    }
}

// ----- PAD VALUE (editor numerik satu field, G9) -----

static void showPadValueResult()
{
    const PadValueField& fld = PAD_VALUE_FIELDS[padValueFieldIdx];

    switch (padValueResultKind)
    {
        case 1:
        {
            char value[8];
            snprintf(value, sizeof(value), "%u", padValueApplied);
            screenShowActionResult(fld.label, value);
            break;
        }
        case 4:
            screenShowActionResult(fld.label, "TIMEOUT");
            break;
        default:
            if (padValueBusy)
            {
                screenShowActionResult(fld.label, "BUSY");
            }
            else if (padValueNameInvalid)
            {
                screenShowActionResult(fld.label, "INVALID");
            }
            else
            {
                screenShowActionResult(fld.label, "FAILED");
            }
            break;
    }
}

static void enterPadValueReturn()
{
    if (PAD_VALUE_FIELDS[padValueFieldIdx].fieldId == FIELD_MIDI_NOTE)
    {
        enterPadMapping();
    }
    else
    {
        enterPadTrigger();
    }
}

static void enterPadValue(uint8_t fieldIdx)
{
    uiState = UIState::PAD_VALUE;

    padValueFieldIdx = fieldIdx;
    padValuePad = menuGetPadIndex();
    padValuePhase = PadValuePhase::READ;
    padValueResultKind = 0;
    padValueBusy = false;
    padValueNameInvalid = false;
    padValueDisplay = 0;
    padValueApplied = 0;
    padValueSpinAccum = 0;
    padIdleMs = millis();

    encoderFlush();

    screenShowPadValueLoading(PAD_VALUE_FIELDS[fieldIdx].label);

    padValueSeq = uartPadGetRequest(
        padValuePad,
        PAD_VALUE_FIELDS[fieldIdx].fieldId);
}

static void handlePadValue()
{
    uint32_t now = millis();
    const PadValueField& fld = PAD_VALUE_FIELDS[padValueFieldIdx];

    // READ: tunggu nilai saat ini.
    if (padValuePhase == PadValuePhase::READ)
    {
        if (uartPadGetAvailable())
        {
            uint8_t pad = 0;
            uint8_t field = 0;
            uint16_t value = 0;

            if (uartPadGetGet(pad, field, value))
            {
                if (value < fld.min)
                {
                    value = fld.min;
                }
                else if (value > fld.max)
                {
                    value = fld.max;
                }

                padValueDisplay = value;
                padValueSpinAccum = 0;
                padValuePhase = PadValuePhase::EDIT;

                screenShowPadValue(fld.label, padValueDisplay);
            }
            else
            {
                // Nilai saat ini gagal dibaca -> kegagalan jujur.
                padValueResultKind = 4;
                padValuePhase = PadValuePhase::RESULT;
                padValueStampMs = now;
                displayTransitionBegin();
                showPadValueResult();
            }
        }
        return;
    }

    // EDIT.
    if (padValuePhase == PadValuePhase::EDIT)
    {
        if (encoderLongPressed())
        {
            encoderFlush();
            enterPadValueReturn();
            return;
        }

        int8_t delta = encoderGetDelta();

        if (delta != 0)
        {
            padIdleMs = now;

            // Akselerasi putar: run cepat dalam jendela 1.2 s menaikkan step 1 -> 10 -> 100.
            if (now - padValueSpinMs >= 1200)
            {
                padValueSpinAccum = 0;
            }
            padValueSpinMs = now;
            padValueSpinAccum += (uint16_t)((delta < 0) ? -delta : delta);

            uint16_t step = 1;
            if (padValueSpinAccum >= 90)
            {
                step = 100;
            }
            else if (padValueSpinAccum >= 30)
            {
                step = 10;
            }

            int16_t inc = (delta > 0) ? (int16_t)step : -(int16_t)step;

            int32_t nv = (int32_t)padValueDisplay + inc;

            if (nv < fld.min)
            {
                nv = fld.min;
            }
            else if (nv > fld.max)
            {
                nv = fld.max;
            }

            padValueDisplay = (uint16_t)nv;

            screenShowPadValue(fld.label, padValueDisplay);
            return;
        }

        if (encoderPressed())
        {
            encoderFlush();

            padValuePhase = PadValuePhase::WRITE;
            padValueApplied = padValueDisplay;
            padValueSeq = uartPadSetRequest(padValuePad, fld.fieldId, padValueApplied);
            padValueStampMs = now;
            padIdleMs = now;

            screenShowPadValueLoading(fld.label);
            return;
        }

        if (now - padIdleMs >= PAD_IDLE_BACK_MS)
        {
            encoderFlush();
            enterMain();
        }
        return;
    }

    // WRITE: tunggu respons PAD_SET di FIFO umum.
    if (padValuePhase == PadValuePhase::WRITE)
    {
        int8_t idx = uartFindResponse(
            CMD_PAD_SET | UART_RESPONSE_BIT,
            padValueSeq);

        if (idx >= 0)
        {
            UartResponse resp;

            if (uartRemoveAt(idx, resp))
            {
                if (resp.status == RESP_OK)
                {
                    padValuePhase = PadValuePhase::READBACK;
                    padValueSeq = uartPadGetRequest(padValuePad, fld.fieldId);
                }
                else
                {
                    padValueResultKind = 3;
                    padValueBusy = (resp.status == RESP_BUSY);
                    padValueNameInvalid = (resp.status == RESP_INVALID_VALUE);

                    padValuePhase = PadValuePhase::RESULT;
                    padValueStampMs = now;
                    displayTransitionBegin();
                    showPadValueResult();
                }
            }
            return;
        }

        if (now - padValueStampMs >= PAD_VALUE_CMD_TIMEOUT_MS)
        {
            padValueResultKind = 4;
            padValuePhase = PadValuePhase::RESULT;
            padValueStampMs = now;
            displayTransitionBegin();
            showPadValueResult();
        }
        return;
    }

    // READBACK: tunggu nilai efektif, lalu tampilkan hasil.
    if (padValuePhase == PadValuePhase::READBACK)
    {
        if (uartPadGetAvailable())
        {
            uint8_t pad = 0;
            uint8_t field = 0;
            uint16_t value = 0;

            if (uartPadGetGet(pad, field, value))
            {
                padValueApplied = value;
            }

            // Sudah dikonfirmasi RESP_OK; GET ini menegaskan nilai.
            padValueResultKind = 1;
            padValuePhase = PadValuePhase::RESULT;
            padValueStampMs = now;
            displayTransitionBegin();
            showPadValueResult();
        }
        return;
    }

    // RESULT: tahan singkat, auto-kembali ke sub-list asal.
    if (encoderPressed())
    {
        encoderFlush();
        enterPadValueReturn();
        return;
    }

    if (displayTransitionActive())
    {
        showPadValueResult();
    }

    if (now - padValueStampMs >= PAD_RESULT_HOLD_MS)
    {
        encoderFlush();
        enterPadValueReturn();
    }
}

// ----- UI INIT / UPDATE -----

void uiInit()
{
    displayInit();
    encoderInit();
    uartClientInit();
    menuInit();

    uiState = UIState::BOOT;

    bootSplashDone = false;
    bootStartMs = 0;
    lastBootFrame = 0xFF;
    lastInitStep = 0xFF;

    lastUpdateMs = millis();
    lastPingMs = millis();

    activeKit = 0;
    activeKitKnown = false;

    invalidateKitName();

    mainStatusPending = false;
    mainStatusSeq = 0;
    mainStatusStartMs = 0;

    selectedKit = 0;
    kitSelectPurpose = KitSelectPurpose::LOAD;

    confirmState.action = KitAction::NONE;
    confirmState.kit = 0;
    confirmState.origin = UIState::KIT_SELECT;
    confirmChoice = 0;

    createMethod = KitCreateMethod::EMPTY;

    kitSubject = 0;
    kitActiveAction = KitAction::NONE;
    kitActionSent = false;
    kitActionPending = false;
    kitActionStartMs = 0;
    kitActionSeq = 0;
    kitActionResultMs = 0;
    kitActionResultKind = 0;
    kitActionNameExists = false;
    kitActionNameInvalid = false;
    kitActionNoSpace = false;

    kitNavIdleMs = 0;

    memset(renameBuf, 0, sizeof(renameBuf));
    renameCursor = 0;

    padIdleMs = 0;

    padCacheGen = 0;

    padValuePhase = PadValuePhase::READ;
    padValueFieldIdx = 0;
    padValuePad = 0;
    padValueDisplay = 0;
    padValueApplied = 0;
    padValueSeq = 0;
    padValueStampMs = 0;
    padValueResultKind = 0;
    padValueBusy = false;
    padValueNameInvalid = false;
    padValueSpinMs = 0;
    padValueSpinAccum = 0;

    padSoundPhase = PadSoundPhase::LOAD;
    padSoundListLoaded = false;
    padSoundCount = 0;
    padSoundCursor = 0;
    padSoundCurrent = 0;
    padSoundSeq = 0;
    padSoundStampMs = 0;
    padSoundResultKind = 0;
    padSoundBusy = false;
    padSoundGenAtRequest = 0;
}

// ----- UI UPDATE -----

void uiUpdate()
{
    uint32_t now = millis();

    if (now - lastUpdateMs < UI_UPDATE_MS)
    {
        return;
    }

    lastUpdateMs = now;

    encoderUpdate();
    uartClientUpdate();

    switch (uiState)
    {
        case UIState::BOOT:
            handleBoot();
            break;

        case UIState::INIT:
            handleInit();
            break;

        case UIState::READY:
            handleReady();
            break;

        case UIState::CHECK_CONN:
            handleCheckConn();
            break;

        case UIState::MAIN:
            handleMain();
            break;

        case UIState::MENU:
            handleMenu();
            break;

        case UIState::KIT:
            handleKit();
            break;

        case UIState::KIT_SELECT:
            handleKitSelect();
            break;

        case UIState::KIT_CREATE:
            handleKitCreate();
            break;

        case UIState::KIT_CREATE_NAME:
            handleKitCreateName();
            break;

        case UIState::KIT_CONFIRM:
            handleKitConfirm();
            break;

        case UIState::KIT_RENAME:
            handleKitRename();
            break;

        case UIState::KIT_ACTION:
            handleKitAction();
            break;

        case UIState::PAD:
            handlePadSelect();
            break;

        case UIState::PAD_EDIT:
            handlePadEdit();
            break;

        case UIState::PAD_SOUND:
            handlePadSound();
            break;

        case UIState::PAD_MAPPING:
            handlePadSubList(MENU_LIST_PAD_MAPPING);
            break;

        case UIState::PAD_TRIGGER:
            handlePadSubList(MENU_LIST_PAD_TRIGGER);
            break;

        case UIState::PAD_VALUE:
            handlePadValue();
            break;
    }

    // PING periodik hanya di MAIN (health check di home screen).
    if (uiState == UIState::MAIN &&
        now - lastPingMs >= PING_INTERVAL_MS)
    {
        lastPingMs = now;

        uartSendPing();
    }

    // Konsumsi respons PING di luar handshake INIT/CHECK_CONN.
    if (uiState != UIState::INIT &&
        uiState != UIState::CHECK_CONN)
    {
        (void)uartPollPing();
    }
}

// ----- CURRENT UI STATE -----

UIState uiGetCurrentState()
{
    return uiState;
}
