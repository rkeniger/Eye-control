#include "servo_channel.h"

#include <math.h>

namespace {

constexpr uint32_t PWM_HZ = 50;
constexpr uint8_t PWM_BITS = 14;
constexpr float PERIOD_US = 1e6f / PWM_HZ;
constexpr float EASE_RATE = 12.0f;  // 1/s; higher = snappier approach to target

}  // namespace

void ServoChannel::begin(int pin, int channel) {
    pin_ = pin;
    channel_ = channel;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttachChannel(pin_, PWM_HZ, PWM_BITS, channel_);
#else
    ledcSetup(channel_, PWM_HZ, PWM_BITS);
    ledcAttachPin(pin_, channel_);
#endif
    detach();
}

void ServoChannel::reset(float us) {
    current_ = target_ = us;
    write();
}

void ServoChannel::update(float dt) {
    float delta = target_ - current_;
    if (fabsf(delta) < 1.0f) {
        current_ = target_;
    } else {
        float step = delta * (1.0f - expf(-EASE_RATE * dt));
        float maxStep = speed_ * dt;
        current_ += constrain(step, -maxStep, maxStep);
    }
    write();
}

void ServoChannel::attach() {
    attached_ = true;
    lastDuty_ = UINT32_MAX;
    write();
}

void ServoChannel::detach() {
    attached_ = false;
    // A 0% duty cycle sends no pulses, so the servo stops holding and goes quiet.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(pin_, 0);
#else
    ledcWrite(channel_, 0);
#endif
    lastDuty_ = 0;
}

void ServoChannel::write() {
    if (!attached_) return;
    uint32_t duty = lroundf(current_ / PERIOD_US * ((1 << PWM_BITS) - 1));
    if (duty == lastDuty_) return;
    lastDuty_ = duty;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(pin_, duty);
#else
    ledcWrite(channel_, duty);
#endif
}
