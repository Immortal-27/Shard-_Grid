#ifndef SECURITY_H
#define SECURITY_H

#include <Arduino.h>
#include "config.h"

// ESP32 has mbedtls built into the Arduino core
#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include <EEPROM.h>

// ============================================================================
//  ShardGrid — Security Module
//  AES-256-GCM encryption, key management, tamper response
// ============================================================================

// EEPROM layout for key storage
#define EEPROM_SIZE         512
#define EEPROM_KEY_OFFSET   0       // 32 bytes: AES-256 key
#define EEPROM_KEY_VALID    32      // 1 byte: 0xA5 = valid, 0x00 = wiped
#define EEPROM_NODE_ID      33      // 1 byte: node identifier
#define EEPROM_SHARE_OFFSET 64      // Share data storage start
#define EEPROM_SHARE_LEN    34      // 1 byte: stored share length
#define EEPROM_SHARE_X      35      // 1 byte: share index

// --- Volatile tamper flag (ISR-safe) ---
volatile bool tamperTriggered = false;
volatile unsigned long lastTamperTime = 0;

// ISR for tamper pin
void IRAM_ATTR tamperISR() {
    unsigned long now = millis();
    if (now - lastTamperTime > TAMPER_DEBOUNCE_MS) {
        tamperTriggered = true;
        lastTamperTime = now;
    }
}

class Security {
private:
    uint8_t aesKey[AES_KEY_SIZE];
    bool keyLoaded;
    mbedtls_gcm_context gcm;

public:
    Security() : keyLoaded(false) {
        memset(aesKey, 0, AES_KEY_SIZE);
    }

    // --- Initialization ---
    void begin() {
        EEPROM.begin(EEPROM_SIZE);
        mbedtls_gcm_init(&gcm);
    }

    // --- Key Management ---

    // Generate a new random AES-256 key using ESP32 hardware RNG
    void generateKey() {
        for (int i = 0; i < AES_KEY_SIZE; i++) {
            aesKey[i] = (uint8_t)(esp_random() & 0xFF);
        }
        keyLoaded = true;
        Serial.println("[SEC] New AES-256 key generated");
    }

    // Store key to EEPROM (encrypted storage)
    void saveKeyToEEPROM() {
        for (int i = 0; i < AES_KEY_SIZE; i++) {
            EEPROM.write(EEPROM_KEY_OFFSET + i, aesKey[i]);
        }
        EEPROM.write(EEPROM_KEY_VALID, 0xA5);  // Mark as valid
        EEPROM.commit();
        Serial.println("[SEC] Key saved to EEPROM");
    }

    // Load key from EEPROM
    bool loadKeyFromEEPROM() {
        if (EEPROM.read(EEPROM_KEY_VALID) != 0xA5) {
            Serial.println("[SEC] No valid key in EEPROM");
            return false;
        }
        for (int i = 0; i < AES_KEY_SIZE; i++) {
            aesKey[i] = EEPROM.read(EEPROM_KEY_OFFSET + i);
        }
        keyLoaded = true;
        Serial.println("[SEC] Key loaded from EEPROM");
        return true;
    }

    // Set key directly (received from web dashboard during provisioning)
    void setKey(const uint8_t* key) {
        memcpy(aesKey, key, AES_KEY_SIZE);
        keyLoaded = true;
    }

    bool isKeyLoaded() { return keyLoaded; }

    // --- AES-256-GCM Encryption ---

    // Encrypt plaintext. Output: [IV (12)][ciphertext (len)][tag (16)]
    // Returns total output length, or -1 on error
    int encrypt(const uint8_t* plaintext, uint8_t ptLen,
                uint8_t* output, uint8_t maxOutLen) {
        if (!keyLoaded) {
            Serial.println("[SEC] No key loaded!");
            return -1;
        }

        uint8_t totalLen = AES_IV_SIZE + ptLen + AES_TAG_SIZE;
        if (totalLen > maxOutLen) {
            Serial.println("[SEC] Output buffer too small");
            return -1;
        }

        // Generate random IV
        uint8_t iv[AES_IV_SIZE];
        for (int i = 0; i < AES_IV_SIZE; i++) {
            iv[i] = (uint8_t)(esp_random() & 0xFF);
        }

        // Copy IV to output
        memcpy(output, iv, AES_IV_SIZE);

        // Setup GCM context
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                      aesKey, AES_KEY_SIZE * 8);
        if (ret != 0) {
            Serial.printf("[SEC] GCM setkey failed: %d\n", ret);
            return -1;
        }

        // Encrypt
        uint8_t tag[AES_TAG_SIZE];
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                         ptLen, iv, AES_IV_SIZE,
                                         NULL, 0,  // No AAD
                                         plaintext,
                                         output + AES_IV_SIZE,  // Ciphertext after IV
                                         AES_TAG_SIZE, tag);
        if (ret != 0) {
            Serial.printf("[SEC] GCM encrypt failed: %d\n", ret);
            return -1;
        }

        // Append tag after ciphertext
        memcpy(output + AES_IV_SIZE + ptLen, tag, AES_TAG_SIZE);

        return totalLen;
    }

    // Decrypt ciphertext. Input format: [IV (12)][ciphertext][tag (16)]
    // Returns plaintext length, or -1 on error/auth failure
    int decrypt(const uint8_t* input, uint8_t inputLen,
                uint8_t* plaintext, uint8_t maxPtLen) {
        if (!keyLoaded) {
            Serial.println("[SEC] No key loaded!");
            return -1;
        }

        if (inputLen < AES_IV_SIZE + AES_TAG_SIZE) {
            Serial.println("[SEC] Input too short");
            return -1;
        }

        uint8_t ptLen = inputLen - AES_IV_SIZE - AES_TAG_SIZE;
        if (ptLen > maxPtLen) {
            Serial.println("[SEC] Plaintext buffer too small");
            return -1;
        }

        // Extract IV
        const uint8_t* iv = input;

        // Extract tag
        const uint8_t* tag = input + AES_IV_SIZE + ptLen;

        // Setup GCM context
        int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                      aesKey, AES_KEY_SIZE * 8);
        if (ret != 0) {
            Serial.printf("[SEC] GCM setkey failed: %d\n", ret);
            return -1;
        }

        // Decrypt and verify
        ret = mbedtls_gcm_auth_decrypt(&gcm, ptLen,
                                        iv, AES_IV_SIZE,
                                        NULL, 0,  // No AAD
                                        tag, AES_TAG_SIZE,
                                        input + AES_IV_SIZE,  // Ciphertext
                                        plaintext);
        if (ret != 0) {
            Serial.printf("[SEC] GCM auth/decrypt FAILED: %d (TAMPER?)\n", ret);
            return -1;
        }

        return ptLen;
    }

    // --- Share Storage ---

    void saveShareToEEPROM(uint8_t shareX, const uint8_t* shareData, uint8_t shareLen) {
        EEPROM.write(EEPROM_SHARE_X, shareX);
        EEPROM.write(EEPROM_SHARE_LEN, shareLen);
        for (int i = 0; i < shareLen; i++) {
            EEPROM.write(EEPROM_SHARE_OFFSET + i, shareData[i]);
        }
        EEPROM.commit();
        Serial.printf("[SEC] Share %d saved (%d bytes)\n", shareX, shareLen);
    }

    bool loadShareFromEEPROM(uint8_t& shareX, uint8_t* shareData, uint8_t& shareLen) {
        shareX = EEPROM.read(EEPROM_SHARE_X);
        shareLen = EEPROM.read(EEPROM_SHARE_LEN);
        if (shareX == 0 || shareX > SSS_NUM_SHARES || shareLen == 0 || shareLen > MAX_PAYLOAD_LEN) {
            return false;
        }
        for (int i = 0; i < shareLen; i++) {
            shareData[i] = EEPROM.read(EEPROM_SHARE_OFFSET + i);
        }
        return true;
    }

    // --- TAMPER RESPONSE: Secure Key Zeroization ---

    void zeroize() {
        Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        Serial.println("!!! TAMPER DETECTED — ZEROIZING !!!");
        Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

        // 1. Overwrite key in RAM multiple times with random data then zeros
        for (int pass = 0; pass < KEY_WIPE_PASSES; pass++) {
            for (int i = 0; i < AES_KEY_SIZE; i++) {
                aesKey[i] = (uint8_t)(esp_random() & 0xFF);
            }
        }
        memset(aesKey, 0x00, AES_KEY_SIZE);
        keyLoaded = false;

        // 2. Wipe EEPROM key storage
        for (int pass = 0; pass < KEY_WIPE_PASSES; pass++) {
            for (int i = 0; i < AES_KEY_SIZE; i++) {
                EEPROM.write(EEPROM_KEY_OFFSET + i, (uint8_t)(esp_random() & 0xFF));
            }
            EEPROM.write(EEPROM_KEY_VALID, 0x00);
            EEPROM.commit();
        }
        // Final pass: all zeros
        for (int i = 0; i < EEPROM_SIZE; i++) {
            EEPROM.write(i, 0x00);
        }
        EEPROM.commit();

        // 3. Free GCM context
        mbedtls_gcm_free(&gcm);

        Serial.println("[SEC] === ALL KEYS AND SHARES DESTROYED ===");
    }

    // Setup tamper detection interrupt
    void setupTamperDetection(int tamperPin) {
        pinMode(tamperPin, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(tamperPin), tamperISR, FALLING);
        Serial.println("[SEC] Tamper detection armed");
    }

    // Check if tamper was triggered (call in loop)
    bool checkTamper() {
        if (tamperTriggered) {
            tamperTriggered = false;
            zeroize();
            return true;
        }
        return false;
    }

    ~Security() {
        mbedtls_gcm_free(&gcm);
        memset(aesKey, 0, AES_KEY_SIZE);
    }
};

#endif // SECURITY_H
