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
    indicators.*      key LEDs: LP5012 (rev A) or WS2812 (DevKit)
    i2c_bus.*         shared I2C bus (LP5012, SHT45)
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
# Required patches (the build stops without them):
#  0001 Ethernet hook for SPI MAC-PHYs, 0002 Kconfig GPIO range for ESP32-C6
git apply ~/src/wired_switchboard/firmware/patches/*.patch
cd ../..
. ~/esp/esp-idf/export.sh
./install.sh
```

Per shell:

```bash
. ~/esp/esp-idf/export.sh && . ~/esp/esp-matter/export.sh
```

Or let the script do all of the above: `tools/setup_esp.sh ~/esp ~/src/wired_switchboard`
(it also works around a Python 3.13 pip conflict in Matter's optional host-tool env),
then `tools/build.sh ~/esp ~/src/wired_switchboard devkit|reva`.

## Build, flash, monitor (ESP32-C6-DevKitC-1-N8)

```bash
cd firmware
idf.py set-target esp32c6
idf.py menuconfig          # Wired Switchboard -> pins, counts, PLCA, MQTT
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The DevKit's USB port is the console (USB-Serial-JTAG).

### Rev A base board

```bash
idf.py -B build-reva -D SDKCONFIG=build-reva/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.board.reva" build
idf.py -B build-reva -p /dev/ttyUSB0 flash monitor    # UART0 via Tag-Connect
```

This selects the rev A pin map, the LP5012 indicator driver and the UART
console. Keep separate build directories per board, because Kconfig pin
defaults only apply to a fresh sdkconfig.

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

**Builds, not yet run on hardware.** Both configurations compile and link
with ESP-IDF v6.0.2 + esp-matter release/v1.6.1 (connectedhomeip `0998b87db7`
with `firmware/patches/*` applied):

| Config | Image | Free in 3 MB OTA slot | Static DIRAM |
|---|---|---|---|
| DevKitC-1 (WS2812, BOOT reset) | 1.73 MB | 45 % | 162 kB of 452 kB (36 %) |
| rev A (LP5012, power_mon, UART console) | 1.72 MB | 45 % | similar |

`dependencies.lock` pins the managed components (lan865x, mqtt, led_strip, …).
Next: flash the DevKit + Two-Wire ETH Click, bring up T1S/PLCA, commission
over BLE.
