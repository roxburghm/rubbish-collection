#include "ZhsunycoMaster.h"

ZhsunycoMaster::ZhsunycoMaster() {
    _client = nullptr;
}

void ZhsunycoMaster::begin() {
    NimBLEDevice::init("RubbishServer");
    _scan = NimBLEDevice::getScan();
    _scan->setInterval(45);
    _scan->setWindow(15);
    _scan->setActiveScan(true);
}

std::vector<String> ZhsunycoMaster::scanTags(int durationSec) {
    std::vector<String> foundTags;
    NimBLEScanResults results = _scan->start(durationSec);
    
    for(int i=0; i<results.getCount(); i++) {
        NimBLEAdvertisedDevice device = results.getDevice(i);
        if (device.isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
            foundTags.push_back(device.getAddress().toString().c_str());
        }
    }
    return foundTags;
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
std::vector<uint8_t> ZhsunycoMaster::compressRLE(const uint8_t* data, size_t len) {
     std::vector<uint8_t> out;
     size_t i = 0;
     while (i < len) {
         int run_len = 0;
         uint8_t current_val = data[i];
         size_t j = i;
         while (j < len && data[j] == current_val) {
             run_len++;
             j++;
             if (run_len >= 0xFFFF) break;
         }
         
         if (run_len < 7) {
             uint8_t byte_val = 0x80;
             if (i < len) byte_val |= (data[i] << 6);
             
             int consumed = 0;
             uint8_t accum = 0;
             for (int k = 0; k < 7; k++) {
                 if (i + k < len) {
                     consumed++;
                     if (k > 0) accum |= (data[i+k] << (6 - k));
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

bool ZhsunycoMaster::updateTag(String macAddress, const uint8_t* bwBitmap, const uint8_t* redBitmap, int width, int height) {
    NimBLEAddress address(macAddress.c_str());
    if (!connect(address)) return false;
    
    // 1. Prepare Data
    // Note: Python script logic:
    // compress bw
    // compress red
    // payload string = FC...hex_bw + FC8...hex_red
    
    // We assume input Bitmaps are 1 byte per pixel? Or packed?
    // The RLE expects specific values.
    // Python RLE: `data: List[int]` where int is 0 or 1.
    // So input `bwBitmap` should be 1 byte per pixel (0 or 1).
    
    size_t numPixels = width * height;
    std::vector<uint8_t> comp_bw = compressRLE(bwBitmap, numPixels);
    std::vector<uint8_t> comp_red = compressRLE(redBitmap, numPixels);
    
    String hex_bw = bytesToHex(comp_bw);
    String hex_red = bytesToHex(comp_red);
    
    int h_minus_1 = height - 1;
    int w_minus_1 = width - 1;
    
    char headerPart1[100];
    // Packet 1 (BW): FC + X0(4) + Y0(4) + Y1(4) + X1(4) + Len(8)
    sprintf(headerPart1, "FC%04X%04X%04X%04X%08X", 0, 0, h_minus_1, w_minus_1, (int)comp_bw.size());
    
    char headerPart2[100];
    // Packet 2 (Red): FC8 + X0(3) + Y0(4) + 8 + Y1(3) + X1(4) + Len(8)
    // NOTE: X0=000, Y1=h_minus_1 (3 chars)
    sprintf(headerPart2, "FC8%03X%04X8%03X%04X%08X", 0, 0, h_minus_1, w_minus_1, (int)comp_red.size());
    
    String fullPayloadHex = String(headerPart1) + hex_bw + String(headerPart2) + hex_red;
    
    // Convert hex string to bytes
    std::vector<uint8_t> payload;
    for (unsigned int i = 0; i < fullPayloadHex.length(); i += 2) {
        String byteString = fullPayloadHex.substring(i, i + 2);
        payload.push_back((uint8_t) strtol(byteString.c_str(), NULL, 16));
    }
    
    // 2. Encryption Key
    uint8_t macXor = 0;
    const uint8_t* macBytes = address.getNative();
    for(int i=0; i<6; i++) macXor ^= macBytes[i];
    uint8_t key = macXor ^ 0x31;
    
    // 3. Build Packets
    int totalLen = payload.size();
    int count = (totalLen + 199) / 200;
    
    // Header Packet
    uint8_t headerPkt[20];
    headerPkt[0] = 0xFF;
    headerPkt[1] = 0xFC;
    memcpy(&headerPkt[2], "easyTag", 7);
    headerPkt[9] = 0x62; // Unencrypted
    
    headerPkt[10] = (totalLen >> 24) & 0xFF;
    headerPkt[11] = (totalLen >> 16) & 0xFF; // Python len(payload).to_bytes(4, 'big')
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
    
    // Send Header
    NimBLERemoteService* pService = _client->getService(SERVICE_UUID);
    if (!pService) return false;
    NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(CHAR_WRITE_UUID);
    if (!pChar) return false;
    
    pChar->writeValue(headerPkt, 20, true); // Write with response
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
        
        memcpy(&chunk[2], &payload[startIdx], len); // Padded with 0s if short
        
        // CRC includes Seq + Data (202 bytes)
        uint16_t dataCrc = crc16(chunk, 202);
        chunk[202] = (dataCrc >> 8) & 0xFF;
        chunk[203] = dataCrc & 0xFF;
        
        // Encrypt
        for(int j=0; j<204; j++) chunk[j] ^= key;
        
        pChar->writeValue(chunk, 204, true);
        // No delay needed if Write With Response, but maybe safer with small one
        // Python said "Write with Response recommended, or 20ms delays"
    }
    
    disconnect();
    return true;
}

bool ZhsunycoMaster::connect(NimBLEAddress address) {
    if (_client != nullptr && _client->isConnected()) {
        if (_client->getPeerAddress().equals(address)) return true;
        _client->disconnect();
    }
    
    _client = NimBLEDevice::createClient();
    if (_client->connect(address)) {
        return true;
    }
    return false;
}

bool ZhsunycoMaster::disconnect() {
    if (_client) {
        _client->disconnect();
        return true;
    }
    return false;
}
