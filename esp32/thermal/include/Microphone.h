#ifndef Microphone_h
#define Microphone_h

#include <Arduino.h>

/**
 * @brief Optional INMP441 I2S MEMS microphone.
 *
 * This is a *redundant* sensor: the firmware must run correctly whether or not
 * the mic is wired in. The whole implementation is compiled only when USE_MIC
 * is defined; when it is not, begin() reports unavailable and readLevel()
 * returns 0.0f so callers need no #ifdefs of their own.
 *
 * Wiring (INMP441, tie L/R -> GND for the left channel):
 *   VDD -> 3V3, GND -> GND, WS -> MIC_WS, SCK -> MIC_SCK, SD -> MIC_SD.
 *
 * Pins default below and can be overridden with -D build flags (see .env /
 * pre_extra_script.py).
 */

#ifndef MIC_WS
#define MIC_WS  12   // word-select / LRCL
#endif
#ifndef MIC_SCK
#define MIC_SCK 13   // bit clock / BCLK
#endif
#ifndef MIC_SD
#define MIC_SD  14   // serial data out / DOUT
#endif

class Microphone {
public:
    Microphone();

    /**
     * @brief Initialise the I2S RX channel for the INMP441.
     * @return true if the channel came up (mic assumed present), false
     *         otherwise. When USE_MIC is not defined this always returns false.
     */
    bool begin();

    /**
     * @brief Whether begin() succeeded and readings are meaningful.
     */
    bool available() const { return _available; }

    /**
     * @brief Read a short block of samples and return the normalized RMS
     *        amplitude in [0.0, 1.0]. Returns 0.0f if the mic is unavailable.
     */
    float readLevel();

private:
    bool _available;
};

#endif
