#include "eye.h"

#include <WiFi.h>

#include "config.h"

namespace {

using Lock = std::lock_guard<std::mutex>;

uint32_t randomBetween(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

float servoDegToUs(const ServoCal &cal, float deg) {
    float us = cal.centerUs + (cal.invert ? -deg : deg) * US_PER_DEG;
    return constrain(us, (float)cal.minUs, (float)cal.maxUs);
}

float servoUsToDeg(const ServoCal &cal, float us) {
    float deg = (us - cal.centerUs) / US_PER_DEG;
    return cal.invert ? -deg : deg;
}

void degLimits(const ServoCal &cal, float &lo, float &hi) {
    float a = servoUsToDeg(cal, cal.minUs), b = servoUsToDeg(cal, cal.maxUs);
    lo = min(a, b);
    hi = max(a, b);
}

const char *modeName(Mode m) { return m == Mode::Auto ? "auto" : "manual"; }

const char *stateName(State s) {
    switch (s) {
        case State::Tracking: return "tracking";
        case State::Hold: return "hold";
        case State::Drift: return "drift";
        case State::Sleep: return "sleep";
    }
    return "?";
}

}  // namespace

float Eye::yawToUs(float deg) const { return servoDegToUs(cal_.yaw, deg); }
float Eye::pitchToUs(float deg) const { return servoDegToUs(cal_.pitch, deg); }
float Eye::lidToUs(float open) const {
    return cal_.lid.closedUs + constrain(open, 0.0f, 1.0f) * (cal_.lid.openUs - cal_.lid.closedUs);
}

void Eye::begin() {
    Lock lock(mutex_);
    cal_ = settings::loadCalibration();
    yaw_.begin(PIN_YAW, 0);
    pitch_.begin(PIN_PITCH, 1);
    lid_.begin(PIN_LID, 2);
    yaw_.reset(cal_.yaw.centerUs);
    pitch_.reset(cal_.pitch.centerUs);
    lid_.reset(lidToUs(1.0f));
    lid_.setSpeed(LID_SPEED_US_S);
    // The servos' real positions are unknown at power-on, so they will snap to centre.
    // Attaching one at a time spreads out the current spike.
    yaw_.attach();
    delay(300);
    pitch_.attach();
    delay(300);
    lid_.attach();

    uint32_t now = millis();
    lastTick_ = now;
    enter(State::Hold, now);
}

void Eye::tick(uint32_t now) {
    Lock lock(mutex_);
    float dt = (now - lastTick_) / 1000.0f;
    lastTick_ = now;

    if (mode_ == Mode::Manual) {
        if (now - lastManual_ > MANUAL_TIMEOUT_MS) {
            mode_ = Mode::Auto;
            enter(State::Hold, now);
        }
    } else {
        switch (state_) {
            case State::Tracking:
                if (now - lastTrackerMsg_ > FAILSAFE_MS) enter(State::Hold, now);
                break;
            case State::Hold:
                if (now - stateSince_ > HOLD_MS) enter(State::Drift, now);
                break;
            case State::Drift:
                if (now - stateSince_ > DRIFT_MS) enter(State::Sleep, now);
                else if ((int32_t)(now - nextDrift_) >= 0) pickDrift(now);
                break;
            case State::Sleep:
                if (yaw_.attached() && now - stateSince_ > SLEEP_DETACH_MS) detachAll();
                break;
        }
    }

    // Lid: open unless asleep (auto) or set by hand (manual); blinks briefly close it.
    float lidOpen = mode_ == Mode::Manual ? manualLid_ : (state_ == State::Sleep ? 0.0f : 1.0f);
    bool canBlink = mode_ == Mode::Auto && state_ != State::Sleep;
    if (canBlink && (int32_t)(now - nextBlink_) >= 0) {
        blinkUntil_ = now + BLINK_CLOSED_MS;
        scheduleBlink(now);
    }
    if ((int32_t)(blinkUntil_ - now) > 0) lidOpen = 0.0f;
    bool rawLid = mode_ == Mode::Manual && manualLidUs_ > 0;
    lid_.setTarget(rawLid ? manualLidUs_ : lidToUs(lidOpen));

    float speed = (mode_ == Mode::Manual || state_ == State::Tracking ? TRACK_SPEED_DEG_S : DRIFT_SPEED_DEG_S) *
                  US_PER_DEG;
    yaw_.setSpeed(speed);
    pitch_.setSpeed(speed);
    yaw_.update(dt);
    pitch_.update(dt);
    lid_.update(dt);
}

void Eye::enter(State state, uint32_t now) {
    if (state_ == State::Sleep && state != State::Sleep) attachAll();
    state_ = state;
    stateSince_ = now;
    switch (state) {
        case State::Drift:
            nextDrift_ = now + randomBetween(DRIFT_STEP_MIN_MS / 2, DRIFT_STEP_MIN_MS);
            break;
        case State::Sleep:
            yaw_.setTarget(cal_.yaw.centerUs);
            pitch_.setTarget(cal_.pitch.centerUs);
            break;
        default:
            break;
    }
    if (state != State::Sleep && nextBlink_ < now) scheduleBlink(now);
}

void Eye::pickDrift(uint32_t now) {
    float yLo, yHi, pLo, pHi;
    degLimits(cal_.yaw, yLo, yHi);
    degLimits(cal_.pitch, pLo, pHi);
    auto pick = [](float lo, float hi) {
        float r = (esp_random() / (float)UINT32_MAX) * 2.0f - 1.0f;  // -1..1
        return r * DRIFT_RANGE * (r < 0 ? -lo : hi);
    };
    yaw_.setTarget(yawToUs(pick(yLo, yHi)));
    pitch_.setTarget(pitchToUs(pick(pLo, pHi)));
    nextDrift_ = now + randomBetween(DRIFT_STEP_MIN_MS, DRIFT_STEP_MAX_MS);
}

void Eye::scheduleBlink(uint32_t now) { nextBlink_ = now + randomBetween(BLINK_MIN_MS, BLINK_MAX_MS); }

void Eye::attachAll() {
    yaw_.attach();
    pitch_.attach();
    lid_.attach();
}

void Eye::detachAll() {
    yaw_.detach();
    pitch_.detach();
    lid_.detach();
}

void Eye::track(float yawDeg, float pitchDeg) {
    Lock lock(mutex_);
    uint32_t now = millis();
    lastTrackerMsg_ = now;
    if (mode_ != Mode::Auto) return;
    if (state_ != State::Tracking) enter(State::Tracking, now);
    yaw_.setTarget(yawToUs(yawDeg));
    pitch_.setTarget(pitchToUs(pitchDeg));
}

void Eye::absent() {
    Lock lock(mutex_);
    uint32_t now = millis();
    lastTrackerMsg_ = now;
    if (mode_ == Mode::Auto && state_ == State::Tracking) enter(State::Hold, now);
}

void Eye::newTarget() {
    Lock lock(mutex_);
    if (mode_ == Mode::Auto && state_ != State::Sleep) blinkUntil_ = millis() + BLINK_CLOSED_MS;
}

void Eye::manual(const float *yawDeg, const float *pitchDeg, const float *lid) {
    Lock lock(mutex_);
    uint32_t now = millis();
    if (mode_ != Mode::Manual) {
        mode_ = Mode::Manual;
        manualLid_ = 1.0f;
        manualLidUs_ = 0;
    }
    lastManual_ = now;
    attachAll();
    if (yawDeg) yaw_.setTarget(yawToUs(*yawDeg));
    if (pitchDeg) pitch_.setTarget(pitchToUs(*pitchDeg));
    if (lid) {
        manualLid_ = constrain(*lid, 0.0f, 1.0f);
        manualLidUs_ = 0;
    }
}

void Eye::manualUs(Servo servo, float us) {
    Lock lock(mutex_);
    us = constrain(us, (float)ABS_MIN_US, (float)ABS_MAX_US);
    mode_ = Mode::Manual;
    lastManual_ = millis();
    attachAll();
    switch (servo) {
        case Servo::Yaw: yaw_.setTarget(us); break;
        case Servo::Pitch: pitch_.setTarget(us); break;
        case Servo::Lid:
            // Calibration may need to go beyond the current open/closed range, so hold a raw pulse
            // until the next normal lid command.
            manualLidUs_ = us;
            break;
    }
}

void Eye::setMode(Mode mode) {
    Lock lock(mutex_);
    uint32_t now = millis();
    if (mode == Mode::Manual) {
        if (mode_ != Mode::Manual) {
            manualLid_ = 1.0f;
            manualLidUs_ = 0;
        }
        mode_ = Mode::Manual;
        lastManual_ = now;
        attachAll();
    } else if (mode_ != Mode::Auto) {
        mode_ = Mode::Auto;
        enter(State::Hold, now);
    }
}

void Eye::blink() {
    Lock lock(mutex_);
    attachAll();
    blinkUntil_ = millis() + BLINK_CLOSED_MS;
}

Calibration Eye::calibration() {
    Lock lock(mutex_);
    return cal_;
}

bool Eye::setCalibration(const Calibration &cal) {
    if (!settings::validCalibration(cal)) return false;
    Lock lock(mutex_);
    cal_ = cal;
    settings::saveCalibration(cal);
    return true;
}

void Eye::status(JsonObject out) {
    Lock lock(mutex_);
    uint32_t now = millis();
    out["mode"] = modeName(mode_);
    out["state"] = stateName(state_);
    out["yaw"] = roundf(servoUsToDeg(cal_.yaw, yaw_.current()) * 10) / 10;
    out["pitch"] = roundf(servoUsToDeg(cal_.pitch, pitch_.current()) * 10) / 10;
    float lidSpan = cal_.lid.openUs - cal_.lid.closedUs;
    out["lid"] = lidSpan ? roundf((lid_.current() - cal_.lid.closedUs) / lidSpan * 100) / 100 : 0;
    JsonObject us = out["us"].to<JsonObject>();
    us["yaw"] = lroundf(yaw_.current());
    us["pitch"] = lroundf(pitch_.current());
    us["lid"] = lroundf(lid_.current());
    out["attached"] = yaw_.attached();
    float lo, hi;
    degLimits(cal_.yaw, lo, hi);
    JsonObject limits = out["limits"].to<JsonObject>();
    JsonArray yl = limits["yaw"].to<JsonArray>();
    yl.add(lo);
    yl.add(hi);
    degLimits(cal_.pitch, lo, hi);
    JsonArray pl = limits["pitch"].to<JsonArray>();
    pl.add(lo);
    pl.add(hi);
    out["trackerActive"] = lastTrackerMsg_ && now - lastTrackerMsg_ < FAILSAFE_MS;
    out["manualRemainingMs"] = mode_ == Mode::Manual ? MANUAL_TIMEOUT_MS - min(now - lastManual_, MANUAL_TIMEOUT_MS) : 0;
    out["uptimeMs"] = now;
    out["rssi"] = WiFi.RSSI();
    out["ip"] = WiFi.localIP().toString();
}
