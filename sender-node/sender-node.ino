// ============================================================================
//  ShardGrid — Arduino Uno Sender Node
//  Receives PLAINTEXT from PC via USB Serial → Transmits optically to Central ESP32
//  This node is a dumb relay — all encryption happens on the Central ESP32.
//  Supported commands from dashboard:
//    #01:HEX_DATA    → Forward plaintext data optically
//    #04:            → Send heartbeat
//    KEY:HEX_KEY     → Forward AES key to Central for provisioning
//    ZEROIZE         → Forward zeroize command to Central
// ============================================================================

#define UNO_SENDER
// Note: config.h uses UNO_SENDER define for pin selection

#define TX_PIN 8          // LED output to Central ESP32's LDR
#define BIT_TIME 250      // ms per bit — must match receiver
#define PREAMBLE_BITS 8
#define FRAME_START 0x7E
#define FRAME_END   0x7F
#define MAX_PAYLOAD 128

// --- CRC-16 CCITT ---
uint16_t crc16_update(uint16_t crc, uint8_t data) {
    crc ^= (uint16_t)data << 8;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
        else crc <<= 1;
    }
    return crc;
}

uint16_t crc16_compute(const uint8_t* data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

// --- Manchester Encoding ---
// Bit 1: HIGH first half, LOW second half
// Bit 0: LOW first half, HIGH second half
void sendBitManchester(uint8_t bit) {
    if (bit) {
        digitalWrite(TX_PIN, HIGH);
        delay(BIT_TIME / 2);
        digitalWrite(TX_PIN, LOW);
        delay(BIT_TIME / 2);
    } else {
        digitalWrite(TX_PIN, LOW);
        delay(BIT_TIME / 2);
        digitalWrite(TX_PIN, HIGH);
        delay(BIT_TIME / 2);
    }
}

void sendByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        sendBitManchester((b >> i) & 1);
    }
}

// Send framed optical data
// Frame: [PREAMBLE][START][SRC][DST][TYPE][LEN][PAYLOAD...][CRC_H][CRC_L][END]
void sendFrame(uint8_t src, uint8_t dst, uint8_t type,
               const uint8_t* payload, uint8_t len) {
    // Preamble for clock sync
    for (int i = 0; i < PREAMBLE_BITS; i++) {
        sendBitManchester(i & 1);
    }

    // Start delimiter
    sendByte(FRAME_START);

    // Header
    sendByte(src);
    sendByte(dst);
    sendByte(type);
    sendByte(len);

    // Payload
    for (int i = 0; i < len; i++) {
        sendByte(payload[i]);
    }

    // CRC-16 over header + payload
    uint8_t crcBuf[4 + MAX_PAYLOAD];
    crcBuf[0] = src;
    crcBuf[1] = dst;
    crcBuf[2] = type;
    crcBuf[3] = len;
    memcpy(&crcBuf[4], payload, len);
    uint16_t crc = crc16_compute(crcBuf, 4 + len);
    sendByte((crc >> 8) & 0xFF);
    sendByte(crc & 0xFF);

    // End delimiter
    sendByte(FRAME_END);

    // Inter-frame gap
    digitalWrite(TX_PIN, LOW);
    delay(BIT_TIME * 3);
}

// --- Serial Protocol from Web Dashboard ---
// Format: #TYPE:HEX_DATA\n
// Example: #01:48656C6C6F  (type=DATA, payload="Hello" in hex)

uint8_t hexCharToNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

uint8_t hexToByte(char hi, char lo) {
    return (hexCharToNibble(hi) << 4) | hexCharToNibble(lo);
}

void setup() {
    Serial.begin(115200);
    pinMode(TX_PIN, OUTPUT);
    digitalWrite(TX_PIN, LOW);

    Serial.println("=== ShardGrid Sender Node (Arduino Uno) ===");
    Serial.println("Ready. Waiting for commands from web dashboard...");
    Serial.println("Format: #TYPE:HEX_PAYLOAD");
    Serial.println("  #01:48656C6C6F  → Send 'Hello' as DATA");
    Serial.println("  #04:            → Send HEARTBEAT");
    Serial.println("READY");  // Signal to web dashboard
}

void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.length() < 1) return;

        // Echo for debug
        Serial.print("[UNO] Received command: ");
        Serial.println(cmd);

        if (cmd.startsWith("KEY:")) {
            // Forward AES-256 key to Central ESP32 via optical link
            // KEY:64_hex_chars → send as MSG_TYPE_KEY_EXCH (0x05)
            String hexKey = cmd.substring(4);
            uint8_t keyPayload[32];  // 32 bytes = 256-bit key
            uint8_t keyLen = hexKey.length() / 2;
            if (keyLen > 32) keyLen = 32;
            for (int i = 0; i < keyLen; i++) {
                keyPayload[i] = hexToByte(hexKey.charAt(i * 2),
                                           hexKey.charAt(i * 2 + 1));
            }

            Serial.print("[UNO] Forwarding AES key optically (");
            Serial.print(keyLen);
            Serial.println(" bytes)");
            sendFrame(0x00, 0x01, 0x05, keyPayload, keyLen);  // 0x05 = KEY_EXCH
            Serial.println("[UNO] TX_DONE");

        } else if (cmd == "ZEROIZE") {
            // Forward zeroize command to Central ESP32
            uint8_t zeroPayload[] = {0xFF};
            Serial.println("[UNO] Forwarding ZEROIZE command optically");
            sendFrame(0x00, 0x01, 0x08, zeroPayload, 1);  // 0x08 = ZEROIZE
            Serial.println("[UNO] TX_DONE");

        } else if (cmd.charAt(0) == '#' && cmd.length() >= 3) {
            // Parse: #TYPE:HEX_DATA (plaintext data)
            uint8_t msgType = hexToByte(cmd.charAt(1), cmd.charAt(2));

            uint8_t payload[MAX_PAYLOAD];
            uint8_t payloadLen = 0;

            int colonIdx = cmd.indexOf(':');
            if (colonIdx > 0 && colonIdx + 1 < (int)cmd.length()) {
                String hexData = cmd.substring(colonIdx + 1);
                payloadLen = hexData.length() / 2;
                if (payloadLen > MAX_PAYLOAD) payloadLen = MAX_PAYLOAD;
                for (int i = 0; i < payloadLen; i++) {
                    payload[i] = hexToByte(hexData.charAt(i * 2),
                                            hexData.charAt(i * 2 + 1));
                }
            }

            Serial.print("[UNO] Sending frame: type=0x");
            Serial.print(msgType, HEX);
            Serial.print(" len=");
            Serial.println(payloadLen);

            // Transmit optically: Uno (0x00) → Central (0x01)
            sendFrame(0x00, 0x01, msgType, payload, payloadLen);

            Serial.println("[UNO] TX_DONE");
        } else {
            Serial.println("[UNO] ERR: Invalid command format");
        }
    }
}
