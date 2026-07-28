#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <vector>

/**
 * @brief Thin PubSubClient wrapper for MQTT communication with a broker.
 *
 * Initialized with a client ID and a topic prefix used for all publishes and
 * subscribes. Survives automatic reconnects by restoring the callback and
 * re-subscribing to remembered topics.
 */
class MqttClient {
public:
    /**
     * @brief Construct a new MqttClient object
     *
     * @param clientId Unique identifier for this MQTT client
     * @param topicPrefix Prefix for all topics published by this client
     */
    MqttClient(String clientId, String topicPrefix);

    /**
     * @brief Connect to the MQTT broker
     *
     * @param port The port number (default: 1883 for non-TLS)
     * @return true if connection successful
     * @return false if connection failed
     */
    bool connectToBroker(int port = 1883);

    /**
     * @brief Publish a message to a specific topic
     *
     * @param subtopic The subtopic to publish to (will be appended to topicPrefix)
     * @param message The message to publish
     * @return true if publish successful
     * @return false if publish failed
     */
    bool publishMessage(String subtopic, String message);

    /**
     * @brief Subscribe to a specific topic
     *
     * @param subtopic The subtopic to subscribe to
     * @return true if subscription successful
     * @return false if subscription failed
     */
    bool subscribeTopic(String subtopic);

    /**
     * @brief Set callback for receiving messages
     *
     * @param callback Function to be called when message is received
     */
    void setCallback(void (*callback)(char*, uint8_t*, unsigned int));

    /**
     * @brief Handle MQTT loop
     * Must be called regularly to maintain connection and process messages
     */
    void loop();

private:
    WiFiClient _wifiClient;
    PubSubClient* _mqttClient;
    String _clientId;
    String _topicPrefix;
    const char* _broker = "broker.emqx.io";
    bool _isTLS;

    // Remembered so they survive an automatic reconnect: the broker forgets
    // subscriptions on disconnect, and a fresh session needs the callback set.
    void (*_callback)(char*, uint8_t*, unsigned int) = nullptr;
    std::vector<String> _subscriptions;
    int _port = 1883;

    void _setupMQTTClient(int port);
    void _restoreSession();
};

#endif
