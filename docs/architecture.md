# Wired Switchboard – Architecture

A wired smart-home node: **nRF52840** + **10BASE-T1S** single-pair Ethernet,
**powered over the data line**, speaking **MQTT** and **Matter**.

Status: phase 1 scaffold (T1S + MQTT). Matter is phase 2, see §5. Not yet
built on hardware (see the README's "Verification status" section).

---

## 1. System overview

```
                        home LAN (IPv4 + IPv6, mDNS)
   ┌───────────────┐          │
   │ QNAP NAS      │──────────┤
   │  Mosquitto    │          │
   │  Home Assist. │          │        ┌────────────────────────────┐
   │  (Matter ctrl)│          ├────────│ T1S ↔ 100BASE-TX bridge    │
   └───────────────┘          │        │ Linux box + EVB-LAN8670-USB│
                              │        │ PLCA coordinator (node 0)  │
                              │        │ + PoDL power injector      │
                              │        └─────────────┬──────────────┘
                              │                      │ 1 twisted pair, multidrop
                              │      ════════════════╪════════╤═════════╤═══  (≤25 m, ≥8 nodes)
                              │                      │        │         │
                              │                 ┌────┴───┐┌───┴────┐┌───┴────┐
                              │                 │ node 1 ││ node 2 ││ node n │
                              │                 └────────┘└────────┘└────────┘
```

Each node:

```
 T1S pair ──┬── coupling (CMC + AC caps) ── LAN8651 MAC-PHY ──SPI (16 MHz)── nRF52840
            │                                  IRQ_N, RESET_N ──────────────┤
            └── PoDL decoupling (inductors) ── TVS / reverse prot. ──┐      ├── relays (GPIO → driver)
                                                                     │      ├── switch inputs (opto/RC)
                         bus voltage sense (divider) ── SAADC ◄──────┤      ├── status LED
                                                                     │      └── BLE (Matter commissioning)
                                        wide-input buck → 3.3 V ─────┘
```

## 2. Hardware decisions

| Topic | Decision | Rationale |
|---|---|---|
| Ethernet MAC | **Microchip LAN8650/8651** (OPEN Alliance TC6 MAC-PHY over SPI) | nRF52840 has no Ethernet MAC/RMII. LAN865x has an upstream Zephyr driver (`microchip,lan865x`) incl. PLCA config. LAN8651 = 3.3 V I/O variant. |
| SPI | **SPIM3** @ 16 MHz | Only nRF52840 SPIM > 8 MHz. SPIM3 offers 16/32 MHz, LAN865x max is 25 MHz → 16 MHz. TC6 overhead (4 B per 64 B chunk) leaves comfortably > 10 Mbit/s. |
| Media access | **PLCA** (802.3cg clause 148) | Deterministic, no collisions on a multidrop segment. Node ID provisioned per node (`sb plca <id>`), coordinator = bridge (node 0). |
| MAC address | Locally administered from FICR DEVICEID | Good for development. For a product: OUI block or EUI-48 EEPROM (e.g. 24AA02E48). |
| Power | **PoDL** – DC superimposed on the pair, inductive decoupling | Power on *multidrop* T1S is still being standardised (IEEE 802.3da); point-to-point PoDL (802.3bu/cg) does not cover a mixing segment. Treat it as a *custom* "DC on the pair" design for now: e.g. 24 V, current-limited injector at the bridge. |
| Bootloader | MCUboot (sysbuild) | DFU over SMP; later over the network (SMP/UDP). |

### PoDL design notes (hardware checklist)

* Decoupling inductors must keep their inductance at the full DC load
  current (saturation!) and present high impedance down to the low end of
  the T1S spectrum – that is the dominant cause of signal integrity issues.
  Use the LAN865x / OPEN Alliance coupling recommendations as the starting
  point and simulate the network (you have Simulink/Simscape: a lumped
  LC + line model is enough to check droop and return loss trends).
* Multidrop + power: worst case is the far-end node with all relays on.
  Budget: bus voltage × allowable drop vs. cable resistance × Σ load.
  Relay coils dominate – prefer latching relays or SSRs/MOSFETs.
* Inrush: each node needs a soft-start / current limit on its buck input,
  otherwise hot-plugging a node disturbs the bus voltage for everyone.
* Firmware side: monitor bus voltage on SAADC (planned module
  `power_mon`), flush pending state to NVS on brown-out warning (POFWARN),
  and stagger relay switching after power-up.
* Brown-outs are *expected* events → channel state restore is on by default
  (`CONFIG_APP_STATE_RESTORE`, debounced NVS write to limit wear).

## 3. Software architecture

```
 ┌────────────────────────────────────────────────────────────────────┐
 │  Front-ends            mqtt_app.c          matter/ (phase 2, C++)   │
 │                        HA discovery        On/Off clusters          │
 │                            ▲  │                 ▲  │                │
 │                  listener  │  │ dm_set()        │  │                │
 │  ┌─────────────────────────┴──▼─────────────────┴──▼─────────────┐  │
 │  │  device_model.c  – single owner of channel state, persistence │  │
 │  └─────────────────────────▲──┬──────────────────────────────────┘  │
 │                  dm_toggle │  │ listener                            │
 │  io_board.c  buttons ──────┘  └──► relays          status_led.c     │
 │                                                                     │
 │  net_t1s.c  MAC, PLCA, hostname, DHCPv4/IPv6, conn_mgr L4 events    │
 │  app_settings.c (NVS)      app_shell.c (`sb …` provisioning)        │
 ├─────────────────────────────────────────────────────────────────────┤
 │  Zephyr: net stack (IPv4/IPv6/TCP/UDP, mDNS), MQTT lib, settings    │
 │  eth_lan865x + OA TC6 + microchip,t1s-phy        MCUboot / mcumgr   │
 └─────────────────────────────────────────────────────────────────────┘
```

Key rules:

1. **The device model owns the state.** MQTT, Matter, buttons and the
   shell only *request* changes. No protocol talks to another protocol.
   This is what keeps Home Assistant (via MQTT) and a Matter controller
   consistent when both are used.
2. **Listeners must not block.** They run on the system work queue.
   `mqtt_app` only marks channels dirty; its own thread publishes.
3. **Network-agnostic front-ends.** They wait for conn_mgr's "L4 connected"
   and reconnect with jittered exponential backoff (important when many
   nodes on one segment lose power together).

### MQTT topic layout

| Topic | Dir | Payload |
|---|---|---|
| `switchboard/<name>/status` | pub, retained, LWT | `online` / `offline` |
| `switchboard/<name>/ch/<n>/state` | pub, retained | `ON` / `OFF` |
| `switchboard/<name>/ch/<n>/set` | sub, QoS 1 | `ON` / `OFF` / `TOGGLE` |
| `homeassistant/switch/<name>/ch<n>/config` | pub, retained | HA discovery JSON |

## 4. Memory budget (nRF52840: 1 MB flash, 256 kB RAM)

Rough estimates, **to be replaced with `west build -t rom_report/ram_report`**:

| Item | Flash | RAM |
|---|---|---|
| Zephyr kernel, drivers, shell, logging | ~70 kB | ~25 kB |
| Net stack (IPv4+IPv6, TCP, DHCP, mDNS) + buffers | ~110 kB | ~60 kB |
| MQTT + app | ~25 kB | ~15 kB |
| MCUboot | 48 kB | – |
| **Phase 1 total** | **~250 kB + 2× slot** | **~100 kB** |
| Matter (no Thread, BLE commissioning, mbedTLS/PSA) | +450–550 kB | +90–110 kB |

Conclusion: phase 1 fits easily with both MCUboot slots in internal flash.
**Phase 2 does not fit with two internal slots** → secondary slot on
external QSPI NOR (MX25R64 on the DK; plan a 2–8 MB QSPI/SPI NOR on the
custom board from day one). RAM for Matter + IPv4/IPv6 + MQTT is tight but
feasible; Thread is not used, which saves ~40 kB RAM vs. the usual nRF52840
Matter builds.

## 5. Matter over 10BASE-T1S – the main technical risk

Matter is IP-based and transport-agnostic, so Ethernet is a legitimate
transport (Matter defines an Ethernet network commissioning feature).
However, **Nordic's Matter port does not support Ethernet today**: in NCS
v3.3.0 `src/platform/nrfconnect/CHIPDevicePlatformConfig.h` states
*"nrfconnect platform does not support ethernet yet"* and forces
`CHIP_DEVICE_CONFIG_ENABLE_ETHERNET 0`. Nordic's Matter samples on nRF52840
are Thread-only.

What is needed (phase 2 work package):

1. Build Matter with Thread and Wi-Fi off, Ethernet on
   (`CHIP_DEVICE_CONFIG_ENABLE_ETHERNET=1`). The shared Zephyr platform layer
   (`src/platform/Zephyr/ConnectivityManagerImpl.cpp`) already has Ethernet
   code paths (multicast join etc.), so this is mostly Kconfig/GN plumbing
   in the `nrfconnect` platform.
2. Implement a small **`EthernetDriver`** for the Network Commissioning
   cluster (Ethernet feature: no credentials, just report the interface).
3. Use **minimal mDNS** from the Matter SDK for operational discovery – and
   then **disable Zephyr's `MDNS_RESPONDER`** to avoid two responders on
   UDP 5353 (or keep Zephyr's and implement the Matter DNS-SD platform API
   on top of it; minimal mDNS is less work).
4. Commissioning: over **BLE** (the nRF52840's radio is otherwise unused),
   or on-network (DNS-SD `_matterc._udp`) since Ethernet is always up. BLE is
   the smoother path with phone-based controllers.
5. Add a `matter/` front-end (C++) that maps On/Off cluster endpoints 1…n to
   device-model channels and registers a DM listener for attribute reports.

Network prerequisites: the T1S bridge must forward IPv6 link-local and
multicast (mDNS) between the T1S segment and the home LAN, i.e. a **bridge,
not a router**. A plain Linux bridge does that.

Fallback options if (1)–(3) turn out heavier than expected:

* **Bridge pattern:** keep the nodes MQTT-only and expose them to Matter via
  a Matter bridge on the NAS / Linux box (e.g. the community *Matterbridge* project in a
  container on the NAS, or a custom `chip-bridge-app`). Cheapest, but the node itself is
  not Matter certified-able.
* **Bigger MCU:** nRF54L15 (1.5 MB RRAM, 256 kB RAM, same SDK) or nRF5340
  (1 MB + 256 kB app core, network core for BLE) relieve the flash pressure;
  the Ethernet port work is the same.

## 6. Roadmap

| Phase | Content | Exit criterion |
|---|---|---|
| 0 | DK + LAN8651 click, Linux bridge with EVB-LAN8670-USB | `ping` and `zperf` over T1S with PLCA, 2+ nodes |
| 1 | **this scaffold**: device model, I/O, settings, MQTT + HA discovery | Channels switchable from HA, state survives power cycle |
| 1.1 | `power_mon` (SAADC + POFWARN), MQTT over TLS, SMP over UDP for network DFU | Field-updatable, secure broker connection |
| 2 | Matter over Ethernet (§5), external flash secondary slot | Commissioned in HA Matter + Apple/Google, MQTT still in sync |
| 3 | Custom board: PoDL front-end, relay drivers, EMC | Hardware validated on a 25 m multidrop segment |
