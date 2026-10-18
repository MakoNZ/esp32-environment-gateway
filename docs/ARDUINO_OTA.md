# ArduinoOTA setup and troubleshooting

This document records the **verified October 2026 setup** for the XIAO ESP32-C6
environment gateway. It is intended to make an otherwise easy-to-lose
machine-local workaround reproducible, not to prescribe these changes for
every Arduino/ESP32 version.

## Verified environment

- Host: Arch Linux laptop running Arduino IDE
- Target: Seeed Studio XIAO ESP32-C6, 4 MiB flash
- Espressif Arduino ESP32 board package: **3.3.12**
- Board ID: `esp32:esp32:XIAO_ESP32C6`
- Network: one trusted `192.168.1.0/24` LAN; sample ESP32 IP `192.168.1.96`
- Host firewall: firewalld, Wi-Fi `wlan0` assigned to zone `home`
- Sketch: `esp32-environment-gateway.ino`, with the repo's `partitions.csv`
- Tested result: complete, password-authenticated OTA transfer, reboot, Wi-Fi
  and MQTT reconnection with the device on an independent USB power supply

IP addresses, interface names and firewall zone are examples from the tested
installation, not hard-coded firmware requirements.

## 1. Install OTA-capable firmware over USB

Copy `secrets.h.example` to ignored `secrets.h` and set Wi-Fi/MQTT credentials
and a strong `OTA_PASSWORD`. Select the XIAO ESP32-C6 board. Keep
`partitions.csv` in the sketch directory and check verbose build output for the
copy of the **sketch-local** partitions file; the displayed Arduino partition
menu preset may still say `huge_app`.

Flash once using USB, then verify the gateway has Wi-Fi, MQTT and sensor data.
The serial message `OTA enabled: environment-sensor-1.local:3232` indicates
initialization was requested, but **is not proof** that IPv4 invitations and
replies work.

## 2. ESP32 core 3.3.12 IPv4 workaround

With IPv6 enabled in the shipped SDK configuration, version 3.3.12's
`ArduinoOTAClass::begin()` binds the OTA UDP socket using:

```cpp
_udp_ota.begin(IN6ADDR_ANY, _port)
```

In the tested configuration the IPv4 OTA invitations were received but replies
did not work reliably until this was changed to the IPv4 bind overload:

```cpp
_udp_ota.begin(_port)
```

Patch the **installed core on the development computer** (not the sketch):

```bash
OTA_CPP="$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.12/libraries/ArduinoOTA/src/ArduinoOTA.cpp"

cp -n "$OTA_CPP" "$OTA_CPP.original"

python3 - <<'PY'
from pathlib import Path

path = Path.home() / ".arduino15/packages/esp32/hardware/esp32/3.3.12/libraries/ArduinoOTA/src/ArduinoOTA.cpp"
before = "_udp_ota.begin(IN6ADDR_ANY, _port)"
after = "_udp_ota.begin(_port)"
text = path.read_text()

if text.count(before) == 1:
    path.write_text(text.replace(before, after, 1))
    print("ArduinoOTA IPv4 patch applied")
elif text.count(after) >= 1:
    print("Inspect ArduinoOTA.cpp: patch may already be applied")
else:
    raise SystemExit("Expected source expression not found; stop and inspect core version")
PY

diff -u "$OTA_CPP.original" "$OTA_CPP" || true
```

Review the diff. The intended change is **one line** inside
`ArduinoOTAClass::begin()`. Do **not** blindly apply this to other board-package
versions. After patching, compile and **USB-flash** the firmware once so the
device actually runs the changed library code. Merely patching the laptop
does not update already-flashed firmware.

Updating or reinstalling the Espressif board package can replace this local
patch. Before applying the patch to a newer core, check whether upstream has
fixed the IPv4/IPv6 behaviour.

Original source:
[ArduinoOTA.cpp, ESP32 core 3.3.12](https://github.com/espressif/arduino-esp32/blob/3.3.12/libraries/ArduinoOTA/src/ArduinoOTA.cpp).

## 3. Allow Arduino IDE discovery and OTA transfer through firewalld

On the verified machine, Avahi could discover both ESP32 Arduino network
services, but Arduino IDE's bundled mDNS discovery utility requested
**unicast** answers. The ESP32 sent replies from UDP source port 5353 to
an **ephemeral destination port** on the laptop. Allowing only the usual mDNS
destination port 5353 did not permit those answers through firewalld.

Check the active zone and subnet before adding any rules:

```bash
sudo firewall-cmd --get-zone-of-interface=wlan0
ip -4 addr show dev wlan0
```

The verified laptop used `home` and `192.168.1.0/24`. To allow these
mDNS replies **from that LAN only**, and the uploader's inbound TCP callback:

```bash
sudo firewall-cmd --permanent --zone=home \
  --add-rich-rule='rule family="ipv4" source address="192.168.1.0/24" source-port port="5353" protocol="udp" accept'

sudo firewall-cmd --permanent --zone=home --add-port=3233/tcp

sudo firewall-cmd --reload

sudo firewall-cmd --zone=home --list-all
```

This is a source-port rule, **not** merely `--add-port=5353/udp`.
The port `3233/tcp` is the local firmware-transfer callback. The laptop
initiates communication to the ESP32 on **UDP 3232**; a broad permanent
UDP-3232 inbound exception was **not needed** during the final successful
tests.

The zone/subnet restriction matters: the source-port mDNS rule permits UDP
packets from those LAN addresses to ephemeral local ports. Use only on a
trusted network and adjust the zone and subnet to your installation.

**Observed firewalld recovery issue:** A `firewall-cmd --reload` performed while
temporary (`--timeout=600`) rich rules were still active failed with
`dictionary changed size during iteration`, followed by `INVALID_ZONE`.
Running `sudo systemctl restart firewalld` locally restored the saved
permanent rules and zone assignment. Avoid a remote-only session when
restarting a firewall.

## 4. Upload from Arduino IDE

- Select **Seeed Studio XIAO ESP32C6** and the network port advertised for
  `environment-sensor-1`. Close Serial Monitor for the network upload.
- Enter the OTA password when prompted. Do not put it in a public command,
  log, or repository file.
- Verify the IDE reports `100% Done` and that the gateway reboots and resumes
  MQTT reporting.

The tested Arduino IDE installation also used this **machine-local override**
in:

```text
~/.arduino15/packages/esp32/hardware/esp32/3.3.12/platform.local.txt
```

```properties
tools.esp_ota.upload.pattern={cmd} -i {upload.port.address} -p 3232 -P 3233 "--auth={upload.field.password}" -f "{build.path}/{build.project_name}.bin"
```

The override selects the ESP32 UDP invitation port (`-p 3232`), the local
TCP callback (`-P 3233`), and the password field. It was **used during the
successful tests**; it may not be necessary with another IDE/core
configuration. Package upgrades can remove this local file or change the
uploader's required arguments.

## 5. Separate discovery failures from OTA handshake failures

| Symptom | What to check |
| --- | --- |
| ESP32 missing in IDE **Tools → Port** | mDNS/Avahi discovery, firewalld replies from UDP source port 5353, IDE discovery process |
| ESP32 listed, upload says `No response from the ESP` | UDP 3232 invitation/reply path, device OTA socket and main-loop responsiveness |
| Authentication fails | Correct `OTA_PASSWORD` and IDE/uploader compatibility |
| Transfer starts but fails | TCP callback 3233 on the uploading host, dual OTA partition layout, firmware size and Wi-Fi stability |
| Works with an independent power supply but fails over USB with monitor closed | Suspected USB serial-output backpressure; unconfirmed, see below |

A safe handshake-only test on the uploading computer:

```bash
python3 - <<'PY'
import socket
import time

target = ("192.168.1.96", 3232)  # replace with ESP32 LAN IP
invitation = b"0 3233 1 " + b"0" * 32 + b"\n"

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.settimeout(3)
    start = time.monotonic()
    sock.sendto(invitation, target)
    try:
        data, sender = sock.recvfrom(512)
        print(f"{time.monotonic() - start:.3f}s: {data[:4]!r} from {sender}")
    except socket.timeout:
        print("No UDP response from OTA listener")
    finally:
        # Invalid authentication cancels this probe; no firmware is sent.
        sock.sendto(b"200 invalid invalid\n", target)
PY
```

An `AUTH` reply demonstrates that the invitation reached the ESP32 and a UDP
response returned. **It does not demonstrate** successful authentication,
firmware transfer or boot. Verify a real OTA upload separately.

## 6. Serial-output observation (not a confirmed root cause)

The gateway sometimes timed out on OTA invitations when connected to the
development laptop over USB **with Serial Monitor closed**. Keeping the USB
serial stream actively drained coincided with a successful OTA upload; so did
powering the device from a separate USB supply without a data connection.
Repeated handshake probes in the latter configuration responded promptly.

A possible explanation is USB CDC serial-output backpressure delaying the
sketch's main loop and therefore `ArduinoOTA.handle()`, but this was not
isolated experimentally. No firmware change was made on that assumption.

For a diagnostic test when USB is connected but Serial Monitor is closed:

```bash
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0 > /tmp/environment-gateway-serial.log
```

Stop with Ctrl+C. Use the actual USB device path for your system. A standalone
power supply is the verified normal operating arrangement.

## 7. Recovery and future upgrades

- Retain a way to USB-flash if a future OTA-enabled build fails to boot.
- Never switch away from the dual OTA layout accidentally when building or
  installing firmware.
- Recheck the installed core patch and `platform.local.txt` after a
  board-package upgrade; these are **not tracked in this repository**.
- Never publish `secrets.h`, OTA passwords or captured credentials.
