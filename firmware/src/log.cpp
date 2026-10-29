#include "log.h"

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/semphr.h>

#include "config.h"

Logger Log;

namespace logbuf {

namespace {

char ring[LOG_BUFFER_SIZE];
uint32_t total = 0;  // bytes ever written; ring position is total % LOG_BUFFER_SIZE
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t idfVprintf = nullptr;
uint32_t boot = 0;

// Safe from any task or ISR (the ROM putc hook can be called from either).
void append(const char *data, size_t len) {
    portENTER_CRITICAL_SAFE(&mux);
    for (size_t i = 0; i < len; i++) ring[total++ % LOG_BUFFER_SIZE] = data[i];
    portEXIT_CRITICAL_SAFE(&mux);
}

// Arduino core log_x() output goes through ets_printf, which writes to every installed ROM channel.
void romPutc(char c) { append(&c, 1); }

int idfLog(const char *format, va_list args) {
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (n > 0) append(line, min((size_t)n, sizeof(line) - 1));
    return idfVprintf ? idfVprintf(format, args) : n;
}

const char *resetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "crash (panic)";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT: return "BROWNOUT (supply voltage dipped)";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown";
    }
}

}  // namespace

void begin() {
    boot = esp_random();
    esp_rom_install_channel_putc(2, romPutc);
    idfVprintf = esp_log_set_vprintf(idfLog);
    Log.printf("Boot: reset reason: %s\n", resetReason());
}

uint32_t bootId() { return boot; }

Chunk read(uint32_t since, char *out, size_t cap) {
    portENTER_CRITICAL(&mux);
    uint32_t oldest = total > LOG_BUFFER_SIZE ? total - LOG_BUFFER_SIZE : 0;
    // `since` beyond the end means the client saw an earlier boot; start again from the oldest.
    uint32_t start = since < oldest || since > total ? oldest : since;
    uint32_t end = start + min((uint32_t)cap, total - start);
    for (uint32_t i = start; i < end; i++) out[i - start] = ring[i % LOG_BUFFER_SIZE];
    portEXIT_CRITICAL(&mux);
    return {start, end};
}

}  // namespace logbuf

size_t Logger::write(const uint8_t *data, size_t len) {
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();  // keeps lines from different tasks whole
    xSemaphoreTake(lock, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        if (atLineStart_) {
            char stamp[16];
            int n = snprintf(stamp, sizeof(stamp), "[%6lu] ", (unsigned long)millis());
            Serial.write(stamp, n);
            logbuf::append(stamp, n);
            atLineStart_ = false;
        }
        Serial.write(data[i]);
        logbuf::append(reinterpret_cast<const char *>(data + i), 1);
        if (data[i] == '\n') atLineStart_ = true;
    }
    xSemaphoreGive(lock);
    return len;
}
