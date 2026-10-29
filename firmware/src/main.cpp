#include <Arduino.h>
#include <LittleFS.h>

#include "eye.h"
#include "log.h"
#include "net.h"

Eye eye;

void setup() {
    Serial.begin(115200);
    logbuf::begin();
    if (!LittleFS.begin(true)) Log.println("LittleFS mount failed");
    eye.begin();
    net::begin(eye);  // joins Wi-Fi (or starts the setup portal) before the servos draw power
    eye.attachServos();
}

void loop() {
    eye.tick(millis());
    net::loop();
    delay(10);
}
