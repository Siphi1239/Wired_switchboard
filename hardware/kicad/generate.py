#!/usr/bin/env python3
"""
Generate the KiCad starting point for the Wired Switchboard rev A boards.

    /usr/bin/python3 hardware/kicad/generate.py        (needs KiCad's pcbnew module, KiCad >= 7)

Creates, per board (base, front):
  * <board>.kicad_pro   net classes + design rules (from docs/hardware.md)
  * <board>.kicad_sch   root schematic with hierarchical sheets
  * sheets/*.kicad_sch  one sheet per function block, with design notes and
                        global labels for every inter-sheet / inter-board net
  * <board>.kicad_pcb   outline, stackup layer count, mounting holes,
                        mechanically fixed footprints (locked, real references
                        from hardware/bom-revA.csv), keep-outs and placement
                        zones on User layers

Mechanically critical footprints carry their BOM reference (U1, J4, SW1 ...).
After drawing the schematics use "Update PCB from Schematic" with
"Re-link footprints to schematic symbols based on their reference designators".

Coordinates: board centre = (100, 100) mm in KiCad, both boards coaxial
(box axis). +y is "down" in KiCad; "top of the wall box" = smaller y.
Existing files are overwritten - this script is the source of truth until
real schematic capture starts; after that, edit in KiCad and stop using it.
"""
import json
import math
import os
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
CX, CY = 100.0, 100.0          # board centre in KiCad coordinates [mm]
DATE = "2026-11-23"

# Shared mechanics (both boards, relative to the box axis) -------------------
B2B_POS = (0.0, 8.5)           # 2x6 1.27 mm board-to-board, centre
STANDOFF_POS = [(-18.0, 12.0), (18.0, 12.0)]       # M2 standoffs base <-> front
FRONT_HOLES = [(-18.0, -12.0), (18.0, -12.0), (-18.0, 12.0), (18.0, 12.0)]

# Net classes (KiCad 7 project JSON) -----------------------------------------
NET_CLASSES = [
    # name, clearance, track, via, drill, diff_w, diff_gap, nets pattern list, description
    ("Default", 0.15, 0.20, 0.6, 0.3, 0.2, 0.2, [], "signals"),
    ("T1S", 0.20, 0.20, 0.6, 0.3, 0.20, 0.20, ["/T1S_P*", "/T1S_N*", "T1S_*_P", "T1S_*_N", "TRX_P", "TRX_N"],
     "MDI pair, keep symmetric, length-match P/N; no impedance control needed (< 5 cm)"),
    ("BUS_24V", 0.30, 0.40, 0.8, 0.4, 0.2, 0.2, ["VBUS*", "T1S_PWR*", "+24V*"],
     "PoDL side, 100 mA, 24 V + surge"),
    ("LOAD_2A", 0.30, 1.00, 0.8, 0.4, 0.2, 0.2, ["VLOAD*", "OUT0", "OUT1", "COM+", "LOAD_GND"],
     "2 A per output, 4 A on VLOAD/COM+: use pours or >= 2 mm on 1 oz"),
    ("PWR_3V3", 0.15, 0.40, 0.6, 0.3, 0.2, 0.2, ["+3V3", "+12V_GATE"], "logic supply"),
]

DRU = """(version 1)
# Wired Switchboard rev A - custom design rules (KiCad >= 7)

(rule "24V creepage on outer layers"
  (condition "A.NetClass == 'BUS_24V' && B.NetClass != 'BUS_24V'")
  (constraint clearance (min 0.3mm)))

(rule "Load current tracks"
  (condition "A.NetClass == 'LOAD_2A'")
  (constraint track_width (min 1.0mm) (opt 2.0mm)))

(rule "No copper under the ESP32-C6 antenna"
  (condition "A.insideArea('ANTENNA_KEEPOUT')")
  (constraint disallow track via zone))

(rule "T1S pair symmetric"
  (condition "A.NetClass == 'T1S'")
  (constraint diff_pair_uncoupled (max 3mm))
  (constraint skew (max 0.5mm)))

(rule "Thermal slot around SHT45"
  (condition "A.insideArea('SENSOR_ISLAND')")
  (constraint disallow zone))
"""


# --------------------------------------------------------------------------
# Project file
# --------------------------------------------------------------------------
def write_project(path, name):
    classes = []
    assignments = []
    for (n, clr, trk, via, drill, dw, dg, pats, desc) in NET_CLASSES:
        classes.append({
            "name": n, "clearance": clr, "track_width": trk, "via_diameter": via, "via_drill": drill,
            "microvia_diameter": 0.3, "microvia_drill": 0.1, "diff_pair_width": dw, "diff_pair_gap": dg,
            "diff_pair_via_gap": 0.25, "wire_width": 6, "bus_width": 12, "line_style": 0,
            "schematic_color": "rgba(0, 0, 0, 0.000)", "pcb_color": "rgba(0, 0, 0, 0.000)",
        })
        for p in pats:
            assignments.append({"netclass": n, "pattern": p})
    pro = {
        "board": {
            "design_settings": {
                "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2,
                             "silk_line_width": 0.12, "silk_text_size_h": 0.8, "silk_text_size_v": 0.8,
                             "silk_text_thickness": 0.12},
                "rules": {"min_clearance": 0.15, "min_track_width": 0.127, "min_via_diameter": 0.45,
                          "min_through_hole_diameter": 0.2, "min_copper_edge_clearance": 0.3,
                          "min_hole_clearance": 0.25, "min_hole_to_hole": 0.25,
                          "min_silk_clearance": 0.0, "min_text_height": 0.8},
                "track_widths": [0.0, 0.2, 0.4, 1.0, 2.0],
                "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.6, "drill": 0.3},
                                   {"diameter": 0.8, "drill": 0.4}],
                "diff_pair_dimensions": [{"width": 0.0, "gap": 0.0, "via_gap": 0.0},
                                         {"width": 0.2, "gap": 0.2, "via_gap": 0.25}],
            },
            "layer_presets": [], "viewports": [],
        },
        "meta": {"filename": f"{name}.kicad_pro", "version": 1},
        "net_settings": {"classes": classes, "meta": {"version": 3},
                         "netclass_patterns": assignments, "netclass_assignments": None},
        "pcbnew": {"last_paths": {}, "page_layout_descr_file": ""},
        "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
        "sheets": [], "text_variables": {"REV": "A", "PROJECT": "Wired Switchboard"},
    }
    with open(path, "w") as f:
        json.dump(pro, f, indent=2)
        f.write("\n")


# --------------------------------------------------------------------------
# Schematics (s-expression, KiCad 7 format 20230121)
# --------------------------------------------------------------------------
def u():
    return str(uuid.uuid4())


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def title_block(title, comment):
    return (f'  (title_block (title {q(title)}) (date {q(DATE)}) (rev "A")\n'
            f'    (company "Wired Switchboard") (comment 1 {q(comment)})\n'
            f'    (comment 2 "Concept - see docs/hardware.md and hardware/bom-revA.csv"))\n')


def text_block(x, y, lines):
    body = "\\n".join(l.replace("\\", "\\\\").replace('"', '\\"') for l in lines)
    return (f'  (text "{body}" (at {x} {y} 0)\n'
            f'    (effects (font (size 1.27 1.27)) (justify left top))\n'
            f'    (uuid {u()}))\n')


def global_labels(x, y, names, shape="bidirectional"):
    out = []
    for i, n in enumerate(names):
        out.append(f'  (global_label {q(n)} (shape {shape}) (at {x} {y + i * 2.54:.2f} 0) (fields_autoplaced)\n'
                   f'    (effects (font (size 1.27 1.27)) (justify left))\n'
                   f'    (uuid {u()})\n'
                   f'    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {x} {y + i * 2.54:.2f} 0)\n'
                   f'      (effects (font (size 1.27 1.27)) (justify left) hide)))\n')
    return "".join(out)


def write_subsheet(path, title, notes, labels):
    s = f'(kicad_sch (version 20230121) (generator eeschema)\n  (uuid {u()})\n  (paper "A4")\n'
    s += title_block(title, "Function block - symbols to be placed")
    s += "  (lib_symbols)\n"
    s += text_block(20, 25, notes)
    s += text_block(200, 25, ["Nets of this block (global labels):"])
    s += global_labels(200, 32, labels)
    s += ")\n"
    with open(path, "w") as f:
        f.write(s)


def write_root(path, project, title, sheets, notes):
    root_uuid = u()
    s = f'(kicad_sch (version 20230121) (generator eeschema)\n  (uuid {root_uuid})\n  (paper "A4")\n'
    s += title_block(title, "Root sheet")
    s += "  (lib_symbols)\n"
    s += text_block(20, 22, notes)
    x0, y0, w, h = 25.0, 95.0, 50.0, 22.0
    for i, (name, fname, _t, _n, _l) in enumerate(sheets):
        x = x0 + (i % 4) * (w + 12)
        y = y0 + (i // 4) * (h + 18)
        s += (f'  (sheet (at {x} {y}) (size {w} {h}) (fields_autoplaced)\n'
              f'    (stroke (width 0.1524) (type solid))\n'
              f'    (fill (color 0 0 0 0.0000))\n'
              f'    (uuid {u()})\n'
              f'    (property "Sheetname" {q(name)} (at {x} {y - 0.7:.2f} 0)\n'
              f'      (effects (font (size 1.27 1.27)) (justify left bottom)))\n'
              f'    (property "Sheetfile" {q("sheets/" + fname)} (at {x} {y + h + 0.6:.2f} 0)\n'
              f'      (effects (font (size 1.27 1.27)) (justify left top)))\n'
              f'    (instances (project {q(project)} (path "/{root_uuid}" (page "{i + 2}")))))\n')
    s += '  (sheet_instances (path "/" (page "1")))\n)\n'
    with open(path, "w") as f:
        f.write(s)


BASE_SHEETS = [
    ("MCU", "mcu.kicad_sch", "ESP32-C6-WROOM-1-N8",
     ["U1 ESP32-C6-WROOM-1-N8 (8 MB flash). Antenna end at the board edge (top),",
      "no copper on any layer under the antenna (rule area ANTENNA_KEEPOUT on the PCB).",
      "",
      "GPIO map (firmware CONFIG_APP_BOARD_REVA, docs/hardware.md 'Pin map'):",
      "  GPIO0  VBUS_SENSE (ADC1)       GPIO1  VARIANT_ID (ADC1)",
      "  GPIO2  T1S_MISO                GPIO3  T1S_RST_N",
      "  GPIO4  KEY0                    GPIO5  KEY1",
      "  GPIO6  T1S_SCLK                GPIO7  T1S_MOSI",
      "  GPIO8  LP_EN (strap: 10k pull-up, must be high at boot)",
      "  GPIO9  BOOT (strap, pull-up, Tag-Connect only)",
      "  GPIO10 T1S_CS_N                GPIO11 T1S_IRQ_N",
      "  GPIO12/13 USB_DN/DP (test pads)  GPIO15 OUT_FAULT_N",
      "  GPIO16/17 UART0_TX/RX (Tag-Connect TC2030, J5)",
      "  GPIO18 OUT0_PWM                GPIO19 OUT1_PWM",
      "  GPIO20 KEY2 (spare)            GPIO21 KEY3 (spare)",
      "  GPIO22 I2C_SDA                 GPIO23 I2C_SCL",
      "",
      "Also on this sheet: EN RC (10k/1uF), decoupling per module datasheet,",
      "R5/R6 VBUS divider 100k/10k + 100nF, R7/R8 variant strap,",
      "R13/R14 2.2k I2C pull-ups, R15 10k GPIO8 pull-up, J5 TC2030."],
     ["+3V3", "GND", "VBUS_SENSE", "VARIANT_ID", "T1S_SCLK", "T1S_MOSI", "T1S_MISO", "T1S_CS_N",
      "T1S_IRQ_N", "T1S_RST_N", "KEY0", "KEY1", "KEY2", "KEY3", "LP_EN", "I2C_SDA", "I2C_SCL",
      "OUT0_PWM", "OUT1_PWM", "OUT_FAULT_N", "UART0_TX", "UART0_RX"]),
    ("T1S_PHY", "t1s_phy.kicad_sch", "LAN8651 + bus interface network",
     ["U2 LAN8651B1 (VQFN-32 5x5), Y2 25 MHz crystal (check load C in datasheet).",
      "",
      "Bus interface network as Microchip EVB-LAN8651 (AN1848 Fig. 6-2):",
      "  L1 ACT1210D-131-2P CMC, C1/C2 0.1uF >= 100 V series caps (see 24 V PoDL)",
      "  R3/R4 10k bleed, RV1/RV2 AVRH10C221KT1R5YA8 varistors",
      "  D1 low-C ESD array, >= 33 V standoff",
      "  R1/R2 49.9R + C3/C4 0.22uF/100V split termination via JP1 (END nodes only)",
      "",
      "T1S_A / T1S_B loop-through to J1 (in/out), stub to the BIN < 10 cm total."],
     ["+3V3", "GND", "T1S_SCLK", "T1S_MOSI", "T1S_MISO", "T1S_CS_N", "T1S_IRQ_N", "T1S_RST_N",
      "T1S_A", "T1S_B"]),
    ("PODL_POWER", "podl_power.kicad_sch", "PoDL decoupling, eFuse, 3.3 V",
     ["L2 47 uH DMC / coupled inductor (Isat >= 300 mA, matched) from T1S_A/T1S_B to VBUS_RAW.",
      "D2 SMBJ33A, U3 TPS2660 eFuse: ILIM = 100 mA, dV/dt soft-start, reverse polarity.",
      "C5 100 uF / 50 V bulk BEHIND the eFuse (VBUS), carries an NVS write on brown-out.",
      "L3/C6 input filter, U4 LMR36006 3.3 V buck (60 V), L4 per datasheet.",
      "Net VBUS feeds the VBUS_SENSE divider on the MCU sheet."],
     ["T1S_A", "T1S_B", "VBUS", "+3V3", "GND"]),
    ("OUTPUTS", "outputs.kicad_sch", "Output option (DNP on keypad variant)",
     ["Variant OUT only - keep this block in one PCB area (zone OUTPUT_OPTION).",
      "F1 5 A fuse, D3 SMBJ33A, U5 12 V LDO (>= 36 V in) from VLOAD -> +12V_GATE.",
      "U6 UCC27524A dual low-side driver, Rg sized for ~100 ns edges.",
      "Q1/Q2 N-MOSFET 40 V, < 15 mOhm @ 10 V, >= 10 A; R11/R12 100k pull-downs on PWM.",
      "R9/R10 10 mOhm shunts, U7 dual comparator (trip ~3 A), open-drain -> OUT_FAULT_N.",
      "LOAD_GND joins GND at ONE star point near the shunts."],
     ["VLOAD", "LOAD_GND", "COM+", "OUT0", "OUT1", "OUT0_PWM", "OUT1_PWM", "OUT_FAULT_N",
      "+12V_GATE", "GND"]),
    ("CONNECTORS", "connectors.kicad_sch", "Terminals and board-to-board",
     ["J1 bus 4-pole push-in (0.2-0.8 mm solid): T1S_A in, T1S_B in, T1S_A out, T1S_B out",
      "J2 shield 2-pole (in/out joined, NOT connected to GND)",
      "J3 load 5-pole 1.5 mm2 >= 10 A (OUT variant): VLOAD+, LOAD_GND, OUT0, OUT1, COM+",
      "J4 board-to-board 2x6 1.27 mm (position fixed on the PCB):",
      "  1 +3V3  2 GND  3 KEY0  4 KEY1  5 KEY2  6 KEY3",
      "  7 I2C_SDA  8 I2C_SCL  9 LP_EN  10 GND  11 spare  12 spare",
      "J5 Tag-Connect TC2030 (on MCU sheet)."],
     ["T1S_A", "T1S_B", "SHIELD", "VLOAD", "LOAD_GND", "COM+", "OUT0", "OUT1", "+3V3", "GND",
      "KEY0", "KEY1", "KEY2", "KEY3", "I2C_SDA", "I2C_SCL", "LP_EN"]),
]

FRONT_SHEETS = [
    ("KEYS_LEDS", "keys_leds.kicad_sch", "Keys and LP5012 key LEDs",
     ["SW1/SW2 tact switches (>= 1 M cycles) under the two 27 x 55 mm caps (55x55 system).",
      "RC 1k / 100 nF per key, keys active low to GND (pull-up on the ESP32 side).",
      "U10 LP5012 (WQFN-20, I2C 0x14: ADDR0/1 = GND), EN = LP_EN.",
      "LED1/LED2 RGB: module n = OUT(3n) R, OUT(3n+1) G, OUT(3n+2) B. Modules 2/3 spare.",
      "Light pipes in the caps above LED1/LED2."],
     ["+3V3", "GND", "KEY0", "KEY1", "KEY2", "KEY3", "I2C_SDA", "I2C_SCL", "LP_EN"]),
    ("SENSOR_B2B", "sensor_b2b.kicad_sch", "SHT45 and board-to-board",
     ["U11 SHT45 (DFN-4 1.5x1.5, I2C 0x44) on the thermally isolated tongue",
      "(slot in Edge.Cuts, no pour: rule area SENSOR_ISLAND), 100 nF at the sensor.",
      "Keep LP5012 and LEDs away from the tongue; vent slots in the caps above.",
      "J6 2x6 1.27 mm socket, mates base J4 (same pinout, mirrored by stacking)."],
     ["+3V3", "GND", "I2C_SDA", "I2C_SCL", "KEY0", "KEY1", "KEY2", "KEY3", "LP_EN"]),
]


# --------------------------------------------------------------------------
# PCB
# --------------------------------------------------------------------------
def pcb(board_name, builder):
    import pcbnew
    board = pcbnew.NewBoard(os.path.join(HERE, board_name, board_name + ".kicad_pcb"))
    builder(pcbnew, board)
    pcbnew.SaveBoard(os.path.join(HERE, board_name, board_name + ".kicad_pcb"), board)
    return board


def mm(pcbnew, x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(CX + x), pcbnew.FromMM(CY + y))


def seg(pcbnew, board, layer, a, b, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(mm(pcbnew, *a))
    s.SetEnd(mm(pcbnew, *b))
    s.SetLayer(layer)
    s.SetWidth(pcbnew.FromMM(width))
    board.Add(s)


def poly_outline(pcbnew, board, layer, pts, width=0.1):
    for i in range(len(pts)):
        seg(pcbnew, board, layer, pts[i], pts[(i + 1) % len(pts)], width)


def circle(pcbnew, board, layer, centre, r, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_CIRCLE)
    s.SetCenter(mm(pcbnew, *centre))
    s.SetEnd(mm(pcbnew, centre[0] + r, centre[1]))
    s.SetLayer(layer)
    s.SetWidth(pcbnew.FromMM(width))
    board.Add(s)


def text(pcbnew, board, layer, pos, txt, size=0.8):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(txt)
    t.SetPosition(mm(pcbnew, *pos))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
    t.SetTextThickness(pcbnew.FromMM(size * 0.15))
    board.Add(t)


def rule_area(pcbnew, board, name, pts, layers, no_copper=True):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetZoneName(name)
    ls = pcbnew.LSET()
    for l in layers:
        ls.AddLayer(l)
    z.SetLayerSet(ls)
    z.SetDoNotAllowTracks(no_copper)
    z.SetDoNotAllowVias(no_copper)
    z.SetDoNotAllowCopperPour(True)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    o = z.Outline()
    o.NewOutline()
    for p in pts:
        o.Append(mm(pcbnew, *p))
    board.Add(z)


def footprint(pcbnew, board, lib, name, ref, value, pos, rot=0.0, back=False, lock=True):
    libdir = os.path.join(FP_ROOT, lib + ".pretty")
    fp = pcbnew.FootprintLoad(libdir, name)
    if fp is None:
        raise RuntimeError(f"footprint {lib}:{name} not found in {FP_ROOT}")
    fp.SetReference(ref)
    fp.SetValue(value)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    board.Add(fp)
    fp.SetPosition(mm(pcbnew, *pos))
    if back:
        fp.Flip(fp.GetPosition(), False)
    if rot:
        fp.SetOrientationDegrees(rot)
    fp.SetLocked(lock)
    return fp


FP_ROOT = os.environ.get("KICAD7_FOOTPRINT_DIR", "/usr/share/kicad/footprints")


def build_base(pcbnew, board):
    board.SetCopperLayerCount(4)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(pcbnew.FromMM(1.6))
    E, F_FAB, CMT, DWG = pcbnew.Edge_Cuts, pcbnew.F_Fab, pcbnew.Cmts_User, pcbnew.Dwgs_User
    R = 25.0
    circle(pcbnew, board, E, (0, 0), R, 0.1)

    # Box reference geometry (Dwgs.User): box inner wall, support-ring screw bosses
    circle(pcbnew, board, DWG, (0, 0), 30.5, 0.15)                 # box inner wall ~ D61
    for sx in (-30.0, 30.0):
        circle(pcbnew, board, DWG, (sx, 0), 3.5, 0.15)             # screw boss
    text(pcbnew, board, DWG, (0, 29.5), "BOX D61 / SCREW BOSSES @ +-30 mm", 0.8)

    # Mounting: M2 standoffs to the front board
    for i, p in enumerate(STANDOFF_POS):
        footprint(pcbnew, board, "MountingHole", "MountingHole_2.2mm_M2_Pad_Via", f"H{i + 1}", "M2 standoff", p)

    # ESP32-C6 module: antenna towards the top edge (smaller y)
    module = None
    for lib, name in (("RF_Module", "ESP32-C6-WROOM-1"), ("RF_Module", "ESP32-S3-WROOM-1"),
                      ("RF_Module", "ESP32-WROOM-32")):
        if os.path.exists(os.path.join(FP_ROOT, lib + ".pretty", name + ".kicad_mod")):
            module = (lib, name)
            break
    u1 = footprint(pcbnew, board, module[0], module[1], "U1", "ESP32-C6-WROOM-1-N8", (0, -9.8))
    if module[1] != "ESP32-C6-WROOM-1":
        text(pcbnew, board, CMT, (0, 32), f"U1 = {module[1]} footprint as placeholder (same 18 x 25.5 outline): replace with ESP32-C6-WROOM-1", 0.6)

    # Antenna keep-out: module antenna end, all copper layers, to the board edge
    ant = [(-9.5, -25.5), (9.5, -25.5), (9.5, -16.0), (-9.5, -16.0)]
    rule_area(pcbnew, board, "ANTENNA_KEEPOUT", ant,
              [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu])

    # Board-to-board header (top side, towards the front board)
    footprint(pcbnew, board, "Connector_PinHeader_1.27mm", "PinHeader_2x06_P1.27mm_Vertical_SMD",
              "J4", "B2B 2x6 1.27", B2B_POS, rot=90)

    # Tag-Connect
    if os.path.exists(os.path.join(FP_ROOT, "Connector.pretty", "Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical.kicad_mod")):
        footprint(pcbnew, board, "Connector", "Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical", "J5", "TC2030", (-9.0, 16.5),
                  back=True)

    # Placement zones (Cmts.User) - guidance for the layout
    zones = {
        "T1S PHY + BIN (top)": [(-8, 13.5), (8, 13.5), (6, 21), (-6, 21)],
        "PoDL / eFuse / buck (top)": [(-22, -4), (-9.5, -4), (-9.5, 10), (-20, 10)],
        "OUTPUT_OPTION (top, DNP on keypad)": [(9.5, -4), (22, -4), (20, 10), (9.5, 10)],
    }
    for name, pts in zones.items():
        poly_outline(pcbnew, board, CMT, pts, 0.12)
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        text(pcbnew, board, CMT, (cx, cy), name, 0.6)
    # Bottom side: terminals along the lower arc
    term = [(-20, 13), (20, 13), (14, 21), (-14, 21)]
    poly_outline(pcbnew, board, pcbnew.B_Fab, term, 0.12)
    text(pcbnew, board, pcbnew.B_Fab, (0, 19.5), "BOTTOM: J1 bus / J2 shield / J3 load terminals", 0.6)
    # Bottom side (box floor): power, bulk cap, output block, passives. Max 8 mm.
    bot = [(-22, -9), (22, -9), (21.5, 11.5), (-21.5, 11.5)]
    poly_outline(pcbnew, board, pcbnew.B_Fab, bot, 0.12)
    text(pcbnew, board, pcbnew.B_Fab, (0, -7.5), "BOTTOM: power / C5 bulk / PoDL choke / outputs, h <= 8 mm", 0.6)
    text(pcbnew, board, CMT, (0, -1.5), "TOP h <= 5 mm (B2B stack 8 mm)", 0.6)

    text(pcbnew, board, pcbnew.F_SilkS, (0, 22.5), "WIRED SWITCHBOARD BASE rev A", 0.8)
    text(pcbnew, board, F_FAB, (0, 0), "+ box axis", 0.6)


def build_front(pcbnew, board):
    board.SetCopperLayerCount(2)
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(pcbnew.FromMM(1.6))
    E, CMT, DWG = pcbnew.Edge_Cuts, pcbnew.Cmts_User, pcbnew.Dwgs_User
    a, c = 23.0, 6.0                     # half size, chamfer
    outline = [(-a + c, -a), (a - c, -a), (a, -a + c), (a, a - c), (a - c, a), (-a + c, a), (-a, a - c),
               (-a, -a + c)]
    # SHT45 tongue at the bottom edge
    poly_outline(pcbnew, board, E, outline, 0.1)
    # U-slot 1 mm wide; the island stays attached by a 2 mm bridge along the bottom edge
    slot = [(-4.0, 21.0), (-4.0, 14.0), (4.0, 14.0), (4.0, 21.0), (3.0, 21.0), (3.0, 15.0), (-3.0, 15.0),
            (-3.0, 21.0)]
    poly_outline(pcbnew, board, E, slot, 0.1)

    # Reference: 55 x 55 cap area and the split between the two caps
    poly_outline(pcbnew, board, DWG, [(-27.5, -27.5), (27.5, -27.5), (27.5, 27.5), (-27.5, 27.5)], 0.15)
    seg(pcbnew, board, DWG, (0, -27.5), (0, 27.5), 0.15)
    circle(pcbnew, board, DWG, (0, 0), 30.5, 0.15)
    text(pcbnew, board, DWG, (-26, -29), "55x55 CAPS (2 x 27x55) / BOX D61", 0.8)

    for i, p in enumerate(FRONT_HOLES):
        footprint(pcbnew, board, "MountingHole", "MountingHole_2.2mm_M2", f"H{i + 1}", "M2", p)

    for ref, x in (("SW1", -11.5), ("SW2", 11.5)):
        footprint(pcbnew, board, "Button_Switch_SMD", "SW_SPST_PTS645", ref, "Tact >= 1M cycles",
                  (x, 0.0))
    for ref, x in (("LED1", -11.5), ("LED2", 11.5)):
        footprint(pcbnew, board, "LED_SMD", "LED_RGB_PLCC-6", ref, "RGB", (x, -7.0))
    footprint(pcbnew, board, "Sensor_Humidity", "Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_NoCentralPad", "U11", "SHT45",
              (0.0, 18.0))
    rule_area(pcbnew, board, "SENSOR_ISLAND", [(-3.0, 15.0), (3.0, 15.0), (3.0, 21.0), (-3.0, 21.0)],
              [pcbnew.F_Cu, pcbnew.B_Cu], no_copper=False)
    # B2B socket on the bottom side, coaxial with base J4
    footprint(pcbnew, board, "Connector_PinSocket_1.27mm", "PinSocket_2x06_P1.27mm_Vertical_SMD", "J6",
              "B2B 2x6 1.27", B2B_POS, rot=90, back=True)

    text(pcbnew, board, pcbnew.B_Fab, (0, -16), "BOTTOM: passives h <= 1.5 mm (except J6)", 0.6)
    poly_outline(pcbnew, board, CMT, [(-5, -12), (5, -12), (5, -5), (-5, -5)], 0.12)
    text(pcbnew, board, CMT, (-4.5, -13), "U10 LP5012 + passives", 0.6)
    text(pcbnew, board, pcbnew.F_SilkS, (0, -20.5), "WIRED SWITCHBOARD FRONT rev A", 0.8)


# --------------------------------------------------------------------------
def main():
    for name, title, sheets, notes in (
        ("wired_switchboard_base", "Wired Switchboard - base board rev A", BASE_SHEETS,
         ["Base board, D50 mm, 4 layers (F.Cu / In1 GND / In2 power / B.Cu), 1.6 mm.",
          "ESP32-C6-WROOM-1-N8 + LAN8651 10BASE-T1S, 24 V PoDL (100 mA/node),",
          "optional 2 x 2 A PWM outputs (variant OUT, DNP on keypad variant).",
          "Sources: docs/hardware.md, hardware/bom-revA.csv, firmware Kconfig (CONFIG_APP_BOARD_REVA).",
          "Variant column in the BOM: ALL / OUT / END."]),
        ("wired_switchboard_front", "Wired Switchboard - front board rev A", FRONT_SHEETS,
         ["Front board 46 x 46 mm (6 mm chamfers), 2 layers, 1.6 mm.",
          "2 keys, LP5012 RGB key LEDs, SHT45 on a thermally isolated tongue.",
          "Mates the base board via J6 (2x6 1.27 mm), M2 standoffs at H3/H4."]),
    ):
        d = os.path.join(HERE, name)
        os.makedirs(os.path.join(d, "sheets"), exist_ok=True)
        write_project(os.path.join(d, name + ".kicad_pro"), name)
        with open(os.path.join(d, name + ".kicad_dru"), "w") as f:
            f.write(DRU)
        for (sname, fname, stitle, snotes, labels) in sheets:
            write_subsheet(os.path.join(d, "sheets", fname), f"{title} - {stitle}", snotes, labels)
        write_root(os.path.join(d, name + ".kicad_sch"), name, title, sheets, notes)

    pcb("wired_switchboard_base", build_base)
    pcb("wired_switchboard_front", build_front)
    print("generated")


if __name__ == "__main__":
    main()
