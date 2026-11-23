# Wired Switchboard – Hardware Proposal (rev A)

Target: fits a standard **Ø 68 mm flush-mount box** (DIN 49073 "Gerätedose"),
behind a standard **71 × 71 mm frame / 60 mm screw spacing** (DIN 49075), so
commercial switch frames (Gira, Jung, Busch-Jaeger, Berker…) can be used.

Status: concept. All part numbers are **candidates**; check availability and
datasheet details before layout.

---

## 1. Concept: two-board stack

```
  wall surface
      │   frame + key caps (own design, 3D-printed first)
      │ ┌───────────────────────────────┐
      │ │  FRONT board  46 × 46 mm      │  tact switches, RGB LEDs, SHT4x,
      ├─┤  (on the support ring)        │  vent slots in the caps
      │ └──────────────┬────────────────┘
      │      support ring (steel, screwed to the box at 60 mm)
      │                │ 2×6 board-to-board, 1.27 mm, ~8 mm stack height
      │      ┌─────────┴─────────┐
      │      │  BASE board Ø50   │  ESP32-C6, LAN8651, PoDL, power, MOSFETs
      │      │                   │
      │      └──┬──┬──┬──┬──┬──┬─┘  push-in terminals on the back side
      │         │  │  │  │  │  │
      │      T1S in/out, [24 V load supply, 2 outputs: optional]
      │   ────────────────────────── box floor (47 mm, better 61 mm "deep")
```

Why two boards:

* **Terminals** must be at the back, near the cable entry. **Keys, LEDs and the
  sensor** must be at the front. One board can't do both in 47 mm.
* The **sensor** has to see room air, not the wall cavity. On the front board,
  behind vent slots in the caps, with a thermal slot in the PCB.
* The front board can be swapped (1-, 2- or 4-key layouts, other styles)
  without touching the network or power design.

### Depth budget (standard box, 47 mm)

| Layer | Height |
|---|---|
| Caps in front of the support ring | outside the box |
| Front PCB + tact switches + LEDs | ~4 mm |
| Board-to-board stack | 8 mm |
| Base PCB, top components (module 3.2 mm, inductors ≤ 5 mm) | ~7 mm |
| Base PCB, bottom side with push-in terminals | ~11 mm |
| **Electronics total** | **~30 mm** |
| Cable reserve | ~15 mm (47 mm box), ~30 mm (61 mm box) |

**Recommendation: deep boxes (61 mm, "Elektronikdose").** Loop-through bus
cabling plus four load wires needs the room.

### Outline constraints

* The box interior is ≈ Ø 60–62 mm. The screw bosses for the support ring sit at
  ±30 mm, so their inner edge is at r ≈ 26 mm. → **Base board Ø 50 mm**
  (r = 25 mm) clears them without notches.
* The front board sits in the box mouth: **46 × 46 mm with 6 mm corner
  chamfers** (corner radius < 30 mm), with four M2 holes to hang it from the
  support ring (or clip it into a printed adapter).

## 2. Safety: no mains in this box

The node is **SELV** (24 V). Installing it in a box that also carries 230 V
(for example, an existing switch circuit) needs separation for the highest
voltage present (IEC 60364-4-41 / -5-52, DIN VDE 0100-410/-520): a partition
or reinforced insulation, which a box this size can't provide reasonably.
→ **Use a dedicated box with only bus and low-voltage wiring.** If a
location needs mains (a mains-powered load), put it in a separate box or a
distribution board.

## 3. Base board (Ø 50 mm, 4 layers)

### Block diagram

```
 T1S IN ─┬───────────────────────────────────────┬─ T1S OUT   (loop-through, stub < 10 cm)
         │                                       │
         ├─ ESD (low-C) ─ CMC ─ AC caps ─ LAN8651 ══SPI══ ESP32-C6-WROOM-1-N8
         │                                  │            │  │  │
         │                            [term. 100 Ω       │  │  └─ B2B to front board
         │                             via cap, jumper]  │  │     (3V3, keys, I2C, IRQ)
         └─ PoDL L (2×) ─ TVS ─ eFuse ─ buck 3.3 V ──────┘  │
                                 │                          │ 4× PWM
 ┌ OUTPUT OPTION (DNP on "keypad" variant) ──────────────────────────────────────────────┐
 │24 V LOAD IN ─ fuse ─ TVS ─┬─ LDO 12 V ─ 1× dual gate driver ─ 2× N-MOSFET ─ OUT1..2  │
 │                           └─────────────── common +24 V to loads ──────────── COM+   │
 └───────────────────────────────────────────────────────────────────────────────────────┘
 Variant ID: resistor divider on an ADC pin tells the firmware which option is fitted.
```

### Parts

| Function | Candidate | Notes |
|---|---|---|
| MCU + radio | **ESP32-C6-WROOM-1-N8** (25.5 × 18 mm) | 8 MB flash matches `partitions.csv`. Pre-certified radio (CE/FCC), which matters for a product. Antenna at the board edge with copper keep-out. |
| T1S MAC-PHY | **Microchip LAN8651** (VQFN-32, 5 × 5 mm) | as on the Two-Wire ETH Click, so firmware is unchanged |
| T1S ESD / surge | low-capacitance (< 3 pF) ESD array with ≥ 33 V standoff, plus 2 × varistor line-to-GND (AN1848 PD uses TDK AVRH10C221KT1R5YA8, 220 V) | the bus carries 24 V DC, so 12 V-class automotive ESD parts are not usable |
| T1S coupling (BIN) | CMC **TDK ACT1210D-131-2P** + 2 × 0.1 µF series caps, rated **≥ 50 V** (100 V preferred) | as on Microchip EVB-LAN8651 (AN1848 Fig. 6-2). At 24 V PoDL the series caps see the full bus voltage. |
| Termination | 2 × 49.9 Ω + 2 × 0.22 µF / 100 V, jumper-selectable | the EVB-LAN8651 BIN (AN1848 Fig. 6-2) uses this AC-coupled split termination on jumpers; populate on end nodes only |
| PoDL decoupling | **47 µH differential-mode choke** (DMC), or 47–68 µH coupled inductor (e.g. Coilcraft LPD5030-473 as in AN1848) | AN1848: DMC gives the best noise margin, uncoupled single inductors the worst. Needs matched values, a guaranteed minimum L (±20 % parts) and I_sat well above node current + inrush. At ~25 mA per node (0.5 W at 24 V) a small DMC (AN1848: 300 mA I_sat, 806 mΩ) is enough. |
| Input protection / inrush | **TI TPS2660** eFuse (4.2–55 V, reverse polarity, adjustable current limit and dV/dt), **current limit set to 100 mA** | soft-start keeps hot-plugging from disturbing the bus. Bulk capacitance (AN1848 PD: 470 µF / 35 V) goes **behind** the eFuse, rated ≥ 35 V for a 24 V bus. |
| Buck input filter | LC / π filter between eFuse and buck | AN1848 §6.3: extra noise on the PoDL setup was likely from the DC/DC with only 1 µF input filtering |
| 3.3 V | **TI LMR36006** (60 V, 0.6 A buck) | 60 V rating gives surge margin on a 24 V bus. Load ≈ 120 mA peak. |
| Gate supply | 12 V LDO from the **load** supply | outputs can only switch when the load supply is present |
| Gate drivers | **1 × TI UCC27524A** (dual low-side) | see "PWM switching" below |
| Output switches | 2 × N-MOSFET, 40 V, R_DS(on) < 15 mΩ at V_GS = 10 V, I_D ≥ 10 A, 3 × 3 mm packages | **2 A per output**; low-side switching (common-anode LED strips). Conduction loss 2² · 15 mΩ = 60 mW per channel. |
| Output protection | 5 A fuse on the load input + per-channel current sense (10 mΩ shunt + comparator, trip ≈ 3 A) | 2 × 2 A = 4 A worst case per node; hardware trip on short circuit, firmware reports it |
| Bus / load terminals | push-in PCB terminals: bus for 0.2–0.8 mm solid (J-Y(St)Y), load for 1.5 mm², ≥ 10 A | 4 bus (in/out) + 2 load-in + 2 outputs + 1 × COM+ (up to 4 A). 1.5 mm² copper and 2 A per strip keep wiring losses low. |

### PWM switching: why gate drivers

The firmware PWMs at 19.5 kHz. A GPIO driving a MOSFET gate directly gives
~0.5 µs edges. At 24 V / 2 A per channel:

P_sw ≈ V · I · t_edge · 2 · f = 24 · 2 · 0.5 µs · 2 · 19.5 kHz ≈ **0.9 W per channel**

That is too much heat for a closed wall box. With a 12 V gate driver and a
gate resistor sized for ~100 ns edges, P_sw ≈ 0.2 W per channel. Edges that
are not too fast also keep EMI off the T1S pair running in the same box.
Alternative without drivers: drop the PWM to ~2–4 kHz (`CONFIG_OUTPUT_PWM_FREQ_HZ`).
That's still flicker-free to the eye, but can show banding on camera.

Heat budget at full load (2 × 2 A, PWM active):

| Source | Loss |
|---|---|
| Electronics from PoDL (≈ 0.5 W nominal, 2.4 W max at 100 mA) | 0.5 W |
| MOSFET conduction, 2 × 60 mW | 0.12 W |
| MOSFET switching with gate drivers, 2 × ≈ 0.2 W | 0.4 W |
| Shunts, 2 × 2² · 10 mΩ | 0.08 W |
| **Total** | **≈ 1.1 W** |

That's inside the 1.5 W target, even in the worst case (both channels
PWM-dimmed at 2 A). At 100 % duty, switching losses vanish (≈ 0.7 W total).
The keypad variant (no outputs) dissipates only the electronics, ≈ 0.5 W.

### Grounding

* PoDL "−" and the load supply 0 V meet at one star point on the base board.
* LED return current flows only in the load-supply loop. Keep it away from the
  LAN8651 / MDI ground.
* Use floating (class II) SELV supplies for the loads, so several nodes with
  their own supplies don't form ground loops through the bus pair.

### PoDL at 24 V: checked against Microchip AN1848

Source: *AN1848 Using Power over Data Line functionality in 10BASE-T1S
System*, DS60001848B (2025).

**What the app note says**

* There is **no PoDL standard for multidrop T1S**. IEEE 802.3cg only covers
  point-to-point. Multidrop PoDL is an "engineered system": the PSE simply
  powers known PDs, with no classification or negotiation (§2).
* Their evaluation PSE runs from 12 V, with options for 12 V straight through
  (16–36 V input), 4–9 V buck, or **16–24 V boost** output, plus slew-rate
  limiting and high/low-side protection (Fig. 3-1). **All published
  measurements are at ~12 V.**
* Tested: up to **8 nodes**, segments up to 49 m, 22 AWG twisted pair
  (0.46 Ω/m DC resistance as stated), nodes **≥ 15 cm apart**. PSE located at the end or in the middle of the segment.
* Measured totals at 12 V with EVB-LAN8651 nodes: 2 nodes 160 mA, 5 nodes 480 mA,
  8 nodes 740 mA, up to ~1 A under load steps (Tables 5-2, 5-3). PoDL had no
  visible effect on eye shape, only slightly lower alien-noise margin (§7).
* PSE injector: 47 µH coupled inductor rated 1.44 A (§6).

**What that means for 24 V** (my assessment, not tested in the AN)

| Aspect | 24 V vs 12 V |
|---|---|
| Line current for 8 × 0.5 W | ≈ 170 mA vs 330 mA: inductor saturation and DCR losses become a non-issue |
| Voltage drop, J-Y(St)Y 0.8 mm (≈ 0.07 Ω/m loop), 25 m, PSE at end | ≈ 0.3 V, negligible at either voltage |
| Component ratings | AC coupling caps, termination caps, bulk caps and ESD standoff must be rated for 24 V + surge (≥ 50 V caps, ≥ 33 V ESD standoff). The AN's 100 V / 220 V parts already are. |
| Headroom | Leaves room for a few watts of extra node loads later, or longer segments |
| Risk | Outside Microchip's measured range: validate eye/noise margin at 24 V on the bench (the AN's PSE concept does include a 16–24 V option) |

**Verdict: 24 V is a sound choice**, provided the MDI-side parts are rated for
it. Keep the design within the AN's tested envelope otherwise: ≤ 8 nodes,
nodes ≥ 15 cm apart, 47 µH DMC or coupled inductor per node, filtered
DC/DC input. Simulate the segment (Microchip used Keysight ADS; the AN shows
simulation was slightly pessimistic vs measurement) and check the 15 ns jitter
limit with alien noise.

**Power budget:** 100 mA per node at 24 V = 2.4 W max per node (nominal
≈ 0.5 W leaves headroom for a presence sensor or brighter LEDs). 8 nodes →
0.8 A max on the segment. Per-node decoupling choke I_sat ≥ 300 mA.

**PSE (at the bridge):** 24 V SELV PSU (≥ 30 W) → eFuse with current limit
≈ 1 A → LC filter for PSU ripple → 47 µH coupled inductor rated ≥ 1.44 A (as in
AN1848) → bus. Voltage drop at 0.8 A over 25 m of 0.8 mm cable ≈ 1.4 V
(worst case, all load at the far end), so the farthest node still sees ≥ 22 V.

## 4. Front board (46 × 46 mm, 2 layers): 2-key variant

| Function | Candidate | Notes |
|---|---|---|
| Keys | **2** × tact switches, long life (≥ 1 M cycles) | two side-by-side caps (each ≈ 22 × 46 mm); the switch sits in the centre of each cap with a pivot at the frame edge |
| Key LEDs | 2 × RGB LED + **TI LP5012** I2C RGB driver (12 ch, 4 RGB, 2 spare for a status LED) | runs at 3.3 V, unlike WS2812 (≥ 3.5–5 V). Shares the I2C bus with the sensor. Light pipes in the caps. |
| Sensor | **Sensirion SHT45** (or SHT40) | thermal slot in the PCB around it, vent slots in the caps, away from the LED driver |
| Optional | ambient light sensor (auto LED brightness), PIR/radar presence | later front-board variants |
| B2B | 2 × 6, 1.27 mm | 3V3, GND, KEY0–1, SDA, SCL, LP5012 EN, spares (kept so a 4-key front board fits the same base) |

Firmware impact: replace `indicators.c` (WS2812/RMT) with an LP5012 driver.
Keys, sensor and the rest stay unchanged. The pin map moves to the custom
board (no USB-JTAG pins needed in the field; keep a 6-pin Tag-Connect pad for
UART/boot).

## 5. Alternative: interface behind standard push-buttons

If you want commercial switch inserts instead of your own caps, use a
"push-button interface" variant (like KNX Tasterschnittstellen):

* Base board only. 4 key inputs on terminals, wired to standard momentary
  push-button inserts (e.g., 2-gang rocker push-buttons).
* Mains-rated push-buttons with silver contacts are unreliable at µA/3.3 V
  (no contact wetting). Provide ~5–10 mA wetting current per input during
  the press (pull-up from 12 V with a series resistor, clamped into the GPIO).
* Drawbacks: no key LEDs, and the sensor ends up in the wall cavity (useless).
* Advantage: matches any switch design and needs no custom mechanics.

My recommendation: **design rev A as the two-board stack** (it covers all
four functions you want), and keep the interface variant as an option for
rooms where you want a specific switch range.

### Assembly variants

| Variant | Fitted | Use |
|---|---|---|
| **Keypad** | base without the output block | wall switch + LEDs + sensor; controls other devices via Matter/MQTT |
| **Keypad + 2 outputs** | full base | also dims up to 2 × 2 A LED loads locally |

The output block (load terminals, fuse, TVS, 12 V LDO, gate driver, MOSFETs,
shunts) sits in one PCB area, so DNP leaves no dead copper in the way.
**Variant ID:** a resistor divider on a spare ADC GPIO (e.g. 10 k / 0 Ω vs
10 k / 10 k) is read at boot. One firmware image serves both variants. The
`sb outputs` console setting overrides it for the DevKit.

## 6. Bring-up / test features

* Tag-Connect TC2030 (or 6 pads): UART0 + EN + GPIO9 (boot) for flashing; USB
  isn't routed to the field board.
* Test points: bus voltage (behind the eFuse), 3V3, 12 V gate, each output, MDI±.
* Bus voltage divider to an ADC pin, for brown-out warning and logging
  (planned `power_mon` in firmware).
* LAN8651 RESET_N and IRQ_N on dedicated GPIOs (as in firmware).
* Solder jumper for the T1S termination; silkscreen "END NODE ONLY".

## 7. Open points before layout

1. ~~Bus voltage~~: **24 V**, **100 mA per node**, PSE limit ≈ 1 A for 8 nodes.
2. ~~Load current~~: **2 A per output**, **2 optional outputs**, 4 A worst case per node (5 A fuse).
3. **Cable**: J-Y(St)Y 2×2×0.8 with one pair for T1S + PoDL. The second pair is
   only good for small loads (≈ 0.7 Ω loop resistance at 10 m).
4. ~~Key layout~~: **2 keys**, key n drives output n locally when fitted. The base
   board keeps 4 key inputs (spare B2B pins) for a later 4-key front.
5. **Enclosure for the caps**: 3D-printed first, then compatible with a
   chosen 50 × 50 / 55 × 55 frame system.
