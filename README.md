# ShardGrid 🔐

**Secure Optical Secret Sharing System**

A hardware security project that transmits encrypted messages between microcontrollers using **light only** (LED → LDR optical links). Messages are protected with **AES-256-GCM encryption** and split using **Shamir's Secret Sharing** so that no single node holds the complete secret. A tamper detection system instantly destroys all cryptographic keys if the secure enclosure is breached.

---

## Architecture

```
   ┌──────────┐          ┌──────────────────┐         ┌──────────────┐
   │    PC    │   USB    │  Arduino Uno     │  LED→   │ ESP32        │
   │ Browser  │─────────▶│  (Sender)        │──LDR──▶│ Central Node │
   │Dashboard │          │  Pin 8 = LED     │  ch1   │              │
   └──────────┘          └──────────────────┘        └──────┬───────┘
                                                            │
                                               SSS Split + Encrypt
                                                     ┌──────┴──────┐
                                               LED→LDR│            │LED→LDR
                                                ch2   ▼            ▼  ch3
                                               ┌──────────┐ ┌──────────┐
                                               │ ESP32    │ │ ESP32    │
                                               │ Node A   │ │ Node B   │
                                               │ (Share2) │ │ (Share3) │
                                               └────┬─────┘ └────┬─────┘
                                                    │LED→LDR      │LED→LDR
                                                    │  ch4        │  ch5
                                                    ▼             ▼
                                               ┌──────────────────────┐
                                               │   Central Node      │
                                               │   Reconstructs msg  │
                                               │   Displays on OLED  │
                                               └──────────────────────┘
```

**Data Flow (Round-Trip):**
1. User enters a secret message in the web dashboard
2. Dashboard sends the **plaintext** message via USB Serial to Arduino Uno
3. Uno transmits the plaintext **optically** (LED → LDR) to the Central ESP32
4. Central ESP32 receives the plaintext
5. Central ESP32 **splits** the message into 3 shares using Shamir's Secret Sharing (k=2, n=3)
6. Central ESP32 keeps Share 1, **encrypts** Shares 2 & 3 with AES-256-GCM
7. Central ESP32 transmits encrypted Share 2 to Node A and Share 3 to Node B **optically**
8. Node A & B decrypt, store their shares, then **send them back** to Central **optically**
9. Central receives both shares back, **reconstructs** the original message via Lagrange interpolation
10. Central **displays the reconstructed message on its OLED**

> **Note:** All encryption happens on the Central ESP32 — the Uno is a dumb optical relay.

---

## Security Features

| Feature | Implementation |
|---------|----------------|
| **Encryption** | AES-256-GCM (authenticated encryption via ESP32's built-in mbedTLS) |
| **Secret Sharing** | Shamir's (2,3) threshold scheme in GF(251) |
| **Tamper Detection** | Hardware interrupt on GPIO 27 (microswitch/reed switch) |
| **Key Zeroization** | 3-pass RAM overwrite + EEPROM wipe on tamper |
| **Optical Isolation** | Data travels ONLY via light — no WiFi, no Bluetooth, no wires |
| **CRC-16 Integrity** | Every optical frame is CRC-protected |
| **Manchester Encoding** | Self-clocking, no DC bias, error-detecting |

---

## Project Structure

```
ShardGrid/
├── sender-node/              # Arduino Uno firmware
│   └── sender-node.ino
├── central-node/             # ESP32 #1 — receives, splits, distributes
│   └── central-node.ino
├── node-a/                   # ESP32 #2 — stores Share 2
│   └── node-a.ino
├── node-b/                   # ESP32 #3 — stores Share 3
│   └── node-b.ino
├── shared-lib/               # Common libraries
│   ├── config.h              # Pin definitions, timing, constants
│   ├── optical_protocol.h    # Framing, CRC, Manchester TX/RX
│   ├── shamir_sss.h          # Shamir's Secret Sharing
│   └── security.h            # AES-256-GCM, key management, tamper
├── web-dashboard/            # Browser control panel
│   ├── index.html
│   ├── css/style.css
│   └── js/
│       ├── app.js            # Main controller
│       ├── serial.js         # Web Serial API bridge
│       ├── shamir.js         # SSS (browser preview)
│       └── crypto.js         # AES-256-GCM (Web Crypto API)
├── docs/
│   ├── wiring-diagram.md     # Complete wiring guide
│   └── protocol-spec.md      # Optical protocol specification
└── README.md
```

---

## Hardware Required

| Qty | Component                    |
|-----|------------------------------|
| 1   | Arduino Uno R3               |
| 3   | ESP32 DevKit v1              |
| 5   | LDR modules (with digital out) |
| 5   | Bright LEDs (white or IR)    |
| 5   | 220Ω resistors               |
| 3   | SSD1306 OLED displays (128×64, I²C) |
| 3   | Micro switches (normally closed) |
| 1   | Opaque enclosure / box       |
| —   | Jumper wires, breadboard, USB cables |

See [`docs/wiring-diagram.md`](docs/wiring-diagram.md) for detailed pin connections.

---

## Setup & Flashing

### Prerequisites
- [Arduino IDE 2.x](https://www.arduino.cc/en/software) with:
  - **ESP32 board package** (by Espressif) installed via Board Manager
  - **Adafruit SSD1306** library installed via Library Manager
  - **Adafruit GFX** library installed via Library Manager

### Step 1: Flash the Arduino Uno
1. Open `sender-node/sender-node.ino` in Arduino IDE
2. Select **Board: Arduino Uno**
3. Select the correct COM port
4. Click **Upload**

### Step 2: Flash ESP32 Central Node
1. Open `central-node/central-node.ino` in Arduino IDE
2. Select **Board: ESP32 Dev Module**
3. Select the correct COM port
4. Click **Upload**

### Step 3: Flash ESP32 Node A
1. Open `node-a/node-a.ino`
2. Same board/upload process

### Step 4: Flash ESP32 Node B
1. Open `node-b/node-b.ino`
2. Same board/upload process

### Step 5: Wire everything
Follow [`docs/wiring-diagram.md`](docs/wiring-diagram.md)

### Step 6: Open the Web Dashboard
1. Open `web-dashboard/index.html` in **Google Chrome** (or Edge)
2. Click **Connect Arduino Uno** → select the Uno's COM port
3. Click **Generate New Key** → then **Provision to Nodes**
4. Type a message → click **🔒 Encrypt & Transmit**

> **Note:** The Web Serial API only works in Chrome and Edge.

---

## How It Works

### Shamir's Secret Sharing (k=2, n=3)

The message is split byte-by-byte using polynomial evaluation over GF(251):

1. For each byte `s` of the message, generate a random polynomial `f(x) = s + a₁x` (degree k-1 = 1)
2. Evaluate: Share 1 = `f(1)`, Share 2 = `f(2)`, Share 3 = `f(3)`
3. Any 2 shares can reconstruct `s` via Lagrange interpolation at x=0
4. 1 share alone reveals **nothing** about `s` (information-theoretic security)

### Tamper Response

When the tamper sensor triggers (box opened, wire cut):
1. ISR sets volatile flag immediately
2. Main loop detects flag and calls `zeroize()`
3. AES key overwritten 3× with random data, then zeroed
4. EEPROM completely erased (3 passes + final zero)
5. GCM context freed
6. Central node broadcasts `TAMPER` frame to Node A & B
7. All nodes enter permanent `LOCKED` state
8. OLED displays "TAMPER!" warning
9. System requires full re-provisioning to recover

---

## Web Dashboard Commands (USB Serial)

| Command | Description |
|---------|-------------|
| `#01:HEX_DATA` | Send encrypted data frame to Central Node |
| `#04:` | Send heartbeat |
| `KEY:64_hex_chars` | Provision AES-256 key |
| `STATUS` | Request node status |
| `ZEROIZE` | Emergency key destruction |

---

## License

This project is for educational and research purposes.
