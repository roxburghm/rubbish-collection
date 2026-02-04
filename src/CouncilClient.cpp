#include "CouncilClient.h"

CouncilClient::CouncilClient() {}

JsonDocument CouncilClient::searchAddress(String postcode) {
    JsonDocument doc;
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure(); // Skip certificate check for simplicity (or add root CA)
    
    _http.begin(*client, String(_baseUrl) + "/GetAddress");
    _http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    int httpCode = _http.POST("Postcode=" + postcode);
    
    if (httpCode > 0) {
        String payload = _http.getString();
        deserializeJson(doc, payload);
    } else {
        doc["error"] = "HTTP Error: " + String(httpCode);
    }
    
    _http.end();
    delete client;
    return doc;
}

JsonDocument CouncilClient::getCollectionData(String uprn) {
    JsonDocument doc;
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure();
    
    _http.begin(*client, String(_baseUrl) + "/FindCollection");
    _http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    
    int httpCode = _http.POST("UPRN=" + uprn);
    
    if (httpCode > 0) {
        String payload = _http.getString();
        deserializeJson(doc, payload);
    } else {
        doc["error"] = "HTTP Error: " + String(httpCode);
    }
    
    _http.end();
    delete client;
    return doc;
}
