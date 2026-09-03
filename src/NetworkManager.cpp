#include "NetworkManager.h"
#include <ESPmDNS.h>
#include <WiFi.h>

WifiHandler::WifiHandler(LGFX *display) : _display(display) {}

void WifiHandler::_onAPCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entering AP Mode");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.println(WiFi.softAPIP());

  if (_display) {
    _display->fillScreen(TFT_BLACK);

    // Landscape Setup Screen
    // Left: Instructions
    // Right: QR

    _display->setCursor(10, 15);
    _display->setTextColor(TFT_MAGENTA);
    _display->setTextSize(2);
    _display->println("SETUP MODE");

    _display->setCursor(10, 50);
    _display->setTextColor(TFT_WHITE);
    _display->setTextSize(1);
    _display->println("Connect to WiFi:");

    _display->setCursor(10, 65);
    _display->setTextColor(TFT_ORANGE);
    _display->setTextSize(2);
    _display->println(myWiFiManager->getConfigPortalSSID());

    _display->setCursor(10, 95);
    _display->setTextColor(TFT_WHITE);
    _display->setTextSize(1);
    _display->println("Password:");

    _display->setCursor(10, 110);
    _display->setTextColor(TFT_ORANGE);
    _display->setTextSize(2);
    _display->println(myWiFiManager->getConfigPortalSSID());

    // QR Code right
    // Payload: Wifi details
    String qrText = "WIFI:T:WPA;S:" + myWiFiManager->getConfigPortalSSID() +
                    ";P:" + myWiFiManager->getConfigPortalSSID() + ";;";
    _display->qrcode(qrText, 190, 25, 120, 6);

    _display->drawFastVLine(180, 10, 150, 0x4208);
  }
}

void WifiHandler::begin() {
  // Generate random ID for RubbishXXXX
  uint32_t chipId = 0;
  for (int i = 0; i < 17; i = i + 8) {
    chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
  }
  String ssid = "Rubbish" + String((uint16_t)chipId);

  _wifiManager.setAPCallback(
      [this](WiFiManager *wm) { this->_onAPCallback(wm); });
  _wifiManager.setConnectTimeout(30);
  _wifiManager.setConnectRetries(3);

  if (!_wifiManager.autoConnect(ssid.c_str(), ssid.c_str())) {
    Serial.println("Failed to connect and hit timeout");
    // Reset or sleep?
    ESP.restart();
  }

  // If we get here, we are connected
  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());

  // Init mDNS
  if (MDNS.begin("rubbish")) {
    Serial.println("mDNS responder started: http://rubbish.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error setting up MDNS responder!");
  }

  // Init NTP (UK Time - Europe/London)
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.nist.gov");

  // Wait for time?
  struct tm timeinfo;
  Serial.print("Syncing Time");
  int retry = 0;
  while (!getLocalTime(&timeinfo, 1000) && retry < 10) {
    Serial.print(".");
    retry++;
  }
  Serial.println("");

  if (retry >= 10) {
    Serial.println("Time sync failed (timeout)");
  } else {
    Serial.println("Time Synced: " + String(asctime(&timeinfo)));
  }

  // No status splash here. Everything it showed — SSID, address, the manage
  // QR — is the Network screen, one tap away, and drawn properly. The panel
  // now belongs to SchedulingManager from this point on; it paints the
  // console as soon as loop() runs.
}

void WifiHandler::loop() {
  // WiFi Auto-Reconnect
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 15000) {
      lastReconnectAttempt = millis();
      Serial.println(
          "NetworkManager: WiFi disconnected. Attempting reconnect...");
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  // The panel belongs to SchedulingManager once we are online: it composes
  // the whole 320x170 frame off-screen and pushes it in one write. Drawing
  // a clock from here too would mean two writers on the same SPI bus with
  // two different ideas of the layout.
}

bool WifiHandler::isConnected() { return WiFi.status() == WL_CONNECTED; }

String WifiHandler::getIpAddress() { return WiFi.localIP().toString(); }
