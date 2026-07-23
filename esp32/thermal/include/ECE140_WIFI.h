#ifndef ECE140_WIFI_h
#define ECE140_WIFI_h

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include "esp_wifi.h"
#include <lwip/dns.h>

/**
 * @brief This is the class to connect to a wifi network.
 *
 * You can either connect to a regular wifi network or a WPA Enterprise network.
 */
class ECE140_WIFI {
public:
  /**
   * @brief Construct a new ECE140_WIFI object
   */
  ECE140_WIFI();

  /**
   * @brief Connect to a regular WiFi network. An empty password joins an
   *        open network (single-arg begin); otherwise WPA-PSK is used.
   *
   * @param ssid The SSID of the WiFi network
   * @param password The password of the WiFi network ("" for an open network)
   * @return true once associated with an IP, false if the timeout elapsed
   */
  bool connectToWiFi(String ssid, String password);

  /**
   * @brief Connect to a WPA Enterprise network
   *
   * @param ssid The SSID of the WiFi network
   * @param username The username for WPA Enterprise
   * @param password The password for WPA Enterprise
   * @return true once associated with an IP, false if the timeout elapsed
   */
  bool connectToWPAEnterprise(String ssid, String username, String password);
};

#endif
