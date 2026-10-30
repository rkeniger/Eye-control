#include "settings.h"

#include <Preferences.h>

#include "config.h"

namespace settings {

namespace {

bool validServo(const ServoCal &s) {
    return s.minUs >= ABS_MIN_US && s.maxUs <= ABS_MAX_US && s.minUs <= s.centerUs && s.centerUs <= s.maxUs;
}

bool validUs(uint16_t us) { return us >= ABS_MIN_US && us <= ABS_MAX_US; }

}  // namespace

bool validCalibration(const Calibration &cal) {
    return validServo(cal.yaw) && validServo(cal.pitch) && validUs(cal.lid.closedUs) && validUs(cal.lid.openUs);
}

Calibration loadCalibration() {
    Preferences prefs;
    Calibration cal = DEFAULT_CALIBRATION;
    prefs.begin("eye", false);  // read-write creates the namespace, avoiding a NOT_FOUND log
    if (prefs.isKey("cal") && prefs.getBytesLength("cal") == sizeof(Calibration)) {
        Calibration stored;
        prefs.getBytes("cal", &stored, sizeof(stored));
        if (validCalibration(stored)) cal = stored;
    }
    prefs.end();
    return cal;
}

void saveCalibration(const Calibration &cal) {
    Preferences prefs;
    prefs.begin("eye", false);
    prefs.putBytes("cal", &cal, sizeof(cal));
    prefs.end();
}

bool loadAuto() {
    Preferences prefs;
    prefs.begin("eye", false);  // read-write creates the namespace, avoiding a NOT_FOUND log
    bool on = prefs.getBool("auto", false);
    prefs.end();
    return on;
}

void saveAuto(bool on) {
    Preferences prefs;
    prefs.begin("eye", false);
    prefs.putBool("auto", on);
    prefs.end();
}

bool loadWifi(String &ssid, String &pass) {
    Preferences prefs;
    prefs.begin("wifi", true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
    return ssid.length() > 0;
}

void saveWifi(const String &ssid, const String &pass) {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
}

void clearWifi() {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
}

}  // namespace settings
