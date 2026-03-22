#include "WebServerHandler.h"
#include <LittleFS.h>

WebServerHandler::WebServerHandler(ConfigManager* config, ZhsunycoMaster* bleCaller, SchedulingManager* scheduler, CouncilClient* api)
    : _server(80), _config(config), _ble(bleCaller), _scheduler(scheduler), _api(api) {
}

void WebServerHandler::begin() {
    if(!LittleFS.begin(true)){
        Serial.println("An Error has occurred while mounting SPIFFS");
        return;
    }

    _setupRoutes();
    _server.begin();
    Serial.println("Web Server Started");
}

void WebServerHandler::_setupRoutes() {
    // Serve Static Files
    _server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Silence Favicon
    _server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(204);
    });

    // Version API
    _server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request){
        String v = String(__DATE__) + " " + String(__TIME__);
        JsonDocument doc;
        doc["version"] = v;
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // API: Config GET
    _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["uprn"] = _config->getUPRN();
        doc["propertyName"] = _config->getPropertyName();
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // API: Config POST
    _server.on("/api/config", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (request->hasParam("uprn", true)) {
            _config->setUPRN(request->getParam("uprn", true)->value());
        }
        if (request->hasParam("propertyName", true)) {
            _config->setPropertyName(request->getParam("propertyName", true)->value());
        }
        request->send(200, "text/plain", "OK");
    });

    // API: List Devices (Configured + Status)
    _server.on("/api/devices", HTTP_GET, [this](AsyncWebServerRequest *request){
         String json = _scheduler->getDeviceStatusJson();
         request->send(200, "application/json", json);
    });

    // API: Update Device Config (Rotation)
    _server.on("/api/device/config", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){

         JsonDocument doc;
         DeserializationError error = deserializeJson(doc, data, len);

         if (error) {
             request->send(400, "text/plain", "Invalid JSON");
             return;
         }

         String mac = doc["mac"];
         bool rotate = doc["rotate"];

         if(mac.length() > 0) {
             _config->setDeviceRotation(mac, rotate);
             _scheduler->requestTriggerDevice(mac);
             request->send(200, "text/plain", "OK");
         } else {
             request->send(400, "text/plain", "Bad Request");
         }
    });

    // API: Add Device
    _server.on("/api/devices", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (request->hasParam("mac", true)) {
            String mac = request->getParam("mac", true)->value();
            _config->addDevice(mac);
            _scheduler->requestTriggerDevice(mac);
            request->send(200, "text/plain", "Added");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });

    // API: Remove Device
    _server.on("/api/devices", HTTP_DELETE, [this](AsyncWebServerRequest *request){
        String mac = "";
        if(request->hasParam("mac", true)) mac = request->getParam("mac", true)->value();
        else if(request->hasParam("mac")) mac = request->getParam("mac")->value();

        if (mac.length() > 0) {
            _config->removeDevice(mac);
            _scheduler->requestRemoveDevice(mac);
            request->send(200, "text/plain", "Removed");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });

    // API: BLE Scan
    _server.on("/api/ble/scan", HTTP_GET, [this](AsyncWebServerRequest *request){
         if (!_ble) {
             request->send(500, "text/plain", "BLE Module Error");
             return;
         }
         std::vector<String> devices = _ble->scanTags(3);
         JsonDocument doc;
         JsonArray arr = doc.to<JsonArray>();
         for(size_t i=0; i<devices.size(); i++) {
             arr.add(devices[i]);
         }
         String json;
         serializeJson(doc, json);
         request->send(200, "application/json", json);
    });

    // API: Postcode Lookup Proxy
    _server.on("/api/lookup/postcode", HTTP_GET, [this](AsyncWebServerRequest *request){
       if (request->hasParam("postcode")) {
           String pc = request->getParam("postcode")->value();
           JsonDocument doc = _api->searchAddress(pc);
           String json;
           serializeJson(doc, json);
           request->send(200, "application/json", json);
       } else {
           request->send(400, "text/plain", "Missing postcode param");
       }
    });

    // API: Refresh Data (non-blocking, queued for main loop)
    _server.on("/api/refresh", HTTP_POST, [this](AsyncWebServerRequest *request){
        _scheduler->requestForceUpdate();
        request->send(200, "text/plain", "OK");
    });

    // API: Status (Timestamps + WiFi)
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        time_t last = _scheduler->getLastUpdateEpoch();

        JsonDocument doc;
        doc["lastUpdate"] = (unsigned long long)last * 1000;
        doc["nextUpdate"] = (unsigned long long)_scheduler->getNextScheduledEpoch() * 1000;
        doc["rssi"] = WiFi.RSSI();
        doc["ssid"] = WiFi.SSID();

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // API: Test Device (non-blocking, queued for main loop)
    _server.on("/api/test", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (request->hasParam("mac", true)) {
            String mac = request->getParam("mac", true)->value();
            _scheduler->requestTestPattern(mac);
            request->send(200, "text/plain", "Test Queued");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });

    // API: Schedule Data
    _server.on("/api/schedule", HTTP_GET, [this](AsyncWebServerRequest *request){
         JsonDocument doc = _scheduler->getLastSchedule();
         String json;
         serializeJson(doc, json);
         request->send(200, "application/json", json);
    });
}
