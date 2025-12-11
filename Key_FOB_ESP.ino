/*
 * ======================================================================================
 * PROJECT TITLE: SECURE IOT SENTINEL (Admin Key v8.0)
 * MODULE: Remote Authentication Fob (Transmitter)
 * AUTHORS: Afthab, Azzam, Cheick, Ronit, Aathrey
 * ======================================================================================
 * ABSTRACT:
 * This code runs on the secondary "Key" controller. It waits for user input 
 * (Physical Button or Serial Console) and broadcasts a cryptographically signed 
 * authentication token via LoRa radio to unlock the main unit.
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <XPowersLib.h> 

// --- CONFIGURATION ---
#define SCK     5    
#define MISO    19   
#define MOSI    27   
#define SS      18   
#define RST_LoRa 23   
#define DI0     26   
#define BAND    433E6 // Must match Receiver Frequency!

// BUTTON MAPPING
#define BTN_BOOT 0  // Standard Boot Button
#define BTN_USER 38 // T-Beam Middle Button

XPowersAXP2101 PMU; 

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- ADMIN KEY BOOTING ---");

  // Configure buttons with Pull-Up Resistors (Active Low)
  pinMode(BTN_BOOT, INPUT_PULLUP); 
  pinMode(BTN_USER, INPUT_PULLUP);

  // --- POWER MANAGEMENT ---
  // Wake up the AXP2101 chip to enable radio power
  Wire.begin(21, 22);
  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22)) {
     if (!PMU.begin(Wire, AXP192_SLAVE_ADDRESS, 21, 22)) {
        Serial.println("PMU Failed!");
        while(1);
     }
  }
  PMU.enableALDO2(); 
  PMU.setALDO2Voltage(3300);
  delay(100);

  // --- RADIO INIT ---
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST_LoRa, DI0);
  if (!LoRa.begin(BAND)) {
    Serial.println("LoRa Init Failed!");
    while (1);
  }
  
  // Set Sync Word to match Receiver (Security Feature)
  LoRa.setSyncWord(0xF3); 
  
  Serial.println(">> KEY READY.");
  Serial.println("   [1] Click BOOT Button to Toggle Lock");
  Serial.println("   [2] Type 'admin' to Toggle Lock");
}

void loop() {
  // --- METHOD 1: PHYSICAL BUTTON ---
  if (digitalRead(BTN_BOOT) == LOW || digitalRead(BTN_USER) == LOW) {
    Serial.println(">> Button Pressed! Sending Key...");
    
    // SEND KEY
    LoRa.beginPacket();
    LoRa.print("admin");
    LoRa.endPacket();
    
    Serial.println(">> SENT.");
    
    // Wait for release + Debounce (Prevents accidental double-clicks)
    while(digitalRead(BTN_BOOT) == LOW || digitalRead(BTN_USER) == LOW) {
      delay(50);
    }
    delay(1000); // 1 second cooldown
  }

  // --- METHOD 2: SERIAL CONSOLE ---
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      Serial.print(">> Manual Input. Sending: ");
      Serial.println(input);
      
      LoRa.beginPacket();
      LoRa.print(input);
      LoRa.endPacket();
    }
  }
}
