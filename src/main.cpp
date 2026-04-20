#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Adafruit_DRV2605.h>
#include <Audio.h>
#include <SPIFFS.h>

// --- Pins ---
#define SENSOR_SDA 8
#define SENSOR_SCL 9
#define LCD_SDA 17
#define LCD_SCL 18
#define SPI_SCK 12
#define SPI_MOSI 11
#define TFT_CS 10
#define TFT_DC 13
#define TFT_RST 14
#define BUTTON_PIN 5 
#define LED_PIN 4    

// --- Audio & Haptic Pins ---
#define I2S_BCLK 1
#define I2S_LRC 2
#define I2S_DIN 3
#define HAPTIC_IN 6 

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Adafruit_DRV2605 drv;
Audio audio;

// --- State Variables ---
int pressCount = 0;
bool lastButtonState = HIGH;
unsigned long lastTelemetryTime = 0;
unsigned long lastTFTFrameTime = 0;
unsigned long lastScrollTime = 0;
int scrollPos = 0;
String songTitle = "";
bool isMPU = false;
bool ledEnabled = false;
bool scrollingEnabled = false;

// --- Audio Callbacks ---
void audio_id3data(const char *info) {
  String s = String(info);
  if (s.startsWith("Title: ")) {
    songTitle = "  " + s.substring(7) + "   ";
    scrollPos = 0;
    scrollingEnabled = true;
  }
}

void audio_showstreamtitle(const char *info) {
  songTitle = "  " + String(info) + "   ";
  scrollPos = 0;
  scrollingEnabled = true;
}

void updateLCDScroll() {
  if (!scrollingEnabled || songTitle == "") return;
  if (millis() - lastScrollTime < 300) return; 
  lastScrollTime = millis();

  lcd.setCursor(0, 0);
  String out = "";
  for (int i=0; i<16; i++) {
    int idx = (scrollPos + i) % songTitle.length();
    out += songTitle[idx];
  }
  lcd.print(out);
  scrollPos++;
}

float smoothAX = 0, smoothAY = 0;
const float filterStrength = 0.05; 

int drawX = 120, drawY = 120;
uint16_t bgColor = GC9A01A_BLACK;

const char* ssid = "adamisphone";
const char* password = "12345678";
bool wifiConnected = false;

void initSensor() {
  Wire1.beginTransmission(0x68);
  Wire1.write(0x6B); Wire1.write(0x00); 
  Wire1.endTransmission();
  Wire1.beginTransmission(0x68);
  Wire1.write(0x10); Wire1.write(0x60); 
  Wire1.endTransmission();
  Wire1.beginTransmission(0x68);
  Wire1.write(0x75);
  Wire1.endTransmission(false);
  Wire1.requestFrom(0x68, 1);
  if (Wire1.read() == 0x68) isMPU = true;
}

void readSensor(float &ax, float &ay, float &az) {
  int16_t rX, rY, rZ;
  Wire1.beginTransmission(0x68);
  Wire1.write(isMPU ? 0x3B : 0x28);
  Wire1.endTransmission(false);
  Wire1.requestFrom(0x68, 6);
  if (Wire1.available() == 6) {
    if (isMPU) {
      rX = (Wire1.read() << 8) | Wire1.read();
      rY = (Wire1.read() << 8) | Wire1.read();
      rZ = (Wire1.read() << 8) | Wire1.read();
      ax = rX/16384.0*9.81; ay = rY/16384.0*9.81; az = rZ/16384.0*9.81;
    } else {
      rX = Wire1.read() | (Wire1.read() << 8);
      rY = Wire1.read() | (Wire1.read() << 8);
      rZ = Wire1.read() | (Wire1.read() << 8);
      ax = rX*0.061/1000.0*9.81; ay = rY*0.061/1000.0*9.81; az = rZ*0.061/1000.0*9.81;
    }
  }
}

void updateTFTBubble(float ax, float ay) {
  smoothAX = (ax * filterStrength) + (smoothAX * (1.0 - filterStrength));
  smoothAY = (ay * filterStrength) + (smoothAY * (1.0 - filterStrength));

  if (millis() - lastTFTFrameTime < 33) return;
  lastTFTFrameTime = millis();

  int newX = 120 + (int)(smoothAX * 9);
  int newY = 120 + (int)(smoothAY * 9);

  float dist = sqrt(sq(newX - 120) + sq(newY - 120));
  if (dist > 85) {
    float angle = atan2(newY - 120, newX - 120);
    newX = 120 + cos(angle) * 85;
    newY = 120 + sin(angle) * 85;
  }

  if (abs(newX - drawX) > 0 || abs(newY - drawY) > 0) {
    tft.fillCircle(drawX, drawY, 15, bgColor); 
    tft.fillCircle(newX, newY, 15, GC9A01A_WHITE); 
    tft.drawFastHLine(115, 120, 10, GC9A01A_DARKGREY);
    tft.drawFastVLine(120, 115, 10, GC9A01A_DARKGREY);
    drawX = newX;
    drawY = newY;
  }
}

void updateLEDBreathe() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, (millis() / 250) % 2);
    return;
  }
  if (ledEnabled) {
    float val = (exp(sin(millis() / 2000.0 * PI)) - 0.36787944) * 108.0;
    analogWrite(LED_PIN, (int)val);
  } else {
    analogWrite(LED_PIN, 0);
  }
}

void triggerHaptic(int strength) {
  int effect = map(strength, 0, 255, 1, 123);
  if (strength > 0) {
    drv.setWaveform(0, effect); 
    drv.setWaveform(1, 0);       
    drv.go();
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize Wire1 for Sensor/Haptic (8, 9)
  Wire1.begin(SENSOR_SDA, SENSOR_SCL);
  initSensor();

  // Initialize Wire for LCD (17, 18) - Library uses 'Wire'
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init(); 
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");

  SPI.begin(SPI_SCK, -1, SPI_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(bgColor);
  tft.drawCircle(120, 120, 118, GC9A01A_WHITE);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  WiFi.begin(ssid, password);

  if (drv.begin(&Wire1)) {
    drv.selectLibrary(1);
    drv.setMode(DRV2605_MODE_INTTRIG); 
  }

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  audio.setVolume(10); 
  audio.setTone(0, 0, 0); 

  if (SPIFFS.begin(true)) {
    Serial.println("{\"log\":\"SPIFFS Mounted\"}");
  }
}

void loop() {
  audio.loop();
  updateLCDScroll();
  float ax=0, ay=0, az=0;
  readSensor(ax, ay, az);
  updateTFTBubble(ax, ay);
  updateLEDBreathe();

  if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    wifiConnected = true;
    lcd.setCursor(0,1);
    lcd.print(WiFi.localIP());
  }

  if (millis() - lastTelemetryTime > 100) {
    lastTelemetryTime = millis();
    bool isPressed = (digitalRead(BUTTON_PIN) == LOW);
    if (lastButtonState == HIGH && isPressed) {
      pressCount++;
      triggerHaptic(150); 
    }
    lastButtonState = isPressed;

    JsonDocument out;
    out["ax"] = ax; out["ay"] = ay; out["az"] = az;
    out["pressed"] = isPressed;
    out["count"] = pressCount;
    serializeJson(out, Serial);
    Serial.println();
  }

  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    JsonDocument in;
    if (deserializeJson(in, input) == DeserializationError::Ok) {
      if (in["lcd1"]) { 
        scrollingEnabled = false;
        lcd.setCursor(0, 0); lcd.print("                "); lcd.setCursor(0, 0); lcd.print(in["lcd1"].as<const char*>()); 
      }
      if (in.containsKey("scroll")) scrollingEnabled = in["scroll"];
      if (in["lcd2"]) { lcd.setCursor(0, 1); lcd.print("                "); lcd.setCursor(0, 1); lcd.print(in["lcd2"].as<const char*>()); }
      if (in.containsKey("led")) ledEnabled = in["led"];
      if (in["tft"]) {
        String col = in["tft"].as<String>();
        if (col == "RED") bgColor = GC9A01A_RED;
        else if (col == "BLUE") bgColor = GC9A01A_BLUE;
        else if (col == "GREEN") bgColor = GC9A01A_GREEN;
        else if (col == "CLEAR") bgColor = GC9A01A_BLACK;
        tft.fillScreen(bgColor);
        tft.drawCircle(120, 120, 118, GC9A01A_WHITE);
      }
      if (in["haptic"]) triggerHaptic(in["haptic"].as<int>());
      if (in["play"]) {
        String p = in["play"].as<String>();
        scrollingEnabled = true;
        if (p == "DOOM") {
          audio.connecttoFS(SPIFFS, "/doomsday.mp3");
        } else {
          audio.connecttohost(p.c_str());
        }
      }
      if (in.containsKey("pause")) {
        audio.pauseResume();
      }
      if (in["vol"]) audio.setVolume(in["vol"].as<int>());
    }
  }
}
