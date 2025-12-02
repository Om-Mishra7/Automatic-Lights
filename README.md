# Automatic Lights

This is an ESP32 sketch that controls a relay based on ultrasonic motion detection and time-of-day schedules.

Key features:
- Works in AUTO, FORCE ON, and FORCE OFF modes.
- Uses NTP to sync time when WiFi is available.
- If WiFi is not available, the device will: 
  - Start a fallback AP called `AutoLight_Config` so you can connect locally for configuration.
  - Continue to operate the motion detection and relay control even without WiFi.
  - Retry connecting to configured WiFi every 5 minutes; when WiFi reconnects, the NTP time is resynced.

Configuration:
- Provision WiFi credentials via the web provisioning page (AP mode): open the `AutoLight_Config` WiFi network and visit the device's web UI, or visit `/wifi` to set SSID/password. You can also use the `Forget WiFi` action from the UI to clear saved credentials.
 - Provision WiFi credentials via the web provisioning page (AP mode): open the `AutoLight_Config` WiFi network and visit the device's web UI, or visit `/wifi` to set SSID/password. You can also use the `Forget WiFi` action from the UI to clear saved credentials.
- Use the web UI (when connected to WiFi or AP) to change times, detection distance, and override mode.

Notes:
- When running without WiFi and time hasn't been synced, the schedule relies on a motion-only fallback: motion detection remains active regardless of time-of-day.
- The device will try reconnecting to WiFi every 5 minutes automatically.

Hardware + LED status:
- The built-in LED on many ESP32 dev boards (GPIO2) indicates status: steady on when WiFi connected, slow blink when the AP is active, and fast blink when relay is ON.

Happy hacking!