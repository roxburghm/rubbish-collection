#pragma once
#include "ConfigManager.h"
#include "CouncilClient.h"
#include "LGFX_Setup.hpp"
#include "ZhsunycoMaster.h"
#include <Arduino.h>

#include <map>
#include <vector>

enum class SystemErrorState { NONE = 0, NO_WIFI, API_ERROR };

struct DeviceStatus {
  time_t lastSuccess = 0;
  time_t lastAttempt = 0;
  time_t lastSeen = 0;
  bool lastResult = false;
  String lastDataHash = "";
  int retryCount = 0;
  time_t nextRetryTime = 0;
  bool pendingUpdate = false;
};

class SchedulingManager {
public:
  SchedulingManager(CouncilClient *api, ZhsunycoMaster *ble,
                    ConfigManager *config, LGFX *display);
  void loop();

  // Thread-safe request methods (safe to call from async web handlers)
  void requestForceUpdate();
  void requestTestPattern(String mac, String type = "pattern");
  void requestTriggerDevice(String mac);
  void requestRemoveDevice(String mac);

  // Thread-safe data accessors (safe to call from async web handlers)
  JsonDocument getLastSchedule();
  time_t getLastUpdateEpoch();
  time_t getNextScheduledEpoch();
  String getDeviceStatusJson();

private:
  CouncilClient *_api;
  ZhsunycoMaster *_ble;
  ConfigManager *_config;
  LGFX *_display;

  time_t _lastUpdateEpoch = 0;
  SystemErrorState _currentErrorState = SystemErrorState::NONE;

  JsonDocument _lastData;
  std::map<String, DeviceStatus> _deviceStatuses;

  // Mutex for shared state access
  SemaphoreHandle_t _mutex;

  // Request queue (set by web handlers, consumed by loop)
  bool _forceUpdateRequested = false;
  String _testPatternRequestMac = "";
  String _testPatternRequestType = "";
  std::vector<String> _pendingTriggerMacs;
  std::vector<String> _pendingRemoveMacs;

  int _lastDisplayDay = -1; // Track day-of-year to detect day changes

  void _processUpdate();
  void
  _generateAndSendImage(JsonDocument &data, String targetMac,
                        SystemErrorState errorState = SystemErrorState::NONE);
  void _sendTestPattern(String mac, String type);
  void _scanForDevices();
};
