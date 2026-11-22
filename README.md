# Wired Switchboard

A wired home-automation node: **nRF52840** + **10BASE-T1S** single-pair Ethernet
(Microchip **LAN8651** MAC-PHY over SPI), **powered over the data line**, with
**MQTT** (Home Assistant discovery) and **Matter** front-ends.

* Firmware: nRF Connect SDK **v3.3.0** (Zephyr 4.3.99), C
* Phase 1 (this repo now): T1S networking with PLCA, relay/button I/O, persistent
  state, MQTT + HA auto-discovery, provisioning shell, MCUboot
* Phase 2: Matter over Ethernet. Needs porting work, because Nordic's Matter port
  doesn't support Ethernet yet. See [docs/architecture.md §5](docs/architecture.md#5-matter-over-10base-t1s--the-main-technical-risk)

Architecture, hardware notes (PoDL, PLCA, memory budget) and roadmap:
**[docs/architecture.md](docs/architecture.md)**

## Repository layout

```
west.yml                     workspace manifest (pins NCS v3.3.0)
app/
  CMakeLists.txt  Kconfig  prj.conf  sysbuild.conf  VERSION
  overlay-debug.conf  overlay-release.conf
  boards/nrf52840dk_nrf52840.overlay   LAN8651 on Arduino SPI + I/O mapping
  src/
    main.c            init order
    device_model.*    single owner of channel state (+ NVS persistence)
    io_board.*        relays, buttons, factory-reset button (from DT zephyr,user)
    net_t1s.*         MAC from FICR, PLCA, hostname, DHCP, online events
    mqtt_app.*        MQTT client, HA discovery, command handling
    app_settings.*    provisioned config in NVS
    app_shell.c       `sb …` shell commands
    status_led.*      status LED patterns
docs/architecture.md
```

## Workspace setup

```bash
# Toolchain (once)
nrfutil sdk-manager toolchain install --ncs-version v3.3.0

# Workspace with this repo as manifest
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- \
  west init -m https://github.com/siphi1239/wired_switchboard --mr main ~/ws-switchboard
cd ~/ws-switchboard
nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- west update
```

> Tip: `alias ncs='nrfutil sdk-manager toolchain launch --ncs-version v3.3.0 -- '`

## Build & flash (nRF52840-DK + LAN8651 click)

```bash
cd ~/ws-switchboard/wired_switchboard/app

ncs west build -b nrf52840dk/nrf52840 --sysbuild -p
ncs west build -b nrf52840dk/nrf52840 --sysbuild -p -- -DEXTRA_CONF_FILE=overlay-debug.conf

# Console on USB CDC-ACM (nRF USB port) instead of the J-Link VCOM:
ncs west build -b nrf52840dk/nrf52840 --sysbuild -p -S cdc-acm-console

ncs west flash
```

Wiring (DK Arduino header → LAN8651): D13 SCK, D12 MISO, D11 MOSI, D10 CS,
**D7 IRQ_N, D8 RESET_N**. Change the last two in
`app/boards/nrf52840dk_nrf52840.overlay` to match your click shield.

## Bring-up

### 1. The Linux side of the T1S segment (bridge + PLCA coordinator)

A USB T1S adapter (e.g. Microchip **EVB-LAN8670-USB**) on any Linux box (a
Raspberry Pi works; the DGX Spark also works for lab use), bridged to the home LAN:

```bash
# PLCA coordinator: node 0, segment size 8
sudo ethtool --set-plca-cfg eth1 enable on node-id 0 node-cnt 8
sudo ethtool --get-plca-status eth1

# L2 bridge so IPv6 link-local + mDNS reach the nodes (required for Matter)
sudo ip link add br0 type bridge
sudo ip link set eth0 master br0 && sudo ip link set eth1 master br0
sudo ip link set br0 up
```

Inject PoDL power at this end through the coupling network (see architecture doc).

### 2. Provision the node (shell on the console)

```
sb show
sb plca 1 8                 # unique node id per node, 1..7 here
sb name kitchen-board       # MQTT topic / HA device id
sb mqtt 192.168.1.10 1883   # Mosquitto on the QNAP
sb ch 0 toggle
net iface                   # Zephyr net shell: addresses, link
net ping 192.168.1.10
```

`<name>-<mac>.local` is announced over mDNS. Button 4 held for 5 s does a
factory reset.

### 3. MQTT / Home Assistant

Mosquitto (e.g. Container Station on the QNAP) plus the HA MQTT integration.
Entities show up automatically through discovery. Manual test:

```bash
mosquitto_sub -h <broker> -t 'switchboard/#' -v
mosquitto_pub -h <broker> -t switchboard/kitchen-board/ch/0/set -m ON
```

## DFU

MCUboot + mcumgr SMP over the shell UART (`CONFIG_MCUMGR_TRANSPORT_SHELL`):

```bash
mcumgr --conntype serial --connstring /dev/ttyACM0,baud=115200 image upload build/app/zephyr/zephyr.signed.bin
mcumgr --conntype serial --connstring /dev/ttyACM0,baud=115200 image list
mcumgr --conntype serial --connstring /dev/ttyACM0,baud=115200 reset
```

SMP over UDP (network DFU) is planned for phase 1.1.

## Verification status

**Not yet compiled.** No full NCS build was possible in the environment this
scaffold was written in. What was checked against the NCS v3.3.0 sources
(`sdk-zephyr@ncs-v3.3.0`):

* every Zephyr API used (net_mgmt Ethernet/T1S requests, conn_mgr, MQTT, hwinfo,
  hostname, settings, GPIO) exists with the signature used
* every `CONFIG_` symbol in `prj.conf` and the overlays exists
* the LAN865x/MDIO/T1S-PHY devicetree layout matches the binding in that tree.
  Upstream Zephyr 4.4 changes it: `phy-handle` becomes mandatory, and the MAC
  address moves to an nvmem-layout node. Update the overlay when moving past
  NCS v3.3.

Expect a round of small compile fixes on the first `west build`.
