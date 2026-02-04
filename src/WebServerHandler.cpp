#include "WebServerHandler.h"
#include <LittleFS.h>
#include "CouncilClient.h" // Needed for searchAddress but we are calling it from here? No, we need CouncilClient access. 
// Actually web server should have access to CouncilClient OR SchedulingManager can expose search?
// Let's pass CouncilClient to WebServerHandler too or keep it simple.
// The task said "Postcode Lookup API proxy".
// Let's give WebServer access to CouncilClient. 
// Wait, main.cpp has all of them. 

// Let's modify WebServerHandler.h to include CouncilClient.
#include "CouncilClient.h"

WebServerHandler::WebServerHandler(ConfigManager* config, ZhsunycoMaster* bleCaller, SchedulingManager* scheduler, CouncilClient* api) 
    : _server(80), _config(config), _ble(bleCaller), _scheduler(scheduler), _api(api) {
}

void WebServerHandler::begin() {
    // Initialize File System
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
        request->send(200, "application/json", "{\"version\":\"" + v + "\"}");
    });

    // API: Config
    _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request){
        request->send(200, "application/json", "{\"uprn\":\"" + _config->getUPRN() + "\"}");
    });
    
    _server.on("/api/config", HTTP_POST, [this](AsyncWebServerRequest *request){
        // Basic Param handling
        if (request->hasParam("uprn", true)) {
            _config->setUPRN(request->getParam("uprn", true)->value());
        }
         request->send(200, "text/plain", "OK");
    });
    
    // API: List Devices (Configured + Status)
    _server.on("/api/devices", HTTP_GET, [this](AsyncWebServerRequest *request){
         // Updated to return objects with status
         String json = _scheduler->getDeviceStatusJson(); 
         request->send(200, "application/json", json);
    });
    
    // API: Add Device
    _server.on("/api/devices", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (request->hasParam("mac", true)) {
            String mac = request->getParam("mac", true)->value();
            _config->addDevice(mac);
            request->send(200, "text/plain", "Added");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });
    
    // API: Remove Device
    _server.on("/api/devices", HTTP_DELETE, [this](AsyncWebServerRequest *request){
        if (request->hasParam("mac", true)) { // DELETE params usually in query, not body for simple requests, or body. 
            // AsyncWebServer handles URL params easily.
            // Let's assume query string for DELETE: /api/devices?mac=XX
            String mac = request->getParam("mac", true)->value(); // true = POST/BODY? No, DELETE might be query. 
            // Let's check query params too.
            // if "true" is passed, it checks body. We want URL param.
            // request->getParam("mac") checks all.
        }
        
        String mac = "";
        if(request->hasParam("mac", true)) mac = request->getParam("mac", true)->value();
        else if(request->hasParam("mac")) mac = request->getParam("mac")->value();
        
        if (mac.length() > 0) {
            _config->removeDevice(mac);
            request->send(200, "text/plain", "Removed");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });

    // API: BLE Scan (Optional Context for manual entry assistance?)
    // Keeping it but devices should now be managed manually as requested.
    _server.on("/api/ble/scan", HTTP_GET, [this](AsyncWebServerRequest *request){
         if (!_ble) {
             request->send(500, "text/plain", "BLE Module Error");
             return;
         }
         std::vector<String> devices = _ble->scanTags(3); // 3 seconds scan
         String json = "[";
         for(size_t i=0; i<devices.size(); i++) {
             json += "\"" + devices[i] + "\"";
             if(i < devices.size()-1) json += ",";
         }
         json += "]";
         request->send(200, "application/json", json);
    });
    
    // API: Proxy Text (Postcode Lookup)
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
    
    // API: Refresh Data
    _server.on("/api/refresh", HTTP_POST, [this](AsyncWebServerRequest *request){
        _scheduler->forceUpdate();
        request->send(200, "text/plain", "OK");
    });
    
    // API: Status (Timestamps + WiFi)
    _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        // We need methods in Scheduler to get these.
        time_t last = _scheduler->getLastUpdateEpoch(); 
        
        // Return JSON
        String json = "{";
        json += "\"lastUpdate\":" + String((uint32_t)last) + "000,"; 
        json += "\"nextUpdate\":" + String((uint32_t)last + (6*60*60)) + "000,";
        
        // WiFi Status
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"ssid\":\"" + WiFi.SSID() + "\"";
        
        json += "}";
        request->send(200, "application/json", json);
    });
    
    // API: Test Device
    _server.on("/api/test", HTTP_POST, [this](AsyncWebServerRequest *request){
        if (request->hasParam("mac", true)) {
            String mac = request->getParam("mac", true)->value();
            _scheduler->sendTestPattern(mac);
            request->send(200, "text/plain", "Test Sent");
        } else {
            request->send(400, "text/plain", "Missing mac");
        }
    });
    

    _server.on("/api/schedule", HTTP_GET, [this](AsyncWebServerRequest *request){
         JsonDocument doc = _scheduler->getLastSchedule();
         String json;
         serializeJson(doc, json);
         request->send(200, "application/json", json);
    });
}
