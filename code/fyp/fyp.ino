#include <Wire.h>
#include "Adafruit_SGP30.h"
#include "Adafruit_PM25AQI.h"
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>

// —— WiFi 配置 —— 
const char* ssid = "vivo50";
const char* password = "bk666666";
const char* thingSpeakURL = "http://api.thingspeak.com/update";
const char* writeAPIKey = "HWLDOM7OUII2PRSC";

// —— 引脚定义 —— 
const int FAN12_PIN = 18;
const int FAN3_PIN = 19;
const int BUTTON_PIN = 4;
#define NEO_TVOC_PIN 25
#define NEO_PM25_PIN 26
#define NEO_COUNT 8

Adafruit_NeoPixel stripTVOC(NEO_COUNT, NEO_TVOC_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripPM25(NEO_COUNT, NEO_PM25_PIN, NEO_GRB + NEO_KHZ800);

int lastButtonStable = HIGH, lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

Adafruit_SGP30 sgp;
Adafruit_PM25AQI aqi;
uint16_t lastPM25 = 0;
int pm25FailCount = 0;
const int PM25_MAX_RETRIES = 5;

const uint16_t TVOC_LOW = 300, TVOC_MED = 600, TVOC_HIGH = 1000;
const uint16_t PM25_LOW = 35, PM25_MED = 75, PM25_HIGH = 150;

unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 2000;
unsigned long lastUploadTime = 0;
const unsigned long uploadInterval = 30000;

const unsigned long totalPreheat = 30000;
const int flashCycles = 3;
const unsigned long flashInterval = 400;
const unsigned long flashTotal = flashCycles * 3 * flashInterval;
const unsigned long stepInterval = (totalPreheat - flashTotal) / NEO_COUNT;

enum State { OFF, PREHEAT, RUNNING };
State state = OFF;

int preheatStepIndex = 0;
unsigned long preheatLastTime = 0;
int flashCount = 0;
bool flashOn = false;
unsigned long flashLastTime = 0;
bool wifiConnected = false;

uint16_t currentTVOC = 0;
uint16_t currentPM25 = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n=== 智能净化器初始化 ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lastButtonReading = digitalRead(BUTTON_PIN);
  lastButtonStable = lastButtonReading;

  pinMode(FAN12_PIN, OUTPUT);
  pinMode(FAN3_PIN, OUTPUT);
  digitalWrite(FAN12_PIN, LOW);
  digitalWrite(FAN3_PIN, LOW);

  stripTVOC.begin(); stripTVOC.show();
  stripPM25.begin(); stripPM25.show();

  Wire.begin(21, 22);
  if (!sgp.begin()) { Serial.println("SGP30 未连接"); while (1); }
  sgp.IAQinit();
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  if (!aqi.begin_UART(&Serial2)) { Serial.println("PMS5003 未连接"); while (1); }
  Serial.println("请按按钮开机");
}

void loop() {
  handleButton();
  if (state == PREHEAT) {
    if (preheatStep()) {
      state = RUNNING;
      Serial.println("\n预热完成，进入运行");
      lastMeasureTime = millis();
    }
  } else if (state == RUNNING) {
    if (millis() - lastMeasureTime >= measureInterval) {
      lastMeasureTime = millis();
      measureAndControl();
    }
    if (wifiConnected && millis() - lastUploadTime >= uploadInterval) {
      lastUploadTime = millis();
      uploadToThingSpeak();
    }
  }
}

void handleButton() {
  int r = digitalRead(BUTTON_PIN);
  if (r != lastButtonReading) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > debounceDelay) {
    if (r != lastButtonStable) {
      lastButtonStable = r;
      if (r == LOW) {
        if (state == OFF) {
          Serial.println("\n按钮：开机，开始预热");
          state = PREHEAT;
          preheatStepIndex = 0;
          preheatLastTime = millis();
          flashCount = 0;
          flashOn = false;
          wifiConnected = connectWiFi();
        } else {
          Serial.println("\n按钮：关机");
          state = OFF;
          digitalWrite(FAN12_PIN, LOW);
          digitalWrite(FAN3_PIN, LOW);
          stripTVOC.clear(); stripTVOC.show();
          stripPM25.clear(); stripPM25.show();
        }
      }
    }
  }
  lastButtonReading = r;
}

bool connectWiFi() {
  Serial.print("连接WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 5000) {
    delay(100);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi连接成功!");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi连接失败，进入离线模式");
    return false;
  }
}

bool preheatStep() {
  handleButton();
  unsigned long now = millis();
  if (preheatStepIndex < NEO_COUNT) {
    if (now - preheatLastTime >= stepInterval) {
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = (i <= preheatStepIndex) ? stripTVOC.Color(0, 0, 255) : 0;
        stripTVOC.setPixelColor(i, c);
        stripPM25.setPixelColor(i, c);
      }
      stripTVOC.show(); stripPM25.show();
      preheatStepIndex++;
      preheatLastTime = now;
    }
    return false;
  } else if (flashCount < flashCycles * 2) {
    if (now - flashLastTime >= flashInterval) {
      flashOn = !flashOn;
      for (int i = 0; i < NEO_COUNT; i++) {
        uint32_t c = flashOn ? stripTVOC.Color(0, 0, 255) : 0;
        stripTVOC.setPixelColor(i, c);
        stripPM25.setPixelColor(i, c);
      }
      stripTVOC.show(); stripPM25.show();
      flashCount++;
      flashLastTime = now;
    }
    return false;
  } else {
    stripTVOC.clear(); stripTVOC.show();
    stripPM25.clear(); stripPM25.show();
    return true;
  }
}

void measureAndControl() {
  uint16_t tvoc = UINT16_MAX;
  if (sgp.IAQmeasure()) {
    tvoc = sgp.TVOC;
    currentTVOC = tvoc;
  }

  uint16_t pm25 = lastPM25;
  PM25_AQI_Data d;
  if (aqi.read(&d)) {
    pm25 = d.pm25_standard;
    lastPM25 = pm25;
    currentPM25 = pm25;
    pm25FailCount = 0;
  } else {
    pm25FailCount++;
    if (pm25FailCount >= PM25_MAX_RETRIES) {
      Serial2.begin(9600, SERIAL_8N1, 16, 17);
      if (aqi.begin_UART(&Serial2)) {
        pm25FailCount = 0;
      }
    }
  }

  Serial.printf("TVOC=%d ppb  PM2.5=%d \xC2\xB5g/m\xC2\xB3\n", tvoc, pm25);

  int lTV = calcLevel(tvoc, TVOC_LOW, TVOC_MED, TVOC_HIGH);
  int lP = calcLevel(pm25, PM25_LOW, PM25_MED, PM25_HIGH);

  if ((lTV > 2) || (lP > 2)) {
    digitalWrite(FAN12_PIN, HIGH); digitalWrite(FAN3_PIN, HIGH);
  } else if ((lTV > 1) || (lP > 1)) {
    digitalWrite(FAN12_PIN, HIGH); digitalWrite(FAN3_PIN, LOW);
  } else if ((lTV > 0) || (lP > 0)) {
    digitalWrite(FAN12_PIN, LOW); digitalWrite(FAN3_PIN, HIGH);
  } else {
    digitalWrite(FAN12_PIN, LOW); digitalWrite(FAN3_PIN, LOW);
  }

  updateStripLevel(stripTVOC, lTV);
  updateStripLevel(stripPM25, lP);
}

void uploadToThingSpeak() {
  if (!wifiConnected) return;
  HTTPClient http;
  http.begin(thingSpeakURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String postData = "api_key=" + String(writeAPIKey) + "&field1=" + String(currentTVOC) + "&field2=" + String(currentPM25);
  int httpResponseCode = http.POST(postData);
  http.end();
}

int calcLevel(uint16_t v, uint16_t lw, uint16_t mw, uint16_t hw) {
  if (v >= hw) return 3;
  if (v >= mw) return 2;
  if (v >= lw) return 1;
  return 0;
}

void updateStripLevel(Adafruit_NeoPixel &s, int level) {
  int cnt; uint32_t col;
  switch (level) {
    case 0: cnt = 2; col = s.Color(0, 255, 0); break;
    case 1: cnt = 4; col = s.Color(255, 255, 0); break;
    case 2: cnt = 4; col = s.Color(255, 165, 0); break;
    default: cnt = 8; col = s.Color(255, 0, 0); break;
  }
  for (int i = 0; i < NEO_COUNT; i++) {
    s.setPixelColor(i, i < cnt ? col : 0);
  }
  s.show();
}
