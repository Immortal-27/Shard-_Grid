#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Receiver Configuration
const int LDR_PIN = 34;       // LDR Digital Out (DO)
const int BIT_TIME = 250;     // Must match sender's BIT_TIME (250ms)

// SET TO TRUE if your LDR module outputs LOW when light hits it, FALSE otherwise
const bool LDR_INVERTED = true; 

String currentMessage = "";
unsigned long lastMessageTime = 0;
const unsigned long CLEAR_DELAY = 15000; // 15 seconds in milliseconds

// Helper function to read digital pin cleanly based on module inversion
bool readLDR() {
  int val = digitalRead(LDR_PIN);
  return LDR_INVERTED ? !val : val;
}

void setup() {
  Serial.begin(115200);
  pinMode(LDR_PIN, INPUT);

  // Initialize OLED display (I2C address 0x3C)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Optical Receiver");
  display.println("Ready for data...");
  display.display();
}

void loop() {
  // Check if 15 seconds have passed since the last message was displayed
  if (currentMessage.length() > 0 && (millis() - lastMessageTime >= CLEAR_DELAY)) {
    currentMessage = "";
    
    // Reset OLED Display to idle state
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Optical Receiver");
    display.println("Ready for data...");
    display.display();
  }

  // Wait for Start Bit (Active state) from the sender
  if (readLDR() == true) {
    
    // Jump to the middle of the first data bit (Bit 7)
    // Start bit (250ms) + half bit (125ms) = 375ms
    delay(BIT_TIME + (BIT_TIME / 2)); 
    
    char c = 0;
    
    // Read 8 data bits (MSB first)
    for (int i = 7; i >= 0; i--) {
      if (readLDR() == true) {
        c |= (1 << i);
      }
      delay(BIT_TIME); // Wait for the next bit center
    }

    // Debug print
    Serial.print("Decoded Char: ");
    Serial.println(c);

    // Handle character appending or message reset
    if (c == '\n' || c == '\r') {
      currentMessage = ""; 
    } else {
      currentMessage += c;
      
      // Prevent text overflow on the 128x64 display
      if (currentMessage.length() > 20) {
        currentMessage = String(c); 
      }
    }

    // Update the timestamp whenever a new character is added/displayed
    lastMessageTime = millis();

    // Update OLED Screen
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Received Message:");
    
    display.setTextSize(2);
    display.setCursor(0, 20);
    display.println(currentMessage);
    
    display.display();

    // Wait out the stop bit and inter-character gap before listening again
    delay(BIT_TIME * 2);
  }
}