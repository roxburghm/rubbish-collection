#include "SchedulingManager.h"
#include <WiFi.h>
#include <time.h>

SchedulingManager::SchedulingManager(CouncilClient *api, ZhsunycoMaster *ble,
                                     ConfigManager *config, LGFX *display)
    : _api(api), _ble(ble), _config(config), _display(display) {
  _mutex = xSemaphoreCreateMutex();
}

// --- Thread-safe request methods (called from async web handlers) ---

void SchedulingManager::requestForceUpdate() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _forceUpdateRequested = true;
  xSemaphoreGive(_mutex);
}

void SchedulingManager::requestTestPattern(String mac) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _testPatternRequestMac = mac;
  xSemaphoreGive(_mutex);
}

void SchedulingManager::requestTriggerDevice(String mac) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _pendingTriggerMacs.push_back(mac);
  xSemaphoreGive(_mutex);
}

void SchedulingManager::requestRemoveDevice(String mac) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _pendingRemoveMacs.push_back(mac);
  xSemaphoreGive(_mutex);
}

// --- Thread-safe data accessors (called from async web handlers) ---

JsonDocument SchedulingManager::getLastSchedule() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  JsonDocument copy = _lastData;
  xSemaphoreGive(_mutex);
  return copy;
}

time_t SchedulingManager::getLastUpdateEpoch() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  time_t val = _lastUpdateEpoch;
  xSemaphoreGive(_mutex);
  return val;
}

time_t SchedulingManager::getNextScheduledEpoch() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return 0;
  }

  // Boundaries at :05 past every 6 hours: 00:05, 06:05, 12:05, 18:05
  const int boundaries[] = {5, 365, 725, 1085}; // in minutes from midnight
  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  for (int i = 0; i < 4; i++) {
    if (currentMinutes < boundaries[i]) {
      struct tm nextTm = timeinfo;
      nextTm.tm_hour = boundaries[i] / 60;
      nextTm.tm_min = boundaries[i] % 60;
      nextTm.tm_sec = 0;
      return mktime(&nextTm);
    }
  }

  // Past 18:05 - next boundary is 00:05 tomorrow
  struct tm nextTm = timeinfo;
  nextTm.tm_mday += 1;
  nextTm.tm_hour = 0;
  nextTm.tm_min = 5;
  nextTm.tm_sec = 0;
  return mktime(&nextTm);
}

String SchedulingManager::getDeviceStatusJson() {
  String configJson = _config->getDevicesJson();
  JsonDocument doc;
  deserializeJson(doc, configJson);
  JsonArray configured = doc.as<JsonArray>();

  JsonDocument outDoc;
  JsonArray outArr = outDoc.to<JsonArray>();

  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (JsonVariant v : configured) {
    String mac = v.as<String>();
    JsonObject obj = outArr.add<JsonObject>();
    obj["mac"] = mac;
    obj["rotate"] = _config->getDeviceRotation(mac);

    if (_deviceStatuses.count(mac)) {
      DeviceStatus &s = _deviceStatuses[mac];
      obj["lastSuccess"] = s.lastSuccess;
      obj["lastAttempt"] = s.lastAttempt;
      obj["lastSeen"] = s.lastSeen;
      obj["result"] = s.lastResult;
      obj["pending"] = s.pendingUpdate;
      obj["retries"] = s.retryCount;
    } else {
      obj["lastSuccess"] = 0;
      obj["lastAttempt"] = 0;
      obj["lastSeen"] = 0;
      obj["result"] = false;
      obj["pending"] = false;
    }
  }
  xSemaphoreGive(_mutex);

  String out;
  serializeJson(outDoc, out);
  return out;
}

// --- Internal: periodic BLE scan to track device visibility ---

void SchedulingManager::_scanForDevices() {
  Serial.println("BLE: Scanning for registered devices (15s)...");
  std::vector<String> found = _ble->scanTags(15);

  String configJson = _config->getDevicesJson();
  JsonDocument doc;
  deserializeJson(doc, configJson);

  time_t now = time(nullptr);

  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (const String &foundMac : found) {
    for (JsonVariant v : doc.as<JsonArray>()) {
      String registered = v.as<String>();
      if (registered.equalsIgnoreCase(foundMac)) {
        _deviceStatuses[registered].lastSeen = now;
        Serial.printf("BLE: Seen %s\n", registered.c_str());
        break;
      }
    }
  }
  xSemaphoreGive(_mutex);
  Serial.printf("BLE: Scan complete, found %d tag(s)\n", found.size());
}

// --- Main loop (runs exclusively on the Arduino loop task) ---

void SchedulingManager::loop() {
  time_t now = time(nullptr);

  // 0a. Check continuous WiFi state and force redraws if state changes
  SystemErrorState detectedErrorState = _currentErrorState;
  if (WiFi.status() != WL_CONNECTED) {
    detectedErrorState = SystemErrorState::NO_WIFI;
  } else if (_currentErrorState == SystemErrorState::NO_WIFI) {
    detectedErrorState = SystemErrorState::NONE;
  }

  if (detectedErrorState != _currentErrorState) {
    Serial.printf("SchedulingManager: Error State Changed to %d\n",
                  (int)detectedErrorState);
    _currentErrorState = detectedErrorState;

    // Queue screen redraw for all registered devices to show new connection
    // status
    xSemaphoreTake(_mutex, portMAX_DELAY);
    String cj = _config->getDevicesJson();
    JsonDocument dd;
    deserializeJson(dd, cj);
    for (JsonVariant v : dd.as<JsonArray>()) {
      String mac = v.as<String>();
      _deviceStatuses[mac].pendingUpdate = true;
      _deviceStatuses[mac].retryCount = 0;
      _deviceStatuses[mac].nextRetryTime = 0;
    }
    xSemaphoreGive(_mutex);
  }

  // 0. Periodic device visibility scan (every 3 minutes)
  static unsigned long lastScanMillis = 0;
  if (millis() - lastScanMillis > 180000) {
    lastScanMillis = millis();
    _scanForDevices();
  }

  // 1. Consume request flags (under mutex)
  bool doForce = false;
  String testMac = "";
  std::vector<String> triggers;
  std::vector<String> removes;

  xSemaphoreTake(_mutex, portMAX_DELAY);
  doForce = _forceUpdateRequested;
  _forceUpdateRequested = false;
  testMac = _testPatternRequestMac;
  _testPatternRequestMac = "";
  triggers.swap(_pendingTriggerMacs);
  removes.swap(_pendingRemoveMacs);
  xSemaphoreGive(_mutex);

  // 2. Apply queued state changes (under mutex)
  if (!removes.empty() || !triggers.empty() || doForce) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    for (auto &mac : removes) {
      _deviceStatuses.erase(mac);
    }
    for (auto &mac : triggers) {
      _deviceStatuses[mac].pendingUpdate = true;
      _deviceStatuses[mac].retryCount = 0;
      _deviceStatuses[mac].nextRetryTime = 0;
    }
    if (doForce) {
      String cj = _config->getDevicesJson();
      JsonDocument dd;
      deserializeJson(dd, cj);
      for (JsonVariant v : dd.as<JsonArray>()) {
        String mac = v.as<String>();
        _deviceStatuses[mac].pendingUpdate = true;
        _deviceStatuses[mac].retryCount = 0;
        _deviceStatuses[mac].nextRetryTime = 0;
      }
    }
    xSemaphoreGive(_mutex);
  }

  // 3. Test pattern request (blocking BLE, runs on main loop task)
  if (testMac.length() > 0) {
    _sendTestPattern(testMac);
  }

  // 4. Boot check
  static bool _hasBooted = false;
  bool shouldFetch = doForce;
  if (!_hasBooted && millis() > 5000) {
    _hasBooted = true;
    shouldFetch = true;
    Serial.println("Boot-up Update Triggered");
  }

  // 4b. Day-change detection: re-render displays when the date changes
  //     (TODAY/TOMORROW/COLLECTION DAY text depends on current date)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int currentDay = timeinfo.tm_yday + timeinfo.tm_year * 366;
    if (_lastDisplayDay != -1 && _lastDisplayDay != currentDay) {
      Serial.println("Day changed - queueing display refresh for all devices");
      xSemaphoreTake(_mutex, portMAX_DELAY);
      String cj = _config->getDevicesJson();
      JsonDocument dd;
      deserializeJson(dd, cj);
      for (JsonVariant v : dd.as<JsonArray>()) {
        String mac = v.as<String>();
        _deviceStatuses[mac].pendingUpdate = true;
        _deviceStatuses[mac].retryCount = 0;
        _deviceStatuses[mac].nextRetryTime = 0;
      }
      xSemaphoreGive(_mutex);
    }
    _lastDisplayDay = currentDay;
  }

  // 5. Scheduled checks (Local Time Europe/London handled by ESP32 config)
  if (!shouldFetch && getLocalTime(&timeinfo)) {
    // Triggers: 00:05, 06:05, 12:05, 18:05
    static int lastTriggerDay = -1;
    static int lastTriggerHour = -1;

    if (timeinfo.tm_min == 5 && (timeinfo.tm_hour % 6 == 0) &&
        (lastTriggerHour != timeinfo.tm_hour ||
         lastTriggerDay != timeinfo.tm_mday)) {

      Serial.println("Scheduled Trigger Fired!");
      lastTriggerHour = timeinfo.tm_hour;
      lastTriggerDay = timeinfo.tm_mday;
      shouldFetch = true;
    }
  }

  if (shouldFetch) {
    _processUpdate();
  }

  // 6. Pending queue consumer
  // Copy _lastData once for all devices
  JsonDocument dataCopy;
  bool hasData = false;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  hasData = !_lastData.isNull();
  if (hasData)
    dataCopy = _lastData;
  xSemaphoreGive(_mutex);

  String configJson = _config->getDevicesJson();
  JsonDocument doc;
  deserializeJson(doc, configJson);
  JsonArray configured = doc.as<JsonArray>();

  for (JsonVariant v : configured) {
    String mac = v.as<String>();

    // Read status under mutex
    bool isPending = false;
    time_t retryTime = 0;
    int retryCount = 0;

    xSemaphoreTake(_mutex, portMAX_DELAY);
    DeviceStatus &status = _deviceStatuses[mac];
    isPending = status.pendingUpdate;
    retryTime = status.nextRetryTime;
    retryCount = status.retryCount;
    xSemaphoreGive(_mutex);

    if (isPending && now >= retryTime) {
      if (!hasData) {
        Serial.printf(
            "No data available for %s, will retry when data arrives.\n",
            mac.c_str());
        continue; // Keep pendingUpdate true, skip to next device
      }

      Serial.printf("Processing Pending Update for %s (Retry %d)\n",
                    mac.c_str(), retryCount);
      _generateAndSendImage(dataCopy, mac, _currentErrorState);
    }
  }
}

// --- Internal methods (called only from loop task) ---

void SchedulingManager::_processUpdate() {
  String uprn = _config->getUPRN();
  if (uprn.length() == 0)
    return;

  Serial.println("Fetching Collection Data...");
  JsonDocument doc = _api->getCollectionData(uprn); // Blocking HTTP, no mutex

  if (doc["error"].is<String>()) {
    Serial.println("Update failed: " + doc["error"].as<String>());
    if (_currentErrorState != SystemErrorState::API_ERROR) {
      _currentErrorState = SystemErrorState::API_ERROR;
      xSemaphoreTake(_mutex, portMAX_DELAY);
      String configJson = _config->getDevicesJson();
      JsonDocument devDoc;
      deserializeJson(devDoc, configJson);
      for (JsonVariant v : devDoc.as<JsonArray>()) {
        String mac = v.as<String>();
        _deviceStatuses[mac].pendingUpdate = true;
        _deviceStatuses[mac].retryCount = 0;
        _deviceStatuses[mac].nextRetryTime = 0;
      }
      xSemaphoreGive(_mutex);
    }
  } else {
    if (_currentErrorState == SystemErrorState::API_ERROR) {
      _currentErrorState = SystemErrorState::NONE; // Recovered
    }
    String newDataStr;
    serializeJson(doc["param2"], newDataStr);

    // Update shared state under mutex
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _lastData = doc;
    _lastUpdateEpoch = time(nullptr);

    String configJson = _config->getDevicesJson();
    JsonDocument devDoc;
    deserializeJson(devDoc, configJson);

    for (JsonVariant v : devDoc.as<JsonArray>()) {
      String mac = v.as<String>();
      if (_deviceStatuses[mac].lastDataHash != newDataStr) {
        Serial.printf("Data changed for %s. Queueing update.\n", mac.c_str());
        _deviceStatuses[mac].pendingUpdate = true;
        _deviceStatuses[mac].retryCount = 0;
        _deviceStatuses[mac].nextRetryTime = 0;
      }
    }
    xSemaphoreGive(_mutex);
  }
}

void SchedulingManager::_generateAndSendImage(JsonDocument &data,
                                              String targetMac,
                                              SystemErrorState errorState) {
  if (targetMac == "")
    return;

  Serial.printf("Generating Image for %s...\n", targetMac.c_str());

  // Rotation
  bool rotate180 = _config->getDeviceRotation(targetMac);

  // Layout Constants
  const int W = 296;
  const int H = 128;

  // Create Sprite
  LGFX_Sprite sprite(_display);
  sprite.setColorDepth(8);
  sprite.createSprite(W, H);

  // 1. CLEAR (White)
  sprite.fillSprite(TFT_WHITE);

  // 2. PARSE DATA & DATE LOGIC
  JsonArray serviceList = data["param2"].as<JsonArray>();
  String nextDate = ""; // Raw dd/mm/yyyy — earliest date across all services

  // Helper: extract first date from collectionDateString
  // Handles both array (["26/01/2026", "02/02/2026"]) and string formats
  auto firstDate = [](JsonVariant field) -> String {
    if (field.is<JsonArray>()) {
      JsonArray arr = field.as<JsonArray>();
      if (arr.size() > 0)
        return arr[0].as<String>();
      return "";
    }
    String raw = field.as<String>();
    raw.trim();
    int comma = raw.indexOf(',');
    if (comma > 0)
      raw = raw.substring(0, comma);
    raw.trim();
    return raw;
  };

  for (JsonVariant v : serviceList) {
    String d = firstDate(v["collectionDateString"]);
    if (d.length() < 10)
      continue;
    if (nextDate.length() == 0) {
      nextDate = d;
    } else {
      // Compare dd/mm/yyyy by converting to yyyymmdd for lexical sort
      String cur = nextDate.substring(6, 10) + nextDate.substring(3, 5) +
                   nextDate.substring(0, 2);
      String cand = d.substring(6, 10) + d.substring(3, 5) + d.substring(0, 2);
      if (cand < cur)
        nextDate = d;
    }
  }
  Serial.printf("nextDate: [%s]\n", nextDate.c_str());

  struct tm timeinfo;
  bool timeKnown = getLocalTime(&timeinfo);

  // Smart Date Logic
  String headerText = nextDate; // Fallback
  bool isUrgent = false;

  if (timeKnown && nextDate.length() >= 10) {
    // Parse nextDate (dd/mm/yyyy)
    int day = nextDate.substring(0, 2).toInt();
    int month = nextDate.substring(3, 5).toInt();
    int year = nextDate.substring(6, 10).toInt();

    // Construct tm for nextDate
    struct tm nextTm = {0};
    nextTm.tm_mday = day;
    nextTm.tm_mon = month - 1;
    nextTm.tm_year = year - 1900;
    nextTm.tm_isdst = -1;
    time_t nextEpoch = mktime(&nextTm);

    // Normalize Today (remove time)
    struct tm todayTm = timeinfo;
    todayTm.tm_hour = 0;
    todayTm.tm_min = 0;
    todayTm.tm_sec = 0;
    time_t todayEpoch = mktime(&todayTm);

    double diff = difftime(nextEpoch, todayEpoch);

    if (diff >= 0 && diff < 86400) {
      // Collection day itself
      headerText = "COLLECTION DAY";
      isUrgent = false; // Black background
    } else if (diff >= 86400 && diff < 172800) {
      // 1 day before collection - put bins out today
      headerText = "TODAY";
      isUrgent = true;
    } else if (diff >= 172800 && diff < 259200) {
      // 2 days before collection - put bins out tomorrow
      headerText = "TOMORROW";
      isUrgent = true;
    } else {
      // Format DOW dd/mm
      char buf[20];
      strftime(buf, 20, "%a %d/%m", &nextTm);
      headerText = String(buf);
      headerText.toUpperCase(); // e.g. MON 27/02
    }
  }

  // Collect all services for this date in preferred display order
  String servicesLine = "";
  const char *binKeys[] = {"RUBBISH", "RECYCLING", "FOOD", "GARDEN"};

  for (int o = 0; o < 4; o++) {
    for (JsonVariant v : serviceList) {
      String d = firstDate(v["collectionDateString"]);
      if (d != nextDate)
        continue;
      String s = v["Service"].as<String>();
      s.toUpperCase();
      if (s.indexOf(binKeys[o]) >= 0) {
        if (servicesLine.length() > 0)
          servicesLine += ", ";
        servicesLine += binKeys[o];
        Serial.printf("  Matched bin: %s from service: %s\n", binKeys[o],
                      s.c_str());
        break;
      }
    }
  }
  Serial.printf("servicesLine: [%s]\n", servicesLine.c_str());

  // 1. SIDE BAR (Time or Error indicator)
  int sideBarW = 26; // Smaller width (was 34)

  if (errorState == SystemErrorState::NO_WIFI ||
      errorState == SystemErrorState::API_ERROR) {
    sprite.fillRect(0, 0, sideBarW, H, TFT_RED);
    sprite.drawFastVLine(sideBarW, 0, H, TFT_BLACK);

    int cx = sideBarW / 2;
    int cy = H / 2;

    if (errorState == SystemErrorState::NO_WIFI) {
      // White WiFi Logo (dot + 3 arcs, crossed out)
      sprite.fillCircle(cx, cy + 9, 2, TFT_WHITE);
      sprite.drawFastHLine(cx - 3, cy + 4, 7, TFT_WHITE);
      sprite.drawFastHLine(cx - 6, cy - 1, 13, TFT_WHITE);
      sprite.drawFastHLine(cx - 9, cy - 6, 19, TFT_WHITE);
      sprite.drawPixel(cx - 4, cy + 5, TFT_WHITE);
      sprite.drawPixel(cx + 4, cy + 5, TFT_WHITE);
      sprite.drawPixel(cx - 7, cy, TFT_WHITE);
      sprite.drawPixel(cx + 7, cy, TFT_WHITE);
      sprite.drawPixel(cx - 10, cy - 5, TFT_WHITE);
      sprite.drawPixel(cx + 10, cy - 5, TFT_WHITE);

      // Cross
      sprite.drawLine(cx - 8, cy - 10, cx + 8, cy + 10, TFT_WHITE);
      sprite.drawLine(cx - 8, cy + 10, cx + 8, cy - 10, TFT_WHITE);
    } else {
      // API_ERROR: Exclamation mark
      sprite.fillCircle(cx, cy + 15, 2, TFT_WHITE);
      sprite.fillRoundRect(cx - 2, cy - 18, 5, 25, 2, TFT_WHITE);
    }

    sprite.fillRect(0, 0, sideBarW, H, TFT_WHITE);
    sprite.drawFastVLine(sideBarW, 0, H, TFT_BLACK);

    // Rotated Time
    LGFX_Sprite sidebar(_display);
    sidebar.setColorDepth(8);
    sidebar.createSprite(H, sideBarW);
    sidebar.fillSprite(TFT_WHITE);
    sidebar.setTextColor(TFT_BLACK);
    sidebar.setFont(nullptr);
    sidebar.setTextSize(1);
    sidebar.setTextDatum(middle_center);

    String updateTime = "@ --/--";
    if (timeKnown) {
      char buf[20];
      strftime(buf, 20, "@ %d/%m %H:%M", &timeinfo);
      updateTime = String(buf);
    }
    sidebar.drawString(updateTime, H / 2, sideBarW / 2 + 2); // Center
    sidebar.pushRotateZoom(&sprite, sideBarW / 2, H / 2, 90, 1, 1);
    sidebar.deleteSprite();
  }

  // 2. HEADER (Date)
  int mainX = sideBarW + 1;
  int mainW = W - sideBarW - 1;
  int headerH = 45;

  uint16_t headerBg = isUrgent ? TFT_RED : TFT_BLACK;
  sprite.fillRect(mainX, 0, mainW, headerH, headerBg);

  sprite.setTextColor(TFT_WHITE);
  sprite.setFont(&fonts::FreeSansBold12pt7b);
  sprite.setTextDatum(middle_center);
  sprite.setTextSize(1);

  sprite.drawString(headerText, mainX + mainW / 2, headerH / 2 + 2);

  // 3. MIDDLE (Main Services List)
  int midY = headerH;
  int midH = 50;
  sprite.fillRect(mainX, midY, mainW, midH, TFT_WHITE);

  sprite.setTextColor(TFT_BLACK);
  sprite.setFont(&fonts::FreeSansBold9pt7b);

  sprite.setTextSize(1);

  sprite.setTextDatum(middle_center);
  sprite.drawString(servicesLine, mainX + mainW / 2, midY + midH / 2 - 2);

  sprite.drawFastHLine(mainX, midY + midH, mainW, TFT_BLACK); // Divider

  // 4. BOTTOM (Grid/Footer)
  int botY = midY + midH;
  int botH = H - botY;
  sprite.fillRect(mainX, botY, mainW, botH, TFT_WHITE);

  sprite.setFont(nullptr); // Use Default Font for smallest clear text
  sprite.setTextSize(1);
  sprite.setTextColor(TFT_BLACK);
  sprite.setTextDatum(top_left);

  // Grid Layout
  int col1X = mainX + 20;
  int col2X = mainX + (mainW / 2) + 30;
  int row1Y = botY + 8;
  int row2Y = botY + 20;

  int itemIdx = 0;
  const char *gridKeys[] = {"RUBBISH", "RECYCLING", "FOOD", "GARDEN"};
  const char *gridShort[] = {"RUBBISH", "RECYCLE", "FOOD", "GARDEN"};

  for (int o = 0; o < 4 && itemIdx < 4; o++) {
    // Find first matching service for this bin type
    for (JsonVariant v : serviceList) {
      String s = v["Service"].as<String>();
      s.toUpperCase();
      if (s.indexOf(gridKeys[o]) < 0)
        continue;

      String d = firstDate(v["collectionDateString"]);

      // Calc Position
      int x = (itemIdx % 2 == 0) ? col1X : col2X;
      int y = (itemIdx < 2) ? row1Y : row2Y;

      // Logic for "NEXT"
      String dateLabel = d.substring(0, 5); // dd/mm
      if (d == nextDate) {
        dateLabel += " *";
      }
      if (timeKnown) {
        int day = d.substring(0, 2).toInt();
        int month = d.substring(3, 5).toInt();
        int year = d.substring(6, 10).toInt();
        struct tm dTm = {0};
        dTm.tm_mday = day;
        dTm.tm_mon = month - 1;
        dTm.tm_year = year - 1900;
        dTm.tm_isdst = -1;
        time_t dEpoch = mktime(&dTm);

        struct tm todayTm = timeinfo;
        todayTm.tm_hour = 0;
        todayTm.tm_min = 0;
        todayTm.tm_sec = 0;
        time_t todayEpoch = mktime(&todayTm);

        double diff = difftime(dEpoch, todayEpoch);
        if (diff >= 86400 && diff < 172800) {
          dateLabel = "NEXT";
        }
      }

      // Display in Grid: Service ... Date
      sprite.drawString(gridShort[o], x, y);
      sprite.drawString(dateLabel, x + 50, y);

      itemIdx++;
      break; // Only first match per bin type
    }
  }

  // --- SEND TO BLE ---

  int bufSize = W * H;
  uint8_t *bwBuf = (uint8_t *)malloc(bufSize);
  uint8_t *redBuf = (uint8_t *)malloc(bufSize);

  if (!bwBuf || !redBuf) {
    Serial.println("FAIL: Malloc Failed (OOM)");
    if (bwBuf)
      free(bwBuf);
    if (redBuf)
      free(redBuf);

    // Treat as retry-able error
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _deviceStatuses[targetMac].retryCount++;
    _deviceStatuses[targetMac].nextRetryTime = time(nullptr) + 60;
    xSemaphoreGive(_mutex);

    sprite.deleteSprite();
    return;
  }

  for (int py = 0; py < H; py++) {
    for (int px = 0; px < W; px++) {

      // Rotation Logic
      int rx = px;
      int ry = py;
      if (rotate180) {
        rx = W - 1 - px;
        ry = H - 1 - py;
      }

      uint16_t c = sprite.readPixel(rx, ry);

      bool isRed = (c == TFT_RED);

      if (isRed) {
        redBuf[py * W + px] = 1;
        bwBuf[py * W + px] = 0;
      } else {
        redBuf[py * W + px] = 0;
        bwBuf[py * W + px] = (c != TFT_WHITE) ? 1 : 0;
      }
    }
  }

  Serial.print("Updating ");
  Serial.print(targetMac);
  Serial.print(" ... ");
  bool success =
      _ble->updateTag(targetMac, bwBuf, redBuf, W, H); // Blocking BLE, no mutex

  // Update status under mutex
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _deviceStatuses[targetMac].lastAttempt = time(nullptr);
  _deviceStatuses[targetMac].lastResult = success;

  if (success) {
    Serial.println("OK");
    _deviceStatuses[targetMac].lastSuccess = time(nullptr);
    _deviceStatuses[targetMac].lastSeen = time(nullptr);
    _deviceStatuses[targetMac].pendingUpdate = false;
    _deviceStatuses[targetMac].retryCount = 0;

    String newDataStr;
    serializeJson(data["param2"], newDataStr);
    _deviceStatuses[targetMac].lastDataHash = newDataStr;

  } else {
    Serial.println("FAIL");
    _deviceStatuses[targetMac].retryCount++;
    int retries = _deviceStatuses[targetMac].retryCount;

    int minutes = 30;
    if (retries == 1)
      minutes = 1;
    else if (retries == 2)
      minutes = 2;
    else if (retries == 3)
      minutes = 3;
    else if (retries == 4)
      minutes = 5;
    else if (retries == 5)
      minutes = 10;

    _deviceStatuses[targetMac].nextRetryTime = time(nullptr) + (minutes * 60);
    Serial.printf("Retry queued in %d mins\n", minutes);
  }
  xSemaphoreGive(_mutex);

  free(bwBuf);
  free(redBuf);
  sprite.deleteSprite();
}

void SchedulingManager::_sendTestPattern(String mac) {
  const int W = 296;
  const int H = 128;

  LGFX_Sprite sprite(_display);
  sprite.setColorDepth(8);
  sprite.createSprite(W, H);

  sprite.fillSprite(TFT_WHITE);

  // Draw Checkerboard
  int sq = 16;
  for (int y = 0; y < H; y += sq) {
    for (int x = 0; x < W; x += sq) {
      if (((x / sq) + (y / sq)) % 2 == 0) {
        sprite.fillRect(x, y, sq, sq, TFT_LIGHTGREY);
      }
    }
  }

  // Red border
  sprite.drawRect(0, 0, W, H, TFT_RED);
  sprite.drawRect(1, 1, W - 2, H - 2, TFT_RED);

  // Text
  sprite.fillRoundRect(20, 20, W - 40, H - 40, 10, TFT_WHITE);
  sprite.drawRoundRect(20, 20, W - 40, H - 40, 10, TFT_BLACK);

  sprite.setTextColor(TFT_BLACK);
  sprite.setTextSize(3);
  sprite.setTextDatum(middle_center);
  sprite.drawString("TEST PATTERN", W / 2, H / 2 - 15);

  sprite.setTextSize(1);
  sprite.setTextColor(TFT_RED);
  sprite.drawString(mac, W / 2, H / 2 + 20);

  // Extraction
  int bufSize = W * H;
  uint8_t *bwBuf = (uint8_t *)malloc(bufSize);
  uint8_t *redBuf = (uint8_t *)malloc(bufSize);

  if (!bwBuf || !redBuf) {
    if (bwBuf)
      free(bwBuf);
    if (redBuf)
      free(redBuf);
    sprite.deleteSprite();
    return;
  }

  for (int py = 0; py < H; py++) {
    for (int px = 0; px < W; px++) {
      uint16_t c = sprite.readPixel(px, py);
      bool isRed = (c == TFT_RED);
      if (isRed) {
        redBuf[py * W + px] = 1;
        bwBuf[py * W + px] = 0;
      } else {
        redBuf[py * W + px] = 0;
        bwBuf[py * W + px] = (c != TFT_WHITE) ? 1 : 0;
      }
    }
  }

  Serial.printf("Sending test pattern to %s ... ", mac.c_str());
  bool success =
      _ble->updateTag(mac, bwBuf, redBuf, W, H); // Blocking BLE, no mutex

  // Update status under mutex
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _deviceStatuses[mac].lastAttempt = time(nullptr);
  _deviceStatuses[mac].lastResult = success;
  if (success) {
    Serial.println("OK");
    _deviceStatuses[mac].lastSuccess = time(nullptr);
    _deviceStatuses[mac].lastSeen = time(nullptr);
    _deviceStatuses[mac].lastDataHash =
        ""; // Invalidate so real data resends next time
  } else {
    Serial.println("FAIL");
  }
  xSemaphoreGive(_mutex);

  free(bwBuf);
  free(redBuf);
  sprite.deleteSprite();
}
