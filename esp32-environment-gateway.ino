#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>

#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "secrets.h"


// =============================================================================
// DEVICE CONFIGURATION
// =============================================================================

static constexpr char DEVICE_NAME[] = "environment-sensor-1";


// =============================================================================
// BME280 CONFIGURATION
// =============================================================================

static constexpr uint8_t I2C_SDA_PIN = D4;
static constexpr uint8_t I2C_SCL_PIN = D5;

static constexpr uint8_t SENSOR_I2C_ADDRESS = 0x76;

static constexpr uint32_t BME_READ_INTERVAL_MS = 30000;

// Continue sampling every 30 seconds, but limit MQTT writes.
// 0 change threshold means periodic-only reporting (used for RSSI).
static constexpr uint32_t SENSOR_HEARTBEAT_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t BATTERY_HEARTBEAT_MS = 60UL * 60UL * 1000UL;

static constexpr float TEMPERATURE_CHANGE_C = 0.1F;
static constexpr float HUMIDITY_CHANGE_PERCENT = 1.0F;
static constexpr float PRESSURE_CHANGE_HPA = 0.5F;
static constexpr float BATTERY_VOLTAGE_CHANGE_V = 0.05F;


// =============================================================================
// XIAOMI LYWSD03MMC / BTHOME CONFIGURATION
// =============================================================================

// Xiaomi MAC:
// A4:C1:38:1D:DB:29
//
// NimBLE's uint64_t representation uses the normal displayed byte order.

static constexpr uint64_t XIAOMI_MAC =
    0xA4C1381DDB29ULL;

// BTHome v2 service UUID.

static const NimBLEUUID BTHOME_SERVICE_UUID(
    static_cast<uint16_t>(0xFCD2)
);

// BLE scan parameters.
//
// Scan forever, but use an 80% duty cycle to leave some radio time available
// for Wi-Fi coexistence.
//
// There are NO multi-second pauses between scans.

static constexpr uint16_t BLE_SCAN_INTERVAL_MS = 100;
static constexpr uint16_t BLE_SCAN_WINDOW_MS   = 80;

// If we hear nothing from the Xiaomi for five minutes, consider it offline.
//
// With roughly 10-second advertisements, this allows many missed packets
// before declaring the sensor unavailable.

static constexpr uint32_t XIAOMI_OFFLINE_TIMEOUT_MS =
    5UL * 60UL * 1000UL;

// If the BLE scanner unexpectedly stops, retry after this interval.

static constexpr uint32_t BLE_RESTART_INTERVAL_MS = 1000;


// =============================================================================
// NETWORK RETRY CONFIGURATION
// =============================================================================

static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
static constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;


// =============================================================================
// MQTT TOPICS
// =============================================================================

// Gateway availability

static constexpr char MQTT_STATUS_TOPIC[] =
    "home/environment/environment-sensor-1/status";


// -----------------------------------------------------------------------------
// BME280
// -----------------------------------------------------------------------------

static constexpr char MQTT_BME_TEMPERATURE_TOPIC[] =
    "home/environment/environment-sensor-1/bme280/temperature";

static constexpr char MQTT_BME_HUMIDITY_TOPIC[] =
    "home/environment/environment-sensor-1/bme280/humidity";

static constexpr char MQTT_BME_PRESSURE_TOPIC[] =
    "home/environment/environment-sensor-1/bme280/pressure";


// -----------------------------------------------------------------------------
// Xiaomi LYWSD03MMC
// -----------------------------------------------------------------------------

static constexpr char MQTT_XIAOMI_STATUS_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/status";

static constexpr char MQTT_XIAOMI_TEMPERATURE_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/temperature";

static constexpr char MQTT_XIAOMI_HUMIDITY_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/humidity";

static constexpr char MQTT_XIAOMI_BATTERY_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/battery";

static constexpr char MQTT_XIAOMI_BATTERY_VOLTAGE_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/battery_voltage";

static constexpr char MQTT_XIAOMI_RSSI_TOPIC[] =
    "home/environment/environment-sensor-1/xiaomi/rssi";


// =============================================================================
// DATA STRUCTURES
// =============================================================================

struct EnvironmentReading
{
    float temperatureC;
    float humidityPercent;
    float pressureHpa;
};


struct XiaomiReading
{
    float temperatureC;
    float humidityPercent;

    uint8_t batteryPercent;

    float batteryVoltage;

    int8_t rssi;

    uint8_t packetId;

    uint32_t receivedAtMs;

    bool haveTemperature;
    bool haveHumidity;
    bool haveBattery;
    bool haveBatteryVoltage;
    bool havePacketId;
};


// =============================================================================
// HARDWARE / NETWORK OBJECTS
// =============================================================================

Adafruit_BME280 bme;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

NimBLEScan *bleScan = nullptr;


// =============================================================================
// BLE -> MAIN LOOP QUEUE
//
// BLE callbacks run in the NimBLE task context.
//
// Rather than performing Serial or MQTT work inside that callback, we decode
// the packet there and place the latest result in a one-element queue.
//
// The main Arduino loop consumes the queue and performs all MQTT operations.
//
// A queue length of one is deliberate: if several advertisements arrive while
// the main loop is busy, only the newest reading matters.
// =============================================================================

QueueHandle_t xiaomiQueue = nullptr;

// Last successfully published values and their timestamps, tracked per topic.
// Reading new sensor data never resets these publication checkpoints.
struct PublishState
{
    bool valid = false;
    double value = 0.0;
    uint32_t atMs = 0;
};

PublishState bmeTemperatureSent, bmeHumiditySent, bmePressureSent;
PublishState xiaomiTemperatureSent, xiaomiHumiditySent;
PublishState xiaomiBatterySent, xiaomiBatteryVoltageSent, xiaomiRssiSent;


// =============================================================================
// STATE
// =============================================================================

// -----------------------------------------------------------------------------
// BME280
// -----------------------------------------------------------------------------

uint32_t lastBmeReadTime = 0;

EnvironmentReading latestBmeReading;
bool haveBmeReading = false;


// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------

uint32_t lastWifiAttemptTime = 0;
bool wifiWasConnected = false;
bool otaStarted = false;


// -----------------------------------------------------------------------------
// MQTT
// -----------------------------------------------------------------------------

uint32_t lastMqttAttemptTime = 0;


// -----------------------------------------------------------------------------
// BLE
// -----------------------------------------------------------------------------

volatile bool bleScanEnded = false;
uint32_t lastBleRestartAttemptTime = 0;


// -----------------------------------------------------------------------------
// Xiaomi
// -----------------------------------------------------------------------------

XiaomiReading latestXiaomiReading;

bool haveXiaomiReading = false;
bool xiaomiOnline = false;

uint32_t xiaomiLastSeenTime = 0;

bool haveLastXiaomiPacketId = false;
uint8_t lastXiaomiPacketId = 0;


// =============================================================================
// LITTLE-ENDIAN HELPERS
// =============================================================================

static uint16_t readUInt16LE(
    const uint8_t *data
)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8)
    );
}


static int16_t readInt16LE(
    const uint8_t *data
)
{
    return static_cast<int16_t>(
        readUInt16LE(data)
    );
}


// =============================================================================
// BTHOME DECODER
// =============================================================================
//
// NimBLE's getServiceData() returns the payload AFTER the 0xFCD2 UUID.
//
// For our observed Xiaomi packet:
//
// 40
// 00 3F
// 01 5A
// 02 F7 08
// 03 15 15
//
// 40          BTHome v2 device-info byte
// 00 3F       packet ID 63
// 01 5A       battery 90%
// 02 F7 08    temperature 22.95 C
// 03 15 15    humidity 53.97%
//
// This decoder is intentionally written as a real BTHome object parser rather
// than relying on fixed byte offsets.
// =============================================================================

bool decodeBTHome(
    const std::string &serviceData,
    XiaomiReading &reading
)
{
    const uint8_t *data =
        reinterpret_cast<const uint8_t *>(
            serviceData.data()
        );

    const size_t length =
        serviceData.size();

    if (length < 1)
    {
        return false;
    }


    // -------------------------------------------------------------------------
    // Device information byte
    // -------------------------------------------------------------------------

    const uint8_t deviceInfo = data[0];

    const bool encrypted =
        (deviceInfo & 0x01) != 0;

    const uint8_t version =
        (deviceInfo >> 5) & 0x07;

    // We expect BTHome v2 and deliberately configured encryption OFF.

    if (version != 2)
    {
        return false;
    }

    if (encrypted)
    {
        return false;
    }


    // -------------------------------------------------------------------------
    // Initialise result
    // -------------------------------------------------------------------------

    reading.temperatureC = 0.0F;
    reading.humidityPercent = 0.0F;

    reading.batteryPercent = 0;
    reading.batteryVoltage = 0.0F;

    reading.packetId = 0;

    reading.haveTemperature = false;
    reading.haveHumidity = false;
    reading.haveBattery = false;
    reading.haveBatteryVoltage = false;
    reading.havePacketId = false;


    // First object begins immediately after device-info byte.

    size_t index = 1;


    // -------------------------------------------------------------------------
    // Parse BTHome objects
    // -------------------------------------------------------------------------

    while (index < length)
    {
        const uint8_t objectId =
            data[index++];

        switch (objectId)
        {
            // -----------------------------------------------------------------
            // 0x00: packet ID
            // uint8
            // -----------------------------------------------------------------

            case 0x00:
            {
                if (index + 1 > length)
                {
                    return false;
                }

                reading.packetId =
                    data[index];

                reading.havePacketId = true;

                index += 1;

                break;
            }


            // -----------------------------------------------------------------
            // 0x01: battery percentage
            // uint8, factor 1
            // -----------------------------------------------------------------

            case 0x01:
            {
                if (index + 1 > length)
                {
                    return false;
                }

                reading.batteryPercent =
                    data[index];

                reading.haveBattery = true;

                index += 1;

                break;
            }


            // -----------------------------------------------------------------
            // 0x02: temperature
            // signed uint16 LE, factor 0.01 C
            // -----------------------------------------------------------------

            case 0x02:
            {
                if (index + 2 > length)
                {
                    return false;
                }

                const int16_t raw =
                    readInt16LE(
                        &data[index]
                    );

                reading.temperatureC =
                    static_cast<float>(raw) * 0.01F;

                reading.haveTemperature = true;

                index += 2;

                break;
            }


            // -----------------------------------------------------------------
            // 0x03: humidity
            // uint16 LE, factor 0.01 %
            // -----------------------------------------------------------------

            case 0x03:
            {
                if (index + 2 > length)
                {
                    return false;
                }

                const uint16_t raw =
                    readUInt16LE(
                        &data[index]
                    );

                reading.humidityPercent =
                    static_cast<float>(raw) * 0.01F;

                reading.haveHumidity = true;

                index += 2;

                break;
            }


            // -----------------------------------------------------------------
            // 0x0C: battery voltage
            // uint16 LE, factor 0.001 V
            //
            // Your current Xiaomi packet doesn't include this, but supporting
            // it costs almost nothing and makes the decoder useful if the
            // firmware configuration later begins advertising voltage.
            // -----------------------------------------------------------------

            case 0x0C:
            {
                if (index + 2 > length)
                {
                    return false;
                }

                const uint16_t raw =
                    readUInt16LE(
                        &data[index]
                    );

                reading.batteryVoltage =
                    static_cast<float>(raw) * 0.001F;

                reading.haveBatteryVoltage = true;

                index += 2;

                break;
            }


            // -----------------------------------------------------------------
            // Unknown object
            //
            // BTHome objects are ordered by object ID. Our useful objects
            // (battery/temp/humidity) are at the beginning, so if newer
            // firmware appends another type we don't understand, preserve
            // what we have already decoded rather than guessing its length.
            // -----------------------------------------------------------------

            default:
            {
                index = length;
                break;
            }
        }
    }


    // Temperature and humidity are the two values required for this sensor.

    return (
        reading.haveTemperature &&
        reading.haveHumidity
    );
}


// =============================================================================
// BME280 ACQUISITION
// =============================================================================

bool readBme280(
    EnvironmentReading &reading
)
{
    if (!bme.takeForcedMeasurement())
    {
        Serial.println(
            "ERROR: BME280 measurement failed"
        );

        return false;
    }

    reading.temperatureC =
        bme.readTemperature();

    reading.humidityPercent =
        bme.readHumidity();

    reading.pressureHpa =
        bme.readPressure() / 100.0F;

    if (
        isnan(reading.temperatureC) ||
        isnan(reading.humidityPercent) ||
        isnan(reading.pressureHpa)
    )
    {
        Serial.println(
            "ERROR: Invalid BME280 reading"
        );

        return false;
    }

    return true;
}


// =============================================================================
// SERIAL OUTPUT
// =============================================================================

void printBmeReading(
    const EnvironmentReading &reading
)
{
    Serial.println();
    Serial.println("BME280 reading");
    Serial.println("--------------");

    Serial.print("Temperature: ");
    Serial.print(reading.temperatureC, 2);
    Serial.println(" °C");

    Serial.print("Humidity:    ");
    Serial.print(reading.humidityPercent, 2);
    Serial.println(" %");

    Serial.print("Pressure:    ");
    Serial.print(reading.pressureHpa, 2);
    Serial.println(" hPa");
}


void printXiaomiReading(
    const XiaomiReading &reading
)
{
    Serial.println();
    Serial.println(
        "Xiaomi LYWSD03MMC reading"
    );

    Serial.println(
        "-------------------------"
    );

    Serial.print("Temperature: ");
    Serial.print(reading.temperatureC, 2);
    Serial.println(" °C");

    Serial.print("Humidity:    ");
    Serial.print(reading.humidityPercent, 2);
    Serial.println(" %");

    if (reading.haveBattery)
    {
        Serial.print("Battery:     ");
        Serial.print(reading.batteryPercent);
        Serial.println(" %");
    }

    if (reading.haveBatteryVoltage)
    {
        Serial.print("Battery V:   ");
        Serial.print(reading.batteryVoltage, 3);
        Serial.println(" V");
    }

    Serial.print("RSSI:        ");
    Serial.print(reading.rssi);
    Serial.println(" dBm");

    if (reading.havePacketId)
    {
        Serial.print("Packet ID:   ");
        Serial.println(reading.packetId);
    }
}


// =============================================================================
// MQTT HELPERS
// =============================================================================

bool mqttPublishFloat(
    const char *topic,
    float value,
    uint8_t decimals = 2
)
{
    char payload[24];

    snprintf(
        payload,
        sizeof(payload),
        "%.*f",
        decimals,
        value
    );

    return mqttClient.publish(
        topic,
        payload,
        true
    );
}


bool mqttPublishUInt(
    const char *topic,
    uint32_t value
)
{
    char payload[16];

    snprintf(
        payload,
        sizeof(payload),
        "%lu",
        static_cast<unsigned long>(value)
    );

    return mqttClient.publish(
        topic,
        payload,
        true
    );
}


bool mqttPublishInt(
    const char *topic,
    int32_t value
)
{
    char payload[16];

    snprintf(
        payload,
        sizeof(payload),
        "%ld",
        static_cast<long>(value)
    );

    return mqttClient.publish(
        topic,
        payload,
        true
    );
}


// =============================================================================
// MQTT CHANGE FILTERS
// =============================================================================

// Periodic-only topics use a minimum change threshold of zero.
bool publishDue(
    const PublishState &state,
    double value,
    double minChange,
    uint32_t heartbeatMs,
    bool force
)
{
    return force ||
        !state.valid ||
        (minChange > 0.0 && fabs(value - state.value) >= minChange) ||
        static_cast<uint32_t>(millis() - state.atMs) >= heartbeatMs;
}

void notePublished(PublishState &state, double value)
{
    state.value = value;
    state.atMs = millis();
    state.valid = true;
}

// A skipped update is successful; a failed publish is NOT recorded so it
// will be retried with a later reading. Reconnection forces a full refresh.
bool publishFloatFiltered(
    const char *topic,
    float value,
    double minChange,
    uint32_t heartbeatMs,
    PublishState &state,
    bool force,
    bool &sent,
    uint8_t decimals = 2
)
{
    if (!publishDue(state, value, minChange, heartbeatMs, force))
        return true;

    if (!mqttPublishFloat(topic, value, decimals))
        return false;

    notePublished(state, value);
    sent = true;
    return true;
}

bool publishUIntFiltered(
    const char *topic,
    uint32_t value,
    double minChange,
    uint32_t heartbeatMs,
    PublishState &state,
    bool force,
    bool &sent
)
{
    if (!publishDue(state, value, minChange, heartbeatMs, force))
        return true;

    if (!mqttPublishUInt(topic, value))
        return false;

    notePublished(state, value);
    sent = true;
    return true;
}

bool publishIntFiltered(
    const char *topic,
    int32_t value,
    double minChange,
    uint32_t heartbeatMs,
    PublishState &state,
    bool force,
    bool &sent
)
{
    if (!publishDue(state, value, minChange, heartbeatMs, force))
        return true;

    if (!mqttPublishInt(topic, value))
        return false;

    notePublished(state, value);
    sent = true;
    return true;
}


// =============================================================================
// MQTT SENSOR PUBLISHING
// =============================================================================

void publishBmeReading(
    const EnvironmentReading &reading,
    bool force = false
)
{
    if (!mqttClient.connected())
        return;

    bool ok = true;
    bool sent = false;

    ok &= publishFloatFiltered(
        MQTT_BME_TEMPERATURE_TOPIC, reading.temperatureC,
        TEMPERATURE_CHANGE_C, SENSOR_HEARTBEAT_MS,
        bmeTemperatureSent, force, sent
    );
    ok &= publishFloatFiltered(
        MQTT_BME_HUMIDITY_TOPIC, reading.humidityPercent,
        HUMIDITY_CHANGE_PERCENT, SENSOR_HEARTBEAT_MS,
        bmeHumiditySent, force, sent
    );
    ok &= publishFloatFiltered(
        MQTT_BME_PRESSURE_TOPIC, reading.pressureHpa,
        PRESSURE_CHANGE_HPA, SENSOR_HEARTBEAT_MS,
        bmePressureSent, force, sent
    );

    if (!ok)
        Serial.println("WARNING: BME280 MQTT publish failed.");
    else if (sent)
        Serial.println("BME280: changed/heartbeat values published.");
}


void publishXiaomiReading(
    const XiaomiReading &reading,
    bool force = false
)
{
    if (!mqttClient.connected())
        return;

    bool ok = true;
    bool sent = false;

    ok &= publishFloatFiltered(
        MQTT_XIAOMI_TEMPERATURE_TOPIC, reading.temperatureC,
        TEMPERATURE_CHANGE_C, SENSOR_HEARTBEAT_MS,
        xiaomiTemperatureSent, force, sent
    );
    ok &= publishFloatFiltered(
        MQTT_XIAOMI_HUMIDITY_TOPIC, reading.humidityPercent,
        HUMIDITY_CHANGE_PERCENT, SENSOR_HEARTBEAT_MS,
        xiaomiHumiditySent, force, sent
    );

    if (reading.haveBattery)
    {
        ok &= publishUIntFiltered(
            MQTT_XIAOMI_BATTERY_TOPIC, reading.batteryPercent,
            1.0, BATTERY_HEARTBEAT_MS,
            xiaomiBatterySent, force, sent
        );
    }

    if (reading.haveBatteryVoltage)
    {
        ok &= publishFloatFiltered(
            MQTT_XIAOMI_BATTERY_VOLTAGE_TOPIC, reading.batteryVoltage,
            BATTERY_VOLTAGE_CHANGE_V, BATTERY_HEARTBEAT_MS,
            xiaomiBatteryVoltageSent, force, sent, 3
        );
    }

    // RSSI fluctuates constantly, so use a heartbeat only.
    ok &= publishIntFiltered(
        MQTT_XIAOMI_RSSI_TOPIC, reading.rssi,
        0.0, SENSOR_HEARTBEAT_MS,
        xiaomiRssiSent, force, sent
    );

    if (!ok)
        Serial.println("WARNING: Xiaomi MQTT publish failed.");
    else if (sent)
        Serial.println("Xiaomi: changed/heartbeat values published.");
}


void publishXiaomiAvailability()
{
    if (!mqttClient.connected())
    {
        return;
    }

    mqttClient.publish(
        MQTT_XIAOMI_STATUS_TOPIC,
        xiaomiOnline ? "online" : "offline",
        true
    );
}


// =============================================================================
// SENSOR DISPATCH
// =============================================================================

void sendBmeReading(
    const EnvironmentReading &reading
)
{
    latestBmeReading = reading;
    haveBmeReading = true;

    printBmeReading(reading);

    publishBmeReading(reading);
}


// =============================================================================
// BLE CALLBACKS
// =============================================================================

class EnvironmentBleCallbacks :
    public NimBLEScanCallbacks
{
    void onResult(
        const NimBLEAdvertisedDevice *advertisedDevice
    ) override
    {
        // ---------------------------------------------------------------------
        // Filter immediately by MAC.
        //
        // There is deliberately NO RSSI filter.
        // ---------------------------------------------------------------------

        const uint64_t address =
            static_cast<uint64_t>(
                advertisedDevice->getAddress()
            );

        if (address != XIAOMI_MAC)
        {
            return;
        }


        // ---------------------------------------------------------------------
        // Extract BTHome service data.
        //
        // getServiceData(UUID) gives us the payload belonging specifically
        // to service UUID 0xFCD2.
        // ---------------------------------------------------------------------

        std::string serviceData =
            advertisedDevice->getServiceData(
                BTHOME_SERVICE_UUID
            );

        if (serviceData.empty())
        {
            return;
        }


        // ---------------------------------------------------------------------
        // Decode advertisement
        // ---------------------------------------------------------------------

        XiaomiReading reading;

        if (!decodeBTHome(
                serviceData,
                reading))
        {
            return;
        }


        reading.rssi =
            advertisedDevice->getRSSI();

        reading.receivedAtMs =
            millis();


        // ---------------------------------------------------------------------
        // Hand the newest reading to the Arduino main loop.
        //
        // No MQTT, Wi-Fi or Serial operations happen in this callback.
        // ---------------------------------------------------------------------

        if (xiaomiQueue != nullptr)
        {
            xQueueOverwrite(
                xiaomiQueue,
                &reading
            );
        }
    }


    void onScanEnd(
        const NimBLEScanResults &results,
        int reason
    ) override
    {
        (void)results;
        (void)reason;

        bleScanEnded = true;
    }
};


EnvironmentBleCallbacks bleCallbacks;


// =============================================================================
// XIAOMI QUEUE PROCESSING
// =============================================================================

void processXiaomiQueue()
{
    if (xiaomiQueue == nullptr)
    {
        return;
    }

    XiaomiReading reading;

    while (
        xQueueReceive(
            xiaomiQueue,
            &reading,
            0
        ) == pdTRUE
    )
    {
        // Every successfully decoded advertisement proves the sensor is alive,
        // even if this is a repeated BTHome packet.

        xiaomiLastSeenTime =
            reading.receivedAtMs;


        const bool wasOnline =
            xiaomiOnline;

        xiaomiOnline = true;


        if (!wasOnline)
        {
            Serial.println();
            Serial.println(
                "Xiaomi sensor online."
            );

            publishXiaomiAvailability();
        }


        // ---------------------------------------------------------------------
        // BTHome transmitters may repeat the same packet to improve reception.
        //
        // Use its packet ID to avoid printing/publishing duplicate measurements.
        // ---------------------------------------------------------------------

        bool newPacket = true;

        if (
            reading.havePacketId &&
            haveLastXiaomiPacketId &&
            reading.packetId ==
                lastXiaomiPacketId
        )
        {
            newPacket = false;
        }


        // If the sensor has just recovered from an offline state, publish it
        // even if for some unlikely reason its packet ID hasn't changed.

        if (!wasOnline)
        {
            newPacket = true;
        }


        latestXiaomiReading = reading;
        haveXiaomiReading = true;


        if (reading.havePacketId)
        {
            lastXiaomiPacketId =
                reading.packetId;

            haveLastXiaomiPacketId =
                true;
        }


        if (newPacket)
        {
            publishXiaomiReading(reading);
        }
    }
}


// =============================================================================
// XIAOMI AVAILABILITY
// =============================================================================

void maintainXiaomiAvailability()
{
    if (!xiaomiOnline)
    {
        return;
    }

    const uint32_t now =
        millis();

    if (
        static_cast<uint32_t>(
            now - xiaomiLastSeenTime
        ) >= XIAOMI_OFFLINE_TIMEOUT_MS
    )
    {
        xiaomiOnline = false;

        Serial.println();
        Serial.println(
            "Xiaomi sensor offline "
            "(no BLE packets for 5 minutes)."
        );

        publishXiaomiAvailability();
    }
}


// =============================================================================
// ARDUINO OTA: available after the initial USB flash
// =============================================================================

void startOta()
{
    if (otaStarted)
        return;

    // Require a non-empty password; never expose unauthenticated OTA.
    if (OTA_PASSWORD[0] == '\0')
    {
        Serial.println("ERROR: OTA disabled (empty OTA_PASSWORD).");
        return;
    }

    ArduinoOTA.setHostname(DEVICE_NAME);
    ArduinoOTA.setPort(3232);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
    {
        Serial.println("OTA firmware upload started.");
    });
    ArduinoOTA.onEnd([]()
    {
        Serial.println("OTA firmware upload finished; restarting.");
    });
    ArduinoOTA.onError([](ota_error_t error)
    {
        Serial.printf("OTA failed, error %u\n", static_cast<unsigned>(error));
    });

    ArduinoOTA.begin();
    otaStarted = true;

    Serial.printf("OTA enabled: %s.local:3232\n", DEVICE_NAME);
}


// =============================================================================
// WI-FI
// =============================================================================

void startWifi()
{
    Serial.println();

    Serial.print(
        "Connecting to Wi-Fi: "
    );

    Serial.println(WIFI_SSID);

    WiFi.persistent(false);

    WiFi.mode(WIFI_STA);

    WiFi.setHostname(
        DEVICE_NAME
    );

    WiFi.setAutoReconnect(true);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    lastWifiAttemptTime =
        millis();
}


void maintainWifi()
{
    const bool connected =
        WiFi.status() == WL_CONNECTED;


    if (connected)
    {
        if (!wifiWasConnected)
        {
            wifiWasConnected = true;

            Serial.println();
            Serial.println(
                "Wi-Fi connected."
            );

            Serial.print(
                "IP address: "
            );

            Serial.println(
                WiFi.localIP()
            );

            Serial.print(
                "RSSI: "
            );

            Serial.print(
                WiFi.RSSI()
            );

            Serial.println(
                " dBm"
            );

            startOta();
        }

        return;
    }


    if (wifiWasConnected)
    {
        wifiWasConnected = false;

        Serial.println();
        Serial.println(
            "Wi-Fi disconnected."
        );
    }


    const uint32_t now =
        millis();

    if (
        static_cast<uint32_t>(
            now - lastWifiAttemptTime
        ) < WIFI_RETRY_INTERVAL_MS
    )
    {
        return;
    }


    lastWifiAttemptTime = now;

    Serial.println(
        "Retrying Wi-Fi connection..."
    );

    WiFi.reconnect();
}


// =============================================================================
// MQTT
// =============================================================================

bool connectMqtt()
{
    if (
        WiFi.status() != WL_CONNECTED
    )
    {
        return false;
    }


    Serial.print(
        "Connecting to MQTT broker "
    );

    Serial.print(MQTT_HOST);
    Serial.print(":");
    Serial.print(MQTT_PORT);
    Serial.println("...");


    bool connected = false;


    if (strlen(MQTT_USERNAME) > 0)
    {
        connected =
            mqttClient.connect(
                DEVICE_NAME,

                MQTT_USERNAME,
                MQTT_PASSWORD,

                MQTT_STATUS_TOPIC,
                1,
                true,
                "offline"
            );
    }
    else
    {
        connected =
            mqttClient.connect(
                DEVICE_NAME,

                MQTT_STATUS_TOPIC,
                1,
                true,
                "offline"
            );
    }


    if (!connected)
    {
        Serial.print(
            "MQTT connection failed. rc="
        );

        Serial.println(
            mqttClient.state()
        );

        return false;
    }


    Serial.println(
        "MQTT connected."
    );


    // Gateway status

    mqttClient.publish(
        MQTT_STATUS_TOPIC,
        "online",
        true
    );


    // Xiaomi status
    //
    // Do not publish "offline" immediately after boot before we've heard
    // from the sensor. Only republish availability if we already have
    // Xiaomi state from this runtime.

    if (haveXiaomiReading)
    {
        publishXiaomiAvailability();
    }


    // Republish cached values after reconnect so the broker immediately
    // receives the newest readings.

    if (haveBmeReading)
    {
        publishBmeReading(
            latestBmeReading, true
        );
    }

    if (haveXiaomiReading)
    {
        publishXiaomiReading(
            latestXiaomiReading, true
        );
    }


    return true;
}


void maintainMqtt()
{
    if (
        WiFi.status() != WL_CONNECTED
    )
    {
        return;
    }


    if (mqttClient.connected())
    {
        mqttClient.loop();

        return;
    }


    const uint32_t now =
        millis();

    if (
        static_cast<uint32_t>(
            now - lastMqttAttemptTime
        ) < MQTT_RETRY_INTERVAL_MS
    )
    {
        return;
    }


    lastMqttAttemptTime = now;

    connectMqtt();
}


// =============================================================================
// BLE SCANNING
// =============================================================================

bool startBleScan()
{
    if (bleScan == nullptr)
    {
        return false;
    }


    if (bleScan->isScanning())
    {
        return true;
    }


    Serial.println();
    Serial.println(
        "Starting continuous BLE scan..."
    );


    bleScanEnded = false;


    // duration = 0 means scan forever.

    const bool started =
        bleScan->start(
            0,
            false,
            true
        );


    if (!started)
    {
        Serial.println(
            "WARNING: BLE scan failed to start."
        );
    }


    return started;
}


void maintainBle()
{
    if (bleScan == nullptr)
    {
        return;
    }


    if (
        !bleScanEnded &&
        bleScan->isScanning()
    )
    {
        return;
    }


    const uint32_t now =
        millis();


    if (
        static_cast<uint32_t>(
            now -
            lastBleRestartAttemptTime
        ) < BLE_RESTART_INTERVAL_MS
    )
    {
        return;
    }


    lastBleRestartAttemptTime =
        now;


    Serial.println(
        "BLE scan stopped unexpectedly; restarting."
    );


    startBleScan();
}


// =============================================================================
// SETUP
// =============================================================================

void setup()
{
    Serial.begin(115200);

    delay(1500);


    Serial.println();
    Serial.println(
        "XIAO ESP32-C6 Environment Gateway"
    );

    Serial.println(
        "---------------------------------"
    );


    // =========================================================================
    // I2C / BME280
    // =========================================================================

    Wire.begin(
        I2C_SDA_PIN,
        I2C_SCL_PIN
    );


    Serial.print(
        "Initialising BME280 at 0x"
    );

    Serial.println(
        SENSOR_I2C_ADDRESS,
        HEX
    );


    if (
        !bme.begin(
            SENSOR_I2C_ADDRESS,
            &Wire
        )
    )
    {
        Serial.println(
            "ERROR: BME280 not detected."
        );

        while (true)
        {
            delay(1000);
        }
    }


    Serial.println(
        "BME280 detected successfully."
    );


    Serial.print(
        "Sensor ID: 0x"
    );

    Serial.println(
        bme.sensorID(),
        HEX
    );


    // Forced mode:
    //
    // One measurement on request, then sensor returns to sleep.

    bme.setSampling(
        Adafruit_BME280::MODE_FORCED,

        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,
        Adafruit_BME280::SAMPLING_X1,

        Adafruit_BME280::FILTER_OFF
    );


    // =========================================================================
    // MQTT
    // =========================================================================

    mqttClient.setServer(
        MQTT_HOST,
        MQTT_PORT
    );

    mqttClient.setKeepAlive(30);


    // =========================================================================
    // WI-FI
    // =========================================================================

    startWifi();


    // =========================================================================
    // XIAOMI QUEUE
    // =========================================================================

    xiaomiQueue =
        xQueueCreate(
            1,
            sizeof(XiaomiReading)
        );


    if (xiaomiQueue == nullptr)
    {
        Serial.println(
            "FATAL: Could not create Xiaomi BLE queue."
        );

        while (true)
        {
            delay(1000);
        }
    }


    // =========================================================================
    // BLUETOOTH LE
    // =========================================================================

    Serial.println();
    Serial.println(
        "Initialising Bluetooth LE..."
    );


    NimBLEDevice::init("");


    bleScan =
        NimBLEDevice::getScan();


    // Passive scan:
    //
    // The Xiaomi advertises everything we need. There is no reason to send
    // active scan requests.

    bleScan->setActiveScan(false);


    // We need duplicate callbacks because the SAME BLE device will advertise
    // repeatedly forever. BTHome's packet ID handles application-level
    // duplicate suppression.

    bleScan->setScanCallbacks(
        &bleCallbacks,
        true
    );


    // Tell the controller itself not to discard duplicate advertisements.

    bleScan->setDuplicateFilter(0);


    // We only use callbacks. Don't accumulate a list of every Bluetooth device
    // in the neighbourhood in RAM for the rest of eternity.

    bleScan->setMaxResults(0);


    // Radio scan duty cycle.

    bleScan->setInterval(
        BLE_SCAN_INTERVAL_MS
    );

    bleScan->setWindow(
        BLE_SCAN_WINDOW_MS
    );


    startBleScan();


    // =========================================================================
    // FIRST BME READING IMMEDIATELY
    // =========================================================================

    lastBmeReadTime =
        millis() -
        BME_READ_INTERVAL_MS;


    Serial.println();
    Serial.println(
        "Environment gateway ready."
    );
}


// =============================================================================
// MAIN LOOP
// =============================================================================

void loop()
{
    // Network housekeeping

    maintainWifi();

    if (otaStarted && WiFi.status() == WL_CONNECTED)
        ArduinoOTA.handle();

    maintainMqtt();


    // BLE housekeeping

    maintainBle();

    processXiaomiQueue();

    maintainXiaomiAvailability();


    // -------------------------------------------------------------------------
    // BME280
    // -------------------------------------------------------------------------

    const uint32_t now =
        millis();


    if (
        static_cast<uint32_t>(
            now - lastBmeReadTime
        ) >= BME_READ_INTERVAL_MS
    )
    {
        lastBmeReadTime = now;


        EnvironmentReading reading;


        if (readBme280(reading))
        {
            sendBmeReading(reading);
        }
    }


    // Yield briefly to the ESP32 RTOS and radio stacks.

    delay(5);
}