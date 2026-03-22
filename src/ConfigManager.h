#pragma once
#include <Preferences.h>
#include <Arduino.h>

class ConfigManager {
public:
    ConfigManager();
    void begin();
    
    // Application Settings
    void setAdminPassword(String password);
    String getAdminPassword();
    
    void setPostcode(String postcode);
    String getPostcode();
    
    void setUPRN(String uprn);
    String getUPRN();
    
    void setAddressLabel(String label);
    String getAddressLabel();
    
    // Device Management
    void addDevice(String mac);
    void removeDevice(String mac);
    String getDevicesJson(); // Returns ["MAC1", "MAC2"]
    
    // Per-Device Config (Rotation)
    void setDeviceRotation(String mac, bool rotate);
    bool getDeviceRotation(String mac);

    void setPropertyName(String name);
    String getPropertyName();

private:
    Preferences _prefs;
};
