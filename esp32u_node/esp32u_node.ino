#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

// =============================================================
//  ESP32U NODE (ตัววัด "นอกห้อง") — ส่งค่าให้ ESP32 MASTER ผ่าน ESP-NOW
// -------------------------------------------------------------
//  * สื่อสารตรง board-to-board ไม่ต้องต่อ WiFi / อินเทอร์เน็ต
//  * ส่งค่าฝุ่น GP2Y1014 + ความดัน/อุณหภูมิ BMP180 ทุก 5 วินาที
//  * ถ้าอยากเอาตัวนี้ไปวัด "ในห้อง" แทน แค่เปลี่ยน NODE_ID เป็น "INSIDE"
// =============================================================

const char *NODE_ID = "OUTSIDE"; // ชื่อ node ("OUTSIDE" หรือ "INSIDE")

// ชื่อ Wi-Fi บ้าน — ใช้ "สแกนหา channel" ให้ตรงกับ MASTER เท่านั้น
// (MASTER ต่อ WiFi อยู่ จึงรับ ESP-NOW ได้เฉพาะ channel เดียวกับ Router)
const char *WIFI_SSID = "KMITL-WiFi";

// =============================================================
// *** ใส่ MAC Address ของ ESP32 MASTER ตรงนี้ ***
// (อัปโหลด esp32_master ก่อน เปิด Serial Monitor ตอนบูต
//   จะพิมพ์ MAC ออกมา เช่น 24:6F:28:AA:BB:CC)
// =============================================================
uint8_t MASTER_MAC[] = {0xCC, 0x7B, 0x5C, 0x35, 0xA5, 0x80};

// 0 = สแกนหา channel ของ Router เองอัตโนมัติ
// 1-13 = บังคับ channel เอง ให้ตรงกับบรรทัด "Wi-Fi Channel: X" ที่ MASTER พิมพ์ออกมาตอนบูต
// (ใช้กรณีสแกนไม่เจอ SSID หรือ MASTER ต่อ WiFi ไม่ได้)
const int FORCE_CHANNEL = 0;

// Hardware Pins (อ้างอิงจาก esp32_inandoutsite.ino เดิม)
const int RESET_PIN = 33;       // ปุ่มกด = สแกน channel + เริ่ม ESP-NOW ใหม่
const int DUST_LED_PIN = 25;
const int DUST_ANALOG_PIN = 34;

// AB129 (BMP180)
Adafruit_BMP085 bmp;
bool isBmpReady = false;

// ส่งข้อมูลทุกๆ 5 วินาที
unsigned long previousMillis = 0;
const long interval = 5000;
uint32_t seq = 0;

// =============================================================
// โครงสร้าง Packet (ต้องเหมือนฝั่ง esp32_master เป๊ะ)
// =============================================================
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

// -------------------------------------------------------------
// Callback ผลการส่ง ESP-NOW
// -------------------------------------------------------------
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  Serial.printf("[ESP-NOW] ส่ง seq %lu : %s\n", (unsigned long)seq,
                status == ESP_NOW_SEND_SUCCESS ? "สำเร็จ" : "ไม่สำเร็จ (เช็ค MAC/Channel)");
}

// -------------------------------------------------------------
// สแกนหา channel ของ Router (ให้ตรงกับฝั่ง MASTER ที่ต่อ WiFi อยู่)
// -------------------------------------------------------------
int scanRouterChannel() {
  int ch = 1; // ค่าสำรอง ถ้าหาไม่เจอ
  Serial.printf("สแกนหา Wi-Fi '%s' เพื่อหา channel...\n", WIFI_SSID);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      ch = WiFi.channel(i);
      break;
    }
  }
  WiFi.scanDelete();
  Serial.printf("ใช้ channel %d\n", ch);
  return ch;
}

// -------------------------------------------------------------
// เริ่ม ESP-NOW
// -------------------------------------------------------------
void initEspNow() {
  int ch = (FORCE_CHANNEL > 0) ? FORCE_CHANNEL : scanRouterChannel();
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init ไม่สำเร็จ");
    return;
  }

  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, MASTER_MAC, 6);
  peer.channel = 0; // ใช้ channel ปัจจุบันของวิทยุ
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ERROR] เพิ่ม peer (MASTER) ไม่สำเร็จ");
  } else {
    Serial.println("[SUCCESS] ESP-NOW พร้อมส่งไปยัง MASTER");
  }
}

// -------------------------------------------------------------
// สแกนบัส I2C — ใช้ดูว่า BMP180 (address 0x77) ต่อสายเข้ามาจริงไหม
// -------------------------------------------------------------
void scanI2C() {
  Serial.println("สแกนบัส I2C (SDA=21, SCL=22)...");
  bool found = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  พบอุปกรณ์ที่ address 0x%02X%s\n", addr,
                    addr == 0x77 ? "  <- นี่คือ BMP180" : "");
      found = true;
    }
  }
  if (!found) {
    Serial.println("  ไม่พบอุปกรณ์ใดเลย! เช็ค: สาย SDA/SCL สลับกันไหม /");
    Serial.println("  ไฟเลี้ยงเซ็นเซอร์ / การบัดกรีขาโมดูล");
  }
}

// -------------------------------------------------------------
// อ่านค่าฝุ่น GP2Y1014
// -------------------------------------------------------------
float readDustDensity() {
  digitalWrite(DUST_LED_PIN, LOW);
  delayMicroseconds(280);

  int rawValue = analogRead(DUST_ANALOG_PIN);

  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH);
  delayMicroseconds(9680);

  float voltage = rawValue * (3.3 / 4095.0);

  // ** หมายเหตุ: ปรับค่า baseline ตามแรงดันจริงขณะไม่มีฝุ่นของแต่ละตัว **
  float baseline = 0.35;

  float dustDensity = 0.0;
  if (voltage > baseline) {
    dustDensity = (voltage - baseline) * 200.0;
  } else {
    dustDensity = 0.0;
  }

  return dustDensity;
}

// -------------------------------------------------------------
// SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);
  delay(100);

  scanI2C();

  if (!bmp.begin()) {
    Serial.println("[ERROR] ไม่พบโมดูล AB129 (BMP180) — จะลองใหม่ทุก 10 วินาที");
    Serial.println("        ค่า Pressure/Temp จะเป็น 0 จนกว่าจะหาเซ็นเซอร์เจอ");
    isBmpReady = false;
  } else {
    Serial.println("[SUCCESS] เชื่อมต่อ AB129 (BMP180) สำเร็จ!");
    isBmpReady = true;
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);
  analogReadResolution(12);

  // ไม่ connect WiFi — แค่เปิดโหมด STA เพื่อให้สแกน channel และใช้ ESP-NOW ได้
  // หมายเหตุ: ใช้ disconnect() เฉยๆ ไม่ปิด driver (ถ้าปิด driver การสแกน/ส่งอาจเพี้ยน)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("==========================================");
  Serial.println("MAC ของบอร์ดนี้: " + WiFi.macAddress());
  Serial.printf("MASTER_MAC ที่ตั้งไว้ในโค้ด: %02X:%02X:%02X:%02X:%02X:%02X\n",
                MASTER_MAC[0], MASTER_MAC[1], MASTER_MAC[2],
                MASTER_MAC[3], MASTER_MAC[4], MASTER_MAC[5]);
  Serial.println("  ^ ต้องตรงกับ MAC ที่ MASTER พิมพ์ออกมาตอนบูตเป๊ะๆ");
  Serial.println("==========================================");

  initEspNow();
}

// -------------------------------------------------------------
// LOOP
// -------------------------------------------------------------
void loop() {
  // ปุ่มกด = สแกน channel + เริ่ม ESP-NOW ใหม่ (ใช้ตอน Router เปลี่ยน channel)
  if (digitalRead(RESET_PIN) == LOW) {
    delay(100);
    if (digitalRead(RESET_PIN) == LOW) {
      initEspNow();
      while (digitalRead(RESET_PIN) == LOW) delay(10);
    }
  }

  unsigned long currentMillis = millis();

  // ถ้าหา BMP180 ไม่เจอตอนบูต -> ลองใหม่ทุก 10 วินาที (เผื่อสายหลวม/เพิ่งเสียบเซ็นเซอร์)
  static unsigned long lastBmpRetryMs = 0;
  if (!isBmpReady && currentMillis - lastBmpRetryMs >= 10000) {
    lastBmpRetryMs = currentMillis;
    if (bmp.begin()) {
      Serial.println("[SUCCESS] พบ BMP180 แล้ว! เริ่มอ่านค่าจริง");
      isBmpReady = true;
    }
  }

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    float dust = readDustDensity();
    float pressure = 0.0;
    float temp = 0.0;

    if (isBmpReady) {
      temp = bmp.readTemperature();
      pressure = bmp.readPressure() / 100.0F; // hPa
    }

    // เตรียม packet
    SensorPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    strncpy(pkt.node_id, NODE_ID, sizeof(pkt.node_id) - 1);
    pkt.dust = dust;
    pkt.pressure = pressure;
    pkt.temperature = temp;
    pkt.seq = ++seq;
    pkt.crc = 0;
    pkt.crc = crc8((const uint8_t *)&pkt, sizeof(pkt));

    esp_err_t sendRes = esp_now_send(MASTER_MAC, (const uint8_t *)&pkt, sizeof(pkt));

    // แสดงผลทาง Serial Monitor
    Serial.println("------------------------------------------");
    Serial.printf("Node ID     : %s\n", NODE_ID);
    Serial.printf("Dust        : %.2f ug/m3\n", dust);
    Serial.printf("Pressure    : %.2f hPa\n", pressure);
    Serial.printf("Temperature : %.2f *C\n", temp);
    if (sendRes != ESP_OK) {
      Serial.printf("[ESP-NOW] esp_now_send() = %s  <- packet ไม่ออกจากบอร์ด\n",
                    esp_err_to_name(sendRes));
    }
  }
}
