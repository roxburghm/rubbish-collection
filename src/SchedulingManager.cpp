#include "SchedulingManager.h"
#include <WiFi.h>
#include <time.h>

namespace {
// Defined with the console helpers below; the tag renderer needs it first.
String fitText(lgfx::LovyanGFX &g, const String &s, int maxW);

// dd/mm/yyyy -> local midnight. 0 if unparseable.
time_t parseDMY(const String &s) {
  if (s.length() < 10) return 0;
  struct tm t = {0};
  t.tm_mday = s.substring(0, 2).toInt();
  t.tm_mon = s.substring(3, 5).toInt() - 1;
  t.tm_year = s.substring(6, 10).toInt() - 1900;
  t.tm_hour = 12; // midday, so DST shifts cannot roll the date over
  t.tm_isdst = -1;
  return mktime(&t);
}

// How often a service repeats, from collectionSchedule:
//   "FOOD5 Mon"     -> weekly
//   "REC8 MonFort1" -> fortnightly
// 0 when the field says nothing, in which case we only trust the one date
// the council actually gave us.
int cadenceFromSchedule(const String &sched) {
  String s = sched;
  s.toUpperCase();
  if (s.length() == 0) return 0;
  if (s.indexOf("FORT") >= 0) return 14;
  const char *days[] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
  for (int i = 0; i < 7; i++) {
    if (s.indexOf(days[i]) >= 0) return 7;
  }
  return 0;
}

// The council only ever returns the NEXT date per service, so a weekly stream
// would otherwise vanish from every repeat after the first.
bool collectsOnDate(time_t serviceEpoch, int cadence, time_t targetEpoch) {
  if (serviceEpoch == 0 || targetEpoch == 0) return false;
  double diff = difftime(targetEpoch, serviceEpoch) / 86400.0;
  int days = (int)(diff + (diff < 0 ? -0.5 : 0.5));
  if (days == 0) return true;
  if (days < 0 || cadence <= 0) return false;
  return (days % cadence) == 0;
}
} // namespace

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

void SchedulingManager::requestTestPattern(String mac, String type) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _testPatternRequestMac = mac;
  _testPatternRequestType = type;
  xSemaphoreGive(_mutex);
}

void SchedulingManager::requestTriggerDevice(String mac) {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _pendingTriggerMacs.push_back(mac);
  xSemaphoreGive(_mutex);
}

// --- Onboard console state (touch task writes, loop() draws) ---

bool SchedulingManager::consoleTap() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  if (!_consoleAwake) {
    // Waking always lands on the schedule — the reason you looked.
    _consoleAwake = true;
    _consoleScreen = CONSOLE_SCHEDULE;
  } else if (_consoleScreen == CONSOLE_SCHEDULE) {
    _consoleScreen = CONSOLE_NETWORK;
  } else {
    _consoleAwake = false;
  }
  _consoleDirty = true;
  bool awake = _consoleAwake;
  xSemaphoreGive(_mutex);
  return awake;
}

void SchedulingManager::consoleSleep() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _consoleAwake = false;
  // Next wake starts from the schedule again.
  _consoleScreen = CONSOLE_SCHEDULE;
  _consoleDirty = true;
  xSemaphoreGive(_mutex);
}

bool SchedulingManager::consoleAwake() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  bool awake = _consoleAwake;
  xSemaphoreGive(_mutex);
  return awake;
}

void SchedulingManager::requestConsoleRedraw() {
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _consoleDirty = true;
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
      obj["rssi"] = s.lastRssi;
      obj["rssiTime"] = s.lastRssiTime;
      obj["result"] = s.lastResult;
      obj["pending"] = s.pendingUpdate;
      obj["retries"] = s.retryCount;
    } else {
      obj["lastSuccess"] = 0;
      obj["lastAttempt"] = 0;
      obj["lastSeen"] = 0;
      obj["rssi"] = 0;
      obj["rssiTime"] = 0;
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
        int sightingRssi = _ble->getLastRssi(registered);
        if (sightingRssi != 0) {
          _deviceStatuses[registered].lastRssi = sightingRssi;
          _deviceStatuses[registered].lastRssiTime = now;
        }
        Serial.printf("BLE: Seen %s at %d dBm\n", registered.c_str(), sightingRssi);
        break;
      }
    }
  }
  _consoleDirty = true; // the "n/m labels" count may have moved
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
  String testType = "";
  std::vector<String> triggers;
  std::vector<String> removes;

  xSemaphoreTake(_mutex, portMAX_DELAY);
  doForce = _forceUpdateRequested;
  _forceUpdateRequested = false;
  testMac = _testPatternRequestMac;
  testType = _testPatternRequestType;
  _testPatternRequestMac = "";
  _testPatternRequestType = "";
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
    _sendTestPattern(testMac, testType);
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
      // TODAY/TOMORROW wording is relative to the date, so the console has
      // to repaint too, not just the tags.
      _consoleDirty = true;
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

  // 7. Onboard console. This is the only place the panel is drawn, so BLE
  //    writes above have already finished and the SPI bus is free.
  //    While the backlight is off there is nothing to see, so skip the work
  //    entirely — waking sets _consoleDirty and repaints with fresh content.
  {
    struct tm ct;
    bool ctKnown = getLocalTime(&ct);

    bool fullRedraw = false;
    bool barOnly = false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (_consoleAwake) {
      if (_consoleDirty) {
        fullRedraw = true;
        _consoleDirty = false;
        if (ctKnown) _consoleLastMinute = ct.tm_min;
      } else if (ctKnown && ct.tm_min != _consoleLastMinute) {
        // Only the clock moved. Repaint the status band, not the frame —
        // a 106KB request every minute fragments the heap that the e-ink
        // builder needs in one contiguous piece.
        barOnly = true;
        _consoleLastMinute = ct.tm_min;
      }
    }
    xSemaphoreGive(_mutex);

    if (fullRedraw) _drawConsole();
    else if (barOnly) _drawConsoleBarOnly();
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
    _consoleDirty = true; // new schedule: the console is showing stale dates

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
                                              SystemErrorState errorState,
                                              int daysOverride,
                                              String previewDate) {
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

  // ---------------------------------------------------------------
  //  The tag frame: 296 x 128, three inks, same tear-off as the LCD
  //  and the web dashboard.
  //
  //  Red is spent exactly once per state:
  //    collection tomorrow  -> the date block floods red
  //    collection in 2 days -> red on the action line only
  //    collection today     -> no red; the moment to act has gone
  //    an error             -> red on the action line
  // ---------------------------------------------------------------

  const int DATE_W = 100;
  const int BODY_X = DATE_W + 12;
  const int BODY_W = W - BODY_X - 12;

  // Footer sits on the bottom edge rather than floating. The update stamp
  // shares its baseline so the two read as one line across the whole tag.
  const int FOOT_TEXT_Y = H - 16;  // 112: 8px glyphs, 8px bottom margin
  const int FOOT_RULE_Y = H - 22;  // 106
  const int STREAM_TOP = 30;
  const int STREAM_BOT = FOOT_RULE_Y - 2;

  // Previewing a specific collection: swap the target date in before anything
  // is derived, so the streams listed are the ones that really fall on it.
  if (previewDate.length() >= 10) {
    nextDate = previewDate;
  }

  // Days until the next collection, from local midnight.
  int daysAway = 999;
  struct tm nextTm = {0};
  bool nextValid = false;

  if (nextDate.length() >= 10) {
    nextTm.tm_mday = nextDate.substring(0, 2).toInt();
    nextTm.tm_mon = nextDate.substring(3, 5).toInt() - 1;
    nextTm.tm_year = nextDate.substring(6, 10).toInt() - 1900;
    nextTm.tm_isdst = -1;
    time_t nextEpoch = mktime(&nextTm);
    localtime_r(&nextEpoch, &nextTm);
    nextValid = true;

    if (timeKnown) {
      struct tm todayTm = timeinfo;
      todayTm.tm_hour = 0;
      todayTm.tm_min = 0;
      todayTm.tm_sec = 0;
      todayTm.tm_isdst = -1;
      time_t todayEpoch = mktime(&todayTm);
      daysAway = (int)(difftime(nextEpoch, todayEpoch) / 86400.0 + 0.5);
    }
  }

  // Preview mode: shift the shown date too, so the wording and the date on
  // the tag agree instead of claiming "tonight" over a date four days out.
  if (daysOverride >= 0 && timeKnown) {
    struct tm shifted = timeinfo;
    shifted.tm_mday += daysOverride;
    shifted.tm_hour = 12;
    shifted.tm_isdst = -1;
    time_t shiftedEpoch = mktime(&shifted);
    localtime_r(&shiftedEpoch, &nextTm);
    nextValid = true;
    daysAway = daysOverride;
  }

  bool hasError = (errorState != SystemErrorState::NONE);
  bool floodDate = (!hasError && daysAway == 1);
  bool redAction = (!hasError && daysAway == 2);

  String actionLine;
  if (hasError) {
    actionLine = (errorState == SystemErrorState::NO_WIFI) ? "NO WIFI - LAST KNOWN"
                                                           : "API ERROR - LAST KNOWN";
  } else if (daysAway == 0) {
    actionLine = "COLLECTION TODAY";
  } else if (daysAway == 1) {
    actionLine = "BINS OUT TONIGHT";
  } else if (daysAway == 2) {
    actionLine = "BINS OUT TOMORROW";
  } else {
    actionLine = "NEXT COLLECTION";
  }

  const char *DOW3[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  const char *MON3[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  // Short forms for the shared line and the footer, in binKeys order.
  const char *gridShort[] = {"RUBBISH", "RECYCLE", "FOOD", "GARDEN"};

  // --- date block ---
  sprite.fillRect(0, 0, DATE_W, H, floodDate ? TFT_RED : TFT_BLACK);
  sprite.setTextColor(TFT_WHITE);
  sprite.setTextSize(1);
  sprite.setTextDatum(top_center);

  if (nextValid) {
    // The stamp is part of this stack, so the four elements are centred as
    // one group: 13 + 6 + 48 + 6 + 13 + 10 + 8 = 104 in 128, 12px each end.
    // MON / 14 / SEP centred as a group above the stamp, with equal gaps:
    // 13 + 6 + 48 + 6 + 13 = 86 in the 106px above the footer line, so 10px
    // of air top and bottom.
    sprite.setFont(&fonts::FreeSansBold9pt7b);
    sprite.drawString(DOW3[nextTm.tm_wday], DATE_W / 2, 10);

    // Font6 is a clean 48px numeric face. Scaling a vector font 2x on a
    // 1-bit panel doubles every stroke and looks crude; this does not.
    sprite.setFont(&fonts::Font6);
    sprite.drawString(String(nextTm.tm_mday), DATE_W / 2, 29);

    sprite.setFont(&fonts::FreeSansBold9pt7b);
    sprite.drawString(MON3[nextTm.tm_mon], DATE_W / 2, 83);
  } else {
    sprite.setFont(&fonts::FreeSansBold9pt7b);
    sprite.drawString("NO", DATE_W / 2, 48);
    sprite.drawString("DATE", DATE_W / 2, 68);
  }

  // --- action line ---
  sprite.setTextDatum(top_left);
  sprite.setTextColor(redAction || hasError ? TFT_RED : TFT_BLACK);
  sprite.setFont(&fonts::FreeSansBold9pt7b);
  sprite.drawString(actionLine, BODY_X, 12);

  // --- the streams going out that day ---
  // Two lines at most. Three or four streams collapse onto a shared line
  // using their short names, exactly as the web preview shows.
  String lines[3];
  int lineCount = 0;
  {
    String names[4];
    String shorts[4];
    int n = 0;
    for (int o = 0; o < 4 && n < 4; o++) {
      for (JsonVariant v : serviceList) {
        String s = v["Service"].as<String>();
        s.toUpperCase();
        if (s.indexOf(binKeys[o]) < 0) continue;
        // Weekly streams recur, so match by cadence rather than by the single
        // date the council happened to return.
        if (!collectsOnDate(parseDMY(firstDate(v["collectionDateString"])),
                            cadenceFromSchedule(v["collectionSchedule"].as<String>()),
                            parseDMY(nextDate))) break;

        if (strcmp(binKeys[o], "RUBBISH") == 0)        names[n] = "RUBBISH";
        else if (strcmp(binKeys[o], "RECYCLING") == 0) names[n] = "RECYCLING";
        else if (strcmp(binKeys[o], "FOOD") == 0)      names[n] = "FOOD WASTE";
        else                                            names[n] = "GARDEN WASTE";
        shorts[n] = gridShort[o];
        n++;
        break;
      }
    }

    if (n <= 3) {
      // Three full names still clear the footer rule at 22px leading.
      for (int i = 0; i < n; i++) lines[i] = names[i];
      lineCount = n;
    } else {
      // Four will not fit, so the tail collapses to short names on one line.
      lines[0] = names[0];
      lines[1] = "";
      for (int i = 1; i < n; i++) {
        if (lines[1].length()) lines[1] += " / ";
        lines[1] += shorts[i];
      }
      lineCount = 2;
    }
  }

  // Use the largest face every line genuinely fits in. "GARDEN WASTE" at
  // 12pt overruns 172px and was being clipped to "GARDEN WA..".
  sprite.setTextColor(TFT_BLACK);
  sprite.setFont(&fonts::FreeSansBold12pt7b);
  int lineH = 17, lineStep = 23;

  bool fits = true;
  for (int i = 0; i < lineCount; i++) {
    if (sprite.textWidth(lines[i]) > BODY_W) { fits = false; break; }
  }
  if (!fits) {
    sprite.setFont(&fonts::FreeSansBold9pt7b);
    lineH = 13;
    lineStep = 18;
  }

  // Centre the block in the gap between the action line and the footer rule.
  int blockH = (lineCount - 1) * lineStep + lineH;
  int lineY = STREAM_TOP + (STREAM_BOT - STREAM_TOP - blockH) / 2;
  if (lineY < STREAM_TOP) lineY = STREAM_TOP;

  for (int i = 0; i < lineCount; i++) {
    sprite.drawString(fitText(sprite, lines[i], BODY_W), BODY_X, lineY);
    lineY += lineStep;
  }

  // --- footer: everything not going out that day ---
  sprite.drawFastHLine(BODY_X, FOOT_RULE_Y, BODY_W, TFT_BLACK);

  sprite.setFont(nullptr); // 6x8 default face — small, and crisp on e-ink
  sprite.setTextSize(1);
  sprite.setTextColor(TFT_BLACK);

  int footX = BODY_X;
  for (int o = 0; o < 4; o++) {
    for (JsonVariant v : serviceList) {
      String s = v["Service"].as<String>();
      s.toUpperCase();
      if (s.indexOf(binKeys[o]) < 0) continue;

      String d = firstDate(v["collectionDateString"]);
      if (collectsOnDate(parseDMY(d),
                         cadenceFromSchedule(v["collectionSchedule"].as<String>()),
                         parseDMY(nextDate))) break; // already named above

      String entry = String(gridShort[o]) + " " + d.substring(0, 5);
      int entryW = entry.length() * 6;
      if (footX + entryW > W - 10) break; // never run off the edge
      sprite.drawString(entry, footX, FOOT_TEXT_Y);
      footX += entryW + 10;
      break;
    }
  }

  // Update stamp, closing the date stack. Small: it answers "is this tag
  // stale?" and nothing more.
  if (timeKnown) {
    char stamp[16];
    strftime(stamp, sizeof(stamp), "%d/%m %H:%M", &timeinfo);
    sprite.setFont(nullptr);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_WHITE);
    sprite.setTextDatum(top_center);
    sprite.drawString(stamp, DATE_W / 2, FOOT_TEXT_Y);
    sprite.setTextDatum(top_left);
  }

  // --- SEND TO BLE ---

  // 1 bit per pixel, MSB first: 4,736 bytes each rather than 37,888. At one
  // byte per pixel this pair plus the sprite came to 113KB against ~94KB of
  // free heap, so every tag write failed with OOM.
  int bufSize = (W * H + 7) / 8;
  uint8_t *bwBuf = (uint8_t *)calloc(bufSize, 1);
  uint8_t *redBuf = (uint8_t *)calloc(bufSize, 1);

  if (!bwBuf || !redBuf) {
    Serial.printf("FAIL: OOM building tag image. free=%u largest=%u needed=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)bufSize);
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

      size_t idx = (size_t)py * W + px;
      uint8_t mask = 0x80 >> (idx & 7);
      if (isRed) {
        redBuf[idx >> 3] |= mask;
      } else if (c != TFT_WHITE) {
        bwBuf[idx >> 3] |= mask;
      }
    }
  }

  Serial.print("Updating ");
  Serial.print(targetMac);
  Serial.print(" ... ");
  bool success =
      _ble->updateTag(targetMac, bwBuf, redBuf, W, H,
                      true); // packed bitmaps; blocking BLE, no mutex

  // Update status under mutex
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _deviceStatuses[targetMac].lastAttempt = time(nullptr);
  _deviceStatuses[targetMac].lastResult = success;

  // The connect path records RSSI as soon as the tag is sighted, so this is
  // populated even when the update itself later fails.
  int sightingRssi = _ble->getLastRssi(targetMac);
  if (sightingRssi != 0) {
    _deviceStatuses[targetMac].lastRssi = sightingRssi;
    _deviceStatuses[targetMac].lastRssiTime = _ble->getLastRssiTime(targetMac);
  }

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

void SchedulingManager::_sendTestPattern(String mac, String type) {
  const int W = 296;
  const int H = 128;

  // The delegating cases must come FIRST. _generateAndSendImage allocates its
  // own sprite plus two full-size buffers (~114KB); holding a 37KB sprite of
  // our own across that call was enough to push it into OOM, so the frame
  // silently never reached the tag.
  if (type == "wifi") {
    _generateAndSendImage(_lastData, mac, SystemErrorState::NO_WIFI);
    return;
  }
  if (type == "api") {
    _generateAndSendImage(_lastData, mac, SystemErrorState::API_ERROR);
    return;
  }
  // "day0".."day9": render the schedule frame as if collection were that many
  // days off, so you can put the red state on a label without waiting for it.
  if (type.startsWith("day") && type.length() > 3) {
    int offset = type.substring(3).toInt();
    Serial.printf("Preview: rendering %s as %d day(s) away\n", mac.c_str(), offset);
    _generateAndSendImage(_lastData, mac, SystemErrorState::NONE, offset);
    return;
  }
  // "date:14/09/2026": render a real future collection, picking the streams
  // that actually fall on that date rather than simulating a wording.
  if (type.startsWith("date:") && type.length() > 5) {
    String target = type.substring(5);
    Serial.printf("Preview: rendering %s for %s\n", mac.c_str(), target.c_str());
    _generateAndSendImage(_lastData, mac, SystemErrorState::NONE, -1, target);
    return;
  }

  // Only the self-contained frames need a sprite of our own.
  LGFX_Sprite sprite(_display);
  sprite.setColorDepth(8);
  sprite.createSprite(W, H);
  sprite.fillSprite(TFT_WHITE);

  if (type == "hello") {
    sprite.setTextColor(TFT_BLACK);
    sprite.setTextDatum(middle_center);
    sprite.setTextSize(3);
    sprite.drawString("HELLO WORLD!", W / 2, H / 2);
    // Draw red border to check alignment
    sprite.drawRect(0, 0, W, H, TFT_RED);

  } else {
    // Draw Checkerboard (Default "pattern" type)
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
  }

  // Extraction
  // 1 bit per pixel, MSB first: 4,736 bytes each rather than 37,888. At one
  // byte per pixel this pair plus the sprite came to 113KB against ~94KB of
  // free heap, so every tag write failed with OOM.
  int bufSize = (W * H + 7) / 8;
  uint8_t *bwBuf = (uint8_t *)calloc(bufSize, 1);
  uint8_t *redBuf = (uint8_t *)calloc(bufSize, 1);

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
      size_t idx = (size_t)py * W + px;
      uint8_t mask = 0x80 >> (idx & 7);
      if (isRed) {
        redBuf[idx >> 3] |= mask;
      } else if (c != TFT_WHITE) {
        bwBuf[idx >> 3] |= mask;
      }
    }
  }

  Serial.printf("Sending test pattern to %s ... ", mac.c_str());
  bool success =
      _ble->updateTag(mac, bwBuf, redBuf, W, H,
                      true); // packed bitmaps; blocking BLE, no mutex

  // Update status under mutex
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _deviceStatuses[mac].lastAttempt = time(nullptr);
  _deviceStatuses[mac].lastResult = success;

  // The connect path records RSSI as soon as the tag is sighted, so this is
  // populated even when the update itself later fails.
  int sightingRssi = _ble->getLastRssi(mac);
  if (sightingRssi != 0) {
    _deviceStatuses[mac].lastRssi = sightingRssi;
    _deviceStatuses[mac].lastRssiTime = _ble->getLastRssiTime(mac);
  }
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

// ===================================================================
//  Onboard console — the 320x170 LCD
//
//  The same tear-off the e-ink tag and the web dashboard use, in colour.
//  The 1.9" panel makes 1px 0.13mm, barely half the tag's, so nothing
//  here is smaller than FreeSansBold9pt.
//
//  loop() is the only caller, so this is the only writer on the SPI bus.
// ===================================================================

namespace {

// The device palette, matching the --dev-* tokens in data/style.css.
const uint16_t C_GROUND = lgfx::color565(0x12, 0x11, 0x10);
// This is a backlit IPS panel: "black" is really a dark grey, so the small
// steps that separate surfaces on a monitor vanish completely here. The
// panel and bar fills are pushed much further apart than the web tokens.
const uint16_t C_PANEL  = lgfx::color565(0x36, 0x31, 0x2d);
const uint16_t C_BAR    = lgfx::color565(0x24, 0x20, 0x1e);
const uint16_t C_RULE   = lgfx::color565(0x30, 0x2c, 0x2a);
const uint16_t C_INK    = lgfx::color565(0xec, 0xe9, 0xe6);
const uint16_t C_INK2   = lgfx::color565(0xa9, 0xa3, 0xa0);
const uint16_t C_INK3   = lgfx::color565(0x7d, 0x77, 0x73);
const uint16_t C_RED    = lgfx::color565(0xe2, 0x3b, 0x30);
const uint16_t C_ON_RED = lgfx::color565(0xff, 0xfa, 0xf9);

const uint16_t C_RUBBISH   = lgfx::color565(0xa9, 0xa8, 0xa2);
const uint16_t C_RECYCLING = lgfx::color565(0x6f, 0xa8, 0xea);
const uint16_t C_FOOD      = lgfx::color565(0xc4, 0x90, 0x6c);
const uint16_t C_GARDEN    = lgfx::color565(0x7e, 0xc2, 0x67);

const int SCREEN_W = 320;
const int SCREEN_H = 170;
const int BAR_H = 26;
const int MAIN_H = SCREEN_H - BAR_H; // 144
// 104, not 122: the numeral never filled the wider column, and the space it
// wasted was space the action line badly needed.
const int DATE_W = 104;

const char *DOW3[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *MON3[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

uint16_t consoleStreamColour(const String &service) {
  if (service.indexOf("RUBBISH") >= 0) return C_RUBBISH;
  if (service.indexOf("RECYCL") >= 0) return C_RECYCLING;
  if (service.indexOf("FOOD") >= 0) return C_FOOD;
  if (service.indexOf("GARDEN") >= 0) return C_GARDEN;
  return C_INK2;
}

String consoleStreamName(const String &service) {
  if (service.indexOf("RUBBISH") >= 0) return "Rubbish";
  if (service.indexOf("RECYCL") >= 0) return "Recycling";
  if (service.indexOf("FOOD") >= 0) return "Food waste";
  if (service.indexOf("GARDEN") >= 0) return "Garden waste";
  return service;
}

// Strip the duplicate the API sometimes returns: "23/03/2026,23/03/2026"
String consoleFirstDate(JsonVariant field) {
  String raw;
  if (field.is<JsonArray>()) {
    JsonArray arr = field.as<JsonArray>();
    raw = arr.size() > 0 ? arr[0].as<String>() : String("");
  } else {
    raw = field.as<String>();
  }
  raw.trim();
  int comma = raw.indexOf(',');
  if (comma > 0) raw = raw.substring(0, comma);
  raw.trim();
  return raw;
}

// dd/mm/yyyy -> local midnight. 0 if unparseable.
time_t consoleParseDMY(const String &s) {
  if (s.length() < 10) return 0;
  struct tm t = {0};
  t.tm_mday = s.substring(0, 2).toInt();
  t.tm_mon = s.substring(3, 5).toInt() - 1;
  t.tm_year = s.substring(6, 10).toInt() - 1900;
  t.tm_isdst = -1;
  return mktime(&t);
}

String two(int v) { return (v < 10 ? String("0") : String("")) + String(v); }

// Nothing on a 320px panel may be allowed to run off the edge, and the
// strings here are not under our control — a long SSID, a long council
// service name. Measure against the space actually available and trim.
// The caller must have selected the font first.
String fitText(lgfx::LovyanGFX &g, const String &s, int maxW) {
  if (g.textWidth(s) <= maxW) return s;
  String out = s;
  while (out.length() > 1 && g.textWidth(out + "..") > maxW) {
    out.remove(out.length() - 1);
  }
  return out + "..";
}

} // namespace

void SchedulingManager::_drawConsole() {
  if (!_display) return;

  struct tm now;
  bool timeKnown = getLocalTime(&now);

  int screen;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  screen = _consoleScreen;
  xSemaphoreGive(_mutex);

  // Preferred path: compose the whole frame off-screen and push it in one
  // write, so the panel never shows a half-drawn state. That costs 106KB,
  // which is not always there once WiFi, BLE and the async server have taken
  // their share — so fall back to drawing straight to the panel. That
  // flickers briefly, which is a great deal better than a blank screen.
  LGFX_Sprite g(_display);
  g.setColorDepth(16);
  bool buffered = g.createSprite(SCREEN_W, SCREEN_H);

  if (buffered) {
    g.fillSprite(C_GROUND);
    if (screen == CONSOLE_NETWORK) {
      _drawConsoleNetwork(g);
    } else {
      _drawConsoleSchedule(g, now, timeKnown);
      _drawConsoleBar(g, now, timeKnown);
    }
    g.pushSprite(0, 0);
    g.deleteSprite();
    return;
  }

  Serial.println("Console: no memory for the frame buffer, drawing direct");
  _display->startWrite();
  _display->fillScreen(C_GROUND);
  if (screen == CONSOLE_NETWORK) {
    _drawConsoleNetwork(*_display);
  } else {
    _drawConsoleSchedule(*_display, now, timeKnown);
    _drawConsoleBar(*_display, now, timeKnown);
  }
  _display->endWrite();
}

void SchedulingManager::_drawConsoleBarOnly() {
  if (!_display) return;

  int screen;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  screen = _consoleScreen;
  xSemaphoreGive(_mutex);

  // The network screen has no clock, so a minute tick changes nothing on it.
  if (screen != CONSOLE_SCHEDULE) return;

  struct tm now;
  bool timeKnown = getLocalTime(&now);

  // The status band is a self-contained strip across the full width at a
  // fixed y, so repainting it on its own cannot clip the tear-off above it.
  // _drawConsoleBar repaints its own background first, and one 320x26 strip
  // over SPI is quick enough that the redraw is not visible.
  _display->startWrite();
  _drawConsoleBar(*_display, now, timeKnown);
  _display->endWrite();
}

void SchedulingManager::_drawConsoleSchedule(lgfx::LovyanGFX &g,
                                             const struct tm &now,
                                             bool timeKnown) {
  JsonDocument data;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  bool hasData = !_lastData.isNull();
  if (hasData) data = _lastData;
  xSemaphoreGive(_mutex);

  JsonArray services;
  if (hasData && data["param2"].is<JsonArray>()) {
    services = data["param2"].as<JsonArray>();
  }

  if (services.isNull() || services.size() == 0) {
    g.setTextDatum(middle_center);
    g.setFont(&fonts::FreeSansBold12pt7b);
    g.setTextColor(C_INK3);
    g.drawString("NO SCHEDULE YET", SCREEN_W / 2, MAIN_H / 2);
    g.setTextDatum(top_left);
    return;
  }

  // Earliest date across all streams, and everything sharing it.
  time_t bestEpoch = 0;
  String bestRaw = "";
  for (JsonVariant v : services) {
    String raw = consoleFirstDate(v["collectionDateString"]);
    time_t e = consoleParseDMY(raw);
    if (e == 0) continue;
    if (bestEpoch == 0 || e < bestEpoch) {
      bestEpoch = e;
      bestRaw = raw;
    }
  }
  if (bestEpoch == 0) return;

  struct tm nextTm;
  localtime_r(&bestEpoch, &nextTm);

  int diff = 999;
  if (timeKnown) {
    struct tm todayTm = now;
    todayTm.tm_hour = 0;
    todayTm.tm_min = 0;
    todayTm.tm_sec = 0;
    todayTm.tm_isdst = -1;
    time_t todayEpoch = mktime(&todayTm);
    diff = (int)(difftime(bestEpoch, todayEpoch) / 86400.0 + 0.5);
  }

  // Three tiers, exactly how the tag spends its red:
  //   collection tomorrow  -> flood the date block
  //   collection in 2 days -> red on the action line only
  //   anything else        -> no red at all
  bool floodDate = (diff == 1);
  bool redAction = (diff == 2);

  const char *action;
  if (diff == 0)      action = "COLLECTION TODAY";
  else if (diff == 1) action = "BINS OUT TONIGHT";
  else if (diff == 2) action = "BINS OUT TOMORROW";
  else                action = "NEXT COLLECTION";

  // --- date block ---
  g.fillRect(0, 0, DATE_W, MAIN_H, floodDate ? C_RED : C_PANEL);

  g.setTextSize(1);
  g.setTextColor(floodDate ? C_ON_RED : C_INK);

  // Day and month sit centred under the numeral rather than ragged-left, so
  // the block reads as one stack however wide the numeral happens to be.
  // Stack of three, optically centred in the 144px column:
  // 13 + 7 + 48 + 7 + 13 = 88, so 28px of air above and below.
  int dateMid = DATE_W / 2;
  g.setTextDatum(top_center);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.drawString(DOW3[nextTm.tm_wday], dateMid, 28);

  // Font6 is a native 48px numeric face. Scaling FreeSansBold24pt to 2x got
  // the size but doubled every stroke and stair-stepped the curves — it read
  // as chunky rather than bold.
  g.setFont(&fonts::Font6);
  g.drawString(String(nextTm.tm_mday), dateMid, 48);

  g.setFont(&fonts::FreeSansBold9pt7b);
  g.drawString(MON3[nextTm.tm_mon], dateMid, 103);

  // --- action line ---
  // The widest string here is "BINS OUT TOMORROW". Measure it rather than
  // trusting it fits: overflow is silently clipped by the panel edge.
  int bodyX = DATE_W + 16;
  int bodyW = SCREEN_W - bodyX - 12;

  g.setTextDatum(top_left);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextColor(redAction ? C_RED : C_INK);
  g.drawString(fitText(g, action, bodyW), bodyX, 20);

  // --- the streams going out that day ---
  int y = 52;
  for (JsonVariant v : services) {
    if (consoleFirstDate(v["collectionDateString"]) != bestRaw) continue;
    if (y + 22 > MAIN_H) break; // never spill into the status bar

    String service = v["Service"].as<String>();
    service.toUpperCase();

    g.fillRect(bodyX, y + 2, 8, 18, consoleStreamColour(service));
    g.setFont(&fonts::FreeSansBold12pt7b);
    g.setTextColor(C_INK);
    g.drawString(fitText(g, consoleStreamName(service), bodyW - 18),
                 bodyX + 18, y);

    y += 28;
  }
}

void SchedulingManager::_drawConsoleBar(lgfx::LovyanGFX &g, const struct tm &now,
                                        bool timeKnown) {
  g.drawFastHLine(0, MAIN_H, SCREEN_W, C_RULE);
  g.fillRect(0, MAIN_H + 1, SCREEN_W, BAR_H - 1, C_BAR);

  int barMid = MAIN_H + BAR_H / 2;

  g.setTextSize(1);
  g.setFont(&fonts::FreeSansBold9pt7b);
  g.setTextDatum(middle_left);
  g.setTextColor(C_INK);
  g.drawString(timeKnown ? two(now.tm_hour) + ":" + two(now.tm_min) : String("--:--"),
               13, barMid);

  // WiFi strength, four bars.
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  int level = 0;
  if (rssi != 0) {
    if (rssi > -60)      level = 4;
    else if (rssi > -70) level = 3;
    else if (rssi > -80) level = 2;
    else                 level = 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    g.fillRect(74 + i * 5, barMid + 6 - h, 3, h, (i < level) ? C_GARDEN : C_RULE);
  }

  // How many labels are carrying the current frame.
  int total = 0, synced = 0;
  xSemaphoreTake(_mutex, portMAX_DELAY);
  for (std::map<String, DeviceStatus>::iterator it = _deviceStatuses.begin();
       it != _deviceStatuses.end(); ++it) {
    total++;
    if (it->second.lastResult) synced++;
  }
  xSemaphoreGive(_mutex);

  g.setTextDatum(middle_right);
  g.setTextColor(C_INK2);
  if (total > 0) {
    g.drawString(String(synced) + "/" + String(total) + " labels", SCREEN_W - 13, barMid);
  } else {
    g.drawString("no labels", SCREEN_W - 13, barMid);
  }
  g.setTextDatum(top_left);
}

void SchedulingManager::_drawConsoleNetwork(lgfx::LovyanGFX &g) {
  const int QR_COL = 190;              // where the QR panel starts
  const int QR_W = SCREEN_W - QR_COL;  // 130
  const int TEXT_W = QR_COL - 15 - 12; // room a value has before the divider

  g.fillRect(QR_COL, 0, QR_W, SCREEN_H, C_PANEL);
  g.drawFastVLine(QR_COL, 0, SCREEN_H, C_RULE);

  bool up = WiFi.isConnected();
  String ip = up ? WiFi.localIP().toString() : String("offline");
  // No RSSI here: it is already four bars on the schedule screen, and
  // appending it pushed long SSIDs straight through the divider.
  String ssid = up ? WiFi.SSID() : String("not connected");

  const char *keys[3] = {"NETWORK", "MANAGE AT", "ADDRESS"};
  String values[3] = {ssid, String("rubbish.local"), ip};

  g.setTextDatum(top_left);
  g.setTextSize(1);

  int y = 22;
  for (int i = 0; i < 3; i++) {
    g.setFont(&fonts::FreeSansBold9pt7b);
    g.setTextColor(C_INK3);
    g.drawString(keys[i], 15, y);

    g.setFont(&fonts::FreeSansBold12pt7b);
    g.setTextColor(C_INK);
    g.drawString(fitText(g, values[i], TEXT_W), 15, y + 18);
    y += 46;
  }

  // Scan this and you are on the dashboard without typing an IP. No caption:
  // a QR code explains itself, and the label only stole room from the code.
  if (up) {
    int qrSize = 112;
    g.qrcode("http://" + ip, QR_COL + (QR_W - qrSize) / 2,
             (SCREEN_H - qrSize) / 2, qrSize, 3);
  }
}
