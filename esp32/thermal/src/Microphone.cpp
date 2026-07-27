#include "Microphone.h"

// The mic is optional. When USE_MIC is not defined we compile a no-op stub so
// the rest of the firmware links and runs unchanged with no mic attached.

#ifdef USE_MIC

#include <driver/i2s_std.h>
#include <math.h>

// One I2S RX channel handle for the INMP441.
static i2s_chan_handle_t s_rx_chan = nullptr;

// How many 32-bit samples to average per readLevel() call. 256 samples at
// 16 kHz is ~16 ms — long enough for a stable RMS, short enough to be cheap.
static const int   MIC_BLOCK_SAMPLES = 256;
static const int   MIC_SAMPLE_RATE   = 16000;

// INMP441 is a 24-bit device left-justified in a 32-bit slot. Full-scale is
// therefore ~2^23. We normalize RMS against that to land roughly in [0, 1].
static const float MIC_FULL_SCALE    = 8388608.0f;  // 2^23

Microphone::Microphone() : _available(false) {}

bool Microphone::begin() {
    _available = false;

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, nullptr, &s_rx_chan) != ESP_OK) {
        Serial.println("[Mic] i2s_new_channel failed — mic disabled.");
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)MIC_SCK,
            .ws   = (gpio_num_t)MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    // INMP441 with L/R -> GND drives the left slot only.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    if (i2s_channel_init_std_mode(s_rx_chan, &std_cfg) != ESP_OK) {
        Serial.println("[Mic] i2s_channel_init_std_mode failed — mic disabled.");
        i2s_del_channel(s_rx_chan);
        s_rx_chan = nullptr;
        return false;
    }
    if (i2s_channel_enable(s_rx_chan) != ESP_OK) {
        Serial.println("[Mic] i2s_channel_enable failed — mic disabled.");
        i2s_del_channel(s_rx_chan);
        s_rx_chan = nullptr;
        return false;
    }

    _available = true;
    Serial.printf("[Mic] INMP441 ready (WS=%d SCK=%d SD=%d).\n",
                  MIC_WS, MIC_SCK, MIC_SD);
    return true;
}

float Microphone::readLevel() {
    if (!_available || s_rx_chan == nullptr) return 0.0f;

    static int32_t buf[MIC_BLOCK_SAMPLES];
    size_t bytes_read = 0;
    if (i2s_channel_read(s_rx_chan, buf, sizeof(buf), &bytes_read, 100) != ESP_OK
        || bytes_read == 0) {
        return 0.0f;
    }

    int n = bytes_read / sizeof(int32_t);
    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        // Sample is left-justified: shift down to recover the 24-bit value.
        int32_t s = buf[i] >> 8;
        double v = (double)s;
        sum_sq += v * v;
    }
    float rms = (n > 0) ? (float)sqrt(sum_sq / n) : 0.0f;
    float level = rms / MIC_FULL_SCALE;
    if (level > 1.0f) level = 1.0f;
    return level;
}

#else  // USE_MIC not defined — stub implementation.

Microphone::Microphone() : _available(false) {}
bool  Microphone::begin()     { Serial.println("[Mic] Compiled out (USE_MIC undefined)."); return false; }
float Microphone::readLevel() { return 0.0f; }

#endif  // USE_MIC
