#pragma once

#include <ArduinoJson.h>

#include <mutex>

#include "servo_channel.h"
#include "settings.h"

enum class Mode { Auto, Manual };
enum class State { Tracking, Hold, Drift, Sleep };
enum class Servo { Yaw, Pitch, Lid };

// Eye behaviour: tracking, idle hold -> drift -> sleep, blinking, manual override, failsafe.
// Public methods are thread-safe (web handlers run on the AsyncTCP task, tick() on the loop task).
class Eye {
   public:
    void begin();
    void tick(uint32_t now);

    // Tracker (auto mode) commands. Angles are degrees around the calibrated centre.
    void track(float yawDeg, float pitchDeg);
    void absent();
    void newTarget();

    // Manual control; any of these switches to manual mode.
    void manual(const float *yawDeg, const float *pitchDeg, const float *lid);
    void manualUs(Servo servo, float us);  // raw pulse for calibration, bypasses calibrated limits

    void setMode(Mode mode);
    void blink();

    Calibration calibration();
    bool setCalibration(const Calibration &cal);
    void status(JsonObject out);

   private:
    void enter(State state, uint32_t now);
    void attachAll();
    void detachAll();
    void pickDrift(uint32_t now);
    void scheduleBlink(uint32_t now);
    float yawToUs(float deg) const;
    float pitchToUs(float deg) const;
    float lidToUs(float open) const;

    std::mutex mutex_;
    Calibration cal_;
    ServoChannel yaw_, pitch_, lid_;
    Mode mode_ = Mode::Auto;
    State state_ = State::Hold;
    uint32_t stateSince_ = 0;
    uint32_t lastTick_ = 0;
    uint32_t lastTrackerMsg_ = 0;
    uint32_t lastManual_ = 0;
    uint32_t nextDrift_ = 0;
    uint32_t nextBlink_ = 0;
    uint32_t blinkUntil_ = 0;
    float manualLid_ = 1.0f;
    float manualLidUs_ = 0;  // >0: raw lid pulse set from the calibration page
};
