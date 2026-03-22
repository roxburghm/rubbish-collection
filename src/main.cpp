#include <Arduino.h>
#include <Wire.h>
#include "LGFX_Setup.hpp"
#include "NetworkManager.h"
#include "ConfigManager.h"
#include "WebServerHandler.h"
#include "CouncilClient.h"
#include "SchedulingManager.h"

#define CST816_ADDR  0x15
#define TCA9554_ADDR 0x20
#define I2C_SDA      18
#define I2C_SCL      8

void touchTask(void* param);

LGFX display;
WifiHandler networkManager(&display);
ConfigManager configManager;
ZhsunycoMaster zhsunycoMaster;
CouncilClient councilClient;
SchedulingManager scheduler(&councilClient, &zhsunycoMaster, &configManager, &display);
WebServerHandler webServer(&configManager, &zhsunycoMaster, &scheduler, &councilClient);

void setup() {
    Serial.begin(115200);
    delay(3000); // Wait for USB Serial/JTAG to enumerate

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

    // Init Network (before BLE - they share the radio, BLE init can interfere)
    networkManager.begin();

    // Init BLE (after WiFi is established)
    zhsunycoMaster.begin();

    // Init I2C bus (TCA9554 + CST816 share the bus)
    Wire.begin(I2C_SDA, I2C_SCL);

    // Init TCA9554 GPIO expander — release CST816 reset
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03); // Config register
    Wire.write(0x00); // All pins as output
    uint8_t tcaResult = Wire.endTransmission();
    Serial.printf("Touch: TCA9554 config -> %s\n", tcaResult == 0 ? "OK" : "NOT FOUND");

    if (tcaResult == 0) {
        Wire.beginTransmission(TCA9554_ADDR);
        Wire.write(0x01); // Output register
        Wire.write(0xFF); // All high (release resets)
        Wire.endTransmission();
        delay(100); // Let CST816 boot after reset release
    }

    // Check touch controller and disable auto-sleep
    Wire.beginTransmission(CST816_ADDR);
    if (Wire.endTransmission() == 0) {
        Serial.println("Touch: CST816 FOUND");
        // Disable auto-sleep so polling works reliably
        Wire.beginTransmission(CST816_ADDR);
        Wire.write(0xFE); // Auto-sleep control register
        Wire.write(0x00); // Disable auto-sleep
        Wire.endTransmission();
    } else {
        Serial.println("Touch: CST816 NOT FOUND");
    }

    // Init Web Server
    webServer.begin();

    // Start touch task — runs independently so backlight works during BLE ops
    xTaskCreate(touchTask, "Touch", 2048, &display, 1, NULL);

    Serial.println("System Ready.");
}

// Separate FreeRTOS task for touch — runs independently of BLE blocking
void touchTask(void* param) {
    LGFX* disp = (LGFX*)param;
    bool backlightOn = true;
    unsigned long lastOnTime = millis();
    bool wasTouched = false;

    while (true) {
        // Read touch via I2C (only this task uses Wire after setup)
        Wire.beginTransmission(CST816_ADDR);
        Wire.write(0x01);
        bool ok = (Wire.endTransmission() == 0);
        bool touched = false;
        if (ok) {
            Wire.requestFrom((uint8_t)CST816_ADDR, (uint8_t)6);
            if (Wire.available() >= 6) {
                Wire.read(); // gesture
                uint8_t points = Wire.read();
                Wire.read(); Wire.read(); Wire.read(); Wire.read();
                touched = points > 0;
            }
        }

        if (touched && !wasTouched) {
            Serial.println("Touch: tap detected");
            backlightOn = !backlightOn;
            disp->setBrightness(backlightOn ? 255 : 0);
            if (backlightOn) lastOnTime = millis();
        }
        wasTouched = touched;

        if (backlightOn && millis() - lastOnTime > 60000) {
            disp->setBrightness(0);
            backlightOn = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void loop() {
    networkManager.loop();
    scheduler.loop();
    delay(100);
}
