# ESP32 Environment Gateway

A Seeed Studio XIAO ESP32-C6 environmental sensor and Bluetooth LE gateway for
MQTT and Gladys Assistant. A wired BME280 provides local environmental readings;
the gateway also receives unencrypted BTHome v2 advertisements from a Xiaomi Mi
Temperature and Humidity Monitor 2 (LYWSD03MMC).

The firmware supports retained MQTT values, per-topic change/heartbeat filtering,
automatic Wi-Fi/MQTT reconnection, and password-protected ArduinoOTA firmware
updates after the first USB installation.

## Hardware

- Seeed Studio XIAO ESP32-C6 (4 MiB flash)
- BME280 temperature, humidity and pressure sensor
- Xiaomi LYWSD03MMC running firmware that broadcasts **unencrypted BTHome v2**
  measurements

### BME280 wiring

| BME280 | XIAO ESP32-C6 |
| --- | --- |
| VIN | 3V3 |
| GND | GND |
| SDA | D4 / GPIO22 |
| SCL | D5 / GPIO23 |

The development BME280 breakout is a nominal 5 V module with an onboard
regulator. It is supplied at **3.3 V** so its I2C pull-ups are also at
ESP32-safe levels. The sketch expects I2C address `0x76`.

## Software requirements

The tested setup uses Arduino IDE with **Espressif ESP32 Arduino core 3.3.12**
and board **Seeed Studio XIAO ESP32C6** (`esp32:esp32:XIAO_ESP32C6`).

Install these Arduino libraries:

- Adafruit BME280 Library (tested with 2.3.0)
- Adafruit Unified Sensor (1.1.15)
- Adafruit BusIO (1.17.4)
- PubSubClient (2.8)
- NimBLE-Arduino (2.5.1)

`Wire`, `WiFi` and `ArduinoOTA` come from the ESP32 board package.

## Configuration and initial USB installation

1. Copy `secrets.h.example` to `secrets.h` alongside the sketch.
2. Set the Wi-Fi and MQTT connection details and a strong, unique
   `OTA_PASSWORD`. An empty password disables OTA; do not leave the
   example placeholder as your real password.
3. Open `esp32-environment-gateway.ino` in Arduino IDE and select the XIAO
   ESP32-C6 board.
4. Keep the project's **`partitions.csv` in the sketch directory**.
   Arduino's build process copies this custom layout over the board's
   partition-scheme preset. In the verified setup the FQBN printed
   `PartitionScheme=huge_app`, but the build log explicitly copied the
   sketch-local `partitions.csv`. Check that line in verbose build output.
5. Perform the **first flash by USB**, which installs the dual OTA partition
   table and OTA-enabled firmware. Verify Wi-Fi, MQTT, the BME280 and Xiaomi
   readings before relying on future wireless uploads.

`secrets.h` is ignored by Git. Check before any commit:

```bash
git check-ignore -v secrets.h
git ls-files --error-unmatch secrets.h  # should report that it is not tracked
git status --short
```

Never commit real Wi-Fi, MQTT or OTA credentials. The example file contains
placeholders only.

## MQTT topics

The default device name and topic prefix are set in the sketch as
`environment-sensor-1` and `home/environment/environment-sensor-1`.

| Device | Metric | Topic suffix |
| --- | --- | --- |
| Gateway | Availability | `status` |
| BME280 | Temperature | `bme280/temperature` |
| BME280 | Humidity | `bme280/humidity` |
| BME280 | Pressure | `bme280/pressure` |
| Xiaomi | Availability | `xiaomi/status` |
| Xiaomi | Temperature | `xiaomi/temperature` |
| Xiaomi | Humidity | `xiaomi/humidity` |
| Xiaomi | Battery percentage | `xiaomi/battery` |
| Xiaomi | Battery voltage, when advertised | `xiaomi/battery_voltage` |
| Xiaomi | BLE signal strength | `xiaomi/rssi` |

Sensor readings are retained in MQTT. Availability is also published using
the gateway's status and Xiaomi status topics.

### Sensor publishing policy

The BME280 is sampled every **30 seconds**. Xiaomi measurements are decoded
from passive BLE advertisements (service UUID `0xFCD2`) with packet-ID
deduplication. The ESP32 does not connect to the Xiaomi device over BLE.

Each metric has its own last-published value and heartbeat timer:

| Reading | Publish when changed by | Heartbeat |
| --- | --- | --- |
| BME280 temperature | 0.1 °C | 5 minutes |
| BME280 humidity | 1 % RH | 5 minutes |
| BME280 pressure | 0.5 hPa | 5 minutes |
| Xiaomi temperature | 0.1 °C | 5 minutes |
| Xiaomi humidity | 1 % RH | 5 minutes |
| Xiaomi battery percentage | 1 percentage point | 60 minutes |
| Xiaomi battery voltage | 0.05 V | 60 minutes |
| Xiaomi RSSI | No change threshold; periodic only | 5 minutes |

Heartbeats require an available reading and, for Xiaomi, continuing
advertisements. The gateway cannot publish a fresh reading from a sensor it
hasn't heard from. A reading is recorded as published only after the MQTT
publish succeeds; reconnecting forces cached readings to be republished.
Xiaomi online/offline availability is not subject to the measurement filters.
The Xiaomi becomes unavailable after approximately five minutes without a
valid advertisement.

Serial output reports BME280 samples and reports detailed Xiaomi measurements
when at least one Xiaomi MQTT metric is published, rather than for each BLE
advertisement.

## Wireless firmware updates (ArduinoOTA)

ArduinoOTA must first be installed by USB using the dual-slot partition layout.
Subsequent updates can be uploaded from Arduino IDE through its **network
port**, for example `environment-sensor-1` at the device's LAN IP.

The tested setup successfully completed a wireless upload while the XIAO was
powered from a separate USB power supply, without any USB data connection.
ArduinoOTA uses:

| Purpose | Protocol / port |
| --- | --- |
| Network-port discovery (mDNS) | UDP 5353 |
| OTA invitation and authentication | **UDP 3232** on the ESP32 |
| Firmware transfer callback | **TCP 3233** on the uploading computer (tested local uploader setting) |

**Important for ESP32 Arduino core 3.3.12:** The tested XIAO needed a
one-line **local ArduinoOTA IPv4 socket workaround** before reliable OTA
responses were possible. That patch is applied to the installed board
package on the compiling computer, **not** to this repository. Board-package
updates may overwrite it. On the tested Arch Linux laptop, firewalld also
needed a LAN-scoped rule for mDNS replies to ephemeral UDP ports.

See **[docs/ARDUINO_OTA.md](docs/ARDUINO_OTA.md)** for the precise core patch,
firewalld configuration, IDE upload override used during testing, and
troubleshooting. Recheck whether the upstream issue is fixed before applying
the workaround to future core versions.

If USB is plugged into a computer but serial output is not being read, the
gateway was observed to respond inconsistently to OTA invitations. Draining
the USB serial stream, or powering the gateway without a USB data connection,
coincided with reliable OTA uploads. USB serial backpressure is a possible
explanation, not a confirmed root cause.

## Gladys Assistant

Create logical devices using the MQTT topics above:

- **BME280:** temperature, humidity, pressure
- **Xiaomi LYWSD03MMC:** temperature, humidity, battery, RSSI, and optional
  battery voltage when transmitted by the sensor firmware

## Flash partition layout

The sketch-local `partitions.csv` targets the XIAO ESP32-C6's 4 MiB flash and
provides **two 1,966,080-byte application slots** (`app0` and `app1`).
The layout supports OTA while allowing a larger application image than the
default small-OTA partition layout. Changing the partition layout later
requires care and may require another USB flash.

## License

See [LICENSE](LICENSE).
