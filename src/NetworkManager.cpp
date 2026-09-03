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

  if (_display) {
    _display->fillScreen(TFT_BLACK);

    // Split Screen Layout (320x170)
    // Left Side: Text info (0-180)
    // Right Side: QR Code (180-320)

    // -- Left Side (Status) --
    _display->setTextDatum(top_left);
    _display->setCursor(10, 15);
    _display->setTextColor(TFT_LIGHTGREY);
    _display->setTextSize(1);
    _display->println("STATUS");

    _display->setCursor(10, 30);
    _display->setTextColor(TFT_GREEN);
    _display->setTextSize(2);
    _display->println("ONLINE");

    // -- Right Side (Time) --
    _display->setTextDatum(top_right);
    _display->setTextColor(TFT_LIGHTGREY);
    _display->setTextSize(1);
    _display->drawString("TIME", 170, 15);

    if (timeinfo.tm_year > 70) {
      char timeBuf[10];
      strftime(timeBuf, 10, "%H:%M", &timeinfo);
      _display->setTextColor(TFT_WHITE);
      _display->setTextSize(2);
      _display->drawString(timeBuf, 170, 30);
    }

    _display->setTextDatum(top_left); // Reset

    _display->setCursor(10, 60);
    _display->setTextColor(TFT_WHITE);
    _display->setTextSize(1);
    _display->println("Network:");
    _display->setCursor(10, 72);
    _display->setTextColor(TFT_CYAN); // Pop color
    _display->setTextSize(2);
    _display->println(WiFi.SSID());

    _display->setCursor(10, 100);
    _display->setTextColor(TFT_WHITE);
    _display->setTextSize(1);
    _display->println("Manage at:");
    _display->setCursor(10, 112);
    _display->setTextColor(TFT_YELLOW);
    _display->setTextSize(2);
    _display->println(WiFi.localIP());

    // -- Right Side --
    String url = "http://" + WiFi.localIP().toString();
    _display->qrcode(url, 190, 25, 120, 6);

    _display->drawFastVLine(180, 10, 150, 0x4208); // Dark Grey
  }
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

  // Update Clock every minute
  static unsigned long lastClock = 0;
  if (_display &&
      (millis() - lastClock >
       1000)) { // Update every second for debug, or minute? Minute is fine.
    // Actually showing seconds might be nice if we have space, but user asked
    // for HH:MM. Let's stick to minute update but check often.

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      // Only redraw if minute changed?
      static int lastMin = -1;
      if (timeinfo.tm_min != lastMin) {
        lastMin = timeinfo.tm_min;

        // Clear Area (Right side of status block)
        // x=90 to 180, y=10 to 50
        _display->fillRect(90, 10, 90, 50, TFT_BLACK);

        // Redraw "Time" Label
        _display->setTextDatum(top_right);
        _display->setTextColor(TFT_LIGHTGREY);
        _display->setTextSize(1);
        _display->drawString("TIME", 170, 15);
        if (timeinfo.tm_year > 70) {
          char timeBuf[10];
          strftime(timeBuf, 10, "%H:%M", &timeinfo);
          _display->setTextColor(TFT_WHITE);
          _display->setTextSize(2);
          _display->drawString(timeBuf, 170, 30);
        }

        _display->setTextDatum(top_left); // Reset
        _display->setCursor(10, 60);
        _display->setTextColor(TFT_WHITE);
        _display->setTextSize(1);
        _display->println("Network SSID");
        _display->setCursor(10, 72);
        _display->setTextColor(TFT_CYAN); // Pop color
        _display->setTextSize(2);
        _display->println(WiFi.SSID());
        _display->setTextColor(TFT_WHITE);
        _display->setTextSize(1);
        _display->println("Manage at:");
        _display->setCursor(10, 112);
        _display->setTextColor(TFT_YELLOW);
        == == == =

                     // Redraw Time Value
            char timeBuf[10];
        strftime(timeBuf, 10, "%H:%M", &timeinfo);
        _display->setTextColor(TFT_WHITE); // White
>>>>>>> c472e7f (Add WiFi auto-reconnect and E-Ink error indicators)
        _display->setTextSize(2);
        _display->drawString(timeBuf, 170, 30);

        // Reset Datum for other draws?
        _display->setTextDatum(top_left);
      }
    }
  }
}

bool WifiHandler::isConnected() { return WiFi.status() == WL_CONNECTED; }

String WifiHandler::getIpAddress() { return WiFi.localIP().toString(); }
