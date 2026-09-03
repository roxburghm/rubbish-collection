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

// to<JsonObject>() replaces whatever was there, so reaching for it
// unconditionally would have rotation wipe the name and vice versa.
static JsonObject deviceEntry(JsonDocument &doc, const String &mac) {
    if (doc[mac].is<JsonObject>()) return doc[mac].as<JsonObject>();
    return doc[mac].to<JsonObject>();
}

void ConfigManager::setDeviceRotation(String mac, bool rotate) {
    String current = _prefs.getString("device_cfgs", "{}");
    JsonDocument doc;
    deserializeJson(doc, current);

    deviceEntry(doc, mac)["rot"] = rotate;

    String output;
    serializeJson(doc, output);
    _prefs.putString("device_cfgs", output);
}

void ConfigManager::setDeviceName(String mac, String name) {
    String current = _prefs.getString("device_cfgs", "{}");
    JsonDocument doc;
    deserializeJson(doc, current);

    name.trim();
    if (name.length() > 24) name = name.substring(0, 24);

    if (name.length() == 0) {
        // Clearing the name falls the card back to showing the MAC.
        if (doc[mac].is<JsonObject>()) doc[mac].remove("name");
    } else {
        deviceEntry(doc, mac)["name"] = name;
    }

    String output;
    serializeJson(doc, output);
    _prefs.putString("device_cfgs", output);
}

String ConfigManager::getDeviceName(String mac) {
    String current = _prefs.getString("device_cfgs", "{}");
    JsonDocument doc;
    deserializeJson(doc, current);

    if (doc[mac].is<JsonObject>() && doc[mac]["name"].is<const char *>()) {
        return doc[mac]["name"].as<String>();
    }
    return "";
}

bool ConfigManager::getDeviceRotation(String mac) {
    String current = _prefs.getString("device_cfgs", "{}");
    JsonDocument doc;
    deserializeJson(doc, current);
    
    if (!doc[mac].isNull()) {
        return doc[mac]["rot"].as<bool>();
    }
    return false; // Default 0
}

void ConfigManager::setPropertyName(String name) {
    _prefs.putString("prop_name", name);
}

String ConfigManager::getPropertyName() {
    return _prefs.getString("prop_name", "");
}
