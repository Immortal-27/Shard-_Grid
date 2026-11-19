#ifndef OPTICAL_PROTOCOL_H
#define OPTICAL_PROTOCOL_H

#include <Arduino.h>
#include "config.h"

// ============================================================================
//  ShardGrid — Optical Communication Protocol
//  Frame: [PREAMBLE x8][START][SRC][DST][TYPE][LEN][PAYLOAD...][CRC16_H][CRC16_L][END]
// ============================================================================

// --- CRC-16 (CCITT) Lookup-free implementation ---
static uint16_t crc16_update(uint16_t crc, uint8_t data) {
    crc ^= (uint16_t)data << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000)
            crc = (crc << 1) ^ 0x1021;
        else
            crc <<= 1;
    }
    return crc;
}

static uint16_t crc16_compute(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

// --- Frame structure ---
struct OpticalFrame {
    uint8_t src;                    // Source node ID
    uint8_t dst;                    // Destination node ID
    uint8_t type;                   // Message type
    uint8_t len;                    // Payload length
    uint8_t payload[MAX_PAYLOAD_LEN]; // Payload data
    uint16_t crc;                   // CRC-16 checksum
    bool valid;                     // Frame validity flag
};

// ============================================================================
//  TRANSMITTER FUNCTIONS
// ============================================================================

class OpticalTX {
public:
    int pin;
    bool inverted;

    OpticalTX() : pin(-1), inverted(false) {}

    void begin(int txPin, bool invertedLogic = false) {
        pin = txPin;
        inverted = invertedLogic;
        pinMode(pin, OUTPUT);
        setLED(false);
    }

    void setLED(bool on) {
        if (inverted)
            digitalWrite(pin, on ? LOW : HIGH);
        else
            digitalWrite(pin, on ? HIGH : LOW);
    }

    // Send a single bit using Manchester encoding
    // Manchester: 0 = LOW→HIGH transition, 1 = HIGH→LOW transition
    void sendBitManchester(uint8_t bit) {
        if (bit) {
            setLED(true);
            delay(BIT_TIME_MS / 2);
            setLED(false);
            delay(BIT_TIME_MS / 2);
        } else {
            setLED(false);
            delay(BIT_TIME_MS / 2);
            setLED(true);
            delay(BIT_TIME_MS / 2);
        }
    }

    // Send a single byte (MSB first) with Manchester encoding
    void sendByte(uint8_t b) {
        for (int i = 7; i >= 0; i--) {
            sendBitManchester((b >> i) & 1);
        }
    }

    // Send a complete frame
    void sendFrame(const OpticalFrame& frame) {
        // 1. Preamble — alternating pattern for receiver clock sync
        for (int i = 0; i < PREAMBLE_BITS; i++) {
            sendBitManchester(i & 1);  // 0,1,0,1...
        }

        // 2. Start delimiter
        sendByte(FRAME_START);

        // 3. Header: SRC, DST, TYPE, LEN
        sendByte(frame.src);
        sendByte(frame.dst);
        sendByte(frame.type);
        sendByte(frame.len);

        // 4. Payload
        for (int i = 0; i < frame.len; i++) {
            sendByte(frame.payload[i]);
        }

        // 5. CRC-16 (computed over header + payload)
        uint8_t headerAndPayload[4 + MAX_PAYLOAD_LEN];
        headerAndPayload[0] = frame.src;
        headerAndPayload[1] = frame.dst;
        headerAndPayload[2] = frame.type;
        headerAndPayload[3] = frame.len;
        memcpy(&headerAndPayload[4], frame.payload, frame.len);
        uint16_t crc = crc16_compute(headerAndPayload, 4 + frame.len);

        sendByte((crc >> 8) & 0xFF);  // CRC high byte
        sendByte(crc & 0xFF);         // CRC low byte

        // 6. End delimiter
        sendByte(FRAME_END);

        // 7. Inter-frame gap
        setLED(false);
        delay(STOP_GAP_MS);
    }

    // Convenience: build and send a data frame
    void sendData(uint8_t src, uint8_t dst, uint8_t msgType,
                  const uint8_t* data, uint8_t dataLen) {
        OpticalFrame frame;
        frame.src = src;
        frame.dst = dst;
        frame.type = msgType;
        frame.len = min(dataLen, (uint8_t)MAX_PAYLOAD_LEN);
        memcpy(frame.payload, data, frame.len);
        sendFrame(frame);
    }
};

// ============================================================================
//  RECEIVER FUNCTIONS
// ============================================================================

class OpticalRX {
public:
    int pin;
    bool inverted;

    OpticalRX() : pin(-1), inverted(false) {}

    void begin(int rxPin, bool invertedLogic = true) {
        pin = rxPin;
        inverted = invertedLogic;
        pinMode(pin, INPUT);
    }

    bool readLDR() {
        int val = digitalRead(pin);
        return inverted ? !val : (val == HIGH);
    }

    // Self-synchronizing, noise-filtered Manchester bit receiver
    // Locks onto the physical mid-bit clock transition of EVERY bit to eliminate drift!
    // Bit 1: HIGH -> LOW (falling edge at mid-bit)
    // Bit 0: LOW -> HIGH (rising edge at mid-bit)
    int receiveBitManchester() {
        // Step 1: Wait past the boundary transition (80% of bit time = ~200ms)
        // This ensures even slow LDR turn-off latency (~50-70ms from t=125ms) has fully settled!
        delay(BIT_TIME_MS * 80 / 100);

        // Step 2: Read stable level in first half of the bit
        bool levelBefore = readLDR();

        // Step 3: Wait for the mid-bit transition (expected at ~250ms, i.e. ~50ms from now)
        unsigned long tStart = millis();
        while (true) {
            if (readLDR() != levelBefore) {
                // Potential edge! Debounce 2ms to reject analog noise spikes:
                delay(2);
                if (readLDR() != levelBefore) {
                    // Confirmed optical transition edge
                    break;
                }
            }
            // Timeout if no transition arrives within 120ms (reaches t=320ms)
            if (millis() - tStart > 120) {
                return -1; // Edge timeout
            }
            delayMicroseconds(200);
        }

        // Step 4: Determine bit directly from transition direction:
        // If levelBefore was HIGH -> transition was HIGH to LOW = Bit 1
        // If levelBefore was LOW  -> transition was LOW to HIGH = Bit 0
        return levelBefore ? 1 : 0;
    }

    // Receive a full byte (MSB first)
    int receiveByte() {
        uint8_t result = 0;
        for (int i = 7; i >= 0; i--) {
            int bit = receiveBitManchester();
            if (bit < 0) return -1; // Encoding error
            result |= (bit << i);
        }
        return result;
    }

    // Drain optical channel until it has been dark/quiet for at least 600ms
    void drainUntilIdle() {
        unsigned long darkStart = millis();
        while (millis() - darkStart < (BIT_TIME_MS * 3)) {
            if (readLDR()) {
                darkStart = millis(); // Reset timer if light is still detected
            }
            delay(10);
        }
    }

    // Wait for preamble pattern, then receive a complete frame
    // Returns true if a valid frame was received
    bool receiveFrame(OpticalFrame& frame, unsigned long timeoutMs = 30000) {
        unsigned long startTime = millis();
        frame.valid = false;

        // --- Wait for activity (first HIGH from preamble bit 0) ---
        while (!readLDR()) {
            if (millis() - startTime > timeoutMs) return false;
            delayMicroseconds(500);
        }

        // Debounce 2ms to verify real light
        delay(2);
        if (!readLDR()) return false;

        // We've caught the mid-bit rising edge of preamble bit 0 (a '0' bit: LOW→HIGH).
        // receiveBitManchester() expects to be called right after a mid-bit edge:
        //   it waits 80% of BIT_TIME to skip past the boundary, then looks for the
        //   next mid-bit transition.  But we arrived here partway through the edge
        //   detection + 2ms debounce — we need to wait the remaining half-bit so
        //   the first receiveBitManchester() call begins at the correct phase
        //   (i.e. right at the end of the current bit's second half).
        delay(BIT_TIME_MS / 2);

        // Now consume the remaining 7 preamble bits:
        for (int i = 0; i < PREAMBLE_BITS - 1; i++) {
            if (receiveBitManchester() < 0) {
                drainUntilIdle();
                return false;
            }
        }

        // --- Read start delimiter ---
        int startByte = receiveByte();
        if (startByte != FRAME_START) {
            if (startByte < 0) {
                Serial.println("[RX] Start delimiter: decode timeout");
            } else {
                Serial.printf("[RX] Bad start delimiter: 0x%02X\n", (uint8_t)startByte);
            }
            drainUntilIdle();
            return false;
        }

        // --- Read header ---
        int src = receiveByte();
        int dst = receiveByte();
        int type = receiveByte();
        int len = receiveByte();

        if (src < 0 || dst < 0 || type < 0 || len < 0) {
            Serial.println("[RX] Header decode error");
            drainUntilIdle();
            return false;
        }

        if (len > MAX_PAYLOAD_LEN) {
            Serial.println("[RX] Payload too long");
            drainUntilIdle();
            return false;
        }

        frame.src = (uint8_t)src;
        frame.dst = (uint8_t)dst;
        frame.type = (uint8_t)type;
        frame.len = (uint8_t)len;

        // --- Read payload ---
        for (int i = 0; i < frame.len; i++) {
            int b = receiveByte();
            if (b < 0) {
                Serial.print("[RX] Payload byte ");
                Serial.print(i);
                Serial.println(" decode error");
                drainUntilIdle();
                return false;
            }
            frame.payload[i] = (uint8_t)b;
        }

        // --- Read CRC ---
        int crcHi = receiveByte();
        int crcLo = receiveByte();
        if (crcHi < 0 || crcLo < 0) {
            Serial.println("[RX] CRC decode error");
            drainUntilIdle();
            return false;
        }
        uint16_t receivedCRC = ((uint16_t)crcHi << 8) | (uint16_t)crcLo;

        // --- Read end delimiter ---
        int endByte = receiveByte();
        if (endByte != FRAME_END) {
            Serial.println("[RX] Bad end delimiter");
            drainUntilIdle();
            return false;
        }

        // --- Verify CRC ---
        uint8_t headerAndPayload[4 + MAX_PAYLOAD_LEN];
        headerAndPayload[0] = frame.src;
        headerAndPayload[1] = frame.dst;
        headerAndPayload[2] = frame.type;
        headerAndPayload[3] = frame.len;
        memcpy(&headerAndPayload[4], frame.payload, frame.len);
        uint16_t computedCRC = crc16_compute(headerAndPayload, 4 + frame.len);

        if (receivedCRC != computedCRC) {
            Serial.print("[RX] CRC mismatch: got 0x");
            Serial.print(receivedCRC, HEX);
            Serial.print(", expected 0x");
            Serial.println(computedCRC, HEX);
            drainUntilIdle();
            return false;
        }

        frame.crc = receivedCRC;
        frame.valid = true;
        return true;
    }
};

#endif // OPTICAL_PROTOCOL_H
