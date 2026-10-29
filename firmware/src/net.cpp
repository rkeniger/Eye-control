#include "net.h"

#include <AsyncJson.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "config.h"
#include "log.h"

namespace net {

namespace {

constexpr uint32_t STATUS_INTERVAL_MS = 200;

Eye *eye = nullptr;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
bool setupMode = false;
String scannedNetworks;  // <option> list for the setup page
uint32_t restartAt = 0;
uint32_t lastStatus = 0;
uint32_t lastCleanup = 0;
bool serverRunning = false;
int ledOnline = -1;  // last colour shown: 1 green, 0 red, -1 not yet set

const char SETUP_PAGE[] PROGMEM = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Eyeball Setup</title>
<style>
body{font:16px system-ui,sans-serif;max-width:420px;margin:40px auto;padding:0 16px;background:#f4f4f2;color:#1d1d1b}
@media (prefers-color-scheme:dark){body{background:#121211;color:#ecebe6}input{background:#1b1b1a;color:#ecebe6;border-color:#2e2e2c}}
label{display:block;margin:16px 0 4px}input{width:100%;padding:10px;font:inherit;border:1px solid #deddd8;border-radius:6px;box-sizing:border-box}
button{margin-top:20px;width:100%;padding:12px;font:inherit;font-weight:600;border:0;border-radius:6px;background:#2563eb;color:#fff}
</style></head><body>
<h1>Eyeball Wi-Fi setup</h1>
<form method="post" action="/api/wifi">
<label for="ssid">Network</label><input id="ssid" name="ssid" list="nets" required autocomplete="off">
<datalist id="nets">%NETWORKS%</datalist>
<label for="pass">Password</label><input id="pass" name="pass" type="password">
<button>Save and restart</button>
</form></body></html>)html";

void sendStatus(AsyncWebServerRequest *request) {
    JsonDocument doc;
    eye->status(doc.to<JsonObject>());
    String body;
    serializeJson(doc, body);
    request->send(200, "application/json", body);
}

void sendError(AsyncWebServerRequest *request, const char *message) {
    String body = String("{\"error\":\"") + message + "\"}";
    request->send(400, "application/json", body);
}

// Tracker command, shared by POST /api/eye and the WebSocket:
// {"yaw":deg,"pitch":deg[,"newTarget":true]} | {"present":false}
void handleEyeCommand(JsonVariantConst cmd) {
    if (cmd["newTarget"] | false) eye->newTarget();
    if (cmd["present"].is<bool>() && !cmd["present"].as<bool>()) {
        eye->absent();
    } else if (cmd["yaw"].is<float>() && cmd["pitch"].is<float>()) {
        eye->track(cmd["yaw"].as<float>(), cmd["pitch"].as<float>());
    }
}

void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) Log.printf("WebSocket #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    if (type == WS_EVT_DISCONNECT) Log.printf("WebSocket #%u disconnected\n", client->id());
    if (type != WS_EVT_DATA) return;
    auto *info = static_cast<AwsFrameInfo *>(arg);
    // Commands are small; only accept complete single-frame text messages.
    if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) return;
    JsonDocument doc;
    if (deserializeJson(doc, data, len)) return;
    handleEyeCommand(doc.as<JsonVariantConst>());
}

void broadcastStatus() {
    if (!ws.count()) return;
    JsonDocument doc;
    JsonObject status = doc.to<JsonObject>();
    status["type"] = "status";
    eye->status(status);
    String body;
    serializeJson(doc, body);
    ws.textAll(body);
}

bool readServoCal(JsonVariantConst in, ServoCal &out) {
    if (!in["minUs"].is<int>() || !in["centerUs"].is<int>() || !in["maxUs"].is<int>()) return false;
    out.minUs = in["minUs"];
    out.centerUs = in["centerUs"];
    out.maxUs = in["maxUs"];
    out.invert = in["invert"] | false;
    return true;
}

void writeServoCal(JsonObject out, const ServoCal &cal) {
    out["minUs"] = cal.minUs;
    out["centerUs"] = cal.centerUs;
    out["maxUs"] = cal.maxUs;
    out["invert"] = cal.invert;
}

void addJsonRoute(const char *path, std::function<void(AsyncWebServerRequest *, JsonVariant &)> fn) {
    auto *handler = new AsyncCallbackJsonWebHandler(path, fn);
    handler->setMethod(HTTP_POST);
    server.addHandler(handler);
}

void setupApiRoutes() {
    server.on("/api/status", HTTP_GET, sendStatus);
    // Log text from byte offset `since`; X-Log-Start/X-Log-End say which range was returned,
    // X-Log-Boot changes when the board restarts.
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        uint32_t since = request->hasParam("since") ? request->getParam("since")->value().toInt() : 0;
        std::unique_ptr<char[]> text(new char[LOG_BUFFER_SIZE]);
        logbuf::Chunk chunk = logbuf::read(since, text.get(), LOG_BUFFER_SIZE);
        AsyncResponseStream *response = request->beginResponseStream("text/plain; charset=utf-8");
        response->addHeader("X-Log-Boot", String(logbuf::bootId()));
        response->addHeader("X-Log-Start", String(chunk.start));
        response->addHeader("X-Log-End", String(chunk.end));
        response->addHeader("Cache-Control", "no-store");
        response->write(reinterpret_cast<const uint8_t *>(text.get()), chunk.end - chunk.start);
        request->send(response);
    });

    addJsonRoute("/api/eye", [](AsyncWebServerRequest *request, JsonVariant &json) {
        handleEyeCommand(json);
        sendStatus(request);
    });

    // {"yaw":deg,"pitch":deg,"lid":0..1} (any subset) or {"servo":"yaw|pitch|lid","us":1500}
    addJsonRoute("/api/manual", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (json["servo"].is<const char *>() && json["us"].is<float>()) {
            String name = json["servo"].as<const char *>();
            Servo servo;
            if (name == "yaw") servo = Servo::Yaw;
            else if (name == "pitch") servo = Servo::Pitch;
            else if (name == "lid") servo = Servo::Lid;
            else return sendError(request, "servo must be yaw, pitch or lid");
            eye->manualUs(servo, json["us"].as<float>());
        } else {
            float yaw = json["yaw"] | 0.0f, pitch = json["pitch"] | 0.0f, lid = json["lid"] | 1.0f;
            eye->manual(json["yaw"].is<float>() ? &yaw : nullptr, json["pitch"].is<float>() ? &pitch : nullptr,
                        json["lid"].is<float>() ? &lid : nullptr);
        }
        sendStatus(request);
    });

    addJsonRoute("/api/mode", [](AsyncWebServerRequest *request, JsonVariant &json) {
        String mode = json["mode"] | "";
        if (mode == "auto") eye->setMode(Mode::Auto);
        else if (mode == "manual") eye->setMode(Mode::Manual);
        else return sendError(request, "mode must be auto or manual");
        sendStatus(request);
    });

    server.on("/api/blink", HTTP_POST, [](AsyncWebServerRequest *request) {
        eye->blink();
        sendStatus(request);
    });

    server.on("/api/calibration", HTTP_GET, [](AsyncWebServerRequest *request) {
        Calibration cal = eye->calibration();
        JsonDocument doc;
        writeServoCal(doc["yaw"].to<JsonObject>(), cal.yaw);
        writeServoCal(doc["pitch"].to<JsonObject>(), cal.pitch);
        doc["lid"]["closedUs"] = cal.lid.closedUs;
        doc["lid"]["openUs"] = cal.lid.openUs;
        String body;
        serializeJson(doc, body);
        request->send(200, "application/json", body);
    });

    addJsonRoute("/api/calibration", [](AsyncWebServerRequest *request, JsonVariant &json) {
        Calibration cal;
        bool ok = readServoCal(json["yaw"], cal.yaw) && readServoCal(json["pitch"], cal.pitch) &&
                  json["lid"]["closedUs"].is<int>() && json["lid"]["openUs"].is<int>();
        if (ok) {
            cal.lid.closedUs = json["lid"]["closedUs"];
            cal.lid.openUs = json["lid"]["openUs"];
        }
        if (!ok || !eye->setCalibration(cal)) {
            return sendError(request, "invalid calibration: need min <= centre <= max within 500-2500 us");
        }
        request->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/wifi/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
        settings::clearWifi();
        request->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
        restartAt = millis() + 1000;
    });
}

void startSetupPortal() {
    setupMode = true;
    // Scan before starting the access point so the setup page can suggest networks.
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "&quot;");
        if (ssid.length()) scannedNetworks += "<option value=\"" + ssid + "\">";
    }
    WiFi.scanDelete();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_SSID);
    dns.start(53, "*", WiFi.softAPIP());
    Log.printf("Setup portal: join '%s' and open http://%s\n", SETUP_AP_SSID,
                  WiFi.softAPIP().toString().c_str());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        String page = SETUP_PAGE;
        page.replace("%NETWORKS%", scannedNetworks);
        request->send(200, "text/html", page);
    });
    server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("ssid", true)) return sendError(request, "ssid required");
        String ssid = request->getParam("ssid", true)->value();
        String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";
        settings::saveWifi(ssid, pass);
        request->send(200, "text/html",
                      "<!doctype html><meta name=viewport content='width=device-width'>"
                      "<p style='font:16px system-ui;margin:40px'>Saved. Restarting and joining <b>" +
                          ssid + "</b>. Then open <b>http://eyeball.local</b>.</p>");
        restartAt = millis() + 1500;
    });
    // Captive portal: send every unknown URL (OS connectivity checks included) to the setup page.
    server.onNotFound([](AsyncWebServerRequest *request) { request->redirect("http://" + WiFi.softAPIP().toString() + "/"); });
}

void showStatusLed(bool online) {
    if (online == ledOnline) return;
    ledOnline = online;
    if (online) neopixelWrite(PIN_STATUS_LED, 0, STATUS_LED_BRIGHTNESS, 0);
    else neopixelWrite(PIN_STATUS_LED, STATUS_LED_BRIGHTNESS, 0, 0);
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        Log.printf("Wi-Fi disconnected (reason %u)\n", info.wifi_sta_disconnected.reason);
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
        Log.printf("Wi-Fi got IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

bool joinWifi() {
    String ssid, pass;
    if (!settings::loadWifi(ssid, pass)) {
        Log.println("No saved Wi-Fi");
        return false;
    }
    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setSleep(false);  // modem sleep adds 100+ ms latency spikes
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Log.printf("Joining '%s'\n", ssid.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) delay(250);
    if (WiFi.status() == WL_CONNECTED) return true;
    Log.printf("Couldn't join '%s' within %lu s\n", ssid.c_str(), (unsigned long)(WIFI_CONNECT_TIMEOUT_MS / 1000));
    return false;
}

}  // namespace

void begin(Eye &e) {
    eye = &e;
    showStatusLed(false);
    // API and WebSocket first so the static file handler never shadows them.
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    setupApiRoutes();

    if (joinWifi()) {
        Log.printf("Connected: http://%s.local (%s)\n", HOSTNAME, WiFi.localIP().toString().c_str());
        if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
        server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain", "Not found"); });
    } else {
        startSetupPortal();
    }
    server.begin();
    serverRunning = true;
}

void loop() {
    uint32_t now = millis();
    showStatusLed(serverRunning && !setupMode && WiFi.status() == WL_CONNECTED);
    if (setupMode) dns.processNextRequest();
    if (restartAt && (int32_t)(now - restartAt) >= 0) ESP.restart();
    if (now - lastStatus >= STATUS_INTERVAL_MS) {
        lastStatus = now;
        broadcastStatus();
    }
    if (now - lastCleanup >= 1000) {
        lastCleanup = now;
        ws.cleanupClients();
    }
}

}  // namespace net
