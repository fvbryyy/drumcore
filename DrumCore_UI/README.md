# DrumCore_UI

Wemos D1 Mini (ESP8266) UI controller for DrumCore ESP32-S3.

Full documentation: [DOCUMENTATION.md](../DOCUMENTATION.md)

---

## Hardware Wiring

| Component | Wemos Pin | Notes |
|---|---|---|
| LCD 16x2 I2C | D1 (SCL), D2 (SDA) | Address 0x27 |
| Encoder S1 | D5 | Navigation |
| Encoder S2 | D6 | Navigation |
| Encoder KEY | D7 | Select / confirm |
| UART TX | GPIO1 | To ESP32-S3 GPIO3 (RX) |
| UART RX | GPIO3 | To ESP32-S3 GPIO1 (TX) |
| GND | GND | Common ground |

UART: 921600 baud, 8N1. Uses UART0 (only hardware UART with RX on ESP8266).

---

## Menu Structure

Top menu: `` `KIT / PAD` ``

No slot screen — kit identity is name-based (no slot numbers shown to user).

### KIT submenu (6 entries)
- INFO, LOAD, SAVE, CREATE (EMPTY / COPY), RENAME, DELETE

### PAD submenu (4 entries)
- INFO, THRESHOLD, MASK, SOUND

---

## PAD → SOUND Screen

Displays available WAV files from the active kit's `samples/` folder.

Status indicators:
- `READING` — scanning sample list from ESP32 (replaces `` `...` `` placeholder)
- `NO SAMPLES` — kit has no WAV files in samples/
- `SOUND OK` — assignment confirmed
- `TIMEOUT` — ESP32 did not respond in time

---

## Build

```bash
cd DrumCore_UI
pio run           # build
pio run -t upload # flash
```
