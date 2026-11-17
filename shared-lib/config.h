#ifndef SHARDGRID_CONFIG_H
#define SHARDGRID_CONFIG_H

// ============================================================================
//  ShardGrid — Global Configuration
// ============================================================================

// --- Optical Link Timing ---
#define BIT_TIME_MS       250     // Duration of one bit (ms) — matches LDR response
#define PREAMBLE_BITS     8       // 0xAA preamble for clock sync
#define STOP_GAP_MS       (BIT_TIME_MS * 3)  // Gap between frames

// --- Frame Constants ---
#define FRAME_PREAMBLE    0xAA
#define FRAME_START       0x7E    // Start-of-frame delimiter
#define FRAME_END         0x7F    // End-of-frame delimiter
#define MAX_PAYLOAD_LEN   128     // Max payload bytes per frame

// --- Message Types ---
#define MSG_TYPE_DATA       0x01  // Encrypted share data
#define MSG_TYPE_ACK        0x02  // Acknowledgement
#define MSG_TYPE_NACK       0x03  // Negative acknowledgement
#define MSG_TYPE_HEARTBEAT  0x04  // Liveness check
#define MSG_TYPE_KEY_EXCH   0x05  // Key exchange
#define MSG_TYPE_TAMPER     0x06  // Tamper alert (broadcast)
#define MSG_TYPE_STATUS     0x07  // Status request/response
#define MSG_TYPE_ZEROIZE    0x08  // Command to destroy keys

// --- Node IDs ---
#define NODE_ID_UNO       0x00
#define NODE_ID_CENTRAL   0x01
#define NODE_ID_A         0x02
#define NODE_ID_B         0x03

// --- Shamir's Secret Sharing ---
#define SSS_THRESHOLD     2       // k = minimum shares to reconstruct
#define SSS_NUM_SHARES    3       // n = total shares (Central keeps 1)
#define SSS_PRIME         251     // Largest prime < 256 for GF(p) arithmetic

// --- Security ---
#define AES_KEY_SIZE      32      // 256 bits
#define AES_IV_SIZE       12      // 96 bits for GCM
#define AES_TAG_SIZE      16      // 128-bit auth tag
#define KEY_WIPE_PASSES   3       // Overwrite count for secure erase

// --- Pin Definitions: Arduino Uno (Sender) ---
#ifdef UNO_SENDER
  #define TX_LED_PIN      8       // LED to Central ESP32
#endif

// --- Pin Definitions: ESP32 Central Node ---
#ifdef CENTRAL_NODE
  #define RX_LDR_PIN      34      // LDR from Arduino Uno
  #define TX_LED_A_PIN    25      // LED to Node A
  #define TX_LED_B_PIN    26      // LED to Node B
  #define TAMPER_PIN      27      // Tamper detection sensor
  #define OLED_SDA        21      // I2C SDA
  #define OLED_SCL        22      // I2C SCL
  #define STATUS_LED_PIN  2       // Built-in LED for status
#endif

// --- Pin Definitions: ESP32 Node A ---
#ifdef NODE_A
  #define RX_LDR_PIN      34      // LDR from Central Node
  #define TAMPER_PIN      27      // Tamper detection sensor
  #define OLED_SDA        21
  #define OLED_SCL        22
  #define STATUS_LED_PIN  2
#endif

// --- Pin Definitions: ESP32 Node B ---
#ifdef NODE_B
  #define RX_LDR_PIN      34      // LDR from Central Node
  #define TAMPER_PIN      27      // Tamper detection sensor
  #define OLED_SDA        21
  #define OLED_SCL        22
  #define STATUS_LED_PIN  2
#endif

// --- Tamper Detection ---
#define TAMPER_DEBOUNCE_MS   50   // Debounce time for tamper switch
#define HEARTBEAT_INTERVAL   5000 // ms between heartbeat checks
#define HEARTBEAT_TIMEOUT    15000 // ms before declaring node dead

// --- OLED ---
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_ADDR       0x3C

#endif // SHARDGRID_CONFIG_H
