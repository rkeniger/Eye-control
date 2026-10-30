#include <Arduino.h>

#include "eye.h"
#include "log.h"
#include "net.h"
#include "ota.h"

Eye eye;

void setup() {
    Serial.begin(115200);
    logbuf::begin();
    ota::begin(eye);
    eye.begin();
    net::begin(eye);  // joins Wi-Fi (or starts the setup portal) before the servos draw power
    eye.attachServos();
}

void loop() {
    eye.tick(millis());
    net::loop();
    delay(10);
}
