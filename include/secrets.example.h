// include/secrets.example.h
//
// Copy this file to include/secrets.h and fill in your own values.
// include/secrets.h is listed in .gitignore and must never be committed.

#pragma once

// ---------------------------------------------------------------------------
// Wi-Fi (2.4 GHz only - the ESP32-C3 has no 5 GHz radio)
// ---------------------------------------------------------------------------
// WIFI_SSID / WIFI_PASSWORD are required.  Add up to two more networks with
// the _2 / _3 variants; the device joins whichever configured network is
// available and strongest (WiFiMulti).  Use "" for an open network's password.
#define WIFI_SSID       "your-2.4ghz-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

// #define WIFI_SSID_2     "backup-2.4ghz-ssid"
// #define WIFI_PASSWORD_2 "backup-wifi-password"

// #define WIFI_SSID_3     "third-2.4ghz-ssid"
// #define WIFI_PASSWORD_3 "third-wifi-password"

// ---------------------------------------------------------------------------
// Gatus push-based (external) monitoring
// ---------------------------------------------------------------------------
// The heartbeat URL is composed at runtime as:
//   <GATUS_BASE_URL>/api/v1/endpoints/<GATUS_ENDPOINT_KEY>/external?success=true
//
// The matching server-side endpoint must be declared under
// `external-endpoints` in your Gatus config - see gatus-config.example.yaml.
// Gatus derives GATUS_ENDPOINT_KEY from "<group>_<name>" with spaces, '/',
// '_', ',', '.', '#' and '&' in either part replaced by '-'.
//
// For https:// this MUST be a DNS hostname that matches the certificate's SAN
// (e.g. "https://gatus.example.com"), not a bare IP address - hostname
// verification would fail against an IP-based URL. HTTPS defaults to port 443;
// do not retain the former HTTP port 8085.
#define GATUS_BASE_URL      "https://gatus.ddns.net"
#define GATUS_ENDPOINT_KEY  "replace-with-group_name"
#define GATUS_TOKEN         "replace-with-the-external-endpoint-token"

// ---------------------------------------------------------------------------
// TLS (only relevant when GATUS_BASE_URL is https://)
// ---------------------------------------------------------------------------
// Trust ISRG Root X1 for the Let's Encrypt chain served by gatus.ddns.net.
#include "gatus_root_ca.h"
// The device verifies the certificate chain + hostname. The device also
// needs a valid clock, which it obtains over NTP (see below).
// For another CA, replace the include above with its PEM-encoded root:
//
// #define GATUS_ROOT_CA \
//   "-----BEGIN CERTIFICATE-----\n" \
//   "MIIB...\n" \
//   "-----END CERTIFICATE-----\n"

// Escape hatch ONLY for testing: sets https:// without certificate
// validation.  Without this (and without GATUS_ROOT_CA) the firmware refuses
// to send over TLS rather than silently connecting unverified.
// #define GATUS_ALLOW_INSECURE_TLS

// Optional NTP overrides (defaults: pool.ntp.org / time.cloudflare.com).
// #define NTP_SERVER_1 "pool.ntp.org"
// #define NTP_SERVER_2 "time.cloudflare.com"
