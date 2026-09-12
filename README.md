# ESP32 Environment Gateway

An ESP32-C6 based environmental sensor and Bluetooth gateway for use with
Gladys Assistant and MQTT.

The gateway combines a locally connected BME280 environmental sensor with
passive Bluetooth LE monitoring of a Xiaomi Mi Temperature and Humidity
Monitor 2 (LYWSD03MMC).

Sensor data is published to an MQTT broker where it can be consumed by
Gladys Assistant or other home automation software.

## Hardware

- Seeed Studio XIAO ESP32-C6
- BME280 temperature, humidity and pressure sensor
- Xiaomi Mi Temperature and Humidity Monitor 2
  - Model: LYWSD03MMC
  - Custom firmware broadcasting unencrypted BTHome v2 advertisements

## BME280 Wiring

The BME280 breakout used during development is a nominal 5 V module with an
onboard regulator. It is powered from 3.3 V so that its I2C pull-ups also
operate at ESP32-safe 3.3 V logic levels.

| BME280 | XIAO ESP32-C6 |
|--------|----------------|
| VIN    | 3V3            |
| GND    | GND            |
| SDA    | D4 / GPIO22    |
| SCL    | D5 / GPIO23    |

The BME280 used during development responds at I2C address `0x76`.

## Features

- BME280 temperature monitoring
- BME280 humidity monitoring
- BME280 atmospheric pressure monitoring
- Passive Bluetooth LE scanning
- BTHome v2 decoding
- Xiaomi LYWSD03MMC temperature monitoring
- Xiaomi LYWSD03MMC humidity monitoring
- Xiaomi battery monitoring
- Xiaomi BLE RSSI monitoring
- MQTT retained sensor values
- MQTT gateway availability
- Xiaomi sensor availability detection
- Automatic Wi-Fi and MQTT reconnection
- Continuous BLE scanning with duplicate packet suppression
- Serial diagnostic output

## MQTT Topics

### Gateway

```text
home/environment/environment-sensor-1/status
```

### BME280

```text
home/environment/environment-sensor-1/bme280/temperature
home/environment/environment-sensor-1/bme280/humidity
home/environment/environment-sensor-1/bme280/pressure
```

### Xiaomi LYWSD03MMC

```text
home/environment/environment-sensor-1/xiaomi/status
home/environment/environment-sensor-1/xiaomi/temperature
home/environment/environment-sensor-1/xiaomi/humidity
home/environment/environment-sensor-1/xiaomi/battery
home/environment/environment-sensor-1/xiaomi/rssi
```

## Xiaomi Sensor

The Xiaomi LYWSD03MMC uses custom firmware configured to transmit
unencrypted BTHome v2 advertisements.

The gateway listens for service UUID:

```text
0xFCD2
```

The ESP32 does not establish a Bluetooth connection to the sensor.
Measurements are received entirely from passive BLE advertisements.

## Arduino Libraries

The sketch requires:

- Adafruit BME280 Library
- Adafruit Unified Sensor
- Adafruit BusIO
- PubSubClient
- NimBLE-Arduino

The ESP32 Arduino core provides Wi-Fi, Wire and FreeRTOS support.

## Configuration

Copy:

```text
secrets.h.example
```

to:

```text
secrets.h
```

and configure the local Wi-Fi and MQTT credentials.

`secrets.h` is excluded from Git and must not be committed.

## Gladys Assistant

The MQTT values can be exposed in Gladys as two logical devices.

### BME280 Environment Sensor

- Temperature
- Humidity
- Pressure

### Xiaomi Mi Temperature and Humidity Monitor 2

- Temperature
- Humidity
- Battery
- RSSI

RSSI is intentionally retained as a sensor because graphs of radio signal
strength are, apparently, a perfectly legitimate form of entertainment.

## Flash Partition Layout

The project includes a custom `partitions.csv` for the XIAO ESP32-C6's
4 MiB flash.

It provides two 1,966,080-byte application partitions, retaining OTA support
while allowing a substantially larger firmware image than the default
Arduino OTA partition layout.

The current firmware does not yet implement OTA updating, but the partition
layout allows OTA support to be added later without repartitioning the device.

## License

See `LICENSE`.
