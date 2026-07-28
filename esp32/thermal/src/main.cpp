// Thermal Presence Detection — autonomous event recorder
//
// Pipeline:
//   - WiFi (multi-SSID failover + WPA2-Enterprise + captive-portal recovery)
//   - MQTT publish of presence events and thermal frames
//   - On-device TFLite inference (setupModel, computeFeatures, runInference)
//   - Autonomous event recording with an arm/disarm off-switch

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>   // captive-portal WiFi provisioning (tzapu/WiFiManager)
#include <Preferences.h>   // NVS storage for portal-configured credentials
#include <Adafruit_AMG88xx.h>

#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "WifiConnection.h"
#include "MqttClient.h"
#include "Microphone.h"
#include "model_data.h"
#include "model_params.h"


// WiFi / MQTT credentials (injected from .env by pre_extra_script.py) 

const char* ucsdUsername = UCSD_USERNAME;
String      ucsdPassword = String(UCSD_PASSWORD);
//const char* ucsdPassword = ucsdPasswordStr.c_str();
const char* wifiSsid = WIFI_SSID;
const char* nonEnterpriseWifiPassword = NON_ENTERPRISE_WIFI_PASSWORD;
const char* CLIENT_ID = MQTT_CLIENT_ID;
const char* TOPIC_PREFIX = MQTT_TOPIC;

WifiConnection wifi;
MqttClient  mqtt(CLIENT_ID, TOPIC_PREFIX);
Microphone  mic;
Preferences prefs;

// Captive-portal identity: the AP the board broadcasts when no known network
// can be joined, and the hostname it advertises on the network.
const char* PORTAL_AP_NAME  = "ThermalSensor-Setup";
const char* PORTAL_HOSTNAME = "wifimanager.duynq.com";


// Candidate networks, tried in order until one associates. The primary
// network comes from the build config (.env): it is treated as WPA2-Enterprise
// when no non-enterprise password was supplied, otherwise as WPA-PSK. Open and
// PSK campus networks are listed as fallbacks. Order = priority.

enum WifiAuth { AUTH_OPEN, AUTH_PSK, AUTH_ENTERPRISE };

struct WifiNet {
    const char* ssid;
    WifiAuth    auth;
    const char* user;  // WPA2-Enterprise identity; nullptr otherwise
    const char* pass;  // PSK / enterprise secret; "" for an open network
};

// Try each candidate in order; return true on the first that joins.
static bool connectToAnyWiFi() {
    bool primaryEnterprise = (strlen(nonEnterpriseWifiPassword) < 2);

    const WifiNet networks[] = {
        // Primary network injected from .env by pre_extra_script.py.
        { wifiSsid,
          primaryEnterprise ? AUTH_ENTERPRISE : AUTH_PSK,
          ucsdUsername,
          primaryEnterprise ? ucsdPassword.c_str() : nonEnterpriseWifiPassword },
        // RESNET device guest network (WPA-PSK).
        { "RESNET-GUEST-DEVICE", AUTH_PSK, nullptr, "ResnetConnect" },
        // Open campus guest fallback.
        { "UCSD-GUEST", AUTH_OPEN, nullptr, "" },
    };

    for (const WifiNet& net : networks) {
        bool ok = false;
        switch (net.auth) {
            case AUTH_OPEN:       ok = wifi.connectToWiFi(net.ssid, ""); break;
            case AUTH_PSK:        ok = wifi.connectToWiFi(net.ssid, net.pass); break;
            case AUTH_ENTERPRISE: ok = wifi.connectToWPAEnterprise(net.ssid, net.user, net.pass); break;
        }
        if (ok) return true;
    }

    Serial.println("[WiFi] No candidate network could be joined.");
    return false;
}

// Connectivity probe. Returns HTTP 204 with an empty body when the internet is
// reachable; a captive portal instead returns a redirect / login page. Lets us
// distinguish "associated with an AP" from "actually online" (e.g. UCSD-GUEST,
// which associates but blocks traffic behind an accept-terms portal).
static const char* CONNECTIVITY_URL = "http://connectivitycheck.gstatic.com/generate_204";

// Probe for real internet access. Returns true only if we get a clean 204.
static bool checkInternet() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Net] Not associated — skipping internet check.");
        return false;
    }

    Serial.printf("[Net] Checking internet via %s ...\n", CONNECTIVITY_URL);

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    // Don't auto-follow redirects: a redirect is the signal of a captive portal.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(CONNECTIVITY_URL)) {
        Serial.println("[Net]   RESULT: HTTP client init failed.");
        return false;
    }

    bool online = false;
    int code = http.GET();
    if (code == 204) {
        Serial.println("[Net]   RESULT: ONLINE — internet reachable (HTTP 204).");
        online = true;
    } else if (code == 301 || code == 302 || code == 307) {
        String loc = http.getLocation();
        Serial.printf("[Net]   RESULT: CAPTIVE PORTAL — redirected (HTTP %d) to %s\n",
                      code, loc.c_str());
        Serial.println("[Net]           Associated but not online (login/accept-terms required).");
    } else if (code > 0) {
        Serial.printf("[Net]   RESULT: CAPTIVE/UNEXPECTED — got HTTP %d (expected 204).\n", code);
    } else {
        Serial.printf("[Net]   RESULT: OFFLINE — request failed (%s).\n",
                      http.errorToString(code).c_str());
    }

    http.end();
    return online;
}


// WiFi bring-up strategy, in priority order:
//   1. Credentials previously saved through the captive portal (NVS). This is
//      how you switch networks without reflashing the board.
//   2. The built-in candidate list (connectToAnyWiFi): .env primary ->
//      RESNET guest -> UCSD-GUEST.
//   3. Recovery: launch the WiFiManager captive portal so a new network can be
//      entered from a browser; on success persist creds to NVS for step 1.
static void setupWiFi() {
    // 1. Portal-saved credentials (NVS). connectToWiFi handles open networks
    //    (empty pass) and enforces its own connect timeout.
    prefs.begin("wifi", /*readOnly=*/true);
    String savedSsid = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();
    if (savedSsid.length() > 0) {
        Serial.println("[WiFi] Trying saved (portal) credentials...");
        if (wifi.connectToWiFi(savedSsid, savedPass)) {
            return;
        }
    }

    // 2. Built-in candidate list.
    if (connectToAnyWiFi()) {
        return;
    }

    // 3. Nothing connected — launch the captive portal. On success, persist the
    //    entered credentials to NVS so the board reconnects automatically next
    //    boot (step 1). On timeout, restart to retry the whole sequence.
    Serial.printf("[WiFi] No known network — starting config portal \"%s\".\n", PORTAL_AP_NAME);
    WiFiManager wm;
    wm.setHostname(PORTAL_HOSTNAME);
    wm.setConfigPortalTimeout(180);  // give up after 3 min
    bool ok = wm.startConfigPortal(PORTAL_AP_NAME);
    if (ok && WiFi.status() == WL_CONNECTED) {
        prefs.begin("wifi", /*readOnly=*/false);
        prefs.putString("ssid", wm.getWiFiSSID());
        prefs.putString("pass", wm.getWiFiPass());
        prefs.end();
        Serial.println("[WiFi] Portal credentials saved to NVS.");
    } else {
        Serial.println("[WiFi] Config portal timed out — restarting.");
        ESP.restart();
    }
}


// Sensor

Adafruit_AMG88xx amg;
float pixels[AMG88xx_PIXEL_ARRAY_SIZE];  // 64 floats


// TFLite globals

constexpr int kTensorArenaSize = 8 * 1024;
alignas(16) uint8_t tensor_arena[kTensorArenaSize];

const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input_tensor = nullptr;
TfLiteTensor* output_tensor = nullptr;

float features[N_FEATURES];


// --- Legacy command state (REPLACED by autonomous event recording) ----------
// The device no longer waits for get_one / start_continuous / stop from the
// server. Kept here (commented out) for reference; see the event pipeline below.
//
// enum Mode { IDLE, GET_ONE, CONTINUOUS };
// volatile Mode currentMode = IDLE;


// --- Autonomous event recording ---------------------------------------------
// The board samples the AMG8833 once per second and runs presence inference. A
// debounced EMPTY->PRESENT transition starts an "event": a log message is sent
// immediately (so the website can notify at detection), then one frame per
// second is streamed until the area is empty (debounced) or the 5-minute cap is
// hit, after which an event_end message is sent.

static const uint32_t SAMPLE_INTERVAL_MS = 1000;    // 1 fps
static const uint32_t MAX_EVENT_MS        = 300000;  // 5 min hard cap
static const int      PRESENT_TO_START     = 2;      // consecutive present -> start
static const int      EMPTY_TO_END         = 10;     // consecutive empty  -> end

bool     eventActive     = false;
String   currentEventUid = "";
uint32_t eventStartMs    = 0;
int      frameSeq        = 0;
int      presentStreak   = 0;
int      emptyStreak     = 0;
uint32_t lastSampleMs    = 0;

// Arm/disarm. When disarmed the camera stops sampling and recording (a manual
// "off switch" from the web UI). Persisted in NVS so a disarmed device stays
// disarmed across reboots rather than silently re-arming.
bool armed = true;


// TFLite: setupModel

void setupModel() {
    model = tflite::GetModel(model_tflite);
    static tflite::AllOpsResolver resolver;
    static tflite::MicroErrorReporter micro_error_reporter;
    static tflite::MicroInterpreter micro_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize, &micro_error_reporter);
    interpreter = &micro_interpreter;
    interpreter->AllocateTensors();
    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);
    Serial.printf("[TFLite] Input type=%d, arena used=%d bytes\n",
                  input_tensor->type, interpreter->arena_used_bytes());
}


// TFLite: largestBlob

int largestBlob(float grid[8][8], float threshold) {
    bool visited[8][8] = {};
    int largest = 0;
    int qr[64], qc[64];

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (visited[r][c] || grid[r][c] <= threshold) continue;
            int size = 0, head = 0, tail = 0;
            qr[tail] = r; qc[tail] = c; tail++;
            visited[r][c] = true;
            while (head < tail) {
                int cr = qr[head], cc = qc[head]; head++;
                size++;
                const int dr[] = {-1, 1, 0, 0};
                const int dc[] = {0, 0, -1, 1};
                for (int d = 0; d < 4; d++) {
                    int nr = cr + dr[d], nc = cc + dc[d];
                    if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8
                        && !visited[nr][nc] && grid[nr][nc] > threshold) {
                        visited[nr][nc] = true;
                        qr[tail] = nr; qc[tail] = nc; tail++;
                    }
                }
            }
            if (size > largest) largest = size;
        }
    }
    return largest;
}


// TFLite: computeFeatures

void computeFeatures(float* raw_pixels, float* out_features) {
    float grid[8][8];
    for (int i = 0; i < 64; i++) grid[i / 8][i % 8] = raw_pixels[i];

    float sorted[64];
    memcpy(sorted, raw_pixels, 64 * sizeof(float));
    for (int i = 1; i < 64; i++) {
        float key = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    float median = (sorted[31] + sorted[32]) / 2.0f;
    float threshold = median + 3.0f;

    float sum_sq = 0.0f;
    float row_min = raw_pixels[0], row_max = raw_pixels[0];
    int count_above_3 = 0, count_above_5 = 0;

    for (int i = 0; i < 64; i++) {
        float diff = raw_pixels[i] - median;
        sum_sq += diff * diff;
        if (raw_pixels[i] < row_min) row_min = raw_pixels[i];
        if (raw_pixels[i] > row_max) row_max = raw_pixels[i];
        if (raw_pixels[i] > threshold)        count_above_3++;
        if (raw_pixels[i] > median + 5.0f)    count_above_5++;
    }
    float std_dev = sqrtf(sum_sq / 64.0f);
    if (std_dev < 0.1f) std_dev = 0.1f;

    for (int i = 0; i < 64; i++) out_features[i] = (raw_pixels[i] - median) / std_dev;

    out_features[64] = row_max;
    out_features[65] = row_max - row_min;
    out_features[66] = (float)count_above_3;
    out_features[67] = (float)count_above_5;

    float h_sum = 0.0f, v_sum = 0.0f;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 7; c++) h_sum += fabsf(grid[r][c+1] - grid[r][c]);
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 8; c++) v_sum += fabsf(grid[r+1][c] - grid[r][c]);
    out_features[68] = (h_sum / 56.0f + v_sum / 56.0f) / 2.0f;

    out_features[69] = (float)largestBlob(grid, threshold);

    float q[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) q[0] += grid[r][c];
    for (int r = 0; r < 4; r++) for (int c = 4; c < 8; c++) q[1] += grid[r][c];
    for (int r = 4; r < 8; r++) for (int c = 0; c < 4; c++) q[2] += grid[r][c];
    for (int r = 4; r < 8; r++) for (int c = 4; c < 8; c++) q[3] += grid[r][c];
    for (int i = 0; i < 4; i++) q[i] /= 16.0f;
    float q_mean = (q[0] + q[1] + q[2] + q[3]) / 4.0f;
    float q_var = 0.0f;
    for (int i = 0; i < 4; i++) q_var += (q[i] - q_mean) * (q[i] - q_mean);
    out_features[70] = q_var / 4.0f;

    float center_sum = 0.0f, outer_sum = 0.0f; int outer_count = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (r >= 2 && r < 6 && c >= 2 && c < 6) center_sum += grid[r][c];
            else { outer_sum += grid[r][c]; outer_count++; }
        }
    }
    out_features[71] = (center_sum / 16.0f) - (outer_sum / (float)outer_count);

    float row_maxes[8], col_maxes[8];
    for (int r = 0; r < 8; r++) {
        row_maxes[r] = grid[r][0];
        for (int c = 1; c < 8; c++) if (grid[r][c] > row_maxes[r]) row_maxes[r] = grid[r][c];
    }
    for (int c = 0; c < 8; c++) {
        col_maxes[c] = grid[0][c];
        for (int r = 1; r < 8; r++) if (grid[r][c] > col_maxes[c]) col_maxes[c] = grid[r][c];
    }
    float rm_mean = 0, cm_mean = 0;
    for (int i = 0; i < 8; i++) { rm_mean += row_maxes[i]; cm_mean += col_maxes[i]; }
    rm_mean /= 8.0f; cm_mean /= 8.0f;
    float rm_var = 0, cm_var = 0;
    for (int i = 0; i < 8; i++) {
        rm_var += (row_maxes[i] - rm_mean) * (row_maxes[i] - rm_mean);
        cm_var += (col_maxes[i] - cm_mean) * (col_maxes[i] - cm_mean);
    }
    out_features[72] = sqrtf(rm_var / 8.0f);
    out_features[73] = sqrtf(cm_var / 8.0f);
    out_features[74] = 0.0f;
    out_features[75] = 0.0f;

    for (int i = 0; i < N_FEATURES; i++)
        out_features[i] = (out_features[i] - SCALER_MEAN[i]) / SCALER_SCALE[i];
}


// TFLite: runInference

float runInference(float scaled_features[N_FEATURES]) {
    for (int i = 0; i < N_FEATURES; i++)
        input_tensor->data.f[i] = scaled_features[i];
    interpreter->Invoke();
    return output_tensor->data.f[0];  // 0.0 = empty, 1.0 = present
}


// --- Legacy single-reading publisher (REPLACED) ------------------------------
// Old command-driven path: published one reading to <TOPIC>/thermal on demand.
// Superseded by the event pipeline (startEvent / publishFrame / endEvent).
// Kept for reference.
//
// void sendReading() {
//     amg.readPixels(pixels);
//     float thermistor = amg.readThermistor();
//     computeFeatures(pixels, features);
//     float confidence = runInference(features);
//     bool present = confidence > 0.5f;
//     String payload = "";
//     payload.reserve(700);
//     payload += "{\"mac_address\":\"";
//     payload += WiFi.macAddress();
//     payload += "\",\"pixels\":[";
//     for (int i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
//         payload += String(pixels[i], 2);
//         if (i < AMG88xx_PIXEL_ARRAY_SIZE - 1) payload += ",";
//     }
//     payload += "],\"thermistor\":";
//     payload += String(thermistor, 2);
//     payload += ",\"prediction\":\"";
//     payload += present ? "PRESENT" : "EMPTY";
//     payload += "\",\"confidence\":";
//     payload += String(confidence, 4);
//     payload += "}";
//     mqtt.publishMessage("thermal", payload);
// }
//
// Old command callback: server told the board what to do. No longer subscribed.
//
// void mqttCallback(char* topic, byte* payload, unsigned int length) {
//     String msg;
//     for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
//     if (msg == "get_one") currentMode = GET_ONE;
//     else if (msg == "start_continuous") currentMode = CONTINUOUS;
//     else if (msg == "stop") currentMode = IDLE;
// }


// Event lifecycle publishers

// Announce a new presence event. Sent immediately on detection so the server
// (and website) can notify right away, before any frames arrive.
void startEvent(float triggerConfidence, float audioLevel) {
    currentEventUid = WiFi.macAddress() + "-" + String(millis());
    eventActive  = true;
    eventStartMs = millis();
    frameSeq     = 0;

    String payload = "";
    payload.reserve(200);
    payload += "{\"type\":\"event_start\",\"event_uid\":\"";
    payload += currentEventUid;
    payload += "\",\"mac_address\":\"";
    payload += WiFi.macAddress();
    payload += "\",\"trigger_confidence\":";
    payload += String(triggerConfidence, 4);
    payload += ",\"audio_level\":";
    payload += String(audioLevel, 4);
    payload += "}";

    mqtt.publishMessage("event", payload);
    Serial.printf("[Event] START %s (conf=%.3f)\n", currentEventUid.c_str(), triggerConfidence);
}

// Stream one thermal frame belonging to the active event.
void publishFrame(float thermistor, float confidence, float audioLevel) {
    String payload = "";
    payload.reserve(700);
    payload += "{\"event_uid\":\"";
    payload += currentEventUid;
    payload += "\",\"seq\":";
    payload += String(frameSeq);
    payload += ",\"pixels\":[";
    for (int i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
        payload += String(pixels[i], 2);
        if (i < AMG88xx_PIXEL_ARRAY_SIZE - 1) payload += ",";
    }
    payload += "],\"thermistor\":";
    payload += String(thermistor, 2);
    payload += ",\"confidence\":";
    payload += String(confidence, 4);
    payload += ",\"audio_level\":";
    payload += String(audioLevel, 4);
    payload += "}";

    mqtt.publishMessage("frame", payload);
    frameSeq++;
}

// Close out the active event.
void endEvent(const char* reason) {
    String payload = "";
    payload.reserve(160);
    payload += "{\"type\":\"event_end\",\"event_uid\":\"";
    payload += currentEventUid;
    payload += "\",\"frame_count\":";
    payload += String(frameSeq);
    payload += "}";

    mqtt.publishMessage("event", payload);
    Serial.printf("[Event] END   %s (%d frames, %s)\n",
                  currentEventUid.c_str(), frameSeq, reason);

    eventActive     = false;
    currentEventUid = "";
    presentStreak   = 0;
    emptyStreak     = 0;
}


// Arm/disarm state

// Report the current armed state so the server + website can reflect the
// device's real status (not just an optimistic UI guess).
void publishState() {
    String payload = "{\"type\":\"state\",\"mac_address\":\"";
    payload += WiFi.macAddress();
    payload += "\",\"armed\":";
    payload += armed ? "true" : "false";
    payload += "}";
    mqtt.publishMessage("event", payload);
}

void setArmed(bool value) {
    if (value != armed) {
        armed = value;
        prefs.begin("cam", /*readOnly=*/false);
        prefs.putBool("armed", armed);
        prefs.end();
        // Disarming mid-event: close it out cleanly.
        if (!armed && eventActive) endEvent("disarmed");
        presentStreak = 0;
        emptyStreak   = 0;
        Serial.printf("[Cam] %s\n", armed ? "ARMED" : "DISARMED");
    }
    publishState();  // always confirm, even on a no-op, so the UI syncs
}

// Command callback: the web "off switch" publishes arm / disarm here.
void commandCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();

    if (msg == "disarm")   setArmed(false);
    else if (msg == "arm") setArmed(true);
    else Serial.printf("[Cam] Ignoring unknown command: %s\n", msg.c_str());
}


// Setup

void setup() {
    Serial.begin(115200);
    delay(2000);

    // WiFi
    Serial.println("[Setup] Connecting to WiFi...");
    setupWiFi();
    Serial.print("[Setup] MAC address: "); Serial.println(WiFi.macAddress());

    // Verify real internet access before attempting MQTT. A captive portal
    // (e.g. UCSD-GUEST) can associate us without letting TLS traffic through,
    // in which case the MQTT connect below would loop indefinitely.
    if (!checkInternet()) {
        Serial.println("[Setup] WARNING: no internet path — MQTT connect may stall "
                       "(captive portal / offline).");
    }

    // MQTT
    while (!mqtt.connectToBroker()) {
        Serial.println("[Setup] MQTT connect failed, retrying...");
        delay(1000);
    }
    // Restore armed state from NVS (default armed) and subscribe to the
    // arm/disarm command channel (the web "off switch").
    prefs.begin("cam", /*readOnly=*/true);
    armed = prefs.getBool("armed", true);
    prefs.end();
    mqtt.setCallback(commandCallback);
    mqtt.subscribeTopic("command");

    // AMG8833 sensor
    Wire.begin();
    if (!amg.begin()) {
        while (1) {
            Serial.println("[ERROR] AMG8833 not detected!");
            delay(1000);
        }
    }
    delay(100);

    // Optional INMP441 microphone (redundant — firmware runs fine without it).
    if (mic.begin()) {
        Serial.println("[Setup] Microphone available.");
    } else {
        Serial.println("[Setup] Microphone not available — continuing without it.");
    }

    // TFLite model
    setupModel();

    publishState();  // announce armed/disarmed so the dashboard starts in sync
    Serial.printf("[Setup] Ready — monitoring %s.\n", armed ? "ARMED" : "DISARMED");
}


// Loop — autonomous presence monitoring + event recording.
//
// Runs the MQTT client continuously, but only samples the sensor once per
// SAMPLE_INTERVAL_MS. Each sample runs inference; a debounced presence signal
// drives the event state machine.

void loop() {
    mqtt.loop();  // keep the MQTT connection alive + process arm/disarm commands

    // Disarmed: skip all sensing/recording but stay responsive to commands.
    if (!armed) return;

    uint32_t now = millis();
    if (now - lastSampleMs < SAMPLE_INTERVAL_MS) return;
    lastSampleMs = now;

    // One thermal sample + inference (+ optional sound level).
    amg.readPixels(pixels);
    float thermistor = amg.readThermistor();
    computeFeatures(pixels, features);
    float confidence = runInference(features);
    bool  present    = confidence > 0.5f;
    float audioLevel = mic.readLevel();  // 0.0 when mic unavailable

    // Debounce streaks.
    if (present) { presentStreak++; emptyStreak = 0; }
    else         { emptyStreak++;   presentStreak = 0; }

    Serial.printf("[%s] conf=%.3f therm=%.2fC audio=%.3f %s\n",
                  present ? "PRESENT" : "EMPTY  ", confidence, thermistor, audioLevel,
                  eventActive ? "(recording)" : "");

    if (!eventActive) {
        // Idle: start an event once presence is confirmed.
        if (presentStreak >= PRESENT_TO_START) {
            startEvent(confidence, audioLevel);
            publishFrame(thermistor, confidence, audioLevel);  // frame 0 = trigger frame
        }
    } else {
        // Recording: stream frames, then close on empty debounce or 5-min cap.
        publishFrame(thermistor, confidence, audioLevel);

        if (emptyStreak >= EMPTY_TO_END) {
            endEvent("empty");
        } else if (now - eventStartMs >= MAX_EVENT_MS) {
            endEvent("max_duration");
        }
    }
}
