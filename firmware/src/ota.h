#pragma once

#include <ESPAsyncWebServer.h>

#include "eye.h"

// Firmware updates over HTTP (POST /api/update, multipart field "firmware"), with automatic
// rollback if a new firmware restarts before it has proved itself.
namespace ota {

void begin(Eye &eye);  // call early in setup(): logs the version and any rollback
void addRoutes(AsyncWebServer &server);
void loop(bool online);  // marks new firmware good once online (or stable for a while)

}  // namespace ota
