#pragma once
#include <Arduino.h>
#include "CouncilClient.h"
#include "ZhsunycoMaster.h"
#include "ConfigManager.h"
#include "LGFX_Setup.hpp"


#include <map>

struct DeviceStatus {
    time_t lastSuccess = 0;
    time_t lastAttempt = 0;
    bool lastResult = false;
};

class SchedulingManager {
public:
    SchedulingManager(CouncilClient* api, ZhsunycoMaster* ble, ConfigManager* config, LGFX* display);
    void loop();
    void forceUpdate();
    JsonDocument getLastSchedule() { return _lastData; }
    unsigned long getLastUpdateTime() { return _lastUpdate; }
    time_t getLastUpdateEpoch() { return _lastUpdateEpoch; }
    void sendTestPattern(String mac);
    
    // Status Accessor
    String getDeviceStatusJson();



private:
    CouncilClient* _api;
    ZhsunycoMaster* _ble;
    ConfigManager* _config;
    LGFX* _display;
    
    unsigned long _lastUpdate = 0; // millis for interval
    time_t _lastUpdateEpoch = 0;   // Wall clock for UI
    const unsigned long _interval = 6 * 60 * 60 * 1000; // 6 hours
    
    JsonDocument _lastData; // Cache
    std::map<String, DeviceStatus> _deviceStatuses;
    
    void _processUpdate();
    void _generateAndSendImage(JsonDocument& data);
};

