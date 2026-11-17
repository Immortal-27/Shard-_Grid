// ============================================================================
//  ShardGrid — ESP32 Node B (Storage Node)
//  Receives encrypted share from Central Node via optical link
//  Stores share securely, tamper detection + key zeroization
// ============================================================================

#define NODE_B

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

#include "../shared-lib/config.h"
#include "../shared-lib/optical_protocol.h"
#include "../shared-lib/security.h"

// --- Hardware ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
OpticalRX receiver;
Security security;

// --- State ---
enum NodeState {
    STATE_INIT,
    STATE_READY,
    STATE_RECEIVING,
    STATE_STORED,
    STATE_TAMPERED,
    STATE_LOCKED
};

NodeState currentState = STATE_INIT;
unsigned long lastHeartbeat = 0;
bool systemLocked = false;
bool shareStored = false;
uint8_t storedShareX = 0;
uint8_t storedShareLen = 0;

// --- OLED ---
void displayStatus(const char* line1, const char* line2 = "",
                   const char* line3 = "") {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("=== NODE B ===");

    display.setCursor(0, 16);
    display.println(line1);
    if (strlen(line2) > 0) {
        display.setCursor(0, 28);
        display.println(line2);
    }
    if (strlen(line3) > 0) {
        display.setCursor(0, 40);
        display.println(line3);
    }

    display.setCursor(0, 54);
    switch (currentState) {
        case STATE_INIT:     display.println("[INIT]"); break;
        case STATE_READY:    display.println("[READY]"); break;
        case STATE_RECEIVING:display.println("[RECEIVING]"); break;
        case STATE_STORED:   display.println("[SHARE STORED]"); break;
        case STATE_TAMPERED: display.println("[!!! TAMPERED !!!]"); break;
        case STATE_LOCKED:   display.println("[LOCKED]"); break;
    }
    display.display();
}

void displayTamperAlert() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 5);
    display.println("TAMPER!");
    display.setTextSize(1);
    display.setCursor(0, 30);
    display.println("Keys destroyed.");
    display.setCursor(0, 42);
    display.println("Share wiped.");
    display.setCursor(0, 54);
    display.println("Node B LOCKED.");
    display.display();
}

void processFrame(const OpticalFrame& frame) {
    if (systemLocked) return;

    if (frame.type == MSG_TYPE_TAMPER) {
        Serial.println("[NODE-B] Tamper broadcast received from Central!");
        security.zeroize();
        systemLocked = true;
        currentState = STATE_LOCKED;
        displayTamperAlert();
        return;
    }

    if (frame.type == MSG_TYPE_KEY_EXCH && frame.dst == NODE_ID_B) {
        // Receive AES key from Central Node (provisioning)
        if (frame.len == AES_KEY_SIZE) {
            security.setKey(frame.payload);
            security.saveKeyToEEPROM();
            Serial.println("[NODE-B] AES key received and stored via optical link");
            Serial.println("KEY_SET_OK");
            displayStatus("Key provisioned!", "Via optical link", "Tamper: ARMED");
        } else {
            Serial.printf("[NODE-B] Invalid key length: %d\n", frame.len);
        }
        return;
    }

    if (frame.type == MSG_TYPE_DATA && frame.dst == NODE_ID_B) {
        currentState = STATE_RECEIVING;
        displayStatus("Receiving share...", "Decrypting...");

        uint8_t plainShare[MAX_PAYLOAD_LEN];
        int ptLen = security.decrypt(frame.payload, frame.len,
                                      plainShare, MAX_PAYLOAD_LEN);
        if (ptLen < 0) {
            Serial.println("[NODE-B] Decryption FAILED!");
            displayStatus("DECRYPT FAILED!", "Auth error!");
            delay(3000);
            currentState = STATE_READY;
            displayStatus("Ready", "Waiting for share...");
            return;
        }

        storedShareX = plainShare[0];
        storedShareLen = ptLen - 1;

        security.saveShareToEEPROM(storedShareX, &plainShare[1], storedShareLen);
        shareStored = true;

        memset(plainShare, 0, sizeof(plainShare));

        currentState = STATE_STORED;
        char statusBuf[32];
        snprintf(statusBuf, sizeof(statusBuf), "Share %d stored", storedShareX);
        char lenBuf[32];
        snprintf(lenBuf, sizeof(lenBuf), "%d bytes secured", storedShareLen);
        displayStatus(statusBuf, lenBuf, "Tamper: ARMED");

        Serial.printf("[NODE-B] Share %d stored (%d bytes)\n",
                      storedShareX, storedShareLen);
        Serial.println("SHARE_STORED_OK");
    }

    if (frame.type == MSG_TYPE_HEARTBEAT) {
        Serial.println("[NODE-B] Heartbeat from Central");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=== ShardGrid Node B (ESP32 #3) ===");

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[NODE-B] OLED init failed");
    }
    displayStatus("Booting...");

    receiver.begin(RX_LDR_PIN, true);

    security.begin();
    if (!security.loadKeyFromEEPROM()) {
        security.generateKey();
        security.saveKeyToEEPROM();
    }

    security.setupTamperDetection(TAMPER_PIN);
    pinMode(STATUS_LED_PIN, OUTPUT);

    uint8_t existingData[MAX_PAYLOAD_LEN];
    uint8_t existingLen;
    if (security.loadShareFromEEPROM(storedShareX, existingData, existingLen)) {
        shareStored = true;
        storedShareLen = existingLen;
        currentState = STATE_STORED;
        char buf[32];
        snprintf(buf, sizeof(buf), "Share %d loaded", storedShareX);
        displayStatus(buf, "From EEPROM", "Tamper: ARMED");
    } else {
        currentState = STATE_READY;
        displayStatus("Ready", "Waiting for share...", "Tamper: ARMED");
    }

    memset(existingData, 0, sizeof(existingData));
    Serial.println("[NODE-B] Ready");
    Serial.println("READY");
}

void loop() {
    if (security.checkTamper()) {
        currentState = STATE_TAMPERED;
        systemLocked = true;
        displayTamperAlert();
        Serial.println("TAMPER_ALERT");
        currentState = STATE_LOCKED;
        return;
    }

    if (systemLocked) {
        delay(1000);
        return;
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd == "STATUS") {
            Serial.println("STATUS_REPORT");
            Serial.printf("  Node: B\n");
            Serial.printf("  State: %d\n", currentState);
            Serial.printf("  Share stored: %s\n", shareStored ? "YES" : "NO");
            Serial.printf("  Share X: %d\n", storedShareX);
            Serial.printf("  Key loaded: %s\n", security.isKeyLoaded() ? "YES" : "NO");
            Serial.println("STATUS_END");
        }
        else if (cmd.startsWith("KEY:")) {
            String hexKey = cmd.substring(4);
            if (hexKey.length() == 64) {
                uint8_t newKey[AES_KEY_SIZE];
                for (int i = 0; i < AES_KEY_SIZE; i++) {
                    char hi = hexKey.charAt(i * 2);
                    char lo = hexKey.charAt(i * 2 + 1);
                    newKey[i] = (((hi >= 'a') ? hi - 'a' + 10 : hi - '0') << 4) |
                                ((lo >= 'a') ? lo - 'a' + 10 : lo - '0');
                }
                security.setKey(newKey);
                security.saveKeyToEEPROM();
                memset(newKey, 0, sizeof(newKey));
                Serial.println("KEY_SET_OK");
            }
        }
        else if (cmd == "ZEROIZE") {
            security.zeroize();
            systemLocked = true;
            currentState = STATE_LOCKED;
            displayTamperAlert();
            Serial.println("ZEROIZE_DONE");
        }
    }

    OpticalFrame frame;
    if (receiver.receiveFrame(frame, 100)) {
        if (frame.valid) {
            Serial.printf("[NODE-B] Frame: type=0x%02X from=0x%02X dst=0x%02X\n",
                          frame.type, frame.src, frame.dst);
            processFrame(frame);
        }
    }

    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(50);
        digitalWrite(STATUS_LED_PIN, LOW);
    }
}
