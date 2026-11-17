# ShardGrid — Wiring Diagram

## Overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        SECURE ENCLOSURE                                  │
│                                                                          │
│  ┌─────────────┐   LED→LDR    ┌──────────────────┐                      │
│  │ Arduino Uno │──────────────▶│ ESP32 Central    │                      │
│  │  (Sender)   │   optical    │  (Processor)     │                      │
│  │             │   channel 1  │                  │                      │
│  └──────┬──────┘              │  LED A    LED B  │                      │
│         │USB                  └──┬──────────┬────┘                      │
│         │                       │optical    │optical                     │
│         │                       │ch 2       │ch 3                        │
│         │                       ▼           ▼                            │
│         │                 ┌──────────┐ ┌──────────┐                      │
│         │                 │ ESP32    │ │ ESP32    │                      │
│         │                 │ Node A   │ │ Node B   │                      │
│         │                 │ (Store)  │ │ (Store)  │                      │
│         │                 └────┬─────┘ └────┬─────┘                      │
│         │                      │optical      │optical                    │
│         │                      │ch 4         │ch 5                       │
│         │                      ▼             ▼                           │
│         │                 ┌──────────────────────┐                       │
│         │                 │   Central ESP32      │                       │
│         │                 │   LDR_A    LDR_B     │                       │
│         │                 │  (receives shares    │                       │
│         │                 │   back, reconstructs │                       │
│         │                 │   & shows on OLED)   │                       │
│         │                 └──────────────────────┘                       │
│         │                                                                │
│         │         [Tamper sensors on enclosure lid/walls]                 │
│         │                                                                │
└─────────┼────────────────────────────────────────────────────────────────┘
          │
          │ USB Cable
          ▼
    ┌───────────┐
    │    PC     │
    │  Browser  │
    │ Dashboard │
    └───────────┘
```

**Round-trip data flow:**
1. PC → USB → Uno → LED→LDR(pin34) → Central (receives plaintext)
2. Central splits via Shamir SSS, encrypts shares
3. Central LED_A(pin25) → LDR(pin34) Node A (receives + stores share 2)
4. Central LED_B(pin26) → LDR(pin34) Node B (receives + stores share 3)
5. Node A LED(pin25) → LDR_A(pin35) Central (share 2 returned)
6. Node B LED(pin25) → LDR_B(pin32) Central (share 3 returned)
7. Central reconstructs message via Lagrange interpolation → **displays on OLED**

---

## Arduino Uno (Sender Node)

| Uno Pin | Connection             | Notes                    |
|---------|------------------------|--------------------------|
| **D8**  | LED anode (→ 220Ω → GND) | Optical TX to Central ESP32 |
| **USB** | PC                     | Receives commands from web dashboard |
| **GND** | Common ground          | Share with all ESP32s     |
| **5V**  | Power from USB         | —                        |

### LED Circuit (Uno → Central)
```
Uno D8 ──── [220Ω] ──── LED(+) ──── LED(-) ──── GND
                              │
                        [Optical gap ~1-3cm]
                              │
                         LDR on Central ESP32 (GPIO 34)
```

---

## ESP32 #1 — Central Node

| ESP32 Pin | Connection                  | Notes                           |
|-----------|-----------------------------|---------------------------------|
| **GPIO 34** | LDR module (DO pin)       | Optical RX from Arduino Uno     |
| **GPIO 35** | LDR module (DO pin)       | Optical RX from Node A (return) |
| **GPIO 32** | LDR module (DO pin)       | Optical RX from Node B (return) |
| **GPIO 25** | LED anode (→ 220Ω → GND)  | Optical TX to Node A            |
| **GPIO 26** | LED anode (→ 220Ω → GND)  | Optical TX to Node B            |
| **GPIO 27** | Tamper switch (NC to GND)  | Tamper detection (INPUT_PULLUP) |
| **GPIO 21** | OLED SDA                   | I²C data                       |
| **GPIO 22** | OLED SCL                   | I²C clock                      |
| **GPIO 2**  | Built-in LED               | Heartbeat indicator            |
| **3.3V**    | LDR VCC ×3, OLED VCC      | Power                          |
| **GND**     | Common ground              | Share with all devices          |
| **USB**     | PC (optional)              | Serial monitor / provisioning   |

### LDR Module Wiring (Central ← Uno)
```
LDR Module (Uno channel):
  VCC ──── ESP32 3.3V
  GND ──── ESP32 GND
  DO  ──── ESP32 GPIO 34

  [Position LDR sensor facing the Uno's LED, ~1-3cm apart]
  [Use a light-tight tube/channel between LED and LDR]
```

### LDR Module Wiring (Central ← Node A return)
```
LDR Module (Node A return channel):
  VCC ──── ESP32 3.3V
  GND ──── ESP32 GND
  DO  ──── ESP32 GPIO 35

  [Position LDR sensor facing Node A's TX LED, ~1-3cm apart]
```

### LDR Module Wiring (Central ← Node B return)
```
LDR Module (Node B return channel):
  VCC ──── ESP32 3.3V
  GND ──── ESP32 GND
  DO  ──── ESP32 GPIO 32

  [Position LDR sensor facing Node B's TX LED, ~1-3cm apart]
```

### LED Circuits (Central → Node A / Node B)
```
ESP32 GPIO 25 ──── [220Ω] ──── LED_A(+) ──── LED_A(-) ──── GND
                                     │
                               [Optical gap]
                                     │
                                LDR on Node A (GPIO 34)


ESP32 GPIO 26 ──── [220Ω] ──── LED_B(+) ──── LED_B(-) ──── GND
                                     │
                               [Optical gap]
                                     │
                                LDR on Node B (GPIO 34)
```

---

## ESP32 #2 — Node A

| ESP32 Pin | Connection                  | Notes                           |
|-----------|-----------------------------|---------------------------------|
| **GPIO 34** | LDR module (DO pin)       | Optical RX from Central Node    |
| **GPIO 25** | LED anode (→ 220Ω → GND)  | Optical TX back to Central Node |
| **GPIO 27** | Tamper switch (NC to GND)  | Tamper detection (INPUT_PULLUP) |
| **GPIO 21** | OLED SDA                   | I²C data                       |
| **GPIO 22** | OLED SCL                   | I²C clock                      |
| **GPIO 2**  | Built-in LED               | Heartbeat indicator            |
| **3.3V**    | LDR VCC, OLED VCC         | —                              |
| **GND**     | Common ground              | —                              |
| **USB**     | PC (optional)              | Serial monitor                 |

### LED Circuit (Node A → Central)
```
ESP32 GPIO 25 ──── [220Ω] ──── LED(+) ──── LED(-) ──── GND
                                     │
                               [Optical gap]
                                     │
                                LDR on Central (GPIO 35)
```

---

## ESP32 #3 — Node B

| ESP32 Pin | Connection                  | Notes                           |
|-----------|-----------------------------|---------------------------------|
| **GPIO 34** | LDR module (DO pin)       | Optical RX from Central Node    |
| **GPIO 25** | LED anode (→ 220Ω → GND)  | Optical TX back to Central Node |
| **GPIO 27** | Tamper switch (NC to GND)  | Tamper detection (INPUT_PULLUP) |
| **GPIO 21** | OLED SDA                   | I²C data                       |
| **GPIO 22** | OLED SCL                   | I²C clock                      |
| **GPIO 2**  | Built-in LED               | Heartbeat indicator            |
| **3.3V**    | LDR VCC, OLED VCC         | —                              |
| **GND**     | Common ground              | —                              |
| **USB**     | PC (optional)              | Serial monitor                 |

### LED Circuit (Node B → Central)
```
ESP32 GPIO 25 ──── [220Ω] ──── LED(+) ──── LED(-) ──── GND
                                     │
                               [Optical gap]
                                     │
                                LDR on Central (GPIO 32)
```

---

## Tamper Switch Wiring (All ESP32 Nodes)

```
Tamper Switch (Normally Closed — NC):
  One terminal ──── ESP32 GPIO 27
  Other terminal ── GND

  [GPIO 27 uses INPUT_PULLUP internally]
  [When switch opens (box lid lifted) → pin goes HIGH → FALLING interrupt triggers]
```

### Recommended Tamper Sensors:
- **Microswitch** on enclosure lid (cheapest, most reliable)
- **Magnetic reed switch** with magnet on lid
- **LDR light sensor** detecting ambient light (box opened = light enters)
- **Tilt switch** for physical movement detection

### Anti-Tamper Best Practices:
1. Use **normally-closed** switches so cutting the wire = tamper
2. Add a **100nF capacitor** between GPIO 27 and GND for hardware debounce
3. Run tamper wires **inside the enclosure walls** so they can't be bypassed
4. Consider multiple sensors (microswitch + light sensor) for redundancy

---

## Optical Channel Summary

| Channel | From | Pin | → | To | Pin | Direction |
|---------|------|-----|---|-----|-----|-----------|
| CH1 | Uno | D8 (LED) | → | Central | GPIO 34 (LDR) | Uno → Central |
| CH2 | Central | GPIO 25 (LED) | → | Node A | GPIO 34 (LDR) | Central → A |
| CH3 | Central | GPIO 26 (LED) | → | Node B | GPIO 34 (LDR) | Central → B |
| CH4 | Node A | GPIO 25 (LED) | → | Central | GPIO 35 (LDR) | A → Central |
| CH5 | Node B | GPIO 25 (LED) | → | Central | GPIO 32 (LDR) | B → Central |

**Total: 5 optical channels, 5 LEDs, 5 LDRs**

---

## Bill of Materials

| Qty | Component              | Purpose                    |
|-----|------------------------|----------------------------|
| 1   | Arduino Uno R3         | Sender node                |
| 3   | ESP32 DevKit v1        | Central + Node A + Node B  |
| 5   | LDR modules (digital)  | Optical receivers (with DO pin) |
| 5   | 5mm LEDs (bright white/IR) | Optical transmitters    |
| 5   | 220Ω resistors         | LED current limiting       |
| 3   | SSD1306 OLED (128×64)  | Status displays            |
| 3   | Micro switches (NC)    | Tamper detection           |
| 3   | 100nF ceramic capacitors | Debounce                 |
| 1   | Enclosure/box          | Secure housing             |
| —   | Jumper wires, breadboard | Prototyping               |
| —   | Light-tight tubes/channels | Optical isolation       |
| 4   | USB cables             | Power + programming        |

---

## Optical Channel Construction

For reliable optical communication, each LED → LDR pair should be:

1. **Aligned** — LED pointing directly at LDR sensor
2. **Enclosed** — Use a small opaque tube (black heat-shrink tubing, cut drinking straw painted black, or 3D-printed channel)
3. **Fixed distance** — 1-3 cm apart (adjust LDR sensitivity pot)
4. **Isolated** — No ambient light should reach the LDR from outside

```
    ┌──────────────────────────────┐
    │  Black opaque tube/channel   │
    │                              │
    │  [LED] ═══════════▶ [LDR]   │
    │                              │
    └──────────────────────────────┘
         ~1-3cm gap
```

### LDR Module Sensitivity:
Most LDR modules have a **potentiometer** for threshold adjustment:
- Turn it so that the DO pin goes **LOW** when the LED is ON
- And **HIGH** when the LED is OFF
- Test with Serial monitor first before enclosing
