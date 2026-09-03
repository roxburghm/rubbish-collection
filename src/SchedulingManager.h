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
  int lastRssi = 0;         // dBm of the last advertisement seen; 0 = never seen
  time_t lastRssiTime = 0;  // when lastRssi was measured
  bool lastResult = false;
  String lastDataHash = "";
  int retryCount = 0;
  time_t nextRetryTime = 0;
  bool pendingUpdate = false;
  // Handed to the BLE task and not yet finished. Stops loop() queueing the
  // same device again every 100ms while its write is in flight.
  bool inFlight = false;
};

// A unit of BLE work. Plain chars, not String: a FreeRTOS queue copies the
// struct byte for byte, which would corrupt a String's heap bookkeeping.
struct WriteJob {
  char mac[20];
  char type[24]; // empty = the normal schedule frame
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

  // --- Onboard console (320x170 LCD) ---
  // The touch task calls these; it must never draw. Only loop() draws, so
  // there is exactly one writer on the SPI bus.
  //
  // A tap cycles: asleep -> schedule -> network -> asleep.
  // Returns true if the backlight should be lit after the tap.
  bool consoleTap();
  void consoleSleep();      // 60s auto-dim, from the touch task
  bool consoleAwake();
  void requestConsoleRedraw();

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

  // BLE runs on its own task. connect() waits up to 65 seconds for a sleeping
  // tag to advertise, and doing that on the Arduino loop task froze the
  // console clock and stalled the web handlers for the duration.
  QueueHandle_t _writeQueue;
  static void _writerTrampoline(void *self);
  void _writerLoop();
  bool _enqueueWrite(const String &mac, const String &type);

  // Request queue (set by web handlers, consumed by loop)
  bool _forceUpdateRequested = false;
  String _testPatternRequestMac = "";
  String _testPatternRequestType = "";
  std::vector<String> _pendingTriggerMacs;
  std::vector<String> _pendingRemoveMacs;

  int _lastDisplayDay = -1; // Track day-of-year to detect day changes

  // Console state. Guarded by _mutex because the touch task writes it.
  enum ConsoleScreen { CONSOLE_SCHEDULE = 0, CONSOLE_NETWORK = 1 };
  int _consoleScreen = CONSOLE_SCHEDULE;
  bool _consoleAwake = true;
  bool _consoleDirty = true;
  int _consoleLastMinute = -1;

  void _processUpdate();
  // daysOverride pretends the next collection is that many days away, so a
  // label can be made to show its tonight/tomorrow/today face on demand
  // instead of waiting for the calendar. -1 means use the real date.
  void
  _generateAndSendImage(JsonDocument &data, String targetMac,
                        SystemErrorState errorState = SystemErrorState::NONE,
                        int daysOverride = -1,
                        String previewDate = "");
  void _sendTestPattern(String mac, String type);
  void _scanForDevices();

  // Console rendering. _drawConsole() composes a whole frame off-screen and
  // pushes it in one go, so the panel never shows a half-drawn state.
  void _drawConsole();
  // Minute ticks only move the clock, so they repaint the 320x26 status band
  // alone (17KB) instead of asking for a whole frame buffer (106KB) that the
  // e-ink builder needs to be able to find.
  void _drawConsoleBarOnly();
  void _drawConsoleSchedule(lgfx::LovyanGFX &g, const struct tm &now, bool timeKnown);
  void _drawConsoleNetwork(lgfx::LovyanGFX &g);
  void _drawConsoleBar(lgfx::LovyanGFX &g, const struct tm &now, bool timeKnown);
};
