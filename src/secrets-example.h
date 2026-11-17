#pragma once
// =====================================================================
//  src/secrets.example.h - TEMPLATE CONFIGURATION
//  1. Copy this file to src/secrets.h
//  2. Replace the placeholder values with your real credentials
//  3. Ensure src/secrets.h is listed in .gitignore before pushing
// =====================================================================

// ---- Wi-Fi Credentials (Secret) ----
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ---- Cloudflare Worker Endpoints ----
#define API_URL         "https://your-worker.your-subdomain.workers.dev/api/access/verify"
#define HEARTBEAT_URL   "https://your-worker.your-subdomain.workers.dev/api/access/heartbeat"

// ---- Door Unit Identity ----
#define BLOCK_NAME      "A"    // Dormitory block identifier (e.g., "A", "B")
#define ROOM_NUMBER     "101"  // Room number assigned to this ESP32 unit
#define ROOM_GENDER     "M"    // "M" for Male block, "F" for Female block

// ---- Shared Hardware Auth Key (Secret) ----
// Must match the DEVICE_API_KEY environment secret in Cloudflare Workers
#define DEVICE_API_KEY  "00000000-0000-0000-0000-000000000000"

// ---- TLS Root Certificate ----
// Only required when USE_INSECURE_TLS is set to false in main.cpp
static const char ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
PASTE_ROOT_CA_HERE
-----END CERTIFICATE-----
)EOF";