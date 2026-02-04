#include <Arduino.h>
#include "LGFX_Setup.hpp"
#include "NetworkManager.h"
#include "ConfigManager.h"
#include "WebServerHandler.h"
#include "CouncilClient.h"
#include "SchedulingManager.h"

LGFX display;
NetworkManager networkManager(&display);
ConfigManager configManager;
ZhsunycoMaster zhsunycoMaster;
CouncilClient councilClient;
SchedulingManager scheduler(&councilClient, &zhsunycoMaster, &configManager, &display);
WebServerHandler webServer(&configManager, &zhsunycoMaster, &scheduler, &councilClient);

void setup() {
    Serial.begin(115200);
    delay(1000); 
    
    // Init Display
    display.init();
    display.setRotation(3); // Landscape (USB left/right depending on mount)
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.println("Booting...");

    // STARTUP VERSION LOG
    Serial.println("\n\n=============================================");
    Serial.print("   RUBBISH COLLECTION SERVER\n");
    Serial.print("   BUILD TIMESTAMP: ");
    Serial.println(__DATE__ " " __TIME__);
    Serial.println("=============================================\n");

    // Init Config
    configManager.begin();
    

    
    // Init BLE
    zhsunycoMaster.begin();

    // Init Network
    networkManager.begin();
    
    // Init Web Server
    webServer.begin();
    
    Serial.println("System Ready.");
}

void loop() {
    networkManager.loop();
    scheduler.loop();
    delay(100);
}
