# DrumCore Documentation

**Version:** 0.4.0  
**Status:** Feature Freeze — Hardware Calibration Phase  
**Platform:** ESP32-S3 + Wemos D1 Mini (ESP8266)

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [System Architecture](#2-system-architecture)
3. [Hardware](#3-hardware)
4. [Pin / GPIO Mapping](#4-pin--gpio-mapping)
5. [Firmware Architecture — DrumCore (ESP32-S3)](#5-firmware-architecture--drumcore-esp32-s3)
6. [Trigger System](#6-trigger-system)
7. [ADC and Calibration](#7-adc-and-calibration)
8. [Audio Engine](#8-audio-engine)
9. [PSRAM and Sample System](#9-psram-and-sample-system)
10. [Kit System](#10-kit-system)
11. [PAD System](#11-pad-system)
12. [UART Protocol](#12-uart-protocol)
13. [MIDI](#13-midi)
14. [SD Card Structure](#14-sd-card-structure)
15. [Configuration / JSON](#15-configuration--json)
16. [Firmware Architecture — DrumCore_UI (Wemos D1 Mini)](#16-firmware-architecture--drumcore_ui-wemos-d1-mini)
17. [Build and Flash](#17-build-and-flash)
18. [Debug / Calibration Tools](#18-debug--calibration-tools)
19. [Current Project Status](#19-current-project-status)
20. [Deferred Features](#20-deferred-features)
21. [Important Constraints](#21-important-constraints)
22. [Development History / Key Decisions](#22-development-history--key-decisions)

---

## 1. Project Overview

DrumCore adalah modul drum elektronik DIY berbasis ESP32-S3 dengan target kualitas
setara drum module kelas atas: latency rendah, velocity sensitif, polifoni tinggi,
dan roll cepat yang aman.

**Komponen utama:**
- **ESP32-S3 N16R8** — realtime drum engine (ADC, trigger, audio, PSRAM, UART server)
- **Wemos D1 Mini (ESP8266)** — UI controller (rotary encoder, LCD, UART client)
- **PCM5102A** — DAC I2S stereo
- **SD card** — penyimpanan kit JSON + WAV sample
- **12 piezo pad** — sensor pukulan (ADC1 ESP32-S3)

**Repo structure:**
```
PROJEK DRUMPAD JOSJISS/
├── DrumCore/           ESP32-S3 firmware (main engine)
├── DrumCore_UI/        Wemos D1 Mini firmware (UI)
├── host_tests/         Python host test suite
├── SDCard/             SD card staging data (kits/)
├── DOCUMENTATION.md    ← file ini
└── README.md           Entry point ringkas
```

---

## 2. System Architecture

```
[12 Piezo Pad] ─ADC1─► [ESP32-S3 DrumCore]
                              │
                         [PSRAM 8MB]     [SD Card]
                              │
                         [PCM5102A I2S] ─► [Speaker/Amp]
                              │
                         [UART1 921600] ─► [Wemos D1 Mini]
                                                │
                                          [LCD 16x2 I2C]
                                          [Rotary Encoder]
```

**FreeRTOS Task Layout:**

| Task | Core | Priority | Fungsi |
|---|---|---|---|
| AudioTask | 0 | 24 | DMA mixing + I2S output + voice management |
| TriggerTask | 1 | 23 | ADC scan batch 12 + state machine per pad |
| MidiTask | 1 | 8 | USB MIDI note stack |
| UartTask | 1 | 5 | UART control plane (kit/pad management) |

`loop()` berjalan di Core 1 sebagai slot diagnostic non-realtime.
TriggerTask dan AudioTask berjalan di FreeRTOS tasks; `loop()` tidak memproses drum.

**Realtime hit path:**
```
TriggerTask (Core1)
  └── updatePad() state machine
        └── calculateVelocity() [LUT, zero-float]
              └── audioTrigger() → SPSC hit ring
                    └── AudioTask (Core0)
                          └── fireHit() → startVoice()
                                └── prefetch + mix → I2S DMA
```

---

## 3. Hardware

### ESP32-S3 Side

| Komponen | Interface | Keterangan |
|---|---|---|
| 12 Piezo pad | ADC1 GPIO 4,5,6,7,8,9,10,11,12,15,16,17 | Semua ADC1, tidak konflik WiFi |
| PCM5102A DAC | I2S BCLK=42, LRCK=21, DOUT=2 | Audio stereo 48kHz |
| SD Card | SPI MOSI=38, MISO=39, SCK=40, CS=41 | Kit + sample storage |
| UART ke Wemos | UART1 TX=GPIO1, RX=GPIO3 | Control plane 921600 baud |
| UART debug | UART0 GPIO43(TX)/44(RX) | Serial0 di code, 115200 |
| EXT Trigger | GPIO13 | Assigned, belum diimplementasi (HOLD) |
| Hi-Hat Switch | GPIO14 | Assigned, belum diimplementasi (HOLD) |
| USB MIDI | GPIO19/20 | Native USB ESP32-S3 (future) |

### Wemos D1 Mini Side

| Komponen | Interface | Keterangan |
|---|---|---|
| LCD 16x2 | I2C 0x27 (D1=SCL, D2=SDA) | Menu display |
| Rotary Encoder S1/S2 | GPIO D5/D6 | Navigasi |
| Encoder Button | GPIO D7 | Confirm/select |
| UART ke ESP32 | UART0 TX=GPIO1, RX=GPIO3 | 921600 baud 8N1 |

**Wemos UART:** Hanya satu hardware UART dengan RX pada ESP8266 (UART0).
UART0 dipakai full-duplex untuk link ke ESP32-S3. Serial Monitor USB
tidak tersedia saat firmware berjalan.

---

## 4. Pin / GPIO Mapping

### PAD ADC Pins (ESP32-S3)

```
PAD_PINS[] = { 4, 5, 6, 7, 15, 16, 17, 8, 9, 10, 11, 12 }
```

| PAD Index (0-based) | PAD Number (user-facing) | GPIO |
|---|---|---|
| 0 | PAD01 | GPIO4 |
| 1 | PAD02 | GPIO5 |
| 2 | PAD03 | GPIO6 |
| 3 | PAD04 | GPIO7 |
| 4 | PAD05 | GPIO15 |
| 5 | PAD06 | GPIO16 |
| 6 | PAD07 | GPIO17 |
| 7 | PAD08 | GPIO8 |
| 8 | PAD09 | GPIO9 |
| 9 | PAD10 | GPIO10 |
| 10 | PAD11 | GPIO11 |
| 11 | PAD12 | GPIO12 |

> **CATATAN:** Mapping PAD → GPIO di atas adalah mapping DEFAULT dari `config.h`.
> Mapping fisik aktual (pad fisik mana yang terhubung ke GPIO mana) belum
> dikonfirmasi — user harus melakukan hardware calibration setelah wiring selesai.
> Ubah `PAD_PINS[]` di `config.h` sebagai satu-satunya sumber kebenaran.

### I2S / Audio

| Fungsi | GPIO |
|---|---|
| I2S BCLK | 42 |
| I2S LRCK | 21 |
| I2S DOUT | 2 |

### SD SPI

| Fungsi | GPIO |
|---|---|
| MOSI | 38 |
| MISO | 39 |
| SCK | 40 |
| CS | 41 |

### UART

| Fungsi | GPIO |
|---|---|
| UART1 TX (ke Wemos RX) | 1 |
| UART1 RX (dari Wemos TX) | 3 |
| UART0 TX (debug Serial0) | 43 |
| UART0 RX (debug Serial0) | 44 |

---

## 5. Firmware Architecture — DrumCore (ESP32-S3)

### File Structure

```
DrumCore/
├── DrumCore.ino          Setup + FreeRTOS task start + loop() diagnostics
├── config.h              Pin map, constants, debug flags, field bounds
├── hardware.h/.cpp       ADC round-robin cache, batch scan, GPIO init
├── pad.h/.cpp            PadConfig + Pad runtime struct, padInit()
├── trigger.h/.cpp        Trigger engine: state machine, updatePad(), task
├── velocity.h/.cpp       Velocity LUT (zero-float runtime)
├── midi.h/.cpp           USB MIDI note stack, task
├── audio.h/.cpp          Audio engine: voice, PSRAM read, I2S mix, task
├── sdcard.h/.cpp         SD SPI init
├── psram_loader.h/.cpp   WAV→PSRAM loader, per-slot ownership, registry
├── kit_loader.h/.cpp     kit.json parser, sample path resolver
├── kit_manager.h/.cpp    Kit CRUD (create/rename/delete/save/load)
├── persistence.h/.cpp    NVS (global: activeKit, volume)
├── uart_protocol.h/.cpp  UART control plane parser + dispatcher + task
├── adc_calibration.h     ADC calibration tool (DEBUG only)
├── adc_calibration.cpp   ADC calibration tool implementation (DEBUG only)
└── platformio.ini        Build config (esp32-s3-devkitc-1, QIO, OPI PSRAM)
```

### Boot Sequence

```
setup()
  hardwareInit()         ADC config, pinMode, round-robin reset
  padInit()              PadConfig dari config.h defaults
  triggerInit()          Reset pad state machines
  midiInit()             MIDI init
  sdInit()               SD SPI mount
  buildVelocityLut()     Pre-compute LUT (4096 entries, zero-float setelahnya)
  persistenceInit()      NVS init
  persistenceLoadGlobal() Baca activeKit + volume dari NVS
  kitManagerMigrateNvsOverrides() Migrasi legacy NVS blob → kit.json (sekali)
  kitLoaderResolveKitDir() Validasi activeKit; fallback ke kit00 bila perlu
  kitLoaderLoad()         Parse kit.json → PadConfig + resolved sample paths
  psramSetup()            Cek PSRAM
  psramLoadKit()          Load 12 WAV → PSRAM (per-slot blocks, G25)
  audioInit()             I2S init + gain table
  uartProtocolInit()      UART1 parser init
  adcCalibrationInit()    [hanya bila DEBUG_ADC_CALIBRATION=true]
  audioTaskStart()        Core0 prio24
  triggerTaskStart()      Core1 prio23
  midiTaskStart()         Core1 prio8
  uartTaskStart()         Core1 prio5
```

---

## 6. Trigger System

File: `trigger.h/.cpp`, `hardware.h/.cpp`

### ADC Scan

ADC dibaca secara round-robin dari cache (`adcCache[]`). TriggerTask
memanggil `adcCacheScanBatch(12)` setiap pass — semua 12 pad dibaca dalam
satu pass, sehingga cadence per pad = ~1 pass (~1.3–1.8 ms). Ini
deterministik dan tidak bergantung pada jumlah active voices.

`readPadADC(padIndex)` mengembalikan nilai dari cache — O(1), non-blocking.

### Pad State Machine

```
IDLE
  │  ADC >= threshold
  ▼
WAIT_PEAK  ←─── track max peak
  │  (a) peak >= 2200 AND elapsed >= 800µs  [adaptive early fire]
  │  (b) elapsed >= peakTime (1200µs)       [normal]
  ▼
MASK  ─── maskTime = adjustMaskTime(maskBaseMs, velocity) * 1000µs
  │  elapsed >= maskTime
  ▼
WAIT_RELEASE
  │  ADC <= releaseThreshold  AND  stable >= releaseStableTime
  ▼
IDLE
```

### Default Per-Pad Parameters (dari config.h + pad.cpp)

| Parameter | Nilai |
|---|---|
| threshold | 300 |
| releaseThreshold | 250 |
| peakTime | 1200 µs |
| releaseStableTime | 3000 µs |
| maskBaseMs (kick, PAD0) | 30 ms |
| maskBaseMs (cymbal) | 17 ms |
| maskBaseMs (lainnya) | 15 ms |

`maskTime` runtime = `adjustMaskTime(maskBaseMs, velocity) * 1000` µs.
Adjustment ±30% berdasarkan velocity (hard hit → mask lebih panjang).

### Velocity

`calculateVelocity(peak)` → lookup LUT 4096 entries (dibangun sekali saat boot).
Tidak ada floating-point di runtime. Kurva non-linear (gamma).

Per-pad sensitivity scaling (tuning): sebelum lookup, peak ADC dikali
`pad.config.sensitivity/100` (default 100 = netral, hasil identik).
Contoh: pad kecil dengan peak 150 dan sensitivity 255 → peak efektif 382.

---

## 7. ADC and Calibration

### Normal Operation

- 12-bit ADC resolution (`analogReadResolution(12)`)
- Attenuation 11dB (`analogSetAttenuation(ADC_11db)`)
- Round-robin cache: satu pad per `adcCacheScan()`, atau batch 12 via `adcCacheScanBatch(12)`
- Cache age tracking tersedia bila `PROFILE_ADC_STALENESS = true`

### ADC Calibration Tool *(DEVELOPMENT / DEBUG ONLY)*

File: `adc_calibration.h`, `adc_calibration.cpp`

Dikontrol via `#define DEBUG_ADC_CALIBRATION` di `config.h`
(`1` = aktif, `0` = seluruh kode compile-out, zero runtime overhead).
**Saat ini: `1` (fase hardware tuning). Kembalikan ke `0` setelah
tuning selesai** — host test `test_gadc_debug_flag` mengharapkan
flag dalam keadaan mati.

**Tujuan:** Mengukur data hardware sebelum menentukan threshold atau tuning parameter.
Tidak mengubah trigger/audio/PSRAM. Dipanggil dari `loop()` (Core 1, non-realtime),
hanya membaca `adcCache[]` — data yang dilihat tool sama dengan yang dilihat engine.

**Mode auto-capture** (tanpa serial command, tanpa user intervention):

| Output | Fungsi |
|---|---|
| `[BASELINE]` tiap 3 detik | Idle noise per pad: min, avg, max, range |
| `[HIT] PADxx GPIOx peak=x` | Setiap pad yang melewati `CAL_HIT_THRESHOLD` (30) lalu turun kembali — peak pukulan |
| `[CROSSTALK]` | Jika source peak ≥ `XTALK_SOURCE_THRESHOLD` (400): window observasi 50 ms dibuka **saat hit terdeteksi** (bukan saat hit selesai), max-hold leak ke 11 pad lain, lalu cetak source peak + leak tiap victim + WORST |

Contoh output:

```
[BASELINE]
PAD01 GPIO4   idle_min=   8  idle_avg=  12  idle_max=  19  range=  11
...

[HIT] PAD03 GPIO6  peak=2847

[CROSSTALK]
SOURCE PAD03 GPIO6  peak=2847
PAD01 GPIO4   leak=45
...
WORST PAD08 GPIO8  peak=45
```

**Alur tuning:**

1. Diamkan pad → baca `[BASELINE]` untuk noise floor tiap pad.
2. Pukul satu pad dengan dinamika main asli (pelan / sedang / keras)
   → catat `[HIT]` peak sebagai rentang ADC efektif pad tersebut.
3. Dari blok `[CROSSTALK]` → set `threshold` tiap pad di atas nilai
   `leak`/`WORST` tertinggi (bukan di atas peak source).
4. Tentukan `sensitivity` per pad: `peak_keras * sensitivity/100`
   harus mencapai zona velocity 127 pada LUT (lihat §6 Velocity).

**Cara mengaktifkan:**
1. Set `#define DEBUG_ADC_CALIBRATION 1` di `config.h`
2. Build dan flash ESP32
3. Buka Serial Monitor 115200 baud (UART0/Serial0, bukan UART1)
4. Baseline terkumpul otomatis — tidak perlu mengetik command apa pun

---

## 8. Audio Engine

File: `audio.h/.cpp`

### Voice Pool

- 32 voices (`VOICE_COUNT = 32`)
- Voice stealing: ambil voice dengan volume paling pelan saat pool penuh
- SPSC hit ring antara TriggerTask → AudioTask (non-blocking, no lock)

### Pipeline

```
audioTrigger(padIndex, velocity) → push ke SPSC hit ring
  └── AudioTask (Core0):
        drain hit ring → fireHit() → startVoice()
        prefetch PCM dari PSRAM → voiceBuffer[]
        mix semua active voices (integer fixed-point)
        I2S DMA write
```

### WAV Requirements

- Format: 16-bit PCM
- Sample rate: 48000 Hz (wajib, tidak ada resampling)
- Channels: mono atau stereo
- Header: standar PCM WAV

### Hit Gate (Kit Switch Support)

`audioSetHitAccept(false)` membuang hit baru dari TriggerTask selama
kit switch berlangsung. `audioHitRingEmpty()` + `audioActiveVoiceMaxRemainingUs()`
dipakai sebagai deadline adaptif kit switch — voice yang sedang aktif
diberi waktu selesai secara natural sebelum PSRAM di-unload.

---

## 9. PSRAM and Sample System

File: `psram_loader.h/.cpp`

### Layout (G25: Per-Slot Block Ownership)

Setiap sample dialokasikan sebagai blok PSRAM sendiri (`ps_malloc`).
Kit load tidak membutuhkan satu blok kontigu sebesar seluruh kit —
hanya cukup free TOTAL yang memadai.

**Slot layout:**
```
Slot 0..11  (PSRAM_KIT_SLOT_COUNT=12)  : kit samples
Slot 12..15 (PSRAM_EXTRA_SLOT_COUNT=4) : runtime samples (PAD → SOUND remap)
```

Total: `PSRAM_MAX_SAMPLES = 16`  
Max size per sample: `PSRAM_MAX_SAMPLE_SIZE = 4 MB`  
Sentinel (pad unassigned): `PSRAM_SLOT_NONE = 0xFF`

### Runtime Remap (G21: PAD → SOUND)

`psramFindSlotByPath(absPath)` → cari slot yang sudah memuat file ini  
`psramLoadSingleSample(absPath)` → muat satu WAV ke slot runtime baru  
`psramSetPadSlot(padIndex, slot)` → update mapping pad→slot (atomic byte store)

Remap tidak menyentuh slot lain, tidak ada rebase pointer, tidak ada voice
yang perlu di-drain (kecuali bila file belum ada di PSRAM — lihat `performSampleAssign`
di uart_protocol.cpp).

### Pad → Slot Registry

`psramSlotForPad(padIndex)` → O(1), memory-only, dipanggil AudioTask tiap hit.
Tidak ada SD, malloc, String, atau blocking pada jalur ini.

---

## 10. Kit System

File: `kit_manager.h/.cpp`, `kit_loader.h/.cpp`, `persistence.h/.cpp`

### Kit Slots

5 slot kit (index 0..4):
- **Slot 0**: Factory kit, dilindungi (tidak bisa di-DELETE atau di-overwrite)
- **Slot 1..4**: User kits (bisa CREATE, RENAME, DELETE, SAVE)

### Identity: Name-Based (G2)

Folder kit mengikuti nama kit: `/kits/<nama>/`  
Kit diidentifikasi oleh field `kitId` di `kit.json` (bukan nama folder).  
`kitLoaderResolveKitDir(slot)` → scan `/kits/*/kit.json` untuk `kitId == slot`.

### Kit Operations

| Operasi | Fungsi |
|---|---|
| CREATE EMPTY | Kit baru dengan baseline safe, mapping sample kosong, `samples/` kosong |
| CREATE COPY | Snapshot kit aktif (config + mapping + file sample) ke slot baru |
| RENAME | Ubah nama JSON + pindah folder; rollback bila gagal |
| DELETE | Hapus pohon lengkap (samples/, kit.json, folder) + NVS |
| SAVE | Serialisasi config efektif RAM ke kit.json (atomic) |
| LOAD | Kit switch aman via `uartPerformKitSwitch()` |

### Sample Naming

Mapping pad → sample disimpan sebagai **nama file** (bukan index).
File sampel harus ada di `/kits/<nama>/samples/`.  
Nama file: maks 32 char, tanpa `/`, `\`, `..`.

### Persistence

NVS hanya menyimpan **global** (activeKit, volume). Per-kit config di `kit.json`.
Legacy NVS blob per-kit di-migrasi ke JSON saat boot pertama kali.

---

## 11. PAD System

File: `pad.h/.cpp`

### PadConfig Fields

| Field | ID | Tipe | Rentang |
|---|---|---|---|
| midiNote | 0 | uint8 | 0–127 |
| threshold | 1 | uint16 | 100–4095 |
| releaseThreshold | 2 | uint16 | 50–4095 |
| peakTime (µs) | 3 | uint16 | 200–10000 |
| releaseStableTime (µs) | 4 | uint32 | 1000–100000 |
| maskBaseMs | 5 | uint32 | 1–65535 |
| sensitivity | 6 | uint8 | 1–255 |
| curve | 7 | uint8 | 0–255 |
| padType | 8 | uint8 | 0–6 (PadType enum) |
| sampleId | 9 | uint8 | 0–11 |
| chokeGroup | 10 | uint8 | 0–255 |

Field bounds canonical di `config.h` (`PAD_FIELD_MIN[]`, `PAD_FIELD_MAX[]`).

### PadType Enum

```cpp
PAD_TYPE_GENERIC=0, PAD_TYPE_KICK=1, PAD_TYPE_SNARE=2,
PAD_TYPE_TOM=3, PAD_TYPE_CYMBAL=4, PAD_TYPE_HIHAT=5, PAD_TYPE_PERCUSSION=6
```

---

## 12. UART Protocol

File: `uart_protocol.h/.cpp`, `DrumCore_UI/uart_client.h/.cpp`

### Frame Format (8 byte, fixed)

```
[0] SOF  = 0xD3
[1] CMD  (bit7=1: response, bit7=0: request)
[2] ARG0
[3] ARG1
[4] ARG2
[5] ARG3
[6] SEQ
[7] CRC  = XOR(byte1..byte6)
```

### Physical Link

- UART1 ESP32-S3: TX=GPIO1, RX=GPIO3
- Wemos UART0: TX=GPIO1, RX=GPIO3
- Baud: 921600, 8N1
- CRC: XOR semua byte index 1–6

### Command IDs

| CMD | Value | Keterangan |
|---|---|---|
| PING | 0x01 | |
| REQUEST_STATUS | 0x02 | ARG1 di resp = activeKit |
| KIT_NEXT | 0x10 | |
| KIT_PREV | 0x11 | |
| KIT_LOAD | 0x12 | ARG0 = slot |
| KIT_SAVE | 0x13 | |
| KIT_INFO | 0x14 | Chunked response (nama kit) |
| KIT_CREATE_EMPTY | 0x15 | |
| KIT_CREATE_COPY | 0x16 | |
| KIT_RENAME | 0x17 | Chunked request (nama) |
| KIT_DELETE | 0x18 | |
| KIT_CREATE_EMPTY_NAMED | 0x19 | Chunked request + nama |
| KIT_CREATE_COPY_NAMED | 0x1A | Chunked request + nama |
| PAD_SET | 0x20 | ARG0=pad, ARG1=field, ARG2/3=value |
| PAD_GET | 0x21 | ARG0=pad, ARG1=field |
| PAD_SOUND | 0x22 | Daftar suara kit aktif (read-only) |
| PAD_SOUND_SET | 0x23 | Assign WAV ke pad (chunked) |
| GLOBAL_SAVE | 0x30 | Simpan activeKit + volume |
| CALIBRATE | 0x40 | |
| FULL_SYNC | 0x50 | |

### Status Codes (ARG0 response)

| Status | Value |
|---|---|
| OK | 0x00 |
| INVALID_COMMAND | 0x01 |
| INVALID_PAD | 0x02 |
| INVALID_VALUE | 0x03 |
| INVALID_KIT | 0x04 |
| BUSY | 0x05 |
| CRC_ERROR | 0x06 |
| NAME_EXISTS | 0x07 |
| NO_SPACE | 0x08 |

### Chunked Transfer (KIT_INFO, RENAME, CREATE_NAMED, PAD_SOUND)

Nama > 3 char dikirim dalam beberapa frame:
- Frame header: ARG0=status/slot, ARG1=panjang nama
- Frame data: ARG0=(chunkIdx | 0x80), ARG1..ARG3 = hingga 3 char

**PENTING:** Frame format, SOF, CRC, SEQ, baud, dan daftar command
adalah **protocol frozen** — tidak boleh diubah.

---

## 13. MIDI

File: `midi.h/.cpp`

- USB MIDI (native USB ESP32-S3, GPIO19/20)
- Note stack: 8 slot, ring buffer
- Note-off: 5 ms setelah note-on
- MIDI notes per pad: `PAD_MIDI_NOTES[]` di `config.h` (GM mapping default)

---

## 14. SD Card Structure

```
SD Card root:
└── kits/
    ├── Acoustic/
    │   ├── kit.json
    │   └── samples/
    │       ├── KICK.wav
    │       ├── SNARE.wav
    │       └── ...
    ├── ROCK LIVE/
    │   ├── kit.json
    │   └── samples/
    ├── Jazz/
    ├── Latin/
    └── Electronic/
```

- 5 kit slots (index 0..4), folder diidentifikasi via `kitId` di `kit.json`
- Semua WAV di dalam `samples/` folder kit masing-masing
- Tidak ada dependency ke file WAV di root SD card

Staging data di repo: `SDCard/kits/` (mirror struktur SD aktual).

---

## 15. Configuration / JSON

### kit.json Structure

```json
{
  "kitId": 0,
  "name": "Acoustic",
  "pads": [
    {
      "pad": 0,
      "midiNote": 36,
      "threshold": 300,
      "releaseThreshold": 250,
      "peakTime": 1200,
      "releaseStableTime": 3000,
      "maskBaseMs": 30,
      "sensitivity": 100,
      "curve": 0,
      "padType": 1,
      "sampleId": 0,
      "chokeGroup": 0,
      "sample": "KICK.wav"
    }
  ]
}
```

Field yang tidak ada di JSON → fallback ke default `config.h`. Parser tidak crash
bila field hilang / corrupt.

### config.h — Key Constants

```cpp
FW_VERSION       = "0.4.0"
PAD_COUNT        = 12
VOICE_COUNT      = 32
PSRAM_KIT_SLOT_COUNT   = 12
PSRAM_EXTRA_SLOT_COUNT = 4
PSRAM_MAX_SAMPLES      = 16
KIT_COUNT        = 5
KIT_FACTORY_INDEX = 0
KIT_NAME_MAX     = 28
SAMPLE_NAME_MAX  = 32
UART1_BAUD       = 921600
SERIAL_BAUD      = 115200
```

---

## 16. Firmware Architecture — DrumCore_UI (Wemos D1 Mini)

```
DrumCore_UI/
├── DrumCore_UI.ino    Setup + loop
├── config.h           Pin, baud, UI constants
├── display.h/.cpp     LCD abstraction
├── encoder.h/.cpp     Rotary encoder + button
├── menu.h/.cpp        Menu state machine
├── screens.h/.cpp     Screen renderers
├── ui.h/.cpp          Top-level UI state, UART request/response
├── uart_client.h/.cpp UART frame builder + parser + response FIFO
└── platformio.ini     Build config (d1_mini, ESP8266)
```

### Fungsi UI

- Navigasi kit (NEXT/PREV/LOAD/SAVE)
- Kit management (CREATE EMPTY, CREATE COPY, RENAME, DELETE)
- Pad parameter editing (threshold, mask, midi note, dll)
- PAD → SOUND: pilih file WAV dari folder samples kit aktif
- Status display (kit aktif, pad info, UART health)

### Catatan UART ESP8266

- UART0 (GPIO1 TX, GPIO3 RX): satu-satunya UART dengan RX
- Dipakai penuh untuk link ke ESP32-S3
- UART1 (TX-only, tidak ada RX): tidak digunakan
- Health UART ditampilkan di LCD (bukan Serial Monitor)

---

## 17. Build and Flash

### Prerequisites

- PlatformIO Core 6.x atau PlatformIO IDE
- Python 3.x (untuk host tests)

### ESP32-S3 (DrumCore)

```bash
cd DrumCore
pio run -e drumcore              # build
pio run -e drumcore -t upload    # flash
pio device monitor --baud 115200 # serial monitor (UART0 debug)
```

`platformio.ini`:
- Board: `esp32-s3-devkitc-1`
- Flash mode: QIO
- PSRAM: OPI
- Upload speed: 921600

### Wemos D1 Mini (DrumCore_UI)

```bash
cd DrumCore_UI
pio run                          # build
pio run -t upload                # flash
```

`platformio.ini`:
- Board: `d1_mini` (ESP8266)
- Dependencies: LiquidCrystal_I2C@1.1.4, RotaryEncoder@1.6.0

### Host Tests

```bash
cd PROJEK DRUMPAD JOSJISS
python host_tests/kit_runtime_test.py
```

Expected: `1588 passed, 0 failed`

---

## 18. Debug / Calibration Tools

Semua debug tool dikontrol via compile-time flags di `config.h`.
Default semua `false` untuk production build.

| Flag | Fungsi |
|---|---|
| `DEBUG_ADC` | Print 12 ADC values tiap 100ms ke Serial0 |
| `DEBUG_TRIGGER` | Print attack/hit/retrigger events ke Serial0 |
| `DEBUG_UART` | Log setiap RX frame UART control plane |
| `DEBUG_ADC_CALIBRATION` | ADC hardware calibration tool (lihat §7) |
| `DEBUG_ROLLING_TEST` | Trigger event ring + mask diagnostic |
| `DEBUG_PLAYABILITY` | Scan timing + audio push/drop/consume stats |
| `PROFILE_TRIGGER_LATENCY` | Trigger latency timing profile |
| `PROFILE_ADC_SCAN` | ADC scan interval profile |
| `PROFILE_ADC_STALENESS` | ADC cache age profile |
| `PROFILE_2VOICE_DEBUG` | 2-voice cutout audit ring |
| `PROFILE_MIX_AUDIT` | Mix pipeline audit ring |

**Cara pakai:** Set flag yang diinginkan ke `true`, rebuild, flash.
Flag dikembalikan ke `false` sebelum production flash.

---

## 19. Current Project Status

- **Feature Freeze** — tidak ada feature baru
- Semua core features PASS (lihat Development History)
- **ADC Hardware Calibration**: fase aktif saat ini
  - Tool sudah ada (`adc_calibration.h/.cpp`)
  - Wiring fisik 12 pad belum selesai dikonfirmasi
  - Mapping PAD → GPIO fisik aktual belum diverifikasi
  - Threshold dan velocity tuning menunggu data hardware

**Build status (terakhir diverifikasi):**
- ESP32 DrumCore: PASS (RAM 33%, Flash 16%)
- Wemos DrumCore_UI: PASS (RAM 39%, Flash 28%)
- Host tests: 1588 PASS, 0 FAIL

---

## 20. Deferred Features

| Fitur | Status | Keterangan |
|---|---|---|
| External Trigger (GPIO13) | HOLD | Pin assigned, belum ada runtime consumer |
| Hi-Hat Switch (GPIO14) | HOLD | Pin assigned, belum ada runtime consumer |
| Multi-velocity layers | FUTURE | 1 sample/pad saat ini |
| Round-robin sampling | FUTURE | — |
| Choke group behavior | FUTURE | Field ada, belum ada logic |
| USB MIDI (GPIO19/20) | PARTIAL | Init ada, belum diuji penuh |
| Amp envelope (attack/release) | FUTURE | — |
| USB audio output | FUTURE | — |
| Bluetooth MIDI | FUTURE | — |

---

## 21. Important Constraints

Hal-hal berikut **tidak boleh diubah** tanpa analisis mendalam:

**Realtime path:**
- Trigger state machine (IDLE/WAIT_PEAK/MASK/WAIT_RELEASE)
- Velocity LUT dan gamma curve
- ADC batch scan size (12/pass) dan cadence TriggerTask
- `audioTrigger()` dan SPSC hit ring
- Task core affinity dan priority
- I2S DMA buffer parameters
- PSRAM pointer arithmetic dan slot registry

**Protocol:**
- UART frame format (8 byte, SOF=0xD3, CRC=XOR1-6)
- Command IDs dan status codes
- UART1 baud rate (921600)
- Chunked transfer encoding

**Storage:**
- kit.json structure dan field names
- NVS keys (global: activeKit, volume)
- SD folder structure (`/kits/<nama>/samples/`)
- Kit identity via `kitId` field

**GPIO:**
- Semua pin assignment (lihat §4)
- Pin tidak boleh di-reassign tanpa hardware rework

---

## 22. Development History / Key Decisions

Ringkasan keputusan arsitektur penting selama development:

**G1–G6: Foundation**
- Migrasi dari superloop ke FreeRTOS dual-core
- AudioTask Core0/prio24, TriggerTask Core1/prio23
- ADC round-robin cache di hardware.cpp
- Kit system berbasis JSON di SD card
- UART control plane dengan 8-byte fixed frame
- Velocity LUT zero-float

**G8: Field Bounds & PAD_SET/GET**
- `PAD_FIELD_MIN[]` / `PAD_FIELD_MAX[]` sebagai single source of truth
- Field ID canonical di `config.h`

**G21: PAD → SOUND (KIT RUNTIME)**
- Remap sample pad tanpa reload kit penuh
- Runtime slots (slot 12..15) dengan PSRAM blok sendiri
- Mapping by filename (bukan index)

**G22: UI Polish**
- Menu dan screen cleanup Wemos

**G23: PSRAM Rollback**
- Kit switch dengan rollback bila PSRAM load gagal

**G24: UI Cleanup + CREATE Named**
- CREATE EMPTY / CREATE COPY dengan nama dari user
- STATUS_NO_SPACE (0x08) untuk slot penuh

**G25: PSRAM Fragmentation Fix**
- Per-slot block ownership menggantikan satu blok kit
- Kit load tidak lagi butuh blok kontigu sebesar seluruh kit
- Audio contract tidak berubah

**G-ADC: ADC Calibration Tool**
- `adc_calibration.h/.cpp` — development/debug tool
- Dipanggil dari `loop()`, zero overhead saat flag false
- Data: baseline, single hit, crosstalk, peak range
