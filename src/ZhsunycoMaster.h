#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <map>
#include <vector>

// BLE transmit power in dBm. The ESP32-C6 supports up to +20 dBm; NimBLE
// defaults to roughly +3 dBm. Raising this is the single largest range gain
// available to us, at the cost of a little extra current while transmitting.
#define BLE_TX_POWER_DBM 20

class ZhsunycoMaster {
public:
    ZhsunycoMaster();
    void begin();
    
    // Scan and return list of found tags (MAC addresses). Blocks for the
    // whole duration — used where a result is needed immediately.
    std::vector<String> scanTags(int durationStr = 5);

    // Non-blocking form: kick it off, poll scanBusy(), then harvest. The
    // periodic visibility sweep uses this so it does not stall the loop.
    void beginScan(int durationSec);
    bool scanBusy();
    std::vector<String> harvestScan();
    
    // Connect to a tag and update its image
    // Pass two buffers: black/white and red. 
    // 0 = White/NoRed, 1 = Black/Red
    // `packed` = bitmaps are 1 bit per pixel, MSB first, rather than one
    // byte per pixel. A 296x128 frame is then 4,736 bytes instead of 37,888,
    // which is the difference between fitting in heap and not.
    bool updateTag(String macAddress, const uint8_t* bwBitmap, const uint8_t* redBitmap, int width, int height, bool packed = false);

    // Signal strength of the last advertisement seen from a tag, in dBm.
    // Returns 0 if the tag has never been seen (a real reading is always negative).
    // Recorded during scans and immediately before each connect, so it reflects
    // the link quality at the moment of the last update attempt.
    int getLastRssi(String macAddress);
    time_t getLastRssiTime(String macAddress);

private:
    NimBLEScan* _scan;
    NimBLEClient* _client;

    // Last observed RSSI per tag MAC. Written only from the loop task
    // (scanTags/connect), so no locking is needed here — SchedulingManager
    // copies these into its own mutex-protected status map.
    std::map<String, int> _lastRssi;
    std::map<String, time_t> _lastRssiTime;
    
    // Protocol Constants
    const char* SERVICE_UUID = "00001523-1212-efde-1523-785feabcd123";
    const char* CHAR_WRITE_UUID = "00001525-1212-efde-1523-785feabcd123";
    const char* CHAR_NOTIFY_UUID = "00001526-1212-efde-1523-785feabcd123";

    // Helpers
    uint16_t crc16(const uint8_t* data, size_t len);
    std::vector<uint8_t> compressRLE(const uint8_t* data, size_t len, bool packed = false);
    uint8_t calculateKey(String macStr);
    String bytesToHex(const std::vector<uint8_t>& data);
    
    void _recordRssi(const String& mac, int rssi);

    bool connect(NimBLEAddress address);
    bool disconnect();
};
