#pragma once

#include <Arduino.h>

// Everything printed since boot (our messages plus Arduino core and ESP-IDF logs), kept in a RAM
// ring buffer so the web UI can show it.
namespace logbuf {

void begin();  // call first in setup(): hooks core logging and logs the reset reason

uint32_t bootId();  // random per boot, so a page can tell the board restarted

struct Chunk {
    uint32_t start;  // running byte offset of the first returned byte
    uint32_t end;    // running byte offset after the last one; pass back as `since`
};
// Copies up to `cap` bytes from offset `since` (or the oldest byte still held) into `out`.
Chunk read(uint32_t since, char *out, size_t cap);

}  // namespace logbuf

// Prints to Serial and the log buffer, prefixing each line with the uptime in ms.
class Logger : public Print {
   public:
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *data, size_t len) override;

   private:
    bool atLineStart_ = true;
};

extern Logger Log;
