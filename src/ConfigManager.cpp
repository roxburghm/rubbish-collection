#include "ConfigManager.h"
#include <ArduinoJson.h>

ConfigManager::ConfigManager() {}

void ConfigManager::begin() {
    _prefs.begin("rubbish_app", false);
}

void ConfigManager::setAdminPassword(String password) {
    _prefs.putString("admin_pass", password);
}

String ConfigManager::getAdminPassword() {
    return _prefs.getString("admin_pass", "admin"); // Default password
}

void ConfigManager::setPostcode(String postcode) {
    _prefs.putString("postcode", postcode);
}

String ConfigManager::getPostcode() {
    return _prefs.getString("postcode", "");
}

void ConfigManager::setUPRN(String uprn) {
    _prefs.putString("uprn", uprn);
}

String ConfigManager::getUPRN() {
    return _prefs.getString("uprn", "");
}

void ConfigManager::setAddressLabel(String label) {
    _prefs.putString("addr_label", label);
}

String ConfigManager::getAddressLabel() {
    return _prefs.getString("addr_label", "");
}

String ConfigManager::getDevicesJson() {
    return _prefs.getString("devices", "[]");
}

void ConfigManager::addDevice(String mac) {
    String current = getDevicesJson();
    JsonDocument doc;
    deserializeJson(doc, current);
    
    JsonArray arr = doc.as<JsonArray>();
    for(JsonVariant v : arr) {
        if (v.as<String>() == mac) return; // Exists
    }
    
    arr.add(mac);
    String output;
    serializeJson(doc, output);
    _prefs.putString("devices", output);
}

void ConfigManager::removeDevice(String mac) {
    String current = getDevicesJson();
    JsonDocument doc;
    deserializeJson(doc, current);
    
    JsonArray arr = doc.as<JsonArray>();
    for (int i=0; i<arr.size(); i++) {
        if (arr[i].as<String>() == mac) {
            arr.remove(i);
            break;
        }
    }
    
    String output;
    serializeJson(doc, output);
    _prefs.putString("devices", output);
}
