#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

class ZhsunycoMaster {
public:
    ZhsunycoMaster();
    void begin();
    
    // Scan and return list of found tags (MAC addresses)
    std::vector<String> scanTags(int durationStr = 5);
    
    // Connect to a tag and update its image
    // Pass two buffers: black/white and red. 
    // 0 = White/NoRed, 1 = Black/Red
    bool updateTag(String macAddress, const uint8_t* bwBitmap, const uint8_t* redBitmap, int width, int height);

private:
    NimBLEScan* _scan;
    NimBLEClient* _client;
    
    // Protocol Constants
    const char* SERVICE_UUID = "00001523-1212-efde-1523-785feabcd123";
    const char* CHAR_WRITE_UUID = "00001525-1212-efde-1523-785feabcd123";
    const char* CHAR_NOTIFY_UUID = "00001526-1212-efde-1523-785feabcd123";

    // Helpers
    uint16_t crc16(const uint8_t* data, size_t len);
    std::vector<uint8_t> compressRLE(const uint8_t* data, size_t len);
    uint8_t calculateKey(String macStr);
    String bytesToHex(const std::vector<uint8_t>& data);
    
    bool connect(NimBLEAddress address);
    bool disconnect();
};
