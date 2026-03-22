#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

class CouncilClient {
public:
    CouncilClient();
    JsonDocument searchAddress(String postcode);
    JsonDocument getCollectionData(String uprn);

private:
    const char* _baseUrl = "https://www.ealing.gov.uk/site/custom_scripts/WasteCollectionWS/home";
};
