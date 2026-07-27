#ifndef WIFI_CONNECTION_H
#define WIFI_CONNECTION_H

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wpa2.h"
#include "esp_wifi.h"
#include <lwip/dns.h>

/**
 * @brief Connects the device to a WiFi network.
 *
 * Supports open, WPA-PSK, and WPA2-Enterprise networks. Each connect attempt
 * is bounded by a timeout so callers can fail over to another network.
 */
class WifiConnection {
public:
  /**
   * @brief Construct a new WifiConnection object
   */
  WifiConnection();

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
