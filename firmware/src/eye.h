#pragma once

#include <ArduinoJson.h>

#include <mutex>

#include "servo_channel.h"
#include "settings.h"

enum class Mode { Auto, Manual };
enum class State { Tracking, Hold, Drift, Sleep };
enum class Servo { Yaw, Pitch, Lid };

// Eye behaviour. Auto (a saved switch): tracking, idle hold -> drift -> sleep, blinking, failsafe.
// Manual (the default): holds wherever it was last put. Teach commands move it in either mode.
// Public methods are thread-safe (web handlers run on the AsyncTCP task, tick() on the loop task).
class Eye {
   public:
    void begin();
    void attachServos();  // call once at boot, after Wi-Fi; blocks for the staggered power-up
    void tick(uint32_t now);

    // Tracker commands (ignored unless auto). Angles are degrees around the calibrated centre.
    void track(float yawDeg, float pitchDeg);
    void absent();
    void newTarget();
    // Point-and-teach from the tracker page: moves the eye in any mode while commands keep coming.
    void teach(float yawDeg, float pitchDeg);

    // Manual control; any of these switches auto off.
    void manual(const float *yawDeg, const float *pitchDeg, const float *lid);
    void manualUs(Servo servo, float us);  // raw pulse for calibration, bypasses calibrated limits

    void setAuto(bool on);  // saved, so it survives restarts
    void blink();

    // Stop driving the servos and ignore all commands (during a firmware update).
    void suspend();
    void resume();

    Calibration calibration();
    bool setCalibration(const Calibration &cal);
    void status(JsonObject out);

   private:
    void enter(State state, uint32_t now);
    void setAutoLocked(bool on, uint32_t now);
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
    Mode mode_ = Mode::Manual;
    State state_ = State::Hold;
    uint32_t stateSince_ = 0;
    uint32_t lastTick_ = 0;
    uint32_t lastTrackerMsg_ = 0;
    uint32_t teachUntil_ = 0;
    bool teaching_ = false;
    bool suspended_ = false;
    uint32_t nextDrift_ = 0;
    uint32_t nextBlink_ = 0;
    uint32_t blinkUntil_ = 0;
    float manualLid_ = 1.0f;
    float manualLidUs_ = 0;  // >0: raw lid pulse set from the calibration page
};
