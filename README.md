# BLE Scanner — ESP32-C3 + 0.42" OLED (PlatformIO)

Scans for nearby BLE devices and shows names / MACs with a
signal-strength indicator on a 72 × 40 px OLED.

---

## Project layout

```
ble_scanner_esp32c3/
├── platformio.ini      ← board, framework, libraries
└── src/
    └── main.cpp        ← all application code
```

---

## Hardware

| Part | Notes |
|------|-------|
| ESP32-C3 board (SuperMini / XIAO / DevKit) | Any variant works |
| 0.42" SSD1306 OLED, 72 × 40 px, I²C | 3.3 V |

### Wiring

```
ESP32-C3        OLED
────────────────────
3V3      →      VCC
GND      →      GND
GPIO 5   →      SDA
GPIO 6   →      SCL
```

> Some ESP32-C3 SuperMini boards already have the OLED wired
> internally. Check your board's schematic and update
> `OLED_SDA` / `OLED_SCL` in `src/main.cpp` if needed.

---

## Quick start

### 1 — Install PlatformIO

- **VS Code extension** (recommended): install the
  *PlatformIO IDE* extension from the VS Code marketplace.
- **CLI**: `pip install platformio`

### 2 — Open the project

```bash
cd ble_scanner_esp32c3
```

Or open the folder in VS Code → PlatformIO will detect
`platformio.ini` automatically.

### 3 — Build & flash

```bash
# CLI
pio run --target upload

# VS Code: click the → Upload button in the PlatformIO toolbar
```

PlatformIO downloads the ESP32 platform and the U8g2 library
automatically on the first build.

### 4 — Monitor serial output

```bash
pio device monitor
```

Every scan prints the device list to the serial console at
115 200 baud.

---

## Screen layout (72 × 40 px)

```
┌──────────────────────┐
│ BLE  5 found         │  ← inverted header
├──────────────────────┤
│ ||| My Phone         │  ← strong  (> −60 dBm)
│ ||  Unknown DD:EE:FF │  ← medium  (−60 to −75 dBm)
│ |   Sensor-42        │  ← weak    (< −75 dBm)    │█│
└──────────────────────┘                         scroll bar
```

- **Name** shown if advertised; otherwise the last 3 MAC bytes.
- **BOOT button** (GPIO 9) manually advances the scroll.
- List auto-scrolls every 1.5 s and re-scans after 12 s.

---

## Configuration (top of `src/main.cpp`)

| Constant | Default | Effect |
|----------|---------|--------|
| `OLED_SDA` | 5 | I²C data pin |
| `OLED_SCL` | 6 | I²C clock pin |
| `OLED_ADDR` | 0x3C | I²C address (try 0x3D if blank) |
| `BTN_PIN` | 9 | Manual-scroll button |
| `SCAN_DURATION_S` | 4 | Seconds per BLE scan |
| `DISPLAY_HOLD_MS` | 12000 | ms to show results before re-scan |
| `AUTO_SCROLL_MS` | 1500 | ms between auto-scroll steps |
| `MAX_DEVICES` | 24 | Maximum tracked devices |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Blank OLED | Check wiring; try `OLED_ADDR 0x3D` |
| `BLEDevice.h` not found | Ensure `espressif32` platform is installed |
| Garbled display | Confirm module is 72 × 40 SSD1306 |
| Upload fails | Set *USB CDC On Boot* = Enabled in `platformio.ini` build_flags (already set) |
