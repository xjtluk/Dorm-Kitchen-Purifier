#include <Wire.h>
#include "Adafruit_SGP30.h"
#include "Adafruit_PM25AQI.h"
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>

// —— WiFi 配置 —— 
const char* ssid = "vivo50";           // 替换为您的WiFi名称
const char* password = "bk666666";   // 替换为您的WiFi密码

// —— ThingSpeak 配置 —— 
const char* thingSpeakURL = "http://api.thingspeak.com/update";
const char* writeAPIKey = "HWLDOM7OUII2PRSC";  // 替换为您的ThingSpeak Write API Key

// —— 引脚定义 —— 
const int FAN12_PIN    = 18;
const int FAN3_PIN     = 19;
const int BUTTON_PIN   = 4;

#define NEO_TVOC_PIN    25
#define NEO_PM25_PIN    26
#define NEO_COUNT       8

// —— NeoPixel 实例 —— 
Adafruit_NeoPixel stripTVOC(NEO_COUNT, NEO_TVOC_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel stripPM25(NEO_COUNT, NEO_PM25_PIN, NEO_GRB + NEO_KHZ800);

// —— 按键去抖 —— 
int  lastButtonStable   = HIGH, lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// —— 传感器与阈值 —— 
Adafruit_SGP30 sgp;
Adafruit_PM25AQI aqi;
uint16_t lastPM25 = 0;
int pm25FailCount = 0;
const int PM25_MAX_RETRIES = 5;

const uint16_t TVOC_LOW  = 300, TVOC_MED  = 600, TVOC_HIGH  = 1000;
const uint16_t PM25_LOW  = 35,  PM25_MED  = 75,  PM25_HIGH  = 150;

// —— 测量定时 —— 
unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 2000;

// —— ThingSpeak 上传定时 —— 
unsigned long lastUploadTime = 0;
const unsigned long uploadInterval = 30000; // 30秒上传一次（ThingSpeak免费账户限制）

// —— 预热设置 —— 
const unsigned long totalPreheat  = 30000;    // 30s
const int flashCycles             = 3;
const unsigned long flashInterval = 400;
const unsigned long flashTotal    = flashCycles * 3 * flashInterval;
const unsigned long stepInterval  = (totalPreheat - flashTotal) / NEO_COUNT;

// —— 状态机 —— 
enum State { OFF, PREHEAT, RUNNING };
State state = OFF;

// —— PREHEAT 子状态 —— 
int preheatStepIndex    = 0;
unsigned long preheatLastTime = 0;
int flashCount          = 0;
bool flashOn            = false;
unsigned long flashLastTime = 0;

// —— 存储最新数据用于上传 —— 
uint16_t currentTVOC = 0;
uint16_t currentPM25 = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n=== 智能净化器初始化 ===");

  // 按键
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lastButtonReading = digitalRead(BUTTON_PIN);
  lastButtonStable  = lastButtonReading;

  // 风扇
  pinMode(FAN12_PIN, OUTPUT);
  pinMode(FAN3_PIN,  OUTPUT);
  digitalWrite(FAN12_PIN, LOW);
  digitalWrite(FAN3_PIN,  LOW);

  // LED
  stripTVOC.begin(); stripTVOC.show();
  stripPM25.begin(); stripPM25.show();

  // SGP30 (I²C)
  Wire.begin(21, 22);
  if (!sgp.begin()) {
    Serial.println("SGP30 未连接");
    while (1);
  }
  sgp.IAQinit();
  Serial.println("SGP30 已就绪");

  // PMS5003 (UART)
  Serial2.begin(9600, SERIAL_8N1, 16, 17);
  if (!aqi.begin_UART(&Serial2)) {
    Serial.println("PMS5003 未连接");
    while (1);
  }
  Serial.println("PMS5003 已就绪");

  // WiFi 连接
  connectWiFi();

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
  }
  else if (state == RUNNING) {
    if (millis() - lastMeasureTime >= measureInterval) {
      lastMeasureTime = millis();
      measureAndControl();
    }
    
    // 检查是否需要上传数据到ThingSpeak
    if (millis() - lastUploadTime >= uploadInterval) {
      lastUploadTime = millis();
      uploadToThingSpeak();
    }
  }
  
  // 检查WiFi连接状态
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi连接丢失，尝试重连...");
    connectWiFi();
  }
}

void connectWiFi() {
  Serial.print("连接WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("WiFi连接成功!");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi连接失败！");
  }
}

void uploadToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过数据上传");
    return;
  }

  HTTPClient http;
  http.begin(thingSpeakURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  
  // 构建POST数据
  String postData = "api_key=" + String(writeAPIKey) + 
                   "&field1=" + String(currentTVOC) +       // TVOC (ppb)
                   "&field2=" + String(currentPM25);       // PM2.5 (µg/m³)
  
  Serial.println("上传数据到ThingSpeak...");
  Serial.println("数据: " + postData);
  
  int httpResponseCode = http.POST(postData);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("ThingSpeak响应: " + response);
    
    if (response.toInt() > 0) {
      Serial.println("✓ 数据上传成功");
    } else {
      Serial.println("✗ 数据上传失败: " + response);
    }
  } else {
    Serial.println("✗ HTTP请求失败，错误代码: " + String(httpResponseCode));
  }
  
  http.end();
}

void handleButton() {
  int r = digitalRead(BUTTON_PIN);
  if (r != lastButtonReading) lastDebounceTime = millis();
  if (millis() - lastDebounceTime > debounceDelay) {
    if (r != lastButtonStable) {
      lastButtonStable = r;
      if (r == LOW) {
        if (state == OFF) {
          state = PREHEAT;
          Serial.println("\n按钮：开机，开始预热");
          preheatStepIndex = 0;
          preheatLastTime  = millis();
          flashCount       = 0;
          flashOn          = false;
        }
        else if (state == RUNNING) {
          state = OFF;
          Serial.println("\n按钮：关机");
          digitalWrite(FAN12_PIN, LOW);
          digitalWrite(FAN3_PIN,  LOW);
          stripTVOC.clear(); stripTVOC.show();
          stripPM25.clear(); stripPM25.show();
        }
      }
    }
  }
  lastButtonReading = r;
}

// 返回 true 当 PREHEAT 完成
bool preheatStep() {
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
  }
  else if (flashCount < flashCycles * 2) {
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
  }
  else {
    stripTVOC.clear(); stripTVOC.show();
    stripPM25.clear(); stripPM25.show();
    return true;
  }
}

void measureAndControl() {
  // 读取 TVOC
  uint16_t tvoc = UINT16_MAX;
  if (sgp.IAQmeasure()) {
    tvoc = sgp.TVOC;
    currentTVOC = tvoc;
  } else {
    Serial.println("Error: TVOC 读取失败");
  }

  // 读取 PM2.5（使用缓存避免误判）
  uint16_t pm25 = lastPM25;
  PM25_AQI_Data d;
  if (aqi.read(&d)) {
    pm25 = d.pm25_standard;
    lastPM25 = pm25;
    currentPM25 = pm25;
    pm25FailCount = 0;
  } else {
    pm25FailCount++;
    Serial.printf("Warning: PM2.5 读取失败 (%d)\n", pm25FailCount);
    if (pm25FailCount >= PM25_MAX_RETRIES) {
      Serial.println("-> 重新初始化 PMS5003 …");
      Serial2.begin(9600, SERIAL_8N1, 16, 17);
      if (aqi.begin_UART(&Serial2)) {
        Serial.println("   PMS5003 重连成功");
        pm25FailCount = 0;
      } else {
        Serial.println("   PMS5003 重连失败");
      }
    }
  }

  Serial.printf("TVOC=%d ppb  PM2.5=%d µg/m³  ", tvoc, pm25);

  // 计算等级
  int lTV = calcLevel(tvoc, TVOC_LOW, TVOC_MED, TVOC_HIGH);
  int lP  = calcLevel(pm25, PM25_LOW, PM25_MED, PM25_HIGH);

  bool hiP  = (lTV > 2) || (lP > 2);
  bool medP = (lTV > 1) || (lP > 1);
  bool lowP = (lTV > 0) || (lP > 0);

  if (hiP) {
    digitalWrite(FAN12_PIN, HIGH);
    digitalWrite(FAN3_PIN,  HIGH);
    Serial.println("等级：严重");
  } else if (medP) {
    digitalWrite(FAN12_PIN, HIGH);
    digitalWrite(FAN3_PIN,  LOW);
    Serial.println("等级：中等");
  } else if (lowP) {
    digitalWrite(FAN12_PIN, LOW);
    digitalWrite(FAN3_PIN,  HIGH);
    Serial.println("等级：初等");
  } else {
    digitalWrite(FAN12_PIN, LOW);
    digitalWrite(FAN3_PIN,  LOW);
    Serial.println("等级：正常");
  }

  updateStripLevel(stripTVOC, lTV);
  updateStripLevel(stripPM25, lP);
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
    case 0: cnt = 2; col = s.Color(0, 255, 0);   break;
    case 1: cnt = 4; col = s.Color(255, 255, 0); break;
    case 2: cnt = 4; col = s.Color(255, 165, 0); break;
    default:cnt = 8; col = s.Color(255,   0, 0); break;
  }
  for (int i = 0; i < NEO_COUNT; i++) {
    s.setPixelColor(i, i < cnt ? col : 0);
  }
  s.show();
}