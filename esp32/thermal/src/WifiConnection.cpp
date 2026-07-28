#include "WifiConnection.h"

// Give up on a single network after this long so the caller can try the next.
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

WifiConnection::WifiConnection() {
    Serial.println("[WifiConnection] Initialized");
}

bool WifiConnection::connectToWiFi(String ssid, String password) {
    bool open = (password.length() == 0);
    Serial.printf("[WiFi] Connecting to \"%s\" (%s)...\n",
                  ssid.c_str(), open ? "open" : "WPA-PSK");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

    if (open) {
        WiFi.begin(ssid.c_str());                    // open network: SSID only
    } else {
        WiFi.begin(ssid.c_str(), password.c_str());  // WPA-PSK
    }

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\n[WiFi] Failed to join \"%s\".\n", ssid.c_str());
        return false;
    }

    Serial.println("\n[WiFi] Connected to WiFi.");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
}

bool WifiConnection::connectToWPAEnterprise(String ssid, String username, String password) {
    Serial.printf("[WiFi] Connecting to \"%s\" (WPA2-Enterprise)...\n", ssid.c_str());

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);

    // Initialize the WPA2 Enterprise parameters
    esp_wifi_sta_wpa2_ent_set_identity((uint8_t *)username.c_str(), username.length());
    esp_wifi_sta_wpa2_ent_set_username((uint8_t *)username.c_str(), username.length());
    esp_wifi_sta_wpa2_ent_set_password((uint8_t *)password.c_str(), password.length());

    // Enable WPA2 Enterprise
    esp_wifi_sta_wpa2_ent_enable();

    WiFi.begin(ssid.c_str());

    Serial.print("Waiting for connection...");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\n[WiFi] Failed to join \"%s\".\n", ssid.c_str());
        return false;
    }

    Serial.println("\n[WiFi] Connected to WPA Enterprise successfully!");

    // Manually set DNS servers if DHCP does not work correctly
    ip_addr_t dnsserver;
    IP_ADDR4(&dnsserver, 8, 8, 8, 8);
    dns_setserver(0, &dnsserver);
    return true;
}
