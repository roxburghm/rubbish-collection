#pragma once

#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include "LGFX_Setup.hpp"

class WifiHandler {
public:
    WifiHandler(LGFX* display);
    void begin();
    void loop();
    bool isConnected();
    String getIpAddress();

private:
    LGFX* _display;
    WiFiManager _wifiManager;
    
    void _onAPCallback(WiFiManager *myWiFiManager);
};
