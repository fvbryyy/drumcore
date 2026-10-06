# DrumCore

DIY electronic drum module — ESP32-S3 engine + Wemos D1 Mini UI.

**Version:** 0.4.0 | **Status:** Feature Freeze / Hardware Calibration Phase

---

## Quick Start

```bash
# Build & flash ESP32-S3
cd DrumCore && pio run -e drumcore -t upload

# Build & flash Wemos D1 Mini
cd DrumCore_UI && pio run -t upload

# Run host tests
python host_tests/kit_runtime_test.py
```

Expected test result: `1588 passed, 0 failed`

---

## Repository Structure

```
DrumCore/       ESP32-S3 firmware — realtime drum engine
DrumCore_UI/    Wemos D1 Mini firmware — LCD + encoder UI
host_tests/     Python regression test suite
SDCard/         SD card staging data (kits + samples)
```

---

## Documentation

Full documentation: **[DOCUMENTATION.md](DOCUMENTATION.md)**

Covers: architecture, hardware wiring, pin mapping, UART protocol,
kit system, ADC calibration tool, build instructions, debug flags,
deferred features, and important constraints.

---

## Hardware

- **ESP32-S3 N16R8** — 12 piezo pads via ADC1, PCM5102A I2S audio,
  SD card SPI, UART1 to Wemos
- **Wemos D1 Mini** — LCD 16x2, rotary encoder, UART to ESP32-S3

---

## Current Phase

ADC hardware calibration for 12-pad prototype. Physical wiring not
yet confirmed — pad-to-GPIO mapping requires hardware verification.
See `DOCUMENTATION.md §7` for calibration tool usage.
