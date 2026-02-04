#include "SchedulingManager.h"
#include <time.h>

// Remove simple malloc from constructor, move to init
SchedulingManager::SchedulingManager(CouncilClient* api, ZhsunycoMaster* ble, ConfigManager* config, LGFX* display) 
    : _api(api), _ble(ble), _config(config), _display(display) {
}

void SchedulingManager::loop() {
    if (millis() - _lastUpdate > _interval || _lastUpdate == 0) {
        _processUpdate();
        _lastUpdate = millis();
        _lastUpdateEpoch = time(nullptr);
    }
}

void SchedulingManager::forceUpdate() {
    _processUpdate();
    _lastUpdate = millis();
    _lastUpdateEpoch = time(nullptr);
}

void SchedulingManager::_processUpdate() {
    String uprn = _config->getUPRN();
    if (uprn.length() == 0) {
        Serial.println("Skipping update: No UPRN set");
        return;
    }
    
    Serial.println("Updating Collection Data...");
    JsonDocument doc = _api->getCollectionData(uprn);

    if (!doc["error"].is<String>()) { // Or check if missing
        _lastData = doc; // Cache it
        _generateAndSendImage(doc);
    } else {
        Serial.println("Update failed: " + doc["error"].as<String>());
    }
}

void SchedulingManager::_generateAndSendImage(JsonDocument& data) {
    Serial.println("--- GENERATING E-INK IMAGE ---");
    
    // Check devices first
    String deviceJson = _config->getDevicesJson();
    if (deviceJson == "[]" || deviceJson == "") {
        Serial.println("ABORT: No Devices Configured to update!");
        return;
    }
    
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
    String nextDate = ""; // Raw dd/mm/yyyy
    
    if (serviceList.size() > 0) {
        nextDate = serviceList[0]["collectionDateString"].as<String>();
    }

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
        todayTm.tm_hour = 0; todayTm.tm_min = 0; todayTm.tm_sec = 0;
        time_t todayEpoch = mktime(&todayTm);

        double diff = difftime(nextEpoch, todayEpoch);
        
        if (diff >= 0 && diff < 86400) {
            headerText = "TODAY";
            isUrgent = true;
        } else if (diff >= 86400 && diff < 172800) {
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

    
    // Collect all services for this date
    String servicesLine = "";
    
    for(JsonVariant v : serviceList) {
        String d = v["collectionDateString"].as<String>();
        String s = v["Service"].as<String>();
        s.toUpperCase(); // Ensure Uppercase
        
        if(d == nextDate) {
            if (servicesLine.length() > 0) servicesLine += ", ";
            
            if (s.indexOf("RUBBISH") >= 0) servicesLine += "RUBBISH";
            else if (s.indexOf("RECYCLING") >= 0) servicesLine += "RECYCLING";
            else if (s.indexOf("FOOD") >= 0) servicesLine += "FOOD";
            else if (s.indexOf("GARDEN") >= 0) servicesLine += "GARDEN";
            else servicesLine += "OTHER"; 
        }
    }
    
    // --- DESIGN REFINEMENT ---
    
    // 1. SIDE BAR (Time)
    int sideBarW = 26; // Smaller width (was 34)
    sprite.fillRect(0, 0, sideBarW, H, TFT_WHITE);
    sprite.drawFastVLine(sideBarW, 0, H, TFT_BLACK);   
    
    // Rotated Time
    LGFX_Sprite sidebar(_display);
    sidebar.setColorDepth(8);
    sidebar.createSprite(H, sideBarW); 
    sidebar.fillSprite(TFT_WHITE);
    sidebar.setTextColor(TFT_BLACK);
    sidebar.setFont(&fonts::FreeSans9pt7b); 
    sidebar.setTextSize(0.7); // Scale down slightly
    sidebar.setTextDatum(middle_center);
    
    String updateTime = "@ --/--";
    if(timeKnown) {
        char buf[20];
        strftime(buf, 20, "@ %d/%m %H:%M", &timeinfo);
        updateTime = String(buf);
    }
    sidebar.drawString(updateTime, H/2, sideBarW/2 + 2); // Center
    sidebar.pushRotateZoom(&sprite, sideBarW/2, H/2, 90, 1, 1);
    sidebar.deleteSprite();

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
    
    sprite.drawString(headerText, mainX + mainW/2, headerH/2 + 2); 
    
    // 3. MIDDLE (Main Services List)
    int midY = headerH;
    int midH = 50;
    sprite.fillRect(mainX, midY, mainW, midH, TFT_WHITE);
    
    sprite.setTextColor(TFT_BLACK);
    sprite.setFont(&fonts::FreeSans9pt7b); 
    
    // Auto-scale font if too long
    if (servicesLine.length() > 20) sprite.setTextSize(0.8);
    else sprite.setTextSize(1);
    
    sprite.setTextDatum(middle_center);
    sprite.drawString(servicesLine, mainX + mainW/2, midY + midH/2 - 2);
    
    sprite.drawFastHLine(mainX, midY+midH, mainW, TFT_BLACK); // Divider
    
    // 4. BOTTOM (Grid/Footer)
    int botY = midY + midH;
    int botH = H - botY;
    sprite.fillRect(mainX, botY, mainW, botH, TFT_WHITE);
    
    sprite.setFont(nullptr); // Use Default Font for smallest clear text
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_BLACK);
    sprite.setTextDatum(top_left);
    
    // Grid Layout
    // Col 1: Service (X=10)
    // Col 2: Date (X=70) e.g. "NEXT" or "28/02"
    
    int col1X = mainX + 10;
    int col2X = mainX + (mainW/2) + 5;
    int row1Y = botY + 8;
    int row2Y = botY + 20;
    
    int itemIdx = 0;
    String foundTypes = "";
    
    for(JsonVariant v : serviceList) {
        String s = v["Service"].as<String>();
        String d = v["collectionDateString"].as<String>();
        s.toUpperCase();
        
        // Shorten Service Name
        String shortS = s;
        if (s.indexOf("RUBBISH") >= 0) shortS = "Rubbish"; // Keep Standard Case or Upper? "Rubbish" looks better small
        else if (s.indexOf("RECYCLING") >= 0) shortS = "Recycle"; 
        else if (s.indexOf("FOOD") >= 0) shortS = "Food";
        else if (s.indexOf("GARDEN") >= 0) shortS = "Garden";
        else continue; 
        
        shortS.toUpperCase(); // USER REQUEST: Uppercase bins

        // Check Duplicates
        if (foundTypes.indexOf(shortS) >= 0) continue;
        foundTypes += shortS + ",";
        
        // Skip current date items (handled in main)
        if (d == nextDate) continue; 

        // Calc Position
        int x = (itemIdx % 2 == 0) ? col1X : col2X;
        int y = (itemIdx < 2) ? row1Y : row2Y;
        
        if(itemIdx >= 4) break; 
        
        // Logic for "NEXT"
        String dateLabel = d.substring(0, 5); // dd/mm
        
        if (timeKnown) {
             // Parse d
            int day = d.substring(0, 2).toInt();
            int month = d.substring(3, 5).toInt();
            int year = d.substring(6, 10).toInt();
            struct tm dTm = {0};
            dTm.tm_mday = day; dTm.tm_mon = month - 1; dTm.tm_year = year - 1900; dTm.tm_isdst = -1;
            time_t dEpoch = mktime(&dTm);
            
            struct tm todayTm = timeinfo;
            todayTm.tm_hour = 0; todayTm.tm_min = 0; todayTm.tm_sec = 0;
            time_t todayEpoch = mktime(&todayTm);
            
            double diff = difftime(dEpoch, todayEpoch);
            if (diff >= 86400 && diff < 172800) {
                dateLabel = "NEXT";
            }
        }
        
        // Display in Grid: Service ... Date
        sprite.drawString(shortS, x, y);
        sprite.drawString(dateLabel, x + 50, y); // Offset for date column
        
        itemIdx++;
    }

    // EXTRACTION
    // Black Buffer: Pixels that are NOT White (and NOT Red)
    // Red Buffer: Pixels that are RED
    
    int bufSize = W * H;
    uint8_t* bwBuf = (uint8_t*)malloc(bufSize);
    uint8_t* redBuf = (uint8_t*)malloc(bufSize);
    
    if(!bwBuf || !redBuf) { free(bwBuf); free(redBuf); return; }
    
    for(int py=0; py<H; py++) {
        for(int px=0; px<W; px++) {
            uint16_t c = sprite.readPixel(px, py);
            
            bool isRed = (c == TFT_RED);
            
            if (isRed) {
                redBuf[py*W + px] = 1;
                bwBuf[py*W + px] = 0; 
            } else {
                redBuf[py*W + px] = 0;
                bwBuf[py*W + px] = (c != TFT_WHITE) ? 1 : 0; 
            }
        }
    }
    
    JsonDocument deviceDoc;
    deserializeJson(deviceDoc, deviceJson);
    JsonArray devices = deviceDoc.as<JsonArray>();
    
    for (JsonVariant v : devices) {
        String mac = v.as<String>();
        
        // Track Attempt
        _deviceStatuses[mac].lastAttempt = time(nullptr);
        
        Serial.print("Updating "); Serial.print(mac); Serial.print(" ... ");
        bool success = _ble->updateTag(mac, bwBuf, redBuf, W, H);
        
        _deviceStatuses[mac].lastResult = success;
        if(success) _deviceStatuses[mac].lastSuccess = time(nullptr);
        
        Serial.println(success ? "OK" : "FAIL");
    }
    
    free(bwBuf);
    free(redBuf);
    sprite.deleteSprite();
}

void SchedulingManager::sendTestPattern(String mac) {
    const int W = 296;
    const int H = 128;
    
    LGFX_Sprite sprite(_display);
    sprite.setColorDepth(8); 
    sprite.createSprite(W, H);
    
    sprite.fillSprite(TFT_WHITE);
    
    // Draw Checkerboard
    int sq = 16;
    for(int y=0; y<H; y+=sq) {
        for(int x=0; x<W; x+=sq) {
            if ( ((x/sq) + (y/sq)) % 2 == 0 ) {
                sprite.fillRect(x, y, sq, sq, TFT_LIGHTGREY);
            }
        }
    }
    
    // Red border
    sprite.drawRect(0, 0, W, H, TFT_RED);
    sprite.drawRect(1, 1, W-2, H-2, TFT_RED);
    
    // Text
    sprite.fillRoundRect(20, 20, W-40, H-40, 10, TFT_WHITE);
    sprite.drawRoundRect(20, 20, W-40, H-40, 10, TFT_BLACK);
    
    sprite.setTextColor(TFT_BLACK);
    sprite.setTextSize(3);
    sprite.setTextDatum(middle_center);
    sprite.drawString("TEST PATTERN", W/2, H/2 - 15);
    
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_RED);
    sprite.drawString(mac, W/2, H/2 + 20);
    
    // Extraction
    int bufSize = W * H;
    uint8_t* bwBuf = (uint8_t*)malloc(bufSize);
    uint8_t* redBuf = (uint8_t*)malloc(bufSize);
    
    if(!bwBuf || !redBuf) { 
        if(bwBuf) free(bwBuf); 
        if(redBuf) free(redBuf); 
        return; 
    }
    
    for(int py=0; py<H; py++) {
        for(int px=0; px<W; px++) {
            uint16_t c = sprite.readPixel(px, py);
            bool isRed = (c == TFT_RED);
            if(isRed) {
                redBuf[py*W + px] = 1;
                bwBuf[py*W + px] = 0;
            } else {
                redBuf[py*W + px] = 0;
                bwBuf[py*W + px] = (c != TFT_WHITE) ? 1 : 0; // LightGrey becomes Black
            }
        }
    }
    
    // Track Attempt
    _deviceStatuses[mac].lastAttempt = time(nullptr);
    bool success = _ble->updateTag(mac, bwBuf, redBuf, W, H);
    _deviceStatuses[mac].lastResult = success;
    if(success) _deviceStatuses[mac].lastSuccess = time(nullptr);
    
    free(bwBuf);
    free(redBuf);
    sprite.deleteSprite();
}

String SchedulingManager::getDeviceStatusJson() {
    // Return Array of Objects: [{"mac":"XX","lastSuccess":123,"lastAttempt":123,"result":true}]
    // Merge with Configured Devices list to ensure we show all
    
    String configJson = _config->getDevicesJson();
    JsonDocument doc;
    deserializeJson(doc, configJson);
    JsonArray configured = doc.as<JsonArray>();
    
    JsonDocument outDoc;
    JsonArray outArr = outDoc.to<JsonArray>();
    
    for(JsonVariant v : configured) {
        String mac = v.as<String>();
        JsonObject obj = outArr.add<JsonObject>();
        obj["mac"] = mac;
        
        if(_deviceStatuses.count(mac)) {
            obj["lastSuccess"] = _deviceStatuses[mac].lastSuccess;
            obj["lastAttempt"] = _deviceStatuses[mac].lastAttempt;
            obj["result"] = _deviceStatuses[mac].lastResult;
        } else {
            obj["lastSuccess"] = 0;
            obj["lastAttempt"] = 0;
            obj["result"] = false;
        }
    }
    
    String out;
    serializeJson(outDoc, out);
    return out;
}
