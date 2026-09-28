ESP32 Power Check uses a USB-powered XIAO ESP32-C3 to monitor power availability at a remote location. The device sends authenticated heartbeats over Wi-Fi to Gatus every 30 seconds; when those heartbeats stop, Gatus can trigger an outage alert. The firmware supports automatic network recovery, HTTP or verified HTTPS, and serial diagnostics. It is a continuity indicator rather than an electrical meter, so an alert may indicate either loss of power or failure elsewhere in the communications path.

Building 1 specifics: Device is plugged into circuit to be monitored, connects to 1 of 3 on premise WiFi Moxee hostspots and sends 'heartbeats' to a monitoring server.

Additional information can be found at [web.git.mil/nets-sandbox/esp32_power_check](https://web.git.mil/nets-sandbox/esp32_power_check)
