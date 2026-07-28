#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"  // defines WIFI_SSID and WIFI_PASSWORD (git-ignored)

static const uint32_t SCAN_INTERVAL_MS = 30000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

static const char* authModeName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
        default:                        return "unknown";
    }
}

static void printMacAddress() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.printf("Device MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void scanNetworks() {
    Serial.println("\nScanning for WiFi networks...");

    int count = WiFi.scanNetworks();
    if (count == 0) {
        Serial.println("No networks found.");
        return;
    }
    if (count < 0) {
        Serial.printf("Scan failed (error %d).\n", count);
        return;
    }

    Serial.printf("Found %d network(s):\n", count);
    Serial.println(" #  SSID                          RSSI  CH  Auth");
    Serial.println("--- ------------------------------ ---- --- ------------");

    for (int i = 0; i < count; i++) {
        Serial.printf("%2d  %-30s %4d  %2d  %s\n",
                      i + 1,
                      WiFi.SSID(i).c_str(),
                      WiFi.RSSI(i),
                      WiFi.channel(i),
                      authModeName(WiFi.encryptionType(i)));
    }

    WiFi.scanDelete();
}

static bool connectWiFi() {
    Serial.printf("\nConnecting to \"%s\" (WPA2-PSK)...\n", WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("Failed to connect (status %d).\n", WiFi.status());
        return false;
    }

    Serial.println("Connected!");
    Serial.printf("  IP address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Gateway:    %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("  RSSI:       %d dBm\n", WiFi.RSSI());
    return true;
}

void setup() {
    Serial.begin(115200);
    // Wait up to 3s for the USB-CDC host to attach so early output isn't lost.
    uint32_t serialWait = millis();
    while (!Serial && (millis() - serialWait) < 3000) {
        delay(10);
    }
    delay(500);

    Serial.println();
    Serial.println("=== ESP32 WiFi Troubleshoot ===");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    printMacAddress();
    scanNetworks();

    WiFi.setAutoReconnect(true);
    connectWiFi();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi disconnected, retrying...");
        connectWiFi();
    } else {
        Serial.printf("WiFi OK — IP %s, RSSI %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    delay(SCAN_INTERVAL_MS);
}
