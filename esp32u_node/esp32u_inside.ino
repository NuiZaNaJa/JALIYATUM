#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

// =============================================================
//  ESP32 INSIDE NODE (ติดตั้ง "ในห้อง")
// =============================================================

const char *NODE_ID = "INSIDE"; 
const char *WIFI_SSID = "KMITL-WiFi"; 

uint8_t MASTER_MAC[] = {0x78, 0x1C, 0x3C, 0xA8, 0x98, 0xE4}; // MAC Address ของ Master

const int DUST_LED_PIN = 25;
const int DUST_ANALOG_PIN = 34;
Adafruit_BMP085 bmp;
bool isBmpReady = false;

unsigned long previousMillis = 0;
const long interval = 5000;
uint32_t seq = 0;
int failCount = 0; // ตัวนับจำนวนครั้งที่ส่งพลาด

typedef struct __attribute__((packed)) {
  char node_id[10];
  float dust;
  float pressure;
  float temperature;
  uint32_t seq;
  uint8_t crc;
} SensorPacket;

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) crc ^= data[i];
  return crc;
}

// ฟังก์ชันสแกนหา Channel
int scanRouterChannel() {
  int ch = 1;
  WiFi.scanNetworks();
  for (int i = 0; i < WiFi.scanComplete(); i++) {
    if (WiFi.SSID(i) == WIFI_SSID) { 
      ch = WiFi.channel(i); 
      break; 
    }
  }
  WiFi.scanDelete();
  return ch;
}

void initEspNow() {
  esp_wifi_set_channel(scanRouterChannel(), WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return;

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = 0; // 0 หมายถึงให้ใช้ Channel ปัจจุบันของบอร์ด
  peer.encrypt = false;
  
  if (esp_now_add_peer(&peer) == ESP_OK) {
    Serial.println("ESP-NOW Initialized!");
  } else {
    // ถ้าเคย add peer ไปแล้วมันจะฟ้องว่ามีแล้ว ก็ไม่เป็นไร
    Serial.println("ESP-NOW Peer already exists or error.");
  }
}

float readDustDensity() {
  digitalWrite(DUST_LED_PIN, LOW);
  delayMicroseconds(280);
  int rawValue = analogRead(DUST_ANALOG_PIN);
  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH);
  delayMicroseconds(9680);
  
  float voltage = rawValue * (3.3 / 4095.0);
  Serial.printf("[DUST DEBUG] Raw ADC: %d | Voltage: %.3f V\n", rawValue, voltage);
  
  float baseline = 0.20; 
  return (voltage > baseline) ? (voltage - baseline) * 200.0 : 0.0;
}

void setup() {
  Serial.begin(115200);
  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);
  analogReadResolution(12);

  Wire.begin(21, 22);
  if (bmp.begin()) isBmpReady = true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  initEspNow();
}

void loop() {
  unsigned long currentMillis = millis();

  // รีเซ็ตเซ็นเซอร์เผื่อรวน
  static unsigned long lastBmpRetryMs = 0;
  if (!isBmpReady && currentMillis - lastBmpRetryMs >= 10000) {
    lastBmpRetryMs = currentMillis;
    if (bmp.begin()) isBmpReady = true;
  }

  // ส่งข้อมูลทุก 5 วินาที
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    
    float dust = readDustDensity();
    float pressure = 0.0, temp = 0.0;
    if (isBmpReady) {
      temp = bmp.readTemperature();
      pressure = bmp.readPressure() / 100.0F;
    }

    SensorPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    strncpy(pkt.node_id, NODE_ID, sizeof(pkt.node_id) - 1);
    pkt.dust = dust;
    pkt.pressure = pressure;
    pkt.temperature = temp;
    pkt.seq = ++seq;
    pkt.crc = 0;
    pkt.crc = crc8((const uint8_t *)&pkt, sizeof(pkt));

    esp_err_t res = esp_now_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));
    
    // ตรวจสอบผลการส่ง และทำระบบ Auto-Recovery
    if (res == ESP_OK) {
      Serial.printf("Send: Dust %.2f, Press %.2f -> SUCCESS\n", dust, pressure);
      failCount = 0; // ส่งสำเร็จให้รีเซ็ตตัวนับ
    } else {
      failCount++;
      Serial.printf("Send: Dust %.2f, Press %.2f -> FAIL (%d times)\n", dust, pressure, failCount);
      
      // ถ้าล้มเหลวติดกัน 3 ครั้ง (แสดงว่าคลื่นเคลื่อนแน่ๆ) ให้สแกนหาใหม่
      if (failCount >= 3) {
        Serial.println("[WARNING] สัญญาณหลุด! กำลังสแกนหา Channel ใหม่...");
        int newCh = scanRouterChannel();
        esp_wifi_set_channel(newCh, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[INFO] เปลี่ยนไปใช้ Channel: %d\n", newCh);
        failCount = 0; // สแกนเสร็จให้รีเซ็ตตัวนับ เริ่มส่งใหม่
      }
    }
  }
}
