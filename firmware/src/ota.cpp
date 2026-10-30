#include "ota.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>

#include "config.h"
#include "log.h"
#include "version_info.h"

// Keep new firmware on probation: by default the core marks it good as soon as it boots. We do that
// ourselves once it has proved itself, and the bootloader rolls back if it restarts before then.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {

namespace {

Eye *eye = nullptr;
bool onProbation = false;
String uploadError;  // problem with the upload in progress, empty if none
uint32_t restartAt = 0;

void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc) {
    String body;
    serializeJson(doc, body);
    request->send(code, "application/json", body);
}

void abortUpload(const char *why) {
    if (Update.isRunning()) Update.abort();
    Log.printf("Update failed: %s\n", why);
    eye->resume();
}

void onUploadData(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len,
                  bool final) {
    if (index == 0) {
        if (Update.isRunning()) Update.abort();  // a previous upload that never finished
        uploadError = "";
        Log.printf("Update: receiving %s\n", filename.c_str());
        eye->suspend();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) uploadError = Update.errorString();
        request->onDisconnect([] {
            if (Update.isRunning()) abortUpload("upload interrupted");
        });
    }
    if (!uploadError.isEmpty()) return;
    if (len && Update.write(data, len) != len) {
        uploadError = Update.errorString();
    } else if (final) {
        // end() checks the image (magic byte, checksum, chip) before switching the boot slot to it.
        if (Update.end(true)) Log.printf("Update: wrote %u bytes\n", (unsigned)(index + len));
        else uploadError = Update.errorString();
    }
}

void onUploadDone(AsyncWebServerRequest *request) {
    JsonDocument doc;
    if (uploadError.isEmpty() && Update.isFinished()) {
        Log.println("Update: installed, restarting");
        doc["ok"] = true;
        doc["restarting"] = true;
        sendJson(request, 200, doc);
        restartAt = millis() + 1000;
        return;
    }
    String error = uploadError.isEmpty() ? String("no firmware file received") : uploadError;
    abortUpload(error.c_str());
    doc["error"] = error;
    sendJson(request, 400, doc);
}

}  // namespace

void begin(Eye &e) {
    eye = &e;
    Log.printf("Firmware %s, built %s\n", FIRMWARE_VERSION, FIRMWARE_BUILT);
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    onProbation = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    if (onProbation) {
        Log.printf("New firmware in %s: kept once online or after %lu s; a restart before then rolls it back\n",
                   running->label, (unsigned long)(OTA_PROBATION_MS / 1000));
    }
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid) Log.printf("The last update (in %s) failed to start and was rolled back\n", invalid->label);
}

void addRoutes(AsyncWebServer &server) { server.on("/api/update", HTTP_POST, onUploadDone, onUploadData); }

void loop(bool online) {
    if (onProbation && (online || millis() > OTA_PROBATION_MS)) {
        onProbation = false;
        esp_ota_mark_app_valid_cancel_rollback();
        Log.printf("Firmware %s marked good (%s)\n", FIRMWARE_VERSION, online ? "online" : "stable");
    }
    if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
}

}  // namespace ota
