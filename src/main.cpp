// src/main.cpp
//
// Building-power monitor liveness heartbeat for Seeed Studio XIAO ESP32-C3.
//
// Behaviour:
//   - Joins the strongest available network from a list of 2.4 GHz Wi-Fi
//     credentials (WiFiMulti), falling back automatically when the preferred
//     network is absent.
//   - POSTs a push-based heartbeat to Gatus immediately after boot and then
//     every HEARTBEAT_INTERVAL_MS (30s):
//         POST <GATUS_BASE_URL>/api/v1/endpoints/<KEY>/external?success=true
//         Authorization: Bearer <GATUS_TOKEN>
//   - Supports plain http:// and verified https://.  Verified TLS requires a
//     pinned GATUS_ROOT_CA and a synchronised clock (NTP); without a CA the
//     firmware refuses to send over TLS unless GATUS_ALLOW_INSECURE_TLS is set.
//   - Reconnects Wi-Fi automatically and retries heartbeats; reboots itself if
//     the network stays down or Gatus stays unreachable for a long time.
//   - Logs boot count, reset reason, uptime, Wi-Fi RSSI, local IP, free heap
//     and the HTTP response code to the serial console (115200 baud).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>

#include "secrets.h"

// ---------------------------------------------------------------------------
// Compile-time validation of the secrets contract.
// ---------------------------------------------------------------------------
#ifndef WIFI_SSID
#error "WIFI_SSID is not defined - copy include/secrets.example.h to include/secrets.h"
#endif
#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD is not defined - copy include/secrets.example.h to include/secrets.h"
#endif
#ifndef GATUS_BASE_URL
#error "GATUS_BASE_URL is not defined - copy include/secrets.example.h to include/secrets.h"
#endif
#ifndef GATUS_ENDPOINT_KEY
#error "GATUS_ENDPOINT_KEY is not defined (format: <group>_<name>)"
#endif
#ifndef GATUS_TOKEN
#error "GATUS_TOKEN is not defined"
#endif

// Optional, overridable via secrets.h.
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "time.cloudflare.com"
#endif

namespace {

// --- Tunables --------------------------------------------------------------
constexpr uint32_t HEARTBEAT_INTERVAL_MS    = 30000;   // 30 s
constexpr uint32_t HTTP_TIMEOUT_MS          = 10000;   // read timeout
constexpr uint32_t TLS_CONNECT_TIMEOUT_MS   = 15000;   // TLS handshake may be slower
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS  = 12000;   // per WiFiMulti.run() attempt
constexpr uint32_t WIFI_RETRY_INTERVAL_MS   = 10000;   // between reconnect attempts
constexpr uint32_t NTP_BOOT_TIMEOUT_MS     = 5000;    // best-effort at boot
constexpr uint32_t NTP_SYNC_TIMEOUT_MS     = 15000;   // retries / TLS gate
constexpr uint32_t NTP_RETRY_INTERVAL_MS   = 60000;   // between NTP attempts
constexpr uint32_t HTTP_REBOOT_AFTER_FAILS  = 20;      // ~10 min of heartbeat failures
constexpr uint32_t WIFI_REBOOT_AFTER_FAILS  = 30;      // ~5 min of failed joins
constexpr uint32_t WIFI_FALLBACK_AFTER_FAILS = 3;      // switch to ordered pass
constexpr time_t   TIME_VALID_THRESHOLD     = 1700000000;  // 2023-11-14
constexpr const char* HOSTNAME              = "xiao-power-monitor";
constexpr const char* USER_AGENT            = "xiao-esp32c3-power-monitor/1.0";

// --- Wi-Fi credentials -----------------------------------------------------
// The table order only matters for the manual fallback pass; WiFiMulti picks
// the strongest *available* configured network regardless of order.
struct WifiCredential {
  const char* ssid;
  const char* password;
};

const WifiCredential kWifiNetworks[] = {
  {WIFI_SSID, WIFI_PASSWORD},
#ifdef WIFI_SSID_2
  {WIFI_SSID_2, WIFI_PASSWORD_2},
#endif
#ifdef WIFI_SSID_3
  {WIFI_SSID_3, WIFI_PASSWORD_3},
#endif
};
constexpr size_t kWifiNetworkCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);
static_assert(kWifiNetworkCount >= 1, "at least one Wi-Fi network is required");

WiFiMulti wifiMulti;

// --- State -----------------------------------------------------------------
uint32_t bootCount          = 0;
uint32_t lastHeartbeatMs    = 0;
uint32_t lastWifiTryMs      = 0;
uint32_t lastNtpTryMs       = 0;
uint32_t httpFailStreak     = 0;
uint32_t wifiFailStreak     = 0;
bool     firstHeartbeatSent = false;

// --- Helpers ---------------------------------------------------------------

const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external-pin";
    case ESP_RST_SW:        return "software-restart";
    case ESP_RST_PANIC:     return "exception/panic";
    case ESP_RST_INT_WDT:   return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "other-watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep-wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
#ifdef ESP_RST_USB
    case ESP_RST_USB:       return "usb-peripheral-reset";
#endif
#ifdef ESP_RST_JTAG
    case ESP_RST_JTAG:      return "jtag-reset";
#endif
#ifdef ESP_RST_EFUSE
    case ESP_RST_EFUSE:     return "efuse-error";
#endif
#ifdef ESP_RST_PWR_GLITCH
    case ESP_RST_PWR_GLITCH: return "power-glitch";
#endif
#ifdef ESP_RST_CPU_LOCKUP
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
#endif
    default:                return "unknown";
  }
}

uint32_t bumpBootCount() {
  Preferences prefs;
  if (!prefs.begin("power-mon", /*readOnly=*/false)) {
    Serial.println("[nvs] WARNING: could not open preferences; boot count unavailable");
    return 0;
  }
  const uint32_t count = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", count);
  prefs.end();
  return count;
}

// Human-readable uptime as d+HH:MM:SS.
String uptimeText(uint32_t ms) {
  const uint32_t totalSec = ms / 1000;
  const uint32_t days = totalSec / 86400;
  const uint32_t hours = (totalSec % 86400) / 3600;
  const uint32_t mins = (totalSec % 3600) / 60;
  const uint32_t secs = totalSec % 60;
  char buf[32];
  snprintf(buf, sizeof(buf), "%lud+%02lu:%02lu:%02lu",
           static_cast<unsigned long>(days), static_cast<unsigned long>(hours),
           static_cast<unsigned long>(mins), static_cast<unsigned long>(secs));
  return String(buf);
}

bool urlUsesTls() {
  return strncmp(GATUS_BASE_URL, "https://", 8) == 0;
}

String heartbeatUrl() {
  String url = GATUS_BASE_URL;
  if (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  url += "/api/v1/endpoints/";
  url += GATUS_ENDPOINT_KEY;
  url += "/external?success=true";
  return url;
}

void logWifiAssociation() {
  Serial.printf("[wifi] associated: ssid=%s ip=%s rssi=%d dBm channel=%d bssid=%s\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                WiFi.channel(), WiFi.BSSIDstr().c_str());
}

// Diagnostic: lists what the radio can actually see, so a failed join can be
// told apart from a network that simply is not in range.
void logVisibleNetworks() {
  const int found = WiFi.scanNetworks();
  if (found <= 0) {
    Serial.printf("[wifi] scan found no networks (result=%d)\n", found);
    WiFi.scanDelete();
    return;
  }
  Serial.printf("[wifi] scan found %d network(s):\n", found);
  for (int i = 0; i < found; ++i) {
    Serial.printf("[wifi]   %2d: rssi=%4d ch=%2d %s%s\n", i + 1, WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.SSID(i).c_str(),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " (open)" : "");
  }
  WiFi.scanDelete();
}

void configureWifiRadio() {
  WiFi.mode(WIFI_STA);            // STA only: no access point, no web server.
  WiFi.setHostname(HOSTNAME);
  WiFi.persistent(false);         // no stale-AP auto-connect, we own selection
  WiFi.setAutoReconnect(false);   // WiFiMulti owns reconnection
}

void registerWifiNetworks() {
  size_t registered = 0;
  for (size_t i = 0; i < kWifiNetworkCount; ++i) {
    if (wifiMulti.addAP(kWifiNetworks[i].ssid, kWifiNetworks[i].password)) {
      registered++;
    } else {
      Serial.printf("[wifi] WARNING: rejected network %u (\"%s\")\n",
                    static_cast<unsigned>(i + 1), kWifiNetworks[i].ssid);
    }
  }
  Serial.printf("[wifi] registered %u/%u network(s)\n", static_cast<unsigned>(registered),
                static_cast<unsigned>(kWifiNetworkCount));
}

// WiFiMulti: scans and joins the strongest *available* configured network.
bool connectToAnyNetwork(uint32_t timeoutMs) {
  const uint8_t status = wifiMulti.run(timeoutMs);
  if (status == WL_CONNECTED) {
    logWifiAssociation();
    return true;
  }
  Serial.printf("[wifi] WiFiMulti.run failed (status=%u)\n", static_cast<unsigned>(status));
  return false;
}

// Ordered pass used when WiFiMulti keeps re-picking a configured-but-failing
// AP (e.g. a stale password) and never reaches a working one.
bool connectToAnyNetworkManual(uint32_t timeoutMs) {
  for (size_t i = 0; i < kWifiNetworkCount; ++i) {
    Serial.printf("[wifi] manual try %u/%u \"%s\"\n", static_cast<unsigned>(i + 1),
                  static_cast<unsigned>(kWifiNetworkCount), kWifiNetworks[i].ssid);
    WiFi.disconnect(false);
    delay(10);
    WiFi.begin(kWifiNetworks[i].ssid, kWifiNetworks[i].password);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      logWifiAssociation();
      return true;
    }
  }
  Serial.println("[wifi] manual pass: no network joined");
  return false;
}

bool clockValid() {
  return time(nullptr) > TIME_VALID_THRESHOLD;
}

bool syncClockOnce(uint32_t timeoutMs) {
  IPAddress ntpIp;
  if (WiFi.hostByName(NTP_SERVER_1, ntpIp)) {
    Serial.printf("[ntp] %s -> %s\n", NTP_SERVER_1, ntpIp.toString().c_str());
  } else {
    Serial.printf("[ntp] %s DNS lookup FAILED\n", NTP_SERVER_1);
  }
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  const uint32_t start = millis();
  while (!clockValid() && (millis() - start) < timeoutMs) {
    delay(200);
  }
  const long epoch = static_cast<long>(time(nullptr));
  if (clockValid()) {
    Serial.printf("[ntp] time synchronized (epoch=%ld)\n", epoch);
    return true;
  }
  Serial.printf("[ntp] NOT synchronized (epoch=%ld)\n", epoch);
  return false;
}

// Sends one heartbeat. Returns the HTTP status code, or a negative value on a
// transport/configuration failure (-1 begin failed, -2 no Wi-Fi, -3 TLS config).
int sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    return -2;
  }

  const String url = heartbeatUrl();
  const bool https = url.startsWith("https://");

  // Both clients live for the whole call so http.end() runs before teardown.
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  bool began;
  if (https) {
    http.setConnectTimeout(TLS_CONNECT_TIMEOUT_MS);
    secureClient.setHandshakeTimeout(15);
#if defined(GATUS_ROOT_CA)
    secureClient.setCACert(GATUS_ROOT_CA);
#elif defined(GATUS_ALLOW_INSECURE_TLS)
    Serial.println("[tls] WARNING: certificate validation disabled (GATUS_ALLOW_INSECURE_TLS)");
    secureClient.setInsecure();
#else
    Serial.println("[tls] ERROR: https URL but no GATUS_ROOT_CA configured; "
                   "define GATUS_ROOT_CA (or GATUS_ALLOW_INSECURE_TLS to override)");
    return -3;
#endif
    began = http.begin(secureClient, url);
  } else {
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    began = http.begin(plainClient, url);
  }

  if (!began) {
    Serial.println("[gatus] ERROR: http.begin() failed (bad URL?)");
    if (https) {
      char tlsErr[128] = {0};
      secureClient.lastError(tlsErr, sizeof(tlsErr));
      Serial.printf("[tls] handshake error: %s\n", tlsErr);
    }
    return -1;
  }

  http.addHeader("Authorization", "Bearer " GATUS_TOKEN);
  http.addHeader("User-Agent", USER_AGENT);
  const int code = http.POST("");

  if (code <= 0 && https) {
    char tlsErr[128] = {0};
    const int tlsCode = secureClient.lastError(tlsErr, sizeof(tlsErr));
    Serial.printf("[tls] error %d: %s\n", tlsCode, tlsErr);
  }

  http.end();
  return code;
}

void logHeartbeat(int code) {
  Serial.printf("[gatus] heartbeat url=%s\n", heartbeatUrl().c_str());
  Serial.printf("[gatus] http_code=%d uptime=%s rssi=%d dBm ip=%s heap=%u\n",
                code, uptimeText(millis()).c_str(), WiFi.RSSI(),
                WiFi.localIP().toString().c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart) < 3000) {
    delay(10);  // give the USB CDC host a moment to attach
  }

  const esp_reset_reason_t reason = esp_reset_reason();
  bootCount = bumpBootCount();

  Serial.println();
  Serial.println("=====================================================");
  Serial.println(" XIAO ESP32-C3 building-power monitor (Gatus push)");
  Serial.printf(" boot_count=%lu reset_reason=%s chip=%s\n",
                static_cast<unsigned long>(bootCount), resetReasonText(reason),
                ESP.getChipModel());
  Serial.printf(" heartbeat_interval=%lus link=%s\n",
                static_cast<unsigned long>(HEARTBEAT_INTERVAL_MS / 1000),
                urlUsesTls() ? "https" : "http");
  Serial.println("=====================================================");

  configureWifiRadio();
  registerWifiNetworks();
  if (connectToAnyNetwork(WIFI_CONNECT_TIMEOUT_MS)) {
    // association already logged by connectToAnyNetwork()
  } else {
    Serial.println("[wifi] not connected yet; will keep retrying");
    logVisibleNetworks();
  }

  // Time is always synced (cheap, and makes log timestamps sane); it is only
  // *required* before a TLS handshake.  Boot uses a short timeout so a blocked
  // NTP server cannot delay the first heartbeat; loop() retries with a longer one.
  syncClockOnce(NTP_BOOT_TIMEOUT_MS);
  lastNtpTryMs = millis();
  // The first heartbeat is sent from loop() as soon as Wi-Fi is up.
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    const uint32_t now = millis();
    if ((now - lastWifiTryMs) >= WIFI_RETRY_INTERVAL_MS) {
      lastWifiTryMs = now;
      wifiFailStreak++;
      Serial.printf("[wifi] disconnected (status=%d, streak=%lu); reconnecting\n",
                    static_cast<int>(WiFi.status()),
                    static_cast<unsigned long>(wifiFailStreak));

      const bool joined =
          (wifiFailStreak >= WIFI_FALLBACK_AFTER_FAILS)
              ? connectToAnyNetworkManual(WIFI_CONNECT_TIMEOUT_MS)
              : connectToAnyNetwork(WIFI_CONNECT_TIMEOUT_MS);

      if (joined) {
        wifiFailStreak = 0;
      } else if (wifiFailStreak >= WIFI_REBOOT_AFTER_FAILS) {
        Serial.println("[wifi] FATAL: cannot join Wi-Fi; rebooting for a clean slate");
        delay(100);
        ESP.restart();
      }
    }
    delay(50);
    return;
  }

  const uint32_t now = millis();

  // Keep trying to set the clock.  TLS rejects certificates while the clock is
  // unset, so under https a still-invalid clock defers the heartbeat (without
  // counting a failure); under plain http heartbeats proceed regardless.
  if (!clockValid() && (now - lastNtpTryMs) >= NTP_RETRY_INTERVAL_MS) {
    lastNtpTryMs = now;
    Serial.println("[ntp] clock not set; syncing");
    syncClockOnce(NTP_SYNC_TIMEOUT_MS);
  }
  if (urlUsesTls() && !clockValid()) {
    delay(50);
    return;
  }

  // Connected: send immediately after boot, then on the fixed interval.
  if (!firstHeartbeatSent || (now - lastHeartbeatMs) >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    const int code = sendHeartbeat();
    firstHeartbeatSent = true;
    logHeartbeat(code);

    if (code == 200) {
      httpFailStreak = 0;
    } else {
      httpFailStreak++;
      if (code <= 0) {
        Serial.printf("[gatus] transport failure (code=%d), wifi_status=%d\n",
                      code, static_cast<int>(WiFi.status()));
      } else {
        Serial.printf("[gatus] unexpected HTTP status %d (check token/key)\n", code);
      }
    }

    if (httpFailStreak >= HTTP_REBOOT_AFTER_FAILS) {
      Serial.println("[gatus] FATAL: heartbeat failing persistently; rebooting");
      delay(100);
      ESP.restart();
    }
  }

  delay(50);
}
