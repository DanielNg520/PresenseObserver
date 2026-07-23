#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_AMG88xx.h>

// Adafruit Feather ESP32-S3 (8MB flash, 2MB QSPI PSRAM).
// Onboard red LED is on GPIO13 (LED_BUILTIN) — used here as a scan heartbeat.

static const uint32_t SCAN_INTERVAL_MS = 30000;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

// Guest network to join, secured with a WPA passphrase.
static const char* GUEST_SSID = "RESNET-GUEST-DEVICE";
static const char* GUEST_PASSPHRASE = "ResnetConnect";

// AMG8833 8x8 thermal sensor (I2C, default address 0x69 on the Adafruit
// breakout; 0x68 if the ADDR jumper is bridged).
static Adafruit_AMG88xx amg;
static bool amgReady = false;

// Connectivity probe. Uses a small endpoint that returns HTTP 204 with an
// empty body when the internet is reachable (Google's "generate_204").
// A captive portal will instead return a redirect / login page (not 204),
// which lets us distinguish "connected to AP" from "actually online".
static const char* CONNECTIVITY_URL = "http://connectivitycheck.gstatic.com/generate_204";

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

// Probe the I2C bus for the AMG8833 thermal sensor. Tries the two possible
// addresses and reports whether the sensor was detected. Sets amgReady.
static void detectThermalSensor() {
    Serial.println("\nDetecting AMG8833 thermal sensor...");

    const uint8_t addresses[] = {AMG88xx_ADDRESS, 0x68};  // 0x69 default, 0x68 alt
    for (uint8_t addr : addresses) {
        if (amg.begin(addr)) {
            amgReady = true;
            Serial.printf("  AMG8833 found at I2C address 0x%02X.\n", addr);
            delay(100);  // sensor boot time before first read
            Serial.printf("  Thermistor: %.2f C\n", amg.readThermistor());
            return;
        }
    }

    amgReady = false;
    Serial.println("  AMG8833 NOT detected — check wiring (SDA/SCL, 3V3, GND).");
}

static void printMacAddress() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.printf("Device MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void scanNetworks() {
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("\nScanning for WiFi networks...");

    int count = WiFi.scanNetworks();
    digitalWrite(LED_BUILTIN, LOW);
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

// Connect to the guest network. Returns true once an IP is obtained.
static bool connectToGuest() {
    Serial.printf("\nConnecting to network \"%s\"...\n", GUEST_SSID);

    WiFi.begin(GUEST_SSID, GUEST_PASSPHRASE);  // WPA-secured network

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // blink while joining
        Serial.print('.');
        delay(300);
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_BUILTIN, LOW);
        Serial.printf("Failed to join \"%s\" within %lu s.\n",
                      GUEST_SSID, (unsigned long)(WIFI_CONNECT_TIMEOUT_MS / 1000));
        return false;
    }

    digitalWrite(LED_BUILTIN, HIGH);  // solid = associated
    Serial.printf("Joined \"%s\".\n", GUEST_SSID);
    Serial.printf("  IP address : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Gateway    : %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("  DNS        : %s\n", WiFi.dnsIP().toString().c_str());
    Serial.printf("  RSSI       : %d dBm\n", WiFi.RSSI());
    return true;
}

// Probe for real internet access. Distinguishes an open internet path from a
// captive portal that has associated us but not let traffic through.
static void checkInternet() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Not associated — skipping internet check.");
        return;
    }

    Serial.printf("Checking internet via %s ...\n", CONNECTIVITY_URL);

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    // Don't auto-follow redirects: a redirect is the signal of a captive portal.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(CONNECTIVITY_URL)) {
        Serial.println("  RESULT: HTTP client init failed.");
        return;
    }

    int code = http.GET();
    if (code == 204) {
        Serial.println("  RESULT: ONLINE — internet reachable (HTTP 204).");
    } else if (code > 0 && (code == 301 || code == 302 || code == 307)) {
        String loc = http.getLocation();
        Serial.printf("  RESULT: CAPTIVE PORTAL — redirected (HTTP %d) to %s\n",
                      code, loc.c_str());
        Serial.println("          Associated but not online (login/accept-terms required).");
    } else if (code > 0) {
        Serial.printf("  RESULT: CAPTIVE/UNEXPECTED — got HTTP %d (expected 204).\n", code);
    } else {
        Serial.printf("  RESULT: OFFLINE — request failed (%s).\n",
                      http.errorToString(code).c_str());
    }

    http.end();
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(115200);
    // Wait up to 3s for the USB-CDC host to attach so early output isn't lost.
    uint32_t serialWait = millis();
    while (!Serial && (millis() - serialWait) < 3000) {
        delay(10);
    }
    delay(500);

    Serial.println();
    Serial.println("=== ESP32-S3 Feather WiFi Troubleshoot ===");

    Wire.begin();
    detectThermalSensor();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    printMacAddress();
    scanNetworks();

    WiFi.setAutoReconnect(true);
    if (connectToGuest()) {
        checkInternet();
    }

    Serial.printf("\nRe-checking every %lu seconds.\n", (unsigned long)(SCAN_INTERVAL_MS / 1000));
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi disconnected, retrying...");
        scanNetworks();
        connectToGuest();
    } else {
        Serial.printf("WiFi OK — IP %s, RSSI %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    checkInternet();
    delay(SCAN_INTERVAL_MS);
}
