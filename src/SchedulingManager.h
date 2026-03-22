#pragma once
#include <Arduino.h>
#include "CouncilClient.h"
#include "ZhsunycoMaster.h"
#include "ConfigManager.h"
#include "LGFX_Setup.hpp"

#include <map>
#include <vector>

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
    SchedulingManager(CouncilClient* api, ZhsunycoMaster* ble, ConfigManager* config, LGFX* display);
    void loop();

    // Thread-safe request methods (safe to call from async web handlers)
    void requestForceUpdate();
    void requestTestPattern(String mac);
    void requestTriggerDevice(String mac);
    void requestRemoveDevice(String mac);

    // Thread-safe data accessors (safe to call from async web handlers)
    JsonDocument getLastSchedule();
    time_t getLastUpdateEpoch();
    time_t getNextScheduledEpoch();
    String getDeviceStatusJson();

private:
    CouncilClient* _api;
    ZhsunycoMaster* _ble;
    ConfigManager* _config;
    LGFX* _display;

    time_t _lastUpdateEpoch = 0;

    JsonDocument _lastData;
    std::map<String, DeviceStatus> _deviceStatuses;

    // Mutex for shared state access
    SemaphoreHandle_t _mutex;

    // Request queue (set by web handlers, consumed by loop)
    bool _forceUpdateRequested = false;
    String _testPatternRequestMac = "";
    std::vector<String> _pendingTriggerMacs;
    std::vector<String> _pendingRemoveMacs;

    int _lastDisplayDay = -1; // Track day-of-year to detect day changes

    void _processUpdate();
    void _generateAndSendImage(JsonDocument& data, String targetMac);
    void _sendTestPattern(String mac);
    void _scanForDevices();
};
