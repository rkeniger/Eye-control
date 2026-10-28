#pragma once

#include <Arduino.h>

// Servo signal pins. Avoids strapping pins, USB (19/20), flash/octal PSRAM (26-37) and UART0.
constexpr int PIN_YAW = 4;
constexpr int PIN_PITCH = 5;
constexpr int PIN_LID = 6;

constexpr const char *HOSTNAME = "eyeball";
constexpr const char *SETUP_AP_SSID = "Eyeball-Setup";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

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
constexpr uint32_t MANUAL_TIMEOUT_MS = 60000;  // manual control reverts to auto after this
constexpr uint32_t BLINK_MIN_MS = 3000;
constexpr uint32_t BLINK_MAX_MS = 8000;
constexpr uint32_t BLINK_CLOSED_MS = 150;

// Motion speeds.
constexpr float TRACK_SPEED_DEG_S = 400.0f;
constexpr float DRIFT_SPEED_DEG_S = 60.0f;
constexpr float LID_SPEED_US_S = 10000.0f;
