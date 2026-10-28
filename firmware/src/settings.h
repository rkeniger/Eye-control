#pragma once

#include <Arduino.h>

struct ServoCal {
    uint16_t minUs;
    uint16_t centerUs;
    uint16_t maxUs;
    bool invert;
};

struct LidCal {
    uint16_t closedUs;
    uint16_t openUs;
};

struct Calibration {
    ServoCal yaw;
    ServoCal pitch;
    LidCal lid;
};

namespace settings {

// Conservative defaults so an uncalibrated eye can't drive into the printed hard stops.
constexpr Calibration DEFAULT_CALIBRATION = {
    {1100, 1500, 1900, false},
    {1200, 1500, 1800, false},
    {1100, 1700},
};

Calibration loadCalibration();
void saveCalibration(const Calibration &cal);
bool validCalibration(const Calibration &cal);

bool loadWifi(String &ssid, String &pass);
void saveWifi(const String &ssid, const String &pass);
void clearWifi();

}  // namespace settings
