#include "ZhsunycoMaster.h"

// Scan callback that stops immediately when the target device is found.
// This avoids waiting the full scan duration once we know the tag is awake.
// Compares by MAC string only — NimBLE v2 address == also checks type,
// but we may not know the type when constructing from a stored string.
class TargetScanCallback : public NimBLEScanCallbacks {
public:
    std::string targetStr;
    NimBLEAddress foundAddress; // Actual address with correct type from scan
    bool found;
    int foundRssi;

    TargetScanCallback(NimBLEAddress addr) : targetStr(addr.toString()), found(false), foundRssi(0) {}

    void onResult(const NimBLEAdvertisedDevice* device) override {
        if (device->getAddress().toString() == targetStr) {
            found = true;
            foundAddress = device->getAddress();
            foundRssi = device->getRSSI();
            Serial.printf("BLE: Target %s seen at %d dBm, connecting immediately\n", targetStr.c_str(), foundRssi);
            NimBLEDevice::getScan()->stop();
        }
    }
};

ZhsunycoMaster::ZhsunycoMaster() {
    _client = nullptr;
}

void ZhsunycoMaster::begin() {
    NimBLEDevice::init("RubbishServer");

    // Raise transmit power from the ~+3 dBm default to the ESP32-C6 maximum.
    // The E-Ink tags are the range-limiting factor and this is the cheapest
    // ~17 dB we can get; note it only improves the ESP->tag direction, since
    // the tags advertise at whatever power their own firmware chose.
    if (!NimBLEDevice::setPower(BLE_TX_POWER_DBM)) {
        Serial.printf("BLE: setPower(%d) rejected, running at default power\n", BLE_TX_POWER_DBM);
    }
    Serial.printf("BLE: TX power %d dBm\n", NimBLEDevice::getPower());

    _scan = NimBLEDevice::getScan();
    _scan->setInterval(45);
    _scan->setWindow(15);
    _scan->setActiveScan(true);
}

std::vector<String> ZhsunycoMaster::scanTags(int durationSec) {
    std::vector<String> foundTags;
    NimBLEScanResults results = _scan->getResults(durationSec * 1000);

    for(int i=0; i<results.getCount(); i++) {
        const NimBLEAdvertisedDevice* device = results.getDevice(i);
        if (device->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
            String mac = String(device->getAddress().toString().c_str());
            _recordRssi(mac, device->getRSSI());
            foundTags.push_back(mac);
        }
    }
    return foundTags;
}

// MAC keys are normalised to lowercase: NimBLE renders addresses lowercase but
// MACs entered through the dashboard are typically uppercase.
void ZhsunycoMaster::_recordRssi(const String& mac, int rssi) {
    if (rssi == 0) return; // no reading available
    String key = mac;
    key.toLowerCase();
    _lastRssi[key] = rssi;
    _lastRssiTime[key] = time(nullptr);
}

int ZhsunycoMaster::getLastRssi(String macAddress) {
    macAddress.toLowerCase();
    if (!_lastRssi.count(macAddress)) return 0;
    return _lastRssi[macAddress];
}

time_t ZhsunycoMaster::getLastRssiTime(String macAddress) {
    macAddress.toLowerCase();
    if (!_lastRssiTime.count(macAddress)) return 0;
    return _lastRssiTime[macAddress];
}

uint16_t ZhsunycoMaster::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    uint16_t crc_table[] = {
        0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
        0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022
    };

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 2; j++) {
            uint16_t v5 = (crc << 4) & 0xFFFF;
            uint8_t temp = (crc >> 8) ^ byte;
            temp = (temp >> 4) & 0xF;
            crc = v5 ^ crc_table[temp];
            byte = (byte << 4) & 0xFF;
        }
    }
    return crc;
}

// RLE Compression based on Python implementation
std::vector<uint8_t> ZhsunycoMaster::compressRLE(const uint8_t* data, size_t len, bool packed) {
     // One accessor for both layouts keeps a single copy of the run-length
     // logic below, which is fiddly enough without a second version of it.
     auto px = [&](size_t idx) -> uint8_t {
         return packed ? (uint8_t)((data[idx >> 3] >> (7 - (idx & 7))) & 1) : data[idx];
     };
     std::vector<uint8_t> out;
     size_t i = 0;
     while (i < len) {
         int run_len = 0;
         uint8_t current_val = px(i);
         size_t j = i;
         while (j < len && px(j) == current_val) {
             run_len++;
             j++;
             if (run_len >= 0xFFFF) break;
         }

         if (run_len < 7) {
             uint8_t byte_val = 0x80;
             if (i < len) byte_val |= (px(i) << 6);

             int consumed = 0;
             uint8_t accum = 0;
             for (int k = 0; k < 7; k++) {
                 if (i + k < len) {
                     consumed++;
                     if (k > 0) accum |= (px(i+k) << (6 - k));
                 }
             }
             byte_val |= accum;
             out.push_back(byte_val);
             i += consumed;
         } else {
             if (run_len <= 31) {
                 out.push_back((current_val << 6) | run_len);
             } else if (run_len <= 255) {
                 out.push_back((current_val << 6) | 0x01);
                 out.push_back(run_len);
             } else {
                 out.push_back((current_val << 6) | 0x00);
                 out.push_back(run_len & 0xFF);
                 out.push_back((run_len >> 8) & 0xFF);
             }
             i += run_len;
         }
     }
     return out;
}

String ZhsunycoMaster::bytesToHex(const std::vector<uint8_t>& data) {
    String s = "";
    char buf[3];
    for (uint8_t b : data) {
        sprintf(buf, "%02X", b);
        s += buf;
    }
    return s;
}

bool ZhsunycoMaster::updateTag(String macAddress, const uint8_t* bwBitmap, const uint8_t* redBitmap, int width, int height, bool packed) {
    NimBLEAddress address(std::string(macAddress.c_str()), BLE_ADDR_PUBLIC);
    if (!connect(address)) return false;

    size_t numPixels = width * height;
    std::vector<uint8_t> comp_bw = compressRLE(bwBitmap, numPixels, packed);
    std::vector<uint8_t> comp_red = compressRLE(redBitmap, numPixels, packed);

    String hex_bw = bytesToHex(comp_bw);
    String hex_red = bytesToHex(comp_red);

    int h_minus_1 = height - 1;
    int w_minus_1 = width - 1;

    char headerPart1[100];
    sprintf(headerPart1, "FC%04X%04X%04X%04X%08X", 0, 0, h_minus_1, w_minus_1, (int)comp_bw.size());

    char headerPart2[100];
    sprintf(headerPart2, "FC8%03X%04X8%03X%04X%08X", 0, 0, h_minus_1, w_minus_1, (int)comp_red.size());

    String fullPayloadHex = String(headerPart1) + hex_bw + String(headerPart2) + hex_red;

    // Convert hex string to bytes
    std::vector<uint8_t> payload;
    for (unsigned int i = 0; i < fullPayloadHex.length(); i += 2) {
        String byteString = fullPayloadHex.substring(i, i + 2);
        payload.push_back((uint8_t) strtol(byteString.c_str(), NULL, 16));
    }

    // Encryption Key
    uint8_t macXor = 0;
    const ble_addr_t* macAddr = address.getBase();
    for(int i=0; i<6; i++) macXor ^= macAddr->val[i];
    uint8_t key = macXor ^ 0x31;

    // Build Packets
    int totalLen = payload.size();
    int count = (totalLen + 199) / 200;

    // Header Packet
    uint8_t headerPkt[20];
    headerPkt[0] = 0xFF;
    headerPkt[1] = 0xFC;
    memcpy(&headerPkt[2], "easyTag", 7);
    headerPkt[9] = 0x62; // Unencrypted

    headerPkt[10] = (totalLen >> 24) & 0xFF;
    headerPkt[11] = (totalLen >> 16) & 0xFF;
    headerPkt[12] = (totalLen >> 8) & 0xFF;
    headerPkt[13] = totalLen & 0xFF;

    headerPkt[14] = (count >> 8) & 0xFF;
    headerPkt[15] = count & 0xFF;

    headerPkt[16] = 0x42; // B
    headerPkt[17] = 0x54; // T

    uint16_t headerCrc = crc16(headerPkt, 18);
    headerPkt[18] = (headerCrc >> 8) & 0xFF;
    headerPkt[19] = headerCrc & 0xFF;

    // Encrypt Header (EXCEPT byte 9)
    for(int i=0; i<20; i++) {
        if (i != 9) headerPkt[i] ^= key;
    }

    // Get Service and Characteristic
    NimBLERemoteService* pService = _client->getService(SERVICE_UUID);
    if (!pService) {
        Serial.println("BLE: Service not found");
        disconnect();
        return false;
    }
    NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(CHAR_WRITE_UUID);
    if (!pChar) {
        Serial.println("BLE: Write characteristic not found");
        disconnect();
        return false;
    }

    // Send Header
    if (!pChar->writeValue(headerPkt, 20, true)) {
        Serial.println("BLE: Header write failed");
        disconnect();
        return false;
    }
    delay(500);

    // Send Data Packets
    for (int i = 0; i < count; i++) {
        uint8_t chunk[204];
        memset(chunk, 0, 204);

        uint16_t seq = i + 1;
        chunk[0] = (seq >> 8) & 0xFF;
        chunk[1] = seq & 0xFF;

        int startIdx = i * 200;
        int len = 200;
        if (startIdx + len > totalLen) len = totalLen - startIdx;

        memcpy(&chunk[2], &payload[startIdx], len);

        uint16_t dataCrc = crc16(chunk, 202);
        chunk[202] = (dataCrc >> 8) & 0xFF;
        chunk[203] = dataCrc & 0xFF;

        // Encrypt
        for(int j=0; j<204; j++) chunk[j] ^= key;

        if (!pChar->writeValue(chunk, 204, true)) {
            Serial.printf("BLE: Chunk %d/%d write failed\n", i+1, count);
            disconnect();
            return false;
        }
    }

    disconnect();
    return true;
}

bool ZhsunycoMaster::connect(NimBLEAddress address) {
    // Clean up any existing client to avoid exhausting the NimBLE client pool
    if (_client != nullptr) {
        if (_client->isConnected()) {
            if (_client->getPeerAddress().toString() == address.toString()) return true;
            _client->disconnect();
        }
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
    }

    // Scan for the target device, waiting for it to wake up.
    // Tags only advertise every 40-60s, so we need a long window.
    // The callback stops the scan immediately when found, so we connect
    // while the tag is still awake rather than waiting the full 65s.
    Serial.printf("BLE: Waiting for %s to wake up (up to 65s)...\n", address.toString().c_str());
    TargetScanCallback cb(address);
    _scan->setScanCallbacks(&cb);
    _scan->clearResults();
    _scan->start(65000, false); // Async scan
    unsigned long scanStart = millis();
    while (!cb.found && (millis() - scanStart < 65000)) {
        delay(10);
    }
    _scan->stop();
    _scan->setScanCallbacks(nullptr);

    if (!cb.found) {
        Serial.printf("BLE: %s not found (out of range?)\n", address.toString().c_str());
        return false;
    }

    // Wait for scan to fully stop before connecting
    NimBLEAddress actualAddress = cb.foundAddress;
    _recordRssi(String(actualAddress.toString().c_str()), cb.foundRssi);
    Serial.printf("BLE: %s found (type=%d)\n", actualAddress.toString().c_str(), actualAddress.getType());
    while (_scan->isScanning()) { delay(10); }

    _client = NimBLEDevice::createClient();
    if (_client == nullptr) {
        Serial.println("BLE: Failed to create client (pool exhausted)");
        return false;
    }
    _client->setConnectTimeout(10000);

    // Try with scanned address type first, then try the other type
    Serial.printf("BLE: Connecting (type=%d)...\n", actualAddress.getType());
    if (_client->connect(actualAddress, false)) {
        Serial.printf("BLE: Connected to %s\n", actualAddress.toString().c_str());
        return true;
    }
    // connect() may return false due to MTU exchange failure even though
    // the link-layer connection succeeded (e-ink tags don't support MTU negotiation)
    if (_client->isConnected()) {
        Serial.println("BLE: Connected (MTU exchange failed, using default MTU)");
        return true;
    }

    // Clean up before retry with alternate address type
    _client->disconnect();
    delay(100);

    // Flip address type and retry (public<->random)
    uint8_t otherType = actualAddress.getType() == BLE_ADDR_PUBLIC ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    NimBLEAddress altAddress(actualAddress.toString(), otherType);
    Serial.printf("BLE: Retrying with type=%d...\n", otherType);
    if (_client->connect(altAddress, false)) {
        Serial.printf("BLE: Connected to %s (type=%d)\n", altAddress.toString().c_str(), otherType);
        return true;
    }
    if (_client->isConnected()) {
        Serial.println("BLE: Connected (MTU exchange failed, using default MTU)");
        return true;
    }

    Serial.printf("BLE: Failed to connect to %s with either address type\n", actualAddress.toString().c_str());
    _client->disconnect();
    NimBLEDevice::deleteClient(_client);
    _client = nullptr;
    return false;
}

bool ZhsunycoMaster::disconnect() {
    if (_client) {
        _client->disconnect();
        NimBLEDevice::deleteClient(_client);
        _client = nullptr;
    }
    return true;
}
