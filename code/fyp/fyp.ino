#include <Wire.h>
#include "Adafruit_SGP30.h"
#include "Adafruit_PM25AQI.h"
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>

// WiFi settings
const char* ssid            = "vivo50";
const char* password        = "bk666666";
const char* thingSpeakURL   = "http://api.thingspeak.com/update";
const char* writeAPIKey     = "HWLDOM7OUII2PRSC";

// Pin definitions
const int FAN_HIGH_PIN  = 18;
const int FAN_LOW_PIN   = 19;
const int BUTTON_PIN    = 4;
#define NEO_TVOC_PIN 25
#define NEO_PM25_PIN 26
#define NEO_COUNT    8

Adafruit_NeoPixel stripTVOC(NEO_COUNT, NEO_TVOC_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripPM25(NEO_COUNT, NEO_PM25_PIN, NEO_GRB + NEO_KHZ800);

// Debounce parameters
int lastButtonStable  = HIGH, lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// Sensor objects and thresholds
Adafruit_SGP30 sgp;
Adafruit_PM25AQI aqi;
uint16_t lastPM25 = 0;
int pm25FailCount = 0;
const int PM25_MAX_RETRIES = 5;
const uint16_t TVOC_LOW  = 300, TVOC_MED  = 600, TVOC_HIGH  = 1000;
const uint16_t PM25_LOW  = 35,  PM25_MED  = 75,  PM25_HIGH  = 150;

// Timing parameters
unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 2000;
unsigned long lastUploadTime  = 0;
const unsigned long uploadInterval = 15000;

// Accumulators for averaging
unsigned long tvocSum   = 0;
unsigned long pm25Sum   = 0;
unsigned int  sampleCount = 0;
uint16_t currentTVOC = 0;
uint16_t currentPM25 = 0;

// Boot animation parameters
const unsigned long totalPreheat  = 30000;
const int flashCycles             = 3;
const unsigned long flashInterval = 400;
const unsigned long flashTotal    = flashCycles * 3 * flashInterval;
const unsigned long stepInterval  = (totalPreheat - flashTotal) / NEO_COUNT;

enum State { OFF, BOOTING, RUNNING };
State state = OFF;
int preheatStepIndex          = 0;
unsigned long preheatLastTime = 0;
int flashCount                = 0;
bool flashOn                  = false;
unsigned long flashLastTime   = 0;

bool wifiConnected = false;

// Function prototypes
bool connectWiFi();
bool bootAnimation();
void handleButton();
void measureAndControl();
void uploadToThingSpeak(uint16_t avgTVOC, uint16_t avgPM25);
int calcAQLevel(uint16_t value, uint16_t low, uint16_t med, uint16_t high);
void updateLEDStrip(Adafruit_NeoPixel &strip, int level);

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("=== Air Purifier Initialization ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lastButtonReading = digitalRead(BUTTON_PIN);
  lastButtonStable  = lastButtonReading;

  pinMode(FAN_HIGH_PIN, OUTPUT);
  pinMode(FAN_LOW_PIN, OUTPUT);
  digitalWrite(FAN_HIGH_PIN, LOW);
  digitalWrite(FAN_LOW_PIN, LOW);

  stripTVOC.begin(); stripTVOC.setBrightness(50); stripTVOC.show();
  stripPM25.begin(); stripPM25.setBrightness(50); stripPM25.show();

  Wire.begin(21, 22);
  if (!sgp.begin()) { Serial.println("Error: SGP30 not detected"); while (1); }
  sgp.IAQinit();

  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  if (!aqi.begin_UART(&Serial2)) { Serial.println("Error: PMS5003 not detected"); while (1); }

  Serial.println("Press button to start");
}

void loop() {
  handleButton();

  if (state == BOOTING) {
    if (bootAnimation()) {
      state = RUNNING;
      Serial.println("Boot complete, system running");
      lastMeasureTime = millis();
      tvocSum = pm25Sum = 0;
      sampleCount = 0;
      lastUploadTime = millis();
    }
  }
  else if (state == RUNNING) {
    unsigned long now = millis();
    if (now - lastMeasureTime >= measureInterval) {
      lastMeasureTime = now;
      measureAndControl();
      tvocSum   += currentTVOC;
      pm25Sum   += currentPM25;
      sampleCount++;
    }
    if (wifiConnected && now - lastUploadTime >= uploadInterval) {
      lastUploadTime = now;
      uint16_t avgTVOC = sampleCount ? tvocSum / sampleCount : currentTVOC;
      uint16_t avgPM25 = sampleCount ? pm25Sum / sampleCount : currentPM25;
      uploadToThingSpeak(avgTVOC, avgPM25);
      tvocSum = pm25Sum = 0;
      sampleCount = 0;
    }
  }
}

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > debounceDelay) {
    if (reading != lastButtonStable) {
      lastButtonStable = reading;
      if (reading == LOW) {
        if (state == OFF) {
          Serial.println("Button pressed: starting boot sequence");
          state = BOOTING;
          preheatStepIndex  = 0;
          preheatLastTime   = millis();
          flashCount        = 0;
          flashOn           = false;
          wifiConnected     = connectWiFi();
        } else {
          Serial.println("Button pressed: shutting down");
          state = OFF;
          digitalWrite(FAN_HIGH_PIN, LOW);
          digitalWrite(FAN_LOW_PIN,  LOW);
          stripTVOC.clear(); stripTVOC.show();
          stripPM25.clear(); stripPM25.show();
        }
      }
    }
  }
  lastButtonReading = reading;
}

bool bootAnimation() {
  handleButton();
  unsigned long now = millis();
  if (preheatStepIndex < NEO_COUNT) {
    if (now - preheatLastTime >= stepInterval) {
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t color = (i <= preheatStepIndex) ? stripTVOC.Color(0,0,255) : 0;
        stripTVOC.setPixelColor(i, color);
        stripPM25.setPixelColor(i, color);
      }
      stripTVOC.show(); stripPM25.show();
      preheatStepIndex++;
      preheatLastTime = now;
    }
    return false;
  } else if (flashCount < flashCycles*2) {
    if (now - flashLastTime >= flashInterval) {
      flashOn = !flashOn;
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t color = flashOn ? stripTVOC.Color(0,0,255) : 0;
        stripTVOC.setPixelColor(i, color);
        stripPM25.setPixelColor(i, color);
      }
      stripTVOC.show(); stripPM25.show();
      flashCount++;
      flashLastTime = now;
    }
    return false;
  }
  stripTVOC.clear(); stripTVOC.show();
  stripPM25.clear(); stripPM25.show();
  return true;
}

void measureAndControl() {
  if (sgp.IAQmeasure()) currentTVOC = sgp.TVOC;
  PM25_AQI_Data d;
  if (aqi.read(&d)) {
    currentPM25 = d.pm25_standard;
    lastPM25    = currentPM25;
    pm25FailCount = 0;
  } else if (++pm25FailCount >= PM25_MAX_RETRIES) {
    Serial2.begin(9600, SERIAL_8N1, 16, 17);
    if (aqi.begin_UART(&Serial2)) pm25FailCount = 0;
  }
  Serial.printf("TVOC=%d ppb  PM2.5=%d µg/m³\n", currentTVOC, currentPM25);
  int lvlTV = calcAQLevel(currentTVOC, TVOC_LOW, TVOC_MED, TVOC_HIGH);
  int lvlPM = calcAQLevel(currentPM25, PM25_LOW, PM25_MED, PM25_HIGH);
  if (lvlTV>2||lvlPM>2)      digitalWrite(FAN_HIGH_PIN,HIGH),digitalWrite(FAN_LOW_PIN,HIGH);
  else if (lvlTV>1||lvlPM>1) digitalWrite(FAN_HIGH_PIN,HIGH),digitalWrite(FAN_LOW_PIN,LOW);
  else if (lvlTV>0||lvlPM>0) digitalWrite(FAN_HIGH_PIN,LOW), digitalWrite(FAN_LOW_PIN,HIGH);
  else                        digitalWrite(FAN_HIGH_PIN,LOW), digitalWrite(FAN_LOW_PIN,LOW);
  updateLEDStrip(stripTVOC, lvlTV);
  updateLEDStrip(stripPM25, lvlPM);
}

bool connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-start<5000) delay(100);
  if (WiFi.status()==WL_CONNECTED) {
    Serial.printf("Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi connection failed, entering offline mode");
  return false;
}

void uploadToThingSpeak(uint16_t avgTVOC, uint16_t avgPM25) {
  if (!wifiConnected) return;
  HTTPClient http;
  http.begin(thingSpeakURL);
  http.addHeader("Content-Type","application/x-www-form-urlencoded");
  String payload = "api_key="+String(writeAPIKey)
                   +"&field1="+String(avgTVOC)
                   +"&field2="+String(avgPM25);
  int code = http.POST(payload);
  if (code>0) Serial.printf("Upload OK, HTTP code: %d\n", code);
  else       Serial.printf("Upload failed, code: %d\n", code);
  http.end();
}

int calcAQLevel(uint16_t v,uint16_t low,uint16_t med,uint16_t high) {
  if (v>=high) return 3;
  if (v>=med)  return 2;
  if (v>=low)  return 1;
  return 0;
}

void updateLEDStrip(Adafruit_NeoPixel &strip,int level) {
  int cnt; uint32_t col;
  switch(level) {
    case 0: cnt=2; col=strip.Color(0,255,0); break;
    case 1: cnt=4; col=strip.Color(255,255,0); break;
    case 2: cnt=4; col=strip.Color(255,165,0); break;
    default:cnt=8; col=strip.Color(255,0,0);   break;
  }
  for(int i=0;i<NEO_COUNT;i++) strip.setPixelColor(i, i<cnt?col:0);
  strip.show();
}
