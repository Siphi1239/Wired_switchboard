# Wired Switchboard – Architecture

A wired smart-home wall node: **ESP32-C6** + **Microchip LAN8651** 10BASE-T1S
single-pair Ethernet, **powered over the data line**, with **Matter** and
**MQTT** front-ends.

Per node:

* **2 keys** by default, up to 4 (momentary wall switches) → Matter Generic Switch events, MQTT
  events, and local control of outputs
* up to 4 **low-voltage PWM outputs** (MOSFET, 12/24 V LED strips) → Matter
  Dimmable Light, HA MQTT light
* one **indicator LED** per key (WS2812 on the DevKit, TI LP5012 on the custom board) → follow their output or
  are driven remotely
* **SHT4x** temperature / humidity → Matter sensors, HA MQTT sensors

Status: firmware scaffold, **not yet built or run on hardware** (see README).

---

## 1. Why ESP32-C6 (the MCU change)

| | nRF52840 (v0.1) | **ESP32-C6** (now) |
|---|---|---|
| Matter over Ethernet | not supported in Nordic's Matter port: porting job | supported in the upstream ESP32 platform (`ConnectivityManagerImpl_Ethernet`, `ESPEthernetDriver`); Espressif ships an Ethernet light example |
| LAN865x driver | Zephyr upstream | Espressif `espressif/lan865x` |
| RAM / flash | 256 kB / 1 MB, too small for Matter + MQTT + OTA | 512 kB SRAM / 8 MB ext. flash, 3 MB OTA slots |
| Commissioning | BLE | BLE (NimBLE, freed after commissioning) |

Remaining gap: the ESP32 Ethernet driver in connectedhomeip hardcodes the
internal EMAC + IP101 PHY, which the C6 doesn't have. The fix is a 50-line
patch (`firmware/patches/0001-esp32-ethernet-app-init-hook.patch`) that adds a
weak `esp_matter_app_ethernet_init()` hook. The app implements it in
`eth_t1s.c` and installs the LAN865x SPI driver; Matter still attaches the
netif and starts it. The patch is upstreamable.

**Risk to retire first:** SPI MAC-PHY + Matter is not one of Espressif's
tested configurations. Week-1 spike: commission the node into Home Assistant
over T1S and check that mDNS (`_matter._tcp`) and IPv6 work through the bridge.

## 2. System overview

```
   ┌──────────────────┐                                ┌──────────────────────────────┐
   │ QNAP NAS         │        home LAN                │ T1S ↔ 100BASE-TX bridge      │
   │  Mosquitto       ├──────────────┬─────────────────┤ Linux + EVB-LAN8670-USB      │
   │  Home Assistant  │              │                 │ PLCA coordinator (node 0)    │
   │  + Matter server │        phone (BLE commission)  │ + PoDL injector (24 V)       │
   └──────────────────┘                                └──────────────┬───────────────┘
                                                                      │ 1 pair, multidrop
                                 ═════════════════════════════╤═══════╪═══════╤═══════
                                                          ┌───┴───┐┌──┴────┐┌─┴─────┐
                                                          │node 1 ││node 2 ││node n │
                                                          └───────┘└───────┘└───────┘
```

Node:

```
T1S pair ─┬─ CMC + AC coupling ─ LAN8651 ══SPI 20 MHz══ ESP32-C6 ─┬─ keys (GPIO, RC + TVS)
          │                       IRQ_N, RESET_N ────────┘         ├─ PWM ─ MOSFETs ─ 12/24 V LED loads
          └─ PoDL decoupling L ─ protection ─ buck 3.3 V           ├─ WS2812 key LEDs
                                                                    └─ I2C ─ SHT4x
```

## 3. Software architecture (ESP-IDF v6.0.2 + esp-matter 1.6.1)

```
  matter_app.cpp          mqtt_app.c              local_control.c
  endpoints, events       HA discovery, JSON      key→output, dimming, LED follow
        ▲  │                  ▲  │                      ▲  │
        │  │ dm_*()           │  │ dm_*()               │  │ dm_*()
  ┌─────┴──▼──────────────────┴──▼──────────────────────┴──▼─────┐
  │ device_model.c: single owner of state, event queue + task    │
  └─────▲─────────────────────▲───────────┬──────────┬───────────┘
        │ dm_key_event        │ dm_sensor │ events   │ events
   inputs.c (10 ms scan)  sensors.c    outputs.c  indicators.c
                                        LEDC PWM   WS2812 (RMT)
  eth_t1s.c: LAN865x hook, PLCA, MAC, hostname   app_settings.c (NVS) + app_console.c (`sb`)
```

Rules:

1. **The device model owns the state.** Front-ends and local logic call
   `dm_*` setters and receive events. Nobody talks to anybody else directly.
2. **Events only on change.** This breaks Matter ↔ MQTT echo loops by design.
   `matter_app` also suppresses callbacks caused by its own `attribute::update()`.
3. **Threads:** listeners run on the DM task. Matter work is marshalled with
   `SystemLayer().ScheduleLambda()`, and MQTT uses the thread-safe
   `esp_mqtt_client_enqueue()`.
4. **Works offline.** Keys switch and dim their mapped output with no
   controller or broker. Events are still sent to both front-ends for scenes.

### Key event model

Keys implement the Matter Switch cluster with MS | MSR | MSL | MSM:

| Gesture | Matter events | MQTT `event_type` | Local action |
|---|---|---|---|
| single | InitialPress, ShortRelease, MultiPressComplete(1) | `single` | toggle output |
| double | … MultiPressOngoing(2) … MultiPressComplete(2) | `double` | output on at 100 % |
| triple | … MultiPressComplete(3) | `triple` | – (automations) |
| hold | InitialPress, LongPress … LongRelease | `long_press`, `long_release` | dim up/down |

Local toggling happens on *release*, so it isn't delayed by the multi-press
window. For a double press, that means the output toggles first and then goes
to 100 %.

### Matter endpoints (default: 2 keys / 4 outputs / 2 LEDs / sensor)

| EP | Device type | Purpose |
|---|---|---|
| 0 | Root Node | Ethernet network commissioning, OTA requestor |
| 1–2 | Generic Switch | keys |
| 3–6 | Dimmable Light | outputs (2 A each; outputs 3–4 remote-only with 2 keys) |
| 7–8 | On/Off Light | key LEDs (when not in "follow" mode) |
| 9, 10 | Temperature, Humidity Sensor | SHT4x (only if detected) |

### MQTT topics

| Topic | Dir | Payload |
|---|---|---|
| `switchboard/<name>/status` | pub, retained, LWT | `online` / `offline` |
| `…/out/<n>/state`, `…/out/<n>/set` | pub / sub | `{"state":"ON","brightness":1..254}` |
| `…/ind/<n>/state`, `…/ind/<n>/set` | pub / sub | `ON` / `OFF` |
| `…/key/<n>/event` | pub | `{"event_type":"double"}` |
| `…/sensor/temperature`, `…/sensor/humidity` | pub, retained | `21.50`, `45.00` |
| `homeassistant/{light,switch,event,sensor}/<id>/…/config` | pub, retained | discovery |

## 4. Hardware notes

### Default pin map (ESP32-C6-DevKitC-1, all in `menuconfig → Wired Switchboard`)

| Function | GPIO | Notes |
|---|---|---|
| LAN8651 SCLK / MOSI / MISO / CS | 6 / 7 / 2 / 10 | SPI2, 20 MHz (LAN865x max 25) |
| LAN8651 IRQ_N / RESET_N | 11 / 3 | |
| Keys 0–3 | 0, 1, 4, 5 | active low, internal pull-up. Fit external pull-ups + RC + TVS for wall cabling |
| PWM outputs 0–3 | 18, 19, 20, 21 | LEDC 12 bit @ 19.5 kHz → logic-level MOSFET gate drivers |
| WS2812 data | 8 | on-board RGB LED = indicator 0 |
| I2C SDA / SCL | 22 / 23 | SHT4x @ 0x44 |
| Factory reset | 9 (BOOT) | hold 10 s |

Avoid: GPIO 12/13 (USB-JTAG console), 16/17 (UART0), 24–30 (flash), and
strapping pins 8/9/15 for anything that loads them at boot.

The MIKROE Two-Wire ETH Click needs transistor Q1 removed for ESP32 SPI
timing (its CS LED slows the CS edge), per Espressif's driver README.

### Power over the data line: budget first

* **The LED loads decide everything.** A 24 V / 1 A strip is 24 W per channel.
  That is far beyond what a shared multidrop pair can deliver to many nodes.
  Recommended: power the loads from a local 12/24 V supply (or a separate
  power pair in the same cable), and power only the electronics from the
  data line: ESP32-C6 ≈ 25–35 mA with BLE idle, LAN8651 ≈ 40 mA, LEDs and
  sensor a few mA, so ≈ **0.5 W per node**.
* Power on *multidrop* T1S is still being standardised (IEEE 802.3da), so
  treat it as a custom "DC on the pair" design for now: e.g. 24 V,
  current-limited injector at the bridge, inductive decoupling at every node.
* Decoupling inductors must hold their inductance at full DC current and stay
  high-impedance down to the low end of the T1S spectrum. Simulate the segment
  (Simscape: lumped LC + line) for droop and return loss.
* Inrush limiting / soft-start on every node's buck input, or hot-plugging
  disturbs the bus voltage for everyone.
* Brown-outs are expected: output state is saved to NVS (2 s debounce) and
  restored at boot. Planned: bus-voltage monitoring on the ADC, plus staggered
  output start after power-up.
* SHT4x placement: the buck and the MCU warm the PCB. Thermally isolate the
  sensor (PCB slot, bottom of enclosure) or calibrate an offset.

## 5. Memory / flash

8 MB flash: 2 × 3 MB OTA slots, NVS, factory partition (Matter DAC/PAI for
production). Unused Matter clusters are disabled in `sdkconfig.defaults`.
Expected image size is ~1.6–1.9 MB. Measure with `idf.py size` on the first
build. RAM: Matter + lwIP + BLE (commissioning only) + MQTT fits in 512 kB
with margin; BLE is freed after commissioning.

## 6. Roadmap

| Phase | Content | Exit criterion |
|---|---|---|
| 0 | DevKitC-1 + Two-Wire ETH Click, Linux bridge with EVB-LAN8670-USB | `ping` over T1S with PLCA, 2+ nodes |
| 1 | **this scaffold**: build, then commission over BLE into HA Matter, MQTT discovery | keys, outputs, LEDs, sensor in HA via both paths, in sync |
| 1.1 | MQTT over TLS, NVS encryption, bus-voltage monitor, Matter Binding (direct key → remote light without HA) | works without the HA host |
| 2 | Custom board: PoDL front-end, MOSFET stage, key inputs with protection, EMC | 25 m segment, 8 nodes, hot-plug |
| 3 | Production: DAC/PAI provisioning, secure boot + flash encryption, certification decision | – |
