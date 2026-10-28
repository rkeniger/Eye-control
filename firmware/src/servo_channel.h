#pragma once

#include <Arduino.h>

// One hobby servo on an LEDC channel, with speed-limited easing towards a target pulse width.
class ServoChannel {
   public:
    void begin(int pin, int channel);
    void reset(float us);  // jump current and target (no easing), e.g. at boot
    void setTarget(float us) { target_ = us; }
    void setSpeed(float usPerSec) { speed_ = usPerSec; }
    float target() const { return target_; }
    float current() const { return current_; }
    void update(float dt);
    void attach();
    void detach();
    bool attached() const { return attached_; }

   private:
    void write();

    int pin_ = -1;
    int channel_ = -1;
    float current_ = 1500;
    float target_ = 1500;
    float speed_ = 4000;
    bool attached_ = false;
    uint32_t lastDuty_ = 0;
};
