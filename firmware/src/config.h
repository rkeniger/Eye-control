#pragma once

#include <Arduino.h>

// Servo signal pins. Avoids strapping pins, USB (19/20), flash/octal PSRAM (26-37) and UART0.
constexpr int PIN_YAW = 4;
constexpr int PIN_PITCH = 5;
constexpr int PIN_LID = 6;

// On-board WS2812 RGB LED: green = on Wi-Fi with the server running, red = not.
// GPIO48 on most N16R8 boards; some revisions wire it to GPIO38.
constexpr int PIN_STATUS_LED = 48;
constexpr uint8_t STATUS_LED_BRIGHTNESS = 24;  // 0-255; the LED is very bright at full power

// Log shown on the web page, from boot. Oldest output is dropped once this fills.
constexpr size_t LOG_BUFFER_SIZE = 32 * 1024;

// A new firmware is kept once it's online, or after running this long without restarting.
constexpr uint32_t OTA_PROBATION_MS = 60000;

constexpr const char *HOSTNAME = "eyeball";
constexpr const char *SETUP_AP_SSID = "Eyeball-Setup";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// Servos power up one at a time, after Wi-Fi is up, so their start-up current spikes
// don't coincide with each other or with the radio's.
constexpr uint32_t SERVO_ATTACH_STAGGER_MS = 1000;

// SG90: roughly 2000 us of pulse width over ~180 degrees.
constexpr float US_PER_DEG = 11.0f;
// Hard bounds for any pulse, even during calibration.
constexpr uint16_t ABS_MIN_US = 500;
constexpr uint16_t ABS_MAX_US = 2500;

// Behaviour timing.
constexpr uint32_t FAILSAFE_MS = 5000;         // no tracker command while tracking -> treat as absent
constexpr uint32_t HOLD_MS = 2000;             // keep looking where the person was
constexpr uint32_t DRIFT_MS = 30000;           // then look around for this long
constexpr uint32_t DRIFT_STEP_MIN_MS = 1500;   // time between drift glances
constexpr uint32_t DRIFT_STEP_MAX_MS = 4000;
constexpr float DRIFT_RANGE = 0.6f;            // fraction of the yaw/pitch range used when drifting
constexpr uint32_t SLEEP_DETACH_MS = 1500;     // after closing, stop pulses so servos rest
constexpr uint32_t TEACH_HOLD_MS = 1000;       // teach mode ends this long after the last teach command
constexpr uint32_t BLINK_MIN_MS = 3000;
constexpr uint32_t BLINK_MAX_MS = 8000;
constexpr uint32_t BLINK_CLOSED_MS = 150;

// Motion speeds.
constexpr float TRACK_SPEED_DEG_S = 400.0f;
constexpr float DRIFT_SPEED_DEG_S = 60.0f;
constexpr float LID_SPEED_US_S = 10000.0f;
