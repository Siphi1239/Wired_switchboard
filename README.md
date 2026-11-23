# Wired Switchboard

A wired home-automation wall node: **ESP32-C6** + **10BASE-T1S** single-pair
Ethernet (Microchip **LAN8651** MAC-PHY over SPI), **powered over the data line**,
speaking **Matter** (over Ethernet) and **MQTT** (Home Assistant discovery).

* Keys (wall switches): Matter Generic Switch events, MQTT events, local control
* 2 optional low-voltage PWM outputs (2 A): dimmable lights
* Key indicator LEDs (WS2812)
* Temperature / humidity (SHT4x)

Design, hardware notes (pins, PoDL budget) and roadmap:
**[docs/architecture.md](docs/architecture.md)**

> v0.1 targeted the nRF52840 with Zephyr. It moved to ESP32-C6 because Matter
> over Ethernet works there with only a small hook patch. The Zephyr code is in
> git history (commit `7dc1a80`).

## Layout

```
firmware/
  CMakeLists.txt  sdkconfig.defaults  partitions.csv (8 MB)
  patches/0001-esp32-ethernet-app-init-hook.patch   connectedhomeip hook for SPI Ethernet
  main/
    app_main.cpp      init order
    device_model.*    single owner of state, event fan-out task
    eth_t1s.*         LAN865x install (Matter hook), MAC, PLCA, hostname, IP events
    matter_app.cpp    endpoints, Matter <-> device model
    mqtt_app.*        esp-mqtt, HA discovery, JSON commands
    inputs.*          key scan, Matter momentary-switch event model
    outputs.*         LEDC PWM with gamma + fades
    indicators.*      WS2812 key LEDs
    sensors.*         SHT4x
    local_control.*   offline key -> output logic, LED follow
    app_settings.*    NVS config        app_console.c   `sb` commands
    board.*           assembly variant (outputs fitted?) via strap / override
    Kconfig.projbuild pins and counts   idf_component.yml
docs/architecture.md
```

## Toolchain setup (Linux, once)

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32c6

git clone -b release/v1.6.1 --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
# Ethernet hook for SPI MAC-PHYs (required, the build stops without it):
git apply ~/src/wired_switchboard/firmware/patches/0001-esp32-ethernet-app-init-hook.patch
cd ../..
. ~/esp/esp-idf/export.sh
./install.sh
```

Per shell:

```bash
. ~/esp/esp-idf/export.sh && . ~/esp/esp-matter/export.sh
```

## Build, flash, monitor (ESP32-C6-DevKitC-1-N8)

```bash
cd firmware
idf.py set-target esp32c6
idf.py menuconfig          # Wired Switchboard -> pins, counts, PLCA, MQTT
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The DevKit's USB port is the console (USB-Serial-JTAG).

## Bring-up

### 1. Linux side of the T1S segment (bridge + PLCA coordinator)

Any Linux box with a USB T1S adapter (e.g. Microchip **EVB-LAN8670-USB**),
bridged so that IPv6 link-local and mDNS reach the nodes (Matter needs this):

```bash
sudo ethtool --set-plca-cfg eth1 enable on node-id 0 node-cnt 8
sudo ip link add br0 type bridge
sudo ip link set eth0 master br0 && sudo ip link set eth1 master br0
sudo ip link set br0 up
```

### 2. Provision the node (console)

```
sb show
sb plca 1 8                          # unique node id per node (1..7 here)
sb name kitchen                      # MQTT topics + hostname; reboot to apply
sb mqtt mqtt://192.168.1.10:1883 user pass   # Mosquitto on the QNAP ("sb mqtt off")
sb map 0 0                           # key 0 drives output 0 locally (-1 = events only)
sb out 0 128
matter onboardingcodes ble           # QR / manual pairing code
```

### 3. Matter

Commission over BLE from the Home Assistant app, Apple Home or Google Home
using the onboarding code. The node stays on Ethernet afterwards; BLE is freed.
Dev builds use Espressif's **test** DAC, so ecosystems show an "uncertified
device" warning.

### 4. MQTT / Home Assistant

Entities appear through discovery: lights (outputs), switches (key LEDs),
events (keys), sensors. Manual test:

```bash
mosquitto_sub -h <broker> -t 'switchboard/#' -v
mosquitto_pub -h <broker> -t switchboard/kitchen/out/0/set -m '{"state":"ON","brightness":200}'
```

Use **one** path per entity in HA (Matter *or* MQTT) to avoid duplicate
entities. Both stay in sync on the device, so mixing works.

## Verification status

**Not compiled and not run.** ESP-IDF and the component registry weren't
available in the environment this was written in. What was checked:

* the hook patch applies cleanly to the pinned connectedhomeip `0998b87db7`
* esp-matter 1.6.1 APIs used (endpoint/cluster configs, switch events, attribute
  update, console) against the release branch sources
* LAN865x / PLCA ioctls against `espressif/esp-eth-drivers`
* ESP-IDF v6.0.2 Ethernet/netif/MAC APIs, esp-mqtt config fields, led_strip 3.x
* every `CONFIG_` symbol in `sdkconfig.defaults` that comes from the chip /
  esp_matter / esp_eth / lwIP Kconfig files exists

Expect a round of small fixes on the first `idf.py build`.
