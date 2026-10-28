#pragma once

#include "eye.h"

namespace net {

// Joins saved Wi-Fi (or opens the Eyeball-Setup portal) and starts the HTTP/WebSocket server.
void begin(Eye &eye);
void loop();

}  // namespace net
