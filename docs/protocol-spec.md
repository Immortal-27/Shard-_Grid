# ShardGrid — Optical Communication Protocol Specification

## Version 1.0

---

## 1. Physical Layer

| Parameter          | Value                    |
|--------------------|--------------------------|
| Medium             | Visible/IR light (LED → LDR) |
| Modulation         | On-Off Keying (OOK) with Manchester encoding |
| Bit time           | 250 ms (4 bps)           |
| Channels           | 3 independent (Uno→Central, Central→A, Central→B) |
| Direction           | Simplex (one-way per channel) |
| Range              | 1-3 cm (enclosed optical channel) |

---

## 2. Manchester Encoding

Each bit is encoded as a transition within the bit period:

```
Bit 1:  HIGH for 125ms → LOW for 125ms   (falling edge)
Bit 0:  LOW  for 125ms → HIGH for 125ms  (rising edge)
```

**Advantages:**
- Self-clocking — receiver can sync from transitions
- No DC bias — equal time HIGH and LOW
- Error detection — missing transition = encoding error

---

## 3. Frame Format

All data is transmitted in frames with the following structure:

```
┌──────────┬───────┬─────┬─────┬──────┬─────┬─────────────┬────────┬────────┬─────┐
│ PREAMBLE │ START │ SRC │ DST │ TYPE │ LEN │   PAYLOAD   │ CRC_HI │ CRC_LO │ END │
│  8 bits  │ 1B    │ 1B  │ 1B  │ 1B   │ 1B  │ 0-128 bytes │  1B    │  1B    │ 1B  │
└──────────┴───────┴─────┴─────┴──────┴─────┴─────────────┴────────┴────────┴─────┘
```

### 3.1 Preamble (8 bits)
- Alternating `0,1,0,1,0,1,0,1` pattern
- Purpose: allows receiver to synchronize clock timing
- Manchester encoded like all other bits

### 3.2 Start Delimiter (1 byte)
- Value: `0x7E`
- Marks the beginning of the frame header

### 3.3 Header Fields

| Field | Size   | Description                     |
|-------|--------|---------------------------------|
| SRC   | 1 byte | Source node ID                  |
| DST   | 1 byte | Destination node ID             |
| TYPE  | 1 byte | Message type (see §4)           |
| LEN   | 1 byte | Payload length (0-128)          |

### 3.4 Payload
- Variable length: 0 to 128 bytes
- Content depends on message type
- For DATA messages: encrypted share data

### 3.5 CRC-16 (2 bytes)
- Algorithm: CRC-16/CCITT (polynomial 0x1021, init 0xFFFF)
- Computed over: SRC + DST + TYPE + LEN + PAYLOAD
- Transmitted high byte first, then low byte
- Receiver recomputes and compares — mismatch = frame rejected

### 3.6 End Delimiter (1 byte)
- Value: `0x7F`
- Marks the end of the frame

### 3.7 Inter-Frame Gap
- 750 ms (3 × BIT_TIME) of silence (LED OFF) after END
- Allows receiver to reset and prepare for next frame

---

## 4. Message Types

| Code   | Name        | Direction            | Payload                       |
|--------|-------------|----------------------|-------------------------------|
| `0x01` | DATA        | Uno→Central, Central→A/B | Encrypted message or share |
| `0x02` | ACK         | Any→Any              | 1 byte: acked frame type      |
| `0x03` | NACK        | Any→Any              | 1 byte: nacked frame type     |
| `0x04` | HEARTBEAT   | Any→Any              | Empty (len=0)                 |
| `0x05` | KEY_EXCH    | Dashboard→Nodes      | Key material                  |
| `0x06` | TAMPER      | Central→A/B          | 1 byte: `0xFF`                |
| `0x07` | STATUS      | Any→Any              | Status data                   |
| `0x08` | ZEROIZE     | Dashboard→All        | Empty — destroy all keys      |

---

## 5. Node IDs

| ID     | Node                |
|--------|---------------------|
| `0x00` | Arduino Uno (Sender)|
| `0x01` | ESP32 Central Node  |
| `0x02` | ESP32 Node A        |
| `0x03` | ESP32 Node B        |

---

## 6. Timing Budget

For a frame with N payload bytes, the total transmission time is:

```
T_total = T_preamble + T_start + T_header + T_payload + T_crc + T_end + T_gap

Where:
  T_preamble = 8 × 250ms = 2,000 ms
  T_start    = 8 × 250ms = 2,000 ms  (1 byte Manchester)
  T_header   = 4 × 8 × 250ms = 8,000 ms  (4 bytes)
  T_payload  = N × 8 × 250ms = N × 2,000 ms
  T_crc      = 2 × 8 × 250ms = 4,000 ms  (2 bytes)
  T_end      = 8 × 250ms = 2,000 ms  (1 byte)
  T_gap      = 750 ms

T_total = 18,750 ms + (N × 2,000 ms)
```

| Payload Size | Total TX Time |
|--------------|---------------|
| 0 bytes      | ~19 seconds   |
| 10 bytes     | ~39 seconds   |
| 32 bytes     | ~83 seconds   |
| 64 bytes     | ~147 seconds  |
| 128 bytes    | ~275 seconds  |

> **Note:** Optical communication is intentionally slow for reliability.
> The 250ms bit time accommodates LDR response time and ambient noise rejection.

---

## 7. Security Payload Format

### 7.1 Encrypted Message (Uno → Central)
```
[IV: 12 bytes][Ciphertext: N bytes][Auth Tag: 16 bytes]
```
- Algorithm: AES-256-GCM
- IV: Random 12-byte nonce (never reused)
- Auth tag: 16-byte authentication tag (integrity + authenticity)

### 7.2 Encrypted Share (Central → Node A/B)
```
[IV: 12 bytes][Ciphertext: 1+M bytes][Auth Tag: 16 bytes]

Where Ciphertext decrypts to:
  [Share X: 1 byte][Share Data: M bytes]
```
- Share X: The share index (1, 2, or 3)
- Share Data: The Shamir share values for each byte of the original message

---

## 8. Error Handling

| Error                  | Action                            |
|------------------------|-----------------------------------|
| No start delimiter     | Discard, keep listening           |
| Manchester decode error| Abort frame, discard              |
| CRC mismatch           | Discard frame, log error          |
| Wrong destination      | Ignore frame                      |
| Payload too long       | Discard frame                     |
| Decryption auth fail   | Discard, log potential tamper     |
| Timeout (no activity)  | Return to listening               |
