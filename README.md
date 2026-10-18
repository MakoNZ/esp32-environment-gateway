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
- Per-topic change-threshold MQTT publishing with periodic heartbeats
- Password-protected Arduino OTA updates after the initial USB upload

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

## Sensor publishing policy

The BME280 still samples every 30 seconds; the Xiaomi is still passively
received as often as its BLE advertisements arrive. MQTT values are retained,
but each metric is published independently only when it changes enough or when
its heartbeat is due.

| Reading | Change threshold | Maximum interval |
|---|---|---|
| BME280 temperature | 0.1 °C | 5 minutes |
| BME280 humidity | 1 % RH | 5 minutes |
| BME280 pressure | 0.5 hPa | 5 minutes |
| Xiaomi temperature | 0.1 °C | 5 minutes |
| Xiaomi humidity | 1 % RH | 5 minutes |
| Xiaomi battery percent | 1 percentage point | 60 minutes |
| Xiaomi battery voltage | 0.05 V | 60 minutes |
| Xiaomi RSSI | Periodic only | 5 minutes |

New values are published on startup and re-published after MQTT reconnect.
Xiaomi online/offline availability remains immediate. Values are only marked
as sent after MQTT accepts the publish call.

## Arduino OTA uploads

OTA is implemented in firmware but **must be installed once using USB first**.
The dual-slot `partitions.csv` must be selected during that first flash.

Set a strong, unique `OTA_PASSWORD` in the local `secrets.h`; never commit
it. After the USB flash, connect to the LAN and use Arduino IDE's network
port `environment-sensor-1.local` or the device IP with TCP port 3232,
if your Arduino IDE setup supports ESP32 ArduinoOTA discovery. mDNS and
firewall rules may affect network-port discovery.

Make sure the ESP32-C6 board, partition scheme, and matching libraries are
selected before compiling. Verify the running device is still reporting
BME280 and Xiaomi MQTT topics before relying on future OTA updates.

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

The firmware implements password-protected ArduinoOTA. Initial USB installation
is necessary to provision OTA support and the correct partition layout.

## License

See `LICENSE`.
