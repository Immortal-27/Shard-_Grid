// ============================================================================
//  ShardGrid — ESP32 Central Node
//  Receives plaintext from Uno → Splits via Shamir SSS → Encrypts shares
//  → Optically transmits to Node A & Node B
//  → Receives shares BACK from both nodes → Reconstructs → Displays on OLED
//  Also: tamper detection, key zeroization, OLED status, web status reporting
// ============================================================================

#define CENTRAL_NODE

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// Shared libraries
#include "../shared-lib/config.h"
#include "../shared-lib/optical_protocol.h"
#include "../shared-lib/shamir_sss.h"
#include "../shared-lib/security.h"

// --- Hardware objects ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledAvailable = false;  // Set true only if OLED init succeeds
OpticalRX receiverUno;      // Receives from Arduino Uno
OpticalRX receiverA;        // Receives share back from Node A
OpticalRX receiverB;        // Receives share back from Node B
OpticalTX transmitterA;     // Sends share to Node A
OpticalTX transmitterB;     // Sends share to Node B
Security security;

// --- State ---
enum NodeState {
    STATE_INIT,
    STATE_READY,
    STATE_RECEIVING,
    STATE_PROCESSING,
    STATE_TRANSMITTING,
    STATE_WAITING_SHARES,
    STATE_RECONSTRUCTING,
    STATE_DISPLAY_MSG,
    STATE_TAMPERED,
    STATE_LOCKED
};

NodeState currentState = STATE_INIT;
unsigned long lastHeartbeat = 0;
bool systemLocked = false;

// --- Reconstruction state ---
// Stores the shares received back from Node A and Node B
bool shareAReceived = false;
bool shareBReceived = false;
SSSShareSet returnedShareA;
SSSShareSet returnedShareB;
unsigned long waitSharesStart = 0;
#define SHARE_RETURN_TIMEOUT  120000  // 2 minutes max wait for shares to come back

// --- OLED Display Helpers (safe when OLED is damaged/absent) ---
void displayStatus(const char* line1, const char* line2 = "",
                   const char* line3 = "") {
    if (!oledAvailable) return;  // Skip if OLED is damaged
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("=== CENTRAL NODE ===");

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

    // Status indicator
    display.setCursor(0, 54);
    switch (currentState) {
        case STATE_INIT:          display.println("[INITIALIZING]"); break;
        case STATE_READY:         display.println("[READY]"); break;
        case STATE_RECEIVING:     display.println("[RECEIVING...]"); break;
        case STATE_PROCESSING:    display.println("[PROCESSING...]"); break;
        case STATE_TRANSMITTING:  display.println("[TRANSMITTING]"); break;
        case STATE_WAITING_SHARES:display.println("[WAITING SHARES]"); break;
        case STATE_RECONSTRUCTING:display.println("[RECONSTRUCTING]"); break;
        case STATE_DISPLAY_MSG:   display.println("[MSG DISPLAYED]"); break;
        case STATE_TAMPERED:      display.println("[!!! TAMPERED !!!]"); break;
        case STATE_LOCKED:        display.println("[LOCKED]"); break;
    }
    display.display();
}

void displayTamperAlert() {
    if (!oledAvailable) return;  // Skip if OLED is damaged
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 5);
    display.println("TAMPER!");
    display.setTextSize(1);
    display.setCursor(0, 30);
    display.println("Keys destroyed.");
    display.setCursor(0, 42);
    display.println("System locked.");
    display.setCursor(0, 54);
    display.println("Re-provision needed.");
    display.display();
}

// --- Display reconstructed message on OLED ---
void displayReconstructedMessage(const uint8_t* message, uint8_t msgLen) {
    if (!oledAvailable) return;  // Skip if OLED is damaged
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("== MSG RECOVERED ==");

    // Display the message in larger text
    display.setTextSize(2);
    display.setCursor(0, 18);

    // Build a null-terminated string from the message bytes
    char msgStr[MAX_PAYLOAD_LEN + 1];
    uint8_t displayLen = min(msgLen, (uint8_t)(MAX_PAYLOAD_LEN));
    memcpy(msgStr, message, displayLen);
    msgStr[displayLen] = '\0';

    display.println(msgStr);

    display.setTextSize(1);
    display.setCursor(0, 54);
    display.print(msgLen);
    display.println(" bytes recovered");
    display.display();
}

// --- Process received message from Uno ---
// Receives PLAINTEXT from Uno. Central does ALL encryption + SSS splitting.
void processMessage(const OpticalFrame& frame) {
    if (systemLocked) {
        Serial.println("[CENTRAL] System locked - ignoring message");
        return;
    }

    currentState = STATE_PROCESSING;
    displayStatus("Msg received!", "Processing...");

    // 1. The payload from Uno is PLAINTEXT (no decryption needed here)
    uint8_t plaintext[MAX_PAYLOAD_LEN];
    uint8_t ptLen = frame.len;
    memcpy(plaintext, frame.payload, ptLen);

    Serial.print("[CENTRAL] Received plaintext message (");
    Serial.print(ptLen);
    Serial.print(" bytes): ");
    for (int i = 0; i < ptLen; i++) Serial.print((char)plaintext[i]);
    Serial.println();

    // 2. Split using Shamir's Secret Sharing (k=2, n=3)
    displayStatus("SSS splitting...", "k=2, n=3");

    SSSShareSet shares[SSS_NUM_SHARES];
    ShamirSSS::splitMessage(plaintext, ptLen, shares,
                            SSS_THRESHOLD, SSS_NUM_SHARES);

    // Wipe plaintext from memory immediately
    memset(plaintext, 0, sizeof(plaintext));

    Serial.println("[CENTRAL] Message split into 3 shares");
    Serial.printf("[CENTRAL] Share 1 (Central): x=%d, len=%d\n",
                  shares[0].shareX, shares[0].len);
    Serial.printf("[CENTRAL] Share 2 (Node A):  x=%d, len=%d\n",
                  shares[1].shareX, shares[1].len);
    Serial.printf("[CENTRAL] Share 3 (Node B):  x=%d, len=%d\n",
                  shares[2].shareX, shares[2].len);

    // 3. Store Share 1 locally (Central keeps one share — not used for reconstruction)
    security.saveShareToEEPROM(shares[0].shareX, shares[0].data, shares[0].len);
    Serial.println("[CENTRAL] Share 1 stored locally");

    // 4. Encrypt shares for Node A and Node B
    uint8_t encShareA[MAX_PAYLOAD_LEN];
    uint8_t encShareB[MAX_PAYLOAD_LEN];

    // Prepend shareX to the data so nodes know their share index
    uint8_t shareABuf[MAX_PAYLOAD_LEN];
    uint8_t shareBBuf[MAX_PAYLOAD_LEN];
    shareABuf[0] = shares[1].shareX;
    memcpy(&shareABuf[1], shares[1].data, shares[1].len);
    shareBBuf[0] = shares[2].shareX;
    memcpy(&shareBBuf[1], shares[2].data, shares[2].len);

    int encLenA = security.encrypt(shareABuf, shares[1].len + 1,
                                    encShareA, MAX_PAYLOAD_LEN);
    int encLenB = security.encrypt(shareBBuf, shares[2].len + 1,
                                    encShareB, MAX_PAYLOAD_LEN);

    // Wipe share data from memory
    memset(shares, 0, sizeof(shares));
    memset(shareABuf, 0, sizeof(shareABuf));
    memset(shareBBuf, 0, sizeof(shareBBuf));

    if (encLenA < 0 || encLenB < 0) {
        Serial.println("[CENTRAL] Share encryption failed!");
        displayStatus("ENCRYPT FAILED!", "Cannot send shares");
        currentState = STATE_READY;
        return;
    }

    // 5. Transmit shares optically
    currentState = STATE_TRANSMITTING;

    // --- Send Share 2 to Node A ---
    displayStatus("TX Share 2", "-> Node A", "via optical link");
    Serial.printf("[CENTRAL] Transmitting Share 2 to Node A (%d bytes)\n", encLenA);
    transmitterA.sendData(NODE_ID_CENTRAL, NODE_ID_A, MSG_TYPE_DATA,
                          encShareA, (uint8_t)encLenA);
    Serial.println("[CENTRAL] Share 2 sent to Node A");
    delay(500);  // Brief pause between transmissions

    // --- Send Share 3 to Node B ---
    displayStatus("TX Share 3", "-> Node B", "via optical link");
    Serial.printf("[CENTRAL] Transmitting Share 3 to Node B (%d bytes)\n", encLenB);
    transmitterB.sendData(NODE_ID_CENTRAL, NODE_ID_B, MSG_TYPE_DATA,
                          encShareB, (uint8_t)encLenB);
    Serial.println("[CENTRAL] Share 3 sent to Node B");

    // Wipe encrypted shares
    memset(encShareA, 0, sizeof(encShareA));
    memset(encShareB, 0, sizeof(encShareB));

    // 6. Enter waiting state — wait for Node A and Node B to send shares back
    shareAReceived = false;
    shareBReceived = false;
    memset(&returnedShareA, 0, sizeof(returnedShareA));
    memset(&returnedShareB, 0, sizeof(returnedShareB));
    waitSharesStart = millis();
    currentState = STATE_WAITING_SHARES;
    displayStatus("Shares sent!", "Waiting for return", "from A & B...");
    Serial.println("[CENTRAL] Waiting for Node A & B to return shares...");

    // Report share distribution via USB serial (for web dashboard)
    Serial.println("SHARE_DIST_OK");
}

// --- Process a share received back from Node A or B ---
void processReturnedShare(const OpticalFrame& frame) {
    if (currentState != STATE_WAITING_SHARES) {
        Serial.printf("[CENTRAL] Ignoring return share — not in waiting state\n");
        return;
    }

    // The returned frame payload is encrypted: [IV][ciphertext+shareX+data][tag]
    // Decrypt it
    uint8_t plainShare[MAX_PAYLOAD_LEN];
    int ptLen = security.decrypt(frame.payload, frame.len,
                                  plainShare, MAX_PAYLOAD_LEN);
    if (ptLen < 0) {
        Serial.printf("[CENTRAL] Failed to decrypt return share from node 0x%02X\n", frame.src);
        return;
    }

    // First byte is shareX, rest is share data
    uint8_t shareX = plainShare[0];
    uint8_t shareLen = ptLen - 1;

    if (frame.src == NODE_ID_A) {
        returnedShareA.shareX = shareX;
        returnedShareA.len = shareLen;
        memcpy(returnedShareA.data, &plainShare[1], shareLen);
        shareAReceived = true;
        Serial.printf("[CENTRAL] Received share %d back from Node A (%d bytes)\n",
                      shareX, shareLen);
        displayStatus("Got share from A!", shareBReceived ? "Both received!" : "Waiting for B...");
    }
    else if (frame.src == NODE_ID_B) {
        returnedShareB.shareX = shareX;
        returnedShareB.len = shareLen;
        memcpy(returnedShareB.data, &plainShare[1], shareLen);
        shareBReceived = true;
        Serial.printf("[CENTRAL] Received share %d back from Node B (%d bytes)\n",
                      shareX, shareLen);
        displayStatus("Got share from B!", shareAReceived ? "Both received!" : "Waiting for A...");
    }

    memset(plainShare, 0, sizeof(plainShare));

    // Check if we have both shares — if so, reconstruct!
    if (shareAReceived && shareBReceived) {
        reconstructAndDisplay();
    }
}

// --- Reconstruct the original message from 2 shares and display on OLED ---
void reconstructAndDisplay() {
    currentState = STATE_RECONSTRUCTING;
    displayStatus("Reconstructing...", "Lagrange interp.");
    Serial.println("[CENTRAL] === RECONSTRUCTING MESSAGE ===");
    Serial.printf("[CENTRAL] Using Share %d (from A) + Share %d (from B)\n",
                  returnedShareA.shareX, returnedShareB.shareX);

    // Build share set array for reconstruction (need k=2 shares)
    SSSShareSet shareSets[SSS_THRESHOLD];
    shareSets[0] = returnedShareA;
    shareSets[1] = returnedShareB;

    // Reconstruct
    uint8_t reconstructed[MAX_PAYLOAD_LEN];
    uint8_t msgLen = 0;
    ShamirSSS::reconstructMessage(shareSets, SSS_THRESHOLD, reconstructed, msgLen);

    // Print reconstructed message to serial
    Serial.print("[CENTRAL] Reconstructed message (");
    Serial.print(msgLen);
    Serial.print(" bytes): ");
    for (int i = 0; i < msgLen; i++) Serial.print((char)reconstructed[i]);
    Serial.println();
    Serial.println("RECONSTRUCT_OK");

    // Display on OLED!
    currentState = STATE_DISPLAY_MSG;
    displayReconstructedMessage(reconstructed, msgLen);

    Serial.println("[CENTRAL] Message displayed on OLED!");

    // Wipe reconstruction data from RAM
    memset(reconstructed, 0, sizeof(reconstructed));
    memset(&returnedShareA, 0, sizeof(returnedShareA));
    memset(&returnedShareB, 0, sizeof(returnedShareB));
    memset(shareSets, 0, sizeof(shareSets));
    shareAReceived = false;
    shareBReceived = false;
}

// --- Handle status request from web dashboard ---
void reportStatus() {
    Serial.println("STATUS_REPORT");
    Serial.printf("  State: %d\n", currentState);
    Serial.printf("  Key loaded: %s\n", security.isKeyLoaded() ? "YES" : "NO");
    Serial.printf("  Tampered: %s\n", systemLocked ? "YES" : "NO");
    Serial.printf("  Share A back: %s\n", shareAReceived ? "YES" : "NO");
    Serial.printf("  Share B back: %s\n", shareBReceived ? "YES" : "NO");
    Serial.printf("  Uptime: %lu ms\n", millis());
    Serial.println("STATUS_END");
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=== ShardGrid Central Node (ESP32 #1) ===");
    Serial.println("[CENTRAL] Initializing...");

    // Initialize OLED (optional — works without it via Network Display)
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[CENTRAL] OLED init failed — using Network Display instead");
        oledAvailable = false;
        // Continue without display — the API server shows everything
    } else {
        oledAvailable = true;
        Serial.println("[CENTRAL] OLED initialized OK");
    }
    displayStatus("Booting...");

    // Initialize optical receiver from Uno
    receiverUno.begin(RX_LDR_PIN, true);  // Inverted LDR
    Serial.println("[CENTRAL] Optical RX (Uno) initialized on pin 34");

    // Initialize optical receivers from Node A and Node B (return path)
    receiverA.begin(RX_LDR_A_PIN, true);
    Serial.println("[CENTRAL] Optical RX (Node A return) initialized on pin 35");
    receiverB.begin(RX_LDR_B_PIN, true);
    Serial.println("[CENTRAL] Optical RX (Node B return) initialized on pin 32");

    // Initialize optical transmitters (to Node A and Node B)
    transmitterA.begin(TX_LED_A_PIN, false);
    transmitterB.begin(TX_LED_B_PIN, false);
    Serial.println("[CENTRAL] Optical TX A & B initialized");

    // Test blink TX LEDs on startup so you can physically verify wiring
    Serial.println("[CENTRAL] Testing TX LEDs (LED A on GPIO 25, LED B on GPIO 26)...");
    digitalWrite(TX_LED_A_PIN, HIGH);
    digitalWrite(TX_LED_B_PIN, HIGH);
    delay(500);
    digitalWrite(TX_LED_A_PIN, LOW);
    digitalWrite(TX_LED_B_PIN, LOW);
    delay(300);
    digitalWrite(TX_LED_A_PIN, HIGH);
    digitalWrite(TX_LED_B_PIN, HIGH);
    delay(500);
    digitalWrite(TX_LED_A_PIN, LOW);
    digitalWrite(TX_LED_B_PIN, LOW);
    Serial.println("[CENTRAL] TX LED test complete");

    // Initialize security
    security.begin();

    // Try to load existing key, or generate new one
    if (!security.loadKeyFromEEPROM()) {
        Serial.println("[CENTRAL] No existing key - generating new one");
        security.generateKey();
        security.saveKeyToEEPROM();
    }

    // Setup tamper detection
    security.setupTamperDetection(TAMPER_PIN);
    pinMode(STATUS_LED_PIN, OUTPUT);

    currentState = STATE_READY;
    displayStatus("Ready", "Waiting for data...", "Tamper: ARMED");
    Serial.println("[CENTRAL] System ready");
    Serial.println("READY");
}

void loop() {
    // --- Priority 1: Check tamper ---
    if (security.checkTamper()) {
        currentState = STATE_TAMPERED;
        systemLocked = true;
        displayTamperAlert();

        // Broadcast tamper alert to both nodes
        uint8_t tamperMsg[] = {0xFF};
        transmitterA.sendData(NODE_ID_CENTRAL, NODE_ID_A, MSG_TYPE_TAMPER,
                              tamperMsg, 1);
        delay(200);
        transmitterB.sendData(NODE_ID_CENTRAL, NODE_ID_B, MSG_TYPE_TAMPER,
                              tamperMsg, 1);

        Serial.println("TAMPER_ALERT");
        currentState = STATE_LOCKED;
        return;  // Permanently locked
    }

    if (systemLocked) {
        delay(1000);
        return;
    }

    // --- Priority 2: Check for USB serial commands from web dashboard ---
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd == "STATUS") {
            reportStatus();
        }
        else if (cmd.startsWith("KEY:")) {
            // Receive new AES key from dashboard: KEY:hex_encoded_32_bytes
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
                displayStatus("Key updated!", "From dashboard");
            } else {
                Serial.println("KEY_ERR: Need 64 hex chars");
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

    // --- Priority 3: Check for optical data from Uno ---
    if (currentState == STATE_READY) {
        OpticalFrame frame;
        if (receiverUno.receiveFrame(frame, 100)) {
            if (frame.valid && frame.dst == NODE_ID_CENTRAL) {
                Serial.printf("[CENTRAL] Frame from Uno: type=0x%02X len=%d src=0x%02X\n",
                              frame.type, frame.len, frame.src);

                currentState = STATE_RECEIVING;
                displayStatus("Frame received!", "Processing...");

                switch (frame.type) {
                    case MSG_TYPE_DATA:
                        processMessage(frame);
                        break;
                    case MSG_TYPE_KEY_EXCH:
                        // Receive AES key optically from Uno (forwarded from dashboard)
                        if (frame.len == AES_KEY_SIZE) {
                            security.setKey(frame.payload);
                            security.saveKeyToEEPROM();
                            Serial.println("[CENTRAL] AES key received optically and stored");
                            Serial.println("KEY_SET_OK");
                            displayStatus("Provisioning key", "-> Node A...");
                            Serial.println("[CENTRAL] Transmitting key to Node A via LED on GPIO 25 (~80s)...");
                            transmitterA.sendData(NODE_ID_CENTRAL, NODE_ID_A, MSG_TYPE_KEY_EXCH,
                                                  frame.payload, frame.len);
                            Serial.println("[CENTRAL] Key sent to Node A");
                            delay(500);
                            displayStatus("Provisioning key", "-> Node B...");
                            Serial.println("[CENTRAL] Transmitting key to Node B via LED on GPIO 26 (~80s)...");
                            transmitterB.sendData(NODE_ID_CENTRAL, NODE_ID_B, MSG_TYPE_KEY_EXCH,
                                                  frame.payload, frame.len);
                            Serial.println("[CENTRAL] Key sent to Node B");
                            Serial.println("[CENTRAL] Key distributed to Node A & B");
                            displayStatus("Key provisioned!", "All 3 nodes ready", "Tamper: ARMED");
                        } else {
                            Serial.printf("[CENTRAL] Invalid key length: %d\n", frame.len);
                        }
                        break;
                    case MSG_TYPE_ZEROIZE:
                        Serial.println("[CENTRAL] ZEROIZE command received optically");
                        transmitterA.sendData(NODE_ID_CENTRAL, NODE_ID_A, MSG_TYPE_TAMPER,
                                              frame.payload, 1);
                        delay(200);
                        transmitterB.sendData(NODE_ID_CENTRAL, NODE_ID_B, MSG_TYPE_TAMPER,
                                              frame.payload, 1);
                        security.zeroize();
                        systemLocked = true;
                        currentState = STATE_LOCKED;
                        displayTamperAlert();
                        Serial.println("ZEROIZE_DONE");
                        break;
                    case MSG_TYPE_HEARTBEAT:
                        Serial.println("[CENTRAL] Heartbeat from Uno");
                        break;
                    case MSG_TYPE_STATUS:
                        reportStatus();
                        break;
                    default:
                        Serial.printf("[CENTRAL] Unknown msg type: 0x%02X\n", frame.type);
                        break;
                }
            }
        }
    }

    // --- Priority 4: Check for return shares from Node A and Node B ---
    if (currentState == STATE_WAITING_SHARES) {
        // Check timeout
        if (millis() - waitSharesStart > SHARE_RETURN_TIMEOUT) {
            Serial.println("[CENTRAL] Timeout waiting for return shares!");
            displayStatus("TIMEOUT!", "Shares not returned");
            delay(3000);
            currentState = STATE_READY;
            displayStatus("Ready", "Waiting for data...", "Tamper: ARMED");
            return;
        }

        // Poll Node A's return LDR
        if (!shareAReceived) {
            OpticalFrame frameA;
            if (receiverA.receiveFrame(frameA, 50)) {
                if (frameA.valid && frameA.dst == NODE_ID_CENTRAL && frameA.src == NODE_ID_A) {
                    Serial.printf("[CENTRAL] Return frame from Node A: type=0x%02X len=%d\n",
                                  frameA.type, frameA.len);
                    if (frameA.type == MSG_TYPE_DATA) {
                        processReturnedShare(frameA);
                    }
                }
            }
        }

        // Poll Node B's return LDR
        if (!shareBReceived) {
            OpticalFrame frameB;
            if (receiverB.receiveFrame(frameB, 50)) {
                if (frameB.valid && frameB.dst == NODE_ID_CENTRAL && frameB.src == NODE_ID_B) {
                    Serial.printf("[CENTRAL] Return frame from Node B: type=0x%02X len=%d\n",
                                  frameB.type, frameB.len);
                    if (frameB.type == MSG_TYPE_DATA) {
                        processReturnedShare(frameB);
                    }
                }
            }
        }
    }

    // --- Heartbeat LED blink ---
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(50);
        digitalWrite(STATUS_LED_PIN, LOW);
    }
}
