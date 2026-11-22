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
      │      T1S in/out, 24 V load supply, 4 outputs
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
 24 V LOAD IN ─ fuse ─ TVS ─┬─ LDO 12 V ─ 2× dual gate driver ─ 4× N-MOSFET ─ OUT1..4
                            └─────────────── common +24 V to loads ───────────── COM+
```

### Parts

| Function | Candidate | Notes |
|---|---|---|
| MCU + radio | **ESP32-C6-WROOM-1-N8** (25.5 × 18 mm) | 8 MB flash matches `partitions.csv`. Pre-certified radio (CE/FCC), which matters for a product. Antenna at the board edge with copper keep-out. |
| T1S MAC-PHY | **Microchip LAN8651** (VQFN-32, 5 × 5 mm) | as on the Two-Wire ETH Click, so firmware is unchanged |
| T1S ESD | low-capacitance (< 3 pF) array, ≥ 24 V standoff, rated for T1S / CAN-FD | the bus carries PoDL DC, so standoff must exceed the bus voltage |
| T1S coupling | common-mode choke + AC coupling caps per Microchip / OPEN Alliance T1S reference | |
| Termination | 100 Ω **in series with a capacitor**, solder jumper | end nodes only. DC-blocked so it doesn't load the PoDL supply. |
| PoDL decoupling | coupled inductor or 2 × power inductor, I_sat > bus current | follow the OPEN Alliance PoDL-for-T1S coupling guidance; simulate return loss |
| Input protection / inrush | **TI TPS2660** eFuse (4.2–55 V, reverse polarity, adjustable current limit and dV/dt) | soft-start keeps hot-plugging from disturbing the bus |
| 3.3 V | **TI LMR36006** (60 V, 0.6 A buck) | 60 V rating gives surge margin on a 24 V bus. Load ≈ 120 mA peak. |
| Gate supply | 12 V LDO from the **load** supply | outputs can only switch when the load supply is present |
| Gate drivers | **2 × TI UCC27524A** (dual low-side) | see "PWM switching" below |
| Output switches | 4 × N-MOSFET, 40 V, R_DS(on) < 15 mΩ, in 3 × 3 mm packages | low-side switching (common-anode LED strips) |
| Output protection | fuse on load input + per-channel current sense (shunt + comparator, or ADC) | short-circuit shutdown in firmware or hardware |
| Bus / load terminals | push-in PCB terminals: bus for 0.2–0.8 mm solid (J-Y(St)Y), load for 1.5 mm² | 4 bus terminals (in/out) + 2 load-in + 4 outputs + 1–2 COM+ |

### PWM switching: why gate drivers

The firmware PWMs at 19.5 kHz. A GPIO driving a MOSFET gate directly gives
~0.5 µs edges. At 24 V / 2 A per channel:

P_sw ≈ V · I · t_edge · 2 · f = 24 · 2 · 0.5 µs · 2 · 19.5 kHz ≈ **0.9 W per channel**

That is too much heat for a closed wall box. With a 12 V gate driver and a
gate resistor sized for ~100 ns edges, P_sw ≈ 0.2 W per channel. Edges that
are not too fast also keep EMI off the T1S pair running in the same box.
Alternative without drivers: drop the PWM to ~2–4 kHz (`CONFIG_OUTPUT_PWM_FREQ_HZ`).
That's still flicker-free to the eye, but can show banding on camera.

Target heat budget for the whole node: **< 1.5 W** (electronics ≈ 0.5 W +
conduction and switching losses).

### Grounding

* PoDL "−" and the load supply 0 V meet at one star point on the base board.
* LED return current flows only in the load-supply loop. Keep it away from the
  LAN8651 / MDI ground.
* Use floating (class II) SELV supplies for the loads, so several nodes with
  their own supplies don't form ground loops through the bus pair.

## 4. Front board (46 × 46 mm, 2 layers)

| Function | Candidate | Notes |
|---|---|---|
| Keys | 4 × right-angle or top tact switches, long life (≥ 1 M cycles) | rocker caps press on them; a 2-key version uses 2 rockers × up/down |
| Key LEDs | 4 × RGB LED + **TI LP5012** I2C RGB driver (12 ch, 4 RGB) | runs at 3.3 V, unlike WS2812 (≥ 3.5–5 V). Shares the I2C bus with the sensor. Light pipes in the caps. |
| Sensor | **Sensirion SHT45** (or SHT40) | thermal slot in the PCB around it, vent slots in the caps, away from the LED driver |
| Optional | ambient light sensor (auto LED brightness), PIR/radar presence | later front-board variants |
| B2B | 2 × 6, 1.27 mm | 3V3, GND, KEY0–3, SDA, SCL, LP5012 EN, spare |

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

## 6. Bring-up / test features

* Tag-Connect TC2030 (or 6 pads): UART0 + EN + GPIO9 (boot) for flashing; USB
  isn't routed to the field board.
* Test points: bus voltage (behind the eFuse), 3V3, 12 V gate, each output, MDI±.
* Bus voltage divider to an ADC pin, for brown-out warning and logging
  (planned `power_mon` in firmware).
* LAN8651 RESET_N and IRQ_N on dedicated GPIOs (as in firmware).
* Solder jumper for the T1S termination; silkscreen "END NODE ONLY".

## 7. Open points before layout

1. **Bus voltage and PoDL current limit**: proposal 24 V, ≤ 100 mA per node
   (0.5 W nominal, inrush-limited).
2. **Load current per output** (sets MOSFETs, terminals, fuse, heat): proposal
   ≤ 2 A per channel, ≤ 4 A total per node.
3. **Cable**: J-Y(St)Y 2×2×0.8 with one pair for T1S + PoDL. The second pair is
   only good for small loads (≈ 0.7 Ω loop resistance at 10 m).
4. **Key layout**: 1, 2 or 4 rockers, which decides the front-board variants.
5. **Enclosure for the caps**: 3D-printed first, then compatible with a
   chosen 50 × 50 / 55 × 55 frame system.
