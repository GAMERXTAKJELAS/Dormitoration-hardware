# DORMITORATION — Smart Door Unit Firmware (ESP32)

ESP32 firmware for the **Dormitory Monitor Operation System (DORMITORATION)** smart door lock unit at Institute Hostel. This edge device scans student QR codes, verifies room allocation against the Cloudflare Workers + D1 backend over HTTPS, displays real-time status on an OLED screen, and controls a fail-safe 12V electromagnetic lock.

- **Live Web Application:** [dormitoration.syamsulock0457.workers.dev](https://dormitoration.syamsulock0457.workers.dev/)
- **Web Platform & Backend Repository:** [GAMERXTAKJELAS/dormitoration](https://github.com/GAMERXTAKJELAS/dormitoration)

---

## Key Features

- **QR Code Access Verification:** Reads student UUID QR codes via the GM65 scanner and sends JSON payloads (`code`, `block`, `room`) to `/api/access/verify` with `X-Device-Key` header authentication.
- **GM65 Induction Mode Auto-Config:** Automatically configures the GM65 scanner into Induction Mode (`zone 0x0000`, `bits[1:0] = 11`) with CRC-CCITT verification on boot, paired with a 2-second scan cooldown (`SCAN_COOLDOWN_MS = 2000`) to prevent duplicate API floods.
- **Fail-Safe Lock Control:** Drives a 5V relay wired to a 12V electromagnetic lock via the Normally Closed (`NC`) terminal, unlocking for 5 seconds (`UNLOCK_MS = 5000`) upon `{"access": true}`.
- **Live OLED Status Feedback:** Displays room identity (`[BLOCK - ROOM]`) and states (`READY`, `WAITING`, `UNLOCKED`, `DENIED`, `NO WIFI`, `ERROR`) on a 128×64 SSD1306 display.
- **Automated Heartbeat & Reconnect:** Pings `/api/access/heartbeat` every 5 minutes (`300000 ms`) to report online status to the admin dashboard and automatically reconnects if Wi-Fi drops.

---

## Hardware Components & Wiring

### Bill of Materials
- **Microcontroller:** ESP32 DevKit
- **QR Scanner:** GM65 1D/2D Barcode & QR Scanner Module (UART, 115200 baud)
- **Display:** 0.96" I2C OLED Display (SSD1306, 128×64)
- **Relay & Lock:** 5V Single-Channel Relay Module + 12V Electromagnetic Lock
- **Power Supply:** 12V 5A DC Adapter + LM2596 Buck Converter (12V $\rightarrow$ 5V)

### Pinout Table

| ESP32 Pin | Connected Component | Signal / Purpose |
| :--- | :--- | :--- |
| `GPIO 16` (`RX2`) | GM65 `TX` | UART2 Receive (QR code data) |
| `GPIO 17` (`TX2`) | GM65 `RX` | UART2 Transmit (Induction Mode commands) |
| `GPIO 21` (`SDA`) | OLED `SDA` | I2C Data (`0x3C` / `0x3D`) |
| `GPIO 22` (`SCL`) | OLED `SCL` | I2C Clock |
| `GPIO 26` | 5V Relay `IN` | Active `HIGH` trigger to release maglock |
| `VIN` / `GND` | LM2596 `5V` / `GND` | Regulated 5V logic power & common ground |

> **Fail-Safe Relay Wiring:** Connect `+12V` to Relay `COM` and Maglock `(+)` to Relay `NC`. When idle, `NC` remains closed and the door stays locked; energizing `GPIO 26` opens the circuit for 5 seconds to unlock.

---

## Project Structure

```text
├── platformio.ini          # PlatformIO environment & library dependencies
├── src/
│   ├── main.cpp            # Core ESP32 firmware (Wi-Fi, GM65, OLED, HTTPS, Relay)
│   ├── secrets-example.h   # Configuration template (tracked by Git)
│   └── secrets.h           # Local credentials & room config (ignored by Git)
└── .gitignore              # Excludes build artifacts and src/secrets.h
