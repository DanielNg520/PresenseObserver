#include <Arduino.h>
#include <WiFi.h>

static const uint32_t SCAN_INTERVAL_MS = 30000;

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

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== ESP32 WiFi Troubleshoot ===");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    printMacAddress();
    scanNetworks();

    Serial.printf("\nRescanning every %lu seconds.\n", (unsigned long)(SCAN_INTERVAL_MS / 1000));
}

void loop() {
    delay(SCAN_INTERVAL_MS);
    scanNetworks();
}
