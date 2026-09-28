# ESP32 Power Check Executive CONOPS Brief

## Mission

ESP32 Power Check supplies an inexpensive, unattended indication that power and
the required communications path remain available at a monitored location. A
USB-powered Seeed Studio XIAO ESP32-C3 sends an authenticated heartbeat to Gatus
immediately after boot and every 30 seconds. Gatus changes endpoint health and
delivers configured alerts when those heartbeats stop.

This is a continuity monitor, not an electrical meter. Loss of heartbeat may
mean loss of monitored power, Wi-Fi, DNS, routing, time service, Gatus, or the
device itself. Operations must correlate an alert with facility and network
evidence before assigning root cause.

## Operating concept

```mermaid
flowchart LR
    P[Monitored USB power] --> E[ESP32-C3]
    E -->|2.4 GHz Wi-Fi| N[Local network]
    N -->|Authenticated heartbeat every 30 s| G[Gatus]
    G -->|Health state and alerts| O[Operations]
```

The installer provisions one to three 2.4 GHz Wi-Fi networks and a matching
Gatus external endpoint. The device connects to the strongest available
configured network, obtains time when needed for HTTPS, and sends outbound-only
heartbeat requests. There is no inbound web server.

Gatus, rather than the device, owns alert thresholds and delivery. The supplied
example waits 90 seconds without a push before the endpoint becomes unhealthy
and applies additional failure/success thresholds to reduce alert flapping.

## Resilience and visibility

The design is intended to run without routine intervention:

- disconnected Wi-Fi is retried every 10 seconds;
- after 3 failed reconnects, the device also tries configured networks in order;
- after 30 failed Wi-Fi attempts, it restarts itself;
- an invalid clock is retried through NTP every 60 seconds, and verified HTTPS
  waits for valid time rather than weakening certificate checks;
- non-`200` Gatus responses and transport errors are retried at the heartbeat
  cadence, with restart after 20 consecutive failures; and
- USB serial records boot count, reset reason, network association, time/TLS
  problems, response status, uptime, signal strength, IP address, and free heap.

At nominal intervals, the restart thresholds correspond to roughly five minutes
of Wi-Fi join failures and ten minutes of heartbeat failures. Connection and
request timeout duration can extend those elapsed times.

## Security posture

Production deployments should use HTTPS with a trusted root CA and a DNS name
that matches the certificate. The firmware authenticates to Gatus with a bearer
token. Wi-Fi credentials, the token, and trust material reside in an ignored
local secrets header and must not enter source control or operational tickets.

An option to disable TLS validation exists strictly for testing and emits a
warning. It is not an acceptable production configuration. Physical access to
the device and its USB maintenance interface must also be controlled.

## Ownership and response

| Owner | Accountability |
| --- | --- |
| System owner | Defines the monitored circuit, alert objective, and accepted residual risk. |
| Installer/maintainer | Provisions, flashes, validates, rotates credentials, and diagnoses serial output. |
| Network/Gatus administrator | Maintains Wi-Fi, DHCP, DNS/NTP, routing, endpoint configuration, and alert delivery. |
| Operator/responder | Monitors Gatus, verifies power first, then checks communications and service health. |

Maintenance is triggered by credential or CA changes, network changes, Gatus
migration, repeated restarts, TLS errors, or firmware updates. Retirement
requires disabling the Gatus endpoint and alerts, revoking the token, removing
the device, and updating asset records.

## Success measures

An accepted installation:

- starts cleanly and reports its boot/reset information over 115200-baud serial;
- joins the intended 2.4 GHz network and obtains a valid address;
- validates time and certificates when using HTTPS;
- receives HTTP `200` from the correct Gatus endpoint at the 30-second cadence;
- becomes unhealthy and alerts according to policy when heartbeats cease;
- recovers when Wi-Fi or Gatus availability returns; and
- stores no production secret in tracked project files.

Detailed interfaces, timings, operational scenarios, fault response, and
acceptance procedures are defined in [`CONOPS.md`](CONOPS.md).
