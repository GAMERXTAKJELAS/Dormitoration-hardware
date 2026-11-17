// =====================================================================
//  Dormitoration Smart Lock - ESP32 firmware (src/main.cpp)
//  1) GM65 QR scan -> JSON POST -> website answers access true/false -> relay
//  2) Heartbeat    -> tells the website this room's hardware is online
//  3) OLED SSD1306 -> Real-time status UI on GPIO 21 (SDA) & GPIO 22 (SCL)
//
//  platformio.ini needs:
//    [env:esp32dev]
//    platform = espressif32
//    board = esp32dev
//    framework = arduino
//    monitor_speed = 115200
//    lib_deps = 
//      bblanchon/ArduinoJson@^7
//      adafruit/Adafruit GFX Library@^1.11.11
//      adafruit/Adafruit SSD1306@^2.5.13
//
//  Wiring:
//    GM65 TX -> GPIO16 (RX2)    GM65 RX -> GPIO17 (TX2)
//    GPIO26  -> Relay IN        Relay COM -> +12V, Relay NC -> Maglock +
//    GPIO21  -> OLED SDA        GPIO22    -> OLED SCL
//  Fail-safe: idle relay = NC closed = maglock powered = LOCKED.
//  Energizing the relay opens NC = lock loses power = UNLOCKED.
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"
// secrets.h must define: WIFI_SSID, WIFI_PASSWORD, API_URL, HEARTBEAT_URL,
//                        DEVICE_ID, DEVICE_API_KEY, BLOCK_NAME, ROOM_NUMBER, ROOT_CA

// ---------- Pins ----------
#define RXD2        16
#define TXD2        17
#define RELAY_PIN   26
#define OLED_SDA    21
#define OLED_SCL    22

// ---------- OLED Specs ----------
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = false;

// ---------- Config ----------
#define GM65_BAUD           115200
#define RELAY_ACTIVE_LEVEL  HIGH    // HIGH if relay jumper = H, LOW if jumper = L
#define UNLOCK_MS           5000    // how long the door stays unlocked
#define SCAN_COOLDOWN_MS    2000    // ignore repeat scans of the same code
#define HTTP_TIMEOUT_MS     8000    // QR verify request
#define HB_TIMEOUT_MS       4000    // heartbeat request (kept short so scans aren't held up)
#define HEARTBEAT_MS        300000  // how often to report "I'm online" (5 min)
// NOTE: the website's "online"/"offline" threshold (dashboard_backend.js's
// ONLINE_WINDOW_SECONDS and cloudflare_worker.js's cron ROOM_OFFLINE_AFTER_SECONDS)
// must stay well above this value, or a room will flicker red between every
// heartbeat. If you change this again, update those two to match (see their
// own comments for the formula).
#define HEARTBEAT_RETRY_MS  5000    // retry sooner after a failed heartbeat
#define WIFI_RETRY_MS       10000
#define MAX_CODE_LEN        128
#define USE_INSECURE_TLS    true    // TESTING ONLY. Set false and fill ROOT_CA before real use.

// ---------- State ----------
bool     unlocked      = false;
uint32_t unlockedAtMs  = 0;
uint32_t lastScanMs    = 0;
uint32_t lastWifiTryMs = 0;
uint32_t nextHbMs      = 0;      // 0 = send as soon as WiFi is up
String   lastCode;

// =====================================================================
//  OLED Display UI Helpers
// =====================================================================
void updateDisplay(const char* title, const char* statusMsg, bool centered = true) {
  if (!oledAvailable) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Header Bar
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("[%s - %s]", BLOCK_NAME, ROOM_NUMBER);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Subtitle / Action Label
  display.setCursor(0, 14);
  display.print(title);

  // Main Status Message
  display.setTextSize(2);
  if (centered) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(statusMsg, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 36);
  } else {
    display.setCursor(0, 36);
  }
  display.print(statusMsg);
  
  display.display();
}

void showIdleScreen() {
  if (WiFi.status() == WL_CONNECTED) {
    updateDisplay("SCAN QR CODE", "READY");
  } else {
    updateDisplay("CONNECTING...", "NO WIFI");
  }
}

// =====================================================================
//  Relay
// =====================================================================
void setRelay(bool unlock) {
  digitalWrite(RELAY_PIN, unlock ? RELAY_ACTIVE_LEVEL : !RELAY_ACTIVE_LEVEL);
  unlocked = unlock;
  if (unlock) {
    unlockedAtMs = millis();
    updateDisplay("ACCESS GRANTED", "UNLOCKED");
  } else {
    showIdleScreen();
  }
  Serial.println(unlock ? "[LOCK] UNLOCKED" : "[LOCK] LOCKED");
}

// =====================================================================
//  WiFi
// =====================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] connecting");
  updateDisplay("NETWORK", "WIFI...");

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    showIdleScreen();
  } else {
    Serial.println("[WiFi] failed, will retry");
    updateDisplay("NETWORK ERROR", "NO WIFI");
  }
}

// =====================================================================
//  GM65 scan reader
// =====================================================================
String readScan() {
  if (!Serial2.available()) return "";
  String s;
  uint32_t last = millis();
  while (millis() - last < 40) {          // wait for the packet to finish
    while (Serial2.available()) {
      char c = Serial2.read();
      if (c != '\r' && c != '\n') s += c;
      last = millis();
    }
    delay(1);
  }
  s.trim();
  if (s.length() > MAX_CODE_LEN) return "";
  return s;
}

// =====================================================================
//  One HTTPS JSON POST used by both features.
//  Returns the HTTP status (or -1 if it couldn't connect); reply body in 'out'.
// =====================================================================
int postJson(const char *url, const String &body, uint16_t timeoutMs, String &out) {
  WiFiClientSecure client;
#if USE_INSECURE_TLS
  client.setInsecure();
#else
  client.setCACert(ROOT_CA);
#endif

  HTTPClient http;
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);

  int status = http.POST(body);
  if (status > 0) out = http.getString();
  http.end();
  return status;
}

// =====================================================================
//  Ask the website about a scanned code.
//  Returns true if the request itself succeeded; 'granted' = the answer.
// =====================================================================
bool verifyCode(const String &code, bool &granted) {
  granted = false;

  JsonDocument req;
  req["code"]  = code;
  req["block"] = BLOCK_NAME;
  req["room"]  = ROOM_NUMBER;
  String body;
  serializeJson(req, body);

  String resp;
  int status = postJson(API_URL, body, HTTP_TIMEOUT_MS, resp);
  if (status != 200) {
    Serial.printf("[HTTP] status %d\n", status);
    return false;
  }

  JsonDocument res;
  DeserializationError err = deserializeJson(res, resp);
  if (err) {
    Serial.printf("[JSON] parse error: %s\n", err.c_str());
    return false;
  }

  granted = res["access"] | false;
  return true;
}

// =====================================================================
//  Heartbeat: "this block/room is online". The website auto-registers
//  the room on the first call and refreshes last_seen_at on every call.
// =====================================================================
bool sendHeartbeat() {
  JsonDocument req;
  req["block"]  = BLOCK_NAME;
  req["room"]   = ROOM_NUMBER;
  req["gender"] = ROOM_GENDER;
  String body;
  serializeJson(req, body);

  String resp;
  int status = postJson(HEARTBEAT_URL, body, HB_TIMEOUT_MS, resp);
  if (status != 200) {
    Serial.printf("[HB] failed, status %d\n", status);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, resp) || !(res["ok"] | false)) {
    Serial.println("[HB] bad reply");
    return false;
  }

  Serial.printf("[HB] online: block %s room %s (room_id %d)%s\n",
                BLOCK_NAME, ROOM_NUMBER, (int)(res["room_id"] | 0),
                (res["registered"] | false) ? " - auto-registered" : "");
  return true;
}

// =====================================================================
void handleScan(const String &code) {
  Serial.printf("[SCAN] %s\n", code.c_str());
  updateDisplay("VERIFYING...", "WAITING");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SCAN] no WiFi, staying locked");
    updateDisplay("VERIFY FAILED", "NO WIFI");
    delay(2000);
    showIdleScreen();
    return;
  }

  bool granted = false;
  if (!verifyCode(code, granted)) {
    Serial.println("[ACCESS] request failed, staying locked");   // fail-safe
    updateDisplay("VERIFY FAILED", "ERROR");
    delay(2000);
    showIdleScreen();
    return;
  }

  if (granted) {
    Serial.println("[ACCESS] GRANTED");
    setRelay(true);
  } else {
    Serial.println("[ACCESS] DENIED");
    updateDisplay("ACCESS DENIED", "DENIED");
    delay(2000);
    showIdleScreen();
  }
}

// =====================================================================
//  GM65 Read Mode fix (manual section 9: serial commands, zone bit 0x0000)
//  Problem: GM65 ships in Continuous Mode — it decodes non-stop forever,
//  with no gate on whether a code is even present. That constant duty
//  cycle is the likely cause of it misbehaving after long uptime, and it
//  also re-sends the same code roughly every second if left in view,
//  which is what was flooding the website with repeat verify calls.
//  Fix: switch it into Induction Mode (zone 0x0000, bits[1:0] = 11). The
//  module then watches for a scene change via its own sensor and only
//  wakes to decode when something is actually presented — the same
//  outcome a proximity sensor would give, with no extra hardware.
//  This runs once at boot: read the byte, flip only bits 1-0 (leaving
//  LED/beep/lighting bits untouched), write it back, save to flash so it
//  survives power loss. Safe to run every boot — it's idempotent.
// =====================================================================
#define GM65_CMD_TIMEOUT_MS 300

// CRC-CCITT (poly 0x1021, init 0), exactly as given in the GM65 manual's
// own reference code — used so these commands work even if a firmware
// revision enforces CRC checking (the manual's "fill 0xAB 0xCD" shortcut
// is only for when checking is OFF, which isn't guaranteed on every unit).
uint16_t gm65Crc(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t n = 0; n < len; n++) {
    for (uint8_t i = 0x80; i != 0; i >>= 1) {
      crc <<= 1;
      if (crc & 0x10000) crc ^= 0x11021;
      if (data[n] & i) crc ^= 0x1021;
      crc &= 0xFFFF;
    }
  }
  return crc;
}

// Sends one command, appends CRC, waits briefly for the module's reply.
// Returns the number of reply bytes read into respBuf (0 = no/short reply).
int gm65SendCommand(const uint8_t *body, size_t bodyLen, uint8_t *respBuf, size_t respBufSize) {
  while (Serial2.available()) Serial2.read();   // clear any stray bytes first

  uint16_t crc = gm65Crc(body, bodyLen);
  Serial2.write(body, bodyLen);
  Serial2.write((uint8_t)(crc >> 8));
  Serial2.write((uint8_t)(crc & 0xFF));

  uint32_t start = millis();
  size_t n = 0;
  while (millis() - start < GM65_CMD_TIMEOUT_MS && n < respBufSize) {
    if (Serial2.available()) {
      respBuf[n++] = Serial2.read();
      start = millis(); // reset the idle timer while bytes are still arriving
    }
  }
  return n;
}

void configureGM65InductionMode() {
  uint8_t resp[16];
  uint8_t readCmd[] = {0x7E, 0x00, 0x07, 0x01, 0x00, 0x00, 0x01};

  // Retry the read a few times — the module may still be finishing its own
  // power-on sequence, or briefly busy mid-decode-cycle (it's still in
  // Continuous Mode at this exact point, that's the whole thing we're fixing).
  int n = 0;
  for (uint8_t attempt = 1; attempt <= 5; attempt++) {
    n = gm65SendCommand(readCmd, sizeof(readCmd), resp, sizeof(resp));
    // Reply shape: {0x02 0x00} {Type 0x00} {Len 0x01} {Data} {CRC hi} {CRC lo}
    if (n >= 5 && resp[2] == 0x00) break;
    Serial.printf("[GM65] read attempt %d/5 got %d bytes, retrying...\n", attempt, n);
    delay(300);
  }
  if (n < 5 || resp[2] != 0x00) {
    Serial.println("[GM65] couldn't read zone bit 0x0000 after 5 tries — skipping mode change");
    return;
  }
  uint8_t current = resp[4];
  uint8_t desired = (current & 0xFC) | 0x03;   // keep bits 7-2, force bits1-0 = 11 (Induction)

  if (current == desired) {
    Serial.println("[GM65] already in Induction Mode");
    return;
  }

  // 2) Write the modified byte back.
  uint8_t writeCmd[] = {0x7E, 0x00, 0x08, 0x01, 0x00, 0x00, desired};
  n = gm65SendCommand(writeCmd, sizeof(writeCmd), resp, sizeof(resp));
  if (n < 3 || resp[2] != 0x00) {
    Serial.println("[GM65] write failed — mode unchanged");
    return;
  }

  // 3) Save to internal flash so it survives a power cycle (fixed command
  //    per the manual: type 0x09, address 0x0000, data 0x00).
  uint8_t saveCmd[] = {0x7E, 0x00, 0x09, 0x01, 0x00, 0x00, 0x00};
  n = gm65SendCommand(saveCmd, sizeof(saveCmd), resp, sizeof(resp));
  if (n < 3 || resp[2] != 0x00) {
    Serial.println("[GM65] set to Induction Mode, but saving to flash failed (will reset to Continuous on power loss)");
    return;
  }

  Serial.println("[GM65] switched to Induction Mode and saved to flash");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Dormitoration Smart Lock ===");
  Serial.printf("Block %s, room %s\n", BLOCK_NAME, ROOM_NUMBER);

  // Initialize OLED Display on GPIO 21 (SDA) & GPIO 22 (SCL)
  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C) || display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledAvailable = true;
    display.clearDisplay();
    updateDisplay("SYSTEM BOOT", "STARTING");
    Serial.println("[OLED] Display initialized successfully");
  } else {
    Serial.println("[OLED] Display init failed — continuing headless mode");
  }

  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);                       // start locked

  Serial2.begin(GM65_BAUD, SERIAL_8N1, RXD2, TXD2);
  delay(2000);                           // give the module time to fully finish its own boot
  configureGM65InductionMode();

  connectWiFi();
  Serial.println("[READY] scan a QR code...");
}

void loop() {
  // Re-lock after the unlock window
  if (unlocked && millis() - unlockedAtMs >= UNLOCK_MS) setRelay(false);

  // Keep WiFi alive without blocking
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTryMs > WIFI_RETRY_MS) {
    lastWifiTryMs = millis();
    Serial.println("[WiFi] reconnecting");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  // Scans come first
  String code = readScan();
  if (code.length() > 0) {
    bool repeat = (code == lastCode) && (millis() - lastScanMs < SCAN_COOLDOWN_MS);
    if (!repeat) {
      lastCode   = code;
      lastScanMs = millis();
      handleScan(code);
    }
  }

  // Heartbeat: not while the door is open, so re-locking is never delayed
  if (WiFi.status() == WL_CONNECTED && !unlocked && (int32_t)(millis() - nextHbMs) >= 0) {
    bool ok = sendHeartbeat();
    nextHbMs = millis() + (ok ? HEARTBEAT_MS : HEARTBEAT_RETRY_MS);
  }

  delay(10);
}