#pragma once
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "ConfigManager.h"
#include "ZhsunycoMaster.h"
#include "SchedulingManager.h"
#include "CouncilClient.h"

class WebServerHandler {
public:
    WebServerHandler(ConfigManager* config, ZhsunycoMaster* bleCaller, SchedulingManager* scheduler, CouncilClient* api);
    void begin();

private:
    AsyncWebServer _server;
    ConfigManager* _config;
    ZhsunycoMaster* _ble;
    SchedulingManager* _scheduler;
    CouncilClient* _api;
    
    void _setupRoutes();
};
