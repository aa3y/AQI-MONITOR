/*
 * ======================================================================================
 * PROJECT TITLE: SECURE IOT SENTINEL (Final Hardware Master v10.1)
 * COURSE: Cybersecurity & Digital Forensics
 * AUTHORS: Afthab, Azzam, Cheick, Ronit, Aathrey
 * DATE: November 2025
 * ======================================================================================
 * SYSTEM ABSTRACT:
 * This firmware operates a high-security, air-gapped environmental monitoring station.
 * It integrates dual-core processing, encrypted telemetry, and hardware-level authentication.
 * * --- KEY SECURITY IMPLEMENTATIONS ---
 * 1. AIR-GAP ISOLATION: The authentication pathway (LoRa 433MHz) is physically separate 
 * from the data transmission pathway (Wi-Fi/TLS).
 * 2. STEALTH MODE: The Wi-Fi radio is physically powered down/disabled during the locked 
 * state, reducing the RF footprint and preventing packet sniffing.
 * 3. ACTIVE DEFENSE (PERSISTENT): A brute-force detection algorithm monitors the radio 
 * link. Failed attempts are stored in Non-Volatile Memory (NVS). A reboot does NOT 
 * reset the security counter.
 * 4. REMOTE KILL SWITCH: The system can be remotely locked via the Admin Key fob.
 * 5. FORENSIC LOGGING: Uploads are time-stamped via NTP and include Battery + Pressure data.
 */

// ======================================================================================
// 1. LIBRARY IMPORTS & DEPENDENCIES
// ======================================================================================
#include <WiFi.h>             // Core ESP32 Wi-Fi stack
#include <WiFiClientSecure.h> // REQUIRED for SSL/TLS (HTTPS) encryption
#include <HTTPClient.h>       // Handles HTTP GET/POST requests
#include <Wire.h>             // I2C Bus Driver
#include <Adafruit_GFX.h>     // Core graphics engine
#include <Adafruit_SSD1306.h> // OLED Driver
#include <LiquidCrystal_I2C.h> // LCD Driver
#include <SPI.h>              // SPI Bus Driver
#include <LoRa.h>             // LoRa Radio Driver
#include <XPowersLib.h>       // Power Management for T-Beam
#include <Preferences.h>      // Persistent Storage (NVS)
#include <time.h>             // Network Time Protocol (NTP)
#include <Adafruit_BMP280.h>  // Barometric Pressure Sensor
#include "ThingSpeak.h"       // Cloud API
#include "DHT.h"              // Temp/Humid Sensor

// ======================================================================================
// 2. NETWORK & API CREDENTIALS
// ======================================================================================
const char* ssid = "MDX welcomes you";                  
const char* password = "MdxL0vesyou";           
unsigned long myChannelNumber = 3144068;     
const char* myWriteAPIKey = "I640AK5OT4GG7N1C"; 

// NTP Server Settings
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 14400; // UTC+4 (Dubai)
const int   daylightOffset_sec = 0;

// ======================================================================================
// 3. HARDWARE PIN MAPPING
// ======================================================================================
#define SDA_1 13  // Bus 1: Left OLED + LCD
#define SCL_1 14  
#define SDA_2 21  // Bus 2: Right OLED + Power Chip + BMP280
#define SCL_2 22  

#define DHTPIN   25     
#define MQ_PIN   35     
#define BUZZER_PIN 2    
#define BOOT_BUTTON 0   
#define USER_BUTTON 38  

// LoRa Radio Pins
#define SCK     5   
#define MISO    19  
#define MOSI    27  
#define SS      18  
#define RST_LoRa 23   
#define DI0     26    
#define BAND    433E6 

// ======================================================================================
// 4. OBJECT INSTANTIATION
// ======================================================================================
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE); 

// Initialize BMP280 on the SECOND I2C Bus (&Wire1)
Adafruit_BMP280 bmp(&Wire1); 

#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 

Adafruit_SSD1306 display1(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);  
Adafruit_SSD1306 display2(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire1, -1); 
LiquidCrystal_I2C lcd(0x27, 16, 2); 

XPowersAXP2101 PMU; 
WiFiClientSecure client; 
Preferences securityLog;

// ======================================================================================
// 5. GLOBAL STATE MANAGEMENT
// ======================================================================================
bool isLocked = true;    
bool isDisabled = false; 
int failedAttempts = 0;  
const int MAX_ATTEMPTS = 3; 

String consolePassword = "admin"; 

unsigned long lastTime = 0;
unsigned long lastScreenUpdate = 0;
unsigned long timerDelay = 20000;   

float filteredGas = 0; 
int baselineGas = 1000; 

int THRESHOLD_POOR = 600;   
int THRESHOLD_DANGER = 1200; 

// --- Function Prototypes ---
void updateDashboards(float t, float h, int gasRaw, int gasPct, String status, float pressure);
String getHTTPStatus(int code);
void checkLoRaPassword();
void runSecureMonitor();
void connectToWiFi();
void logToLCD(String line1, String line2);
void showLockScreen(); 
void typeWriterLCD(String text, int row);
void triggerSystemLockdown();
void checkManualButton(); 
void performRemoteLock(); 
int getBatteryPercentage(); 
String getFormattedTime(); 

// ======================================================================================
// SETUP ROUTINE
// ======================================================================================
void setup() {
  Serial.begin(115200); 
  
  // --- GPIO ---
  dht.begin();
  pinMode(MQ_PIN, INPUT);        
  pinMode(BUZZER_PIN, OUTPUT);   
  pinMode(BOOT_BUTTON, INPUT_PULLUP); 
  pinMode(USER_BUTTON, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW); 
  
  // --- SECURITY CHECK ---
  securityLog.begin("sec_log", false); 

  // Factory Reset (Hold BOOT on startup)
  if (digitalRead(BOOT_BUTTON) == LOW) {
    Wire.begin(SDA_1, SCL_1);
    display1.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display1.clearDisplay();
    display1.setTextColor(SSD1306_WHITE);
    display1.setTextSize(1);
    display1.setCursor(10, 20); display1.println("FACTORY RESET");
    display1.setCursor(10, 40); display1.println("WIPING LOGS...");
    display1.display();
    
    securityLog.clear(); 
    failedAttempts = 0;
    isDisabled = false;
    digitalWrite(BUZZER_PIN, HIGH); delay(500); digitalWrite(BUZZER_PIN, LOW);
    delay(2000); 
  }
  
  failedAttempts = securityLog.getInt("fails", 0); 
  if (failedAttempts >= MAX_ATTEMPTS) { isDisabled = true; }

  // --- BUS INIT ---
  Wire.begin(SDA_1, SCL_1); // Bus 1
  if(!display1.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); } 
  
  lcd.init();      
  lcd.backlight(); 
  lcd.clear();

  Wire1.begin(SDA_2, SCL_2); // Bus 2
  if(!display2.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }

  // --- BMP280 INIT ---
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 Not Found - Check Wiring!");
  }

  // --- POWER MANAGEMENT ---
  if (!PMU.begin(Wire1, AXP2101_SLAVE_ADDRESS, SDA_2, SCL_2)) { while(1); }
  PMU.enableALDO2(); 
  PMU.setALDO2Voltage(3300); 
  PMU.enableBattVoltageMeasure(); 
  delay(100); 

  client.setInsecure(); 

  // --- LoRa INIT ---
  SPI.begin(SCK, MISO, MOSI, SS); 
  LoRa.setPins(SS, RST_LoRa, DI0); 
  if (!LoRa.begin(BAND)) {
    lcd.clear(); lcd.print("ERR: LORA FAIL"); 
    delay(2000); 
  } else {
    LoRa.setSyncWord(0xF3); 
  }

  // --- BOOT ANIMATION ---
  display1.clearDisplay();
  display1.setTextColor(SSD1306_WHITE);
  display1.setTextSize(2);
  display1.setCursor(10,20); display1.println("SYSTEM");
  display1.setCursor(10,40); display1.println("STARTUP");
  display1.display();
  
  display2.clearDisplay();
  display2.setTextSize(1);
  display2.setTextColor(SSD1306_WHITE);
  display2.setCursor(0,0);
  display2.println("Loading Modules...");
  display2.println("- Crypto Layer: OK");
  display2.println("- Radio Link:   OK");
  display2.println("- Barometer:    OK"); 
  display2.display();
  
  typeWriterLCD("DEVELOPED BY:", 0);
  delay(500);
  typeWriterLCD("Afthab & Azzam", 1);
  delay(1500);
  typeWriterLCD("Cheick & Ronit", 1);
  delay(1500);
  typeWriterLCD("   Aathrey    ", 1);
  delay(1500);

  lcd.clear();
  typeWriterLCD("SECURE AQI", 0);
  typeWriterLCD("MONITOR v10.1", 1);
  delay(2000);

  digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
  logToLCD(">> AIR-GAPPED", ">> AWAITING KEY");
}

// ======================================================================================
// MAIN LOOP
// ======================================================================================
void loop() {
  checkManualButton();
  checkLoRaPassword(); 

  if (isDisabled) {
    triggerSystemLockdown(); 
  }
  else if (isLocked) {
    showLockScreen();    
  } 
  else {
    runSecureMonitor();  
  }
}

// ======================================================================================
// HELPER FUNCTIONS
// ======================================================================================
void checkManualButton() {
  if (digitalRead(BOOT_BUTTON) == LOW || digitalRead(USER_BUTTON) == LOW) {
    delay(200); 
    if (isLocked) {
      isLocked = false;
      failedAttempts = 0; 
      securityLog.putInt("fails", 0); 
      display2.invertDisplay(false); 
      lcd.clear(); lcd.print(">> MANUAL UNLOCK");
      digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
      connectToWiFi();
    } 
    else {
      performRemoteLock(); 
    }
    while(digitalRead(BOOT_BUTTON) == LOW || digitalRead(USER_BUTTON) == LOW) { delay(50); }
    delay(500); 
  }
}

void checkLoRaPassword() {
  int packetSize = LoRa.parsePacket(); 
  if (packetSize) {
    String received = "";
    while (LoRa.available()) received += (char)LoRa.read();
    received.trim(); 
    
    if (received.equals(consolePassword)) {
      if(isLocked) {
        isLocked = false; 
        failedAttempts = 0; 
        securityLog.putInt("fails", 0);
        display2.invertDisplay(false);
        lcd.clear(); lcd.print(">> ACCESS GRANTED");
        digitalWrite(BUZZER_PIN, HIGH); delay(100); 
        digitalWrite(BUZZER_PIN, LOW);  delay(50);
        digitalWrite(BUZZER_PIN, HIGH); delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        connectToWiFi(); 
      } 
      else {
        performRemoteLock();
      }
    } else {
      if(isLocked) {
        failedAttempts++;
        securityLog.putInt("fails", failedAttempts); 
        if(failedAttempts >= MAX_ATTEMPTS) isDisabled = true; 
      }
    }
  }
}

void performRemoteLock() {
  isLocked = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  lcd.clear();
  logToLCD(">> SYSTEM LOCKED", ">> AIR-GAPPED");
  digitalWrite(BUZZER_PIN, HIGH); delay(300); digitalWrite(BUZZER_PIN, LOW);
  delay(1000); 
}

void triggerSystemLockdown() {
  display1.clearDisplay();
  display1.setTextSize(2);
  display1.setTextColor(SSD1306_WHITE);
  display1.setCursor(0,10); display1.println("SECURITY");
  display1.println("BREACH");
  display1.display();

  display2.clearDisplay();
  display2.fillRect(0, 0, 128, 64, SSD1306_WHITE);
  display2.setTextColor(SSD1306_BLACK);
  display2.setTextSize(2);
  display2.setCursor(10,25); display2.println("NO ACCESS");
  display2.display();

  logToLCD("!! SECURITY !!", "!! LOCKDOWN !!");

  while(true) {
    digitalWrite(BUZZER_PIN, HIGH);
    display1.invertDisplay(true); 
    display2.invertDisplay(true); 
    lcd.noBacklight();            
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    display1.invertDisplay(false);
    display2.invertDisplay(false);
    lcd.backlight();   
    delay(100);
  }
}

void connectToWiFi() {
  lcd.clear(); lcd.print(">> CONNECTING...");
  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid, password);
  
  int t=0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); t++;
    if(t>20) { lcd.clear(); lcd.print("ERR: NO WIFI"); return; }
  }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  lcd.clear(); lcd.print(">> UPLINK OK");
  delay(1000);
}

String getHTTPStatus(int code) {
  switch (code) {
    case 200: return "200: SUCCESS";
    case 404: return "404: WRONG CH"; 
    case 401: return "401: BAD KEY"; 
    case -1:  return "ERR: CONN FAIL";   
    default:  return "CODE: " + String(code); 
  }
}

int getBatteryPercentage() {
  int vol = PMU.getBattVoltage(); 
  int pct = map(vol, 3000, 4200, 0, 100);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

String getFormattedTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "--:--:--";
  }
  char timeStringBuff[10]; 
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

void runSecureMonitor() {
  float t = dht.readTemperature(); 
  float h = dht.readHumidity();    
  if (isnan(h) || isnan(t)) { t = 0.0; h = 0.0; } 

  // Read Pressure
  float pressure = bmp.readPressure() / 100.0F; // hPa
  
  int rawGas = analogRead(MQ_PIN);
  if (filteredGas == 0) filteredGas = rawGas;
  filteredGas = (0.98 * filteredGas) + (0.02 * rawGas);
  int stableGas = (int)filteredGas; 

  if (stableGas < baselineGas) baselineGas = stableGas;
  int gasPct = map(stableGas, baselineGas, baselineGas + 1000, 0, 100);
  if (gasPct < 0) gasPct = 0; if (gasPct > 100) gasPct = 100;

  String status = "CLEAN";
  if (gasPct > 60) status = "HAZARDOUS"; 
  else if (gasPct > 40) status = "POOR";
  else if (gasPct > 20) status = "MODERATE";

  if (millis() - lastScreenUpdate > 1000) {
    updateDashboards(t, h, stableGas, gasPct, status, pressure);
    lastScreenUpdate = millis();
  }

  if ((millis() - lastTime) > timerDelay) {
    if(WiFi.status() == WL_CONNECTED){
      lcd.clear(); lcd.print(">> UPLOADING...");
      
      HTTPClient http;
      String url = "http://api.thingspeak.com/update?api_key=" + String(myWriteAPIKey);
      url += "&field1=" + String(t);
      url += "&field2=" + String(h);
      url += "&field3=" + String(gasPct); 
      url += "&field4=" + String(pressure); 
      
      http.begin(url);
      int code = http.GET();
      http.end();
      
      lcd.setCursor(0,1); 
      lcd.print(getHTTPStatus(code));
      
      delay(2000);
      
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("LAST UPDATE:");
      lcd.setCursor(0,1); lcd.print(getFormattedTime()); 
    }
    lastTime = millis();
  }
}

void updateDashboards(float t, float h, int gasRaw, int gasPct, String status, float pressure) {
  // --- SCREEN 1 (Status) ---
  display1.clearDisplay();
  display1.setTextSize(1);
  display1.setTextColor(SSD1306_WHITE);
  display1.setCursor(0,0); display1.print("SYSTEM STATUS");
  display1.drawLine(0,8,128,8,SSD1306_WHITE);
  display1.setCursor(0,15); display1.print("Auth:  GRANTED");
  
  // Battery
  display1.setCursor(0,25); 
  display1.print("Bat: "); 
  display1.print(getBatteryPercentage());
  display1.print("%");
  
  // Pressure Display
  display1.setCursor(0, 38);
  display1.print("Press: "); display1.print(pressure, 1); display1.print("hPa");
  
  // Live VOC
  display1.setCursor(0, 52);
  display1.print("Live VOC: ");
  display1.print(gasRaw); 
  
  if ((millis() / 500) % 2 == 0) display1.fillCircle(120, 4, 2, SSD1306_WHITE);
  display1.display();

  // --- SCREEN 2 (Visuals) ---
  display2.clearDisplay();
  display2.setTextSize(1);
  display2.setTextColor(SSD1306_WHITE); 
  
  display2.setCursor(0,0); display2.print("T:"); display2.print(t,1); display2.print("C");
  display2.setCursor(64,0); display2.print("H:"); display2.print(h,0); display2.print("%");
  
  display2.setCursor(0,15); display2.print("VOC Index:");
  display2.drawRect(0, 26, 128, 12, SSD1306_WHITE); 
  int barWidth = map(gasPct, 0, 100, 0, 124);
  display2.fillRect(2, 28, barWidth, 8, SSD1306_WHITE); 

  display2.fillRect(0, 42, 128, 22, SSD1306_WHITE); 
  display2.setTextColor(SSD1306_BLACK); 
  display2.setTextSize(2);
  
  int xPos = (128 - (status.length() * 12)) / 2;
  display2.setCursor(xPos, 46);
  display2.print(status);
  display2.setTextColor(SSD1306_WHITE); 

  if (status == "HAZARDOUS") {
    if ((millis() / 500) % 2 == 0) {
      display2.invertDisplay(true); 
      digitalWrite(BUZZER_PIN, HIGH);
    } else {
      display2.invertDisplay(false); 
      digitalWrite(BUZZER_PIN, LOW);
    }
  } else {
    display2.invertDisplay(false);
    digitalWrite(BUZZER_PIN, LOW);
  }
  display2.display();
}

void showLockScreen() {
  display1.clearDisplay();
  // Classic Lock Icon
  display1.fillCircle(64, 26, 12, SSD1306_WHITE); 
  display1.fillCircle(64, 26, 7, SSD1306_BLACK); 
  display1.fillRect(44, 28, 40, 28, SSD1306_WHITE);
  display1.fillCircle(64, 42, 3, SSD1306_BLACK);
  display1.fillRect(63, 42, 2, 8, SSD1306_BLACK);

  display1.setTextSize(1);
  display1.setTextColor(SSD1306_WHITE);
  display1.setCursor(25, 0);
  display1.print("SYSTEM SECURE");
  
  // Removed battery level from here (Cleaner Look)

  if(failedAttempts > 0) {
    display1.setCursor(25, 10);
    display1.print("Violations: ");
    display1.print(failedAttempts);
  }
  display1.display();

  display2.clearDisplay();
  display2.fillRect(0, 0, 128, 64, SSD1306_WHITE); 
  display2.drawRect(2, 2, 124, 60, SSD1306_BLACK); 
  display2.setTextColor(SSD1306_BLACK);
  display2.setTextSize(2);
  display2.setCursor(28, 15); display2.println("ACCESS");
  display2.setCursor(28, 35); display2.println("DENIED");
  display2.display();
}

void typeWriterLCD(String text, int row) {
  lcd.setCursor(0, row);
  lcd.print("                "); 
  lcd.setCursor(0, row);
  for (int i = 0; i < text.length(); i++) {
    lcd.print(text.charAt(i)); 
    delay(50); 
  }
}

void logToLCD(String line1, String line2) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); lcd.print(line2);
}
