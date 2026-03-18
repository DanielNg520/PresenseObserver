// Tech Assignment 7 — Thermal Presence Detection with MQTT
//
// Combines:
//   - WiFi + MQTT from TA4 challenge2 / TA7 starter
//   - TFLite inference (setupModel, computeFeatures, runInference) from TA6 lab_challenge
//   - Full JSON payload (mac_address, pixels, thermistor, prediction, confidence)
//   - Command handling: get_one, start_continuous, stop

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_AMG88xx.h>

#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "ECE140_WIFI.h"
#include "ECE140_MQTT.h"
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

ECE140_WIFI wifi;
ECE140_MQTT mqtt(CLIENT_ID, TOPIC_PREFIX);


// Sensor

Adafruit_AMG88xx amg;
float pixels[AMG88xx_PIXEL_ARRAY_SIZE];  // 64 floats


// TFLite globals (from TA6 lab_challenge)

constexpr int kTensorArenaSize = 8 * 1024;
alignas(16) uint8_t tensor_arena[kTensorArenaSize];

const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* input_tensor = nullptr;
TfLiteTensor* output_tensor = nullptr;

float features[N_FEATURES];


// Command state

enum Mode { IDLE, GET_ONE, CONTINUOUS };
volatile Mode currentMode = IDLE;


// TFLite: setupModel (reused from TA6)

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


// TFLite: largestBlob (reused from TA6)

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


// TFLite: computeFeatures (reused from TA6)

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


// TFLite: runInference (reused from TA6)

float runInference(float scaled_features[N_FEATURES]) {
    for (int i = 0; i < N_FEATURES; i++)
        input_tensor->data.f[i] = scaled_features[i];
    interpreter->Invoke();
    return output_tensor->data.f[0];  // 0.0 = empty, 1.0 = present
}


// Build and send one reading

void sendReading() {
    amg.readPixels(pixels);
    float thermistor = amg.readThermistor();

    computeFeatures(pixels, features);
    float confidence = runInference(features);
    bool present = confidence > 0.5f;

    Serial.printf("[%s] conf=%.3f therm=%.2fC\n",
                  present ? "PRESENT" : "EMPTY  ", confidence, thermistor);

    // Build JSON payload matching server's expected format
    String payload = "";
    payload.reserve(700);
    payload += "{\"mac_address\":\"";
    payload += WiFi.macAddress();
    payload += "\",\"pixels\":[";
    for (int i = 0; i < AMG88xx_PIXEL_ARRAY_SIZE; i++) {
        payload += String(pixels[i], 2);
        if (i < AMG88xx_PIXEL_ARRAY_SIZE - 1) payload += ",";
    }
    payload += "],\"thermistor\":";
    payload += String(thermistor, 2);
    payload += ",\"prediction\":\"";
    payload += present ? "PRESENT" : "EMPTY";
    payload += "\",\"confidence\":";
    payload += String(confidence, 4);
    payload += "}";

    mqtt.publishMessage("thermal", payload);
}


// MQTT command callback (pattern from TA4 challenge2)

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

    Serial.print("[MQTT] Command: "); Serial.println(msg);

    if (msg == "get_one") {
        currentMode = GET_ONE;
    } else if (msg == "start_continuous") {
        currentMode = CONTINUOUS;
    } else if (msg == "stop") {
        currentMode = IDLE;
    }
}


// Setup

void setup() {
    Serial.begin(115200);
    delay(2000);

    // WiFi (pattern from TA7 starter + TA4 challenge2)
    Serial.println("[Setup] Connecting to WiFi...");
    if (strlen(nonEnterpriseWifiPassword) < 2) {
        wifi.connectToWPAEnterprise(wifiSsid, ucsdUsername, ucsdPassword);
    } else {
        wifi.connectToWiFi(wifiSsid, nonEnterpriseWifiPassword);
    }
    Serial.print("[Setup] MAC address: "); Serial.println(WiFi.macAddress());

    // MQTT (pattern from TA4 challenge2)
    while (!mqtt.connectToBroker()) {
        Serial.println("[Setup] MQTT connect failed, retrying...");
        delay(1000);
    }
    mqtt.setCallback(mqttCallback);
    mqtt.subscribeTopic("command");

    // AMG8833 sensor (pattern from TA4 challenge2)
    Wire.begin();
    if (!amg.begin()) {
        while (1) {
            Serial.println("[ERROR] AMG8833 not detected!");
            delay(1000);
        }
    }
    delay(100);

    // TFLite model (from TA6 lab_challenge)
    setupModel();

    Serial.println("[Setup] Ready — waiting for commands");
}


// Loop

void loop() {
    mqtt.loop();

    if (currentMode == GET_ONE) {
        sendReading();
        currentMode = IDLE;
    } else if (currentMode == CONTINUOUS) {
        sendReading();
        delay(1000);
    }
}
