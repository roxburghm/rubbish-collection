#include "CouncilClient.h"

CouncilClient::CouncilClient() {}

JsonDocument CouncilClient::searchAddress(String postcode) {
    JsonDocument doc;
    HTTPClient http;
    NetworkClientSecure *client = new NetworkClientSecure;
    client->setInsecure();

    http.begin(*client, String(_baseUrl) + "/GetAddress");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpCode = http.POST("Postcode=" + postcode);

    if (httpCode > 0) {
        String payload = http.getString();
        deserializeJson(doc, payload);
    } else {
        doc["error"] = "HTTP Error: " + String(httpCode);
    }

    http.end();
    delete client;
    return doc;
}

JsonDocument CouncilClient::getCollectionData(String uprn) {
    JsonDocument doc;
    HTTPClient http;
    NetworkClientSecure *client = new NetworkClientSecure;
    client->setInsecure();

    http.begin(*client, String(_baseUrl) + "/FindCollection");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpCode = http.POST("UPRN=" + uprn);

    if (httpCode > 0) {
        String payload = http.getString();
        deserializeJson(doc, payload);
    } else {
        doc["error"] = "HTTP Error: " + String(httpCode);
    }

    http.end();
    delete client;
    return doc;
}
