#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

// =============================================================
//  ESP32 MASTER (ติดตั้ง "นอกห้อง")
// -------------------------------------------------------------
//  หน้าที่:
//  1. วัดฝุ่น/ความดัน "นอกห้อง" (ต่อเซ็นเซอร์ตรง)
//  2. รับค่าฝุ่น/ความดัน "ในห้อง" จาก ESP32 Node ผ่าน ESP-NOW
//  3. ควบคุม Relay พัดลม
//  4. ส่งข้อมูลขึ้น Google Sheets
// =============================================================

// โครงสร้าง Packet (ต้องตรงกับตัวในห้อง)
typedef struct __attribute__((packed)) {
  char node_id[10];
  float dust;
  float pressure;
  float temperature;
  uint32_t seq;
  uint8_t crc;
} SensorPacket;

struct NodeState {
  bool hasData = false;
  unsigned long lastMs = 0;
  float dustEMA = 0, pressEMA = 0, tempEMA = 0;
  uint32_t lastSeq = 0;
};

// 1. ตั้งค่า Wi-Fi & Google Sheets
const char *ssid = "KMITL-WiFi";
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbxcmGw3BYhbpe0Lh9iT59iEPWHb6lTW3sfFZ6AQLef-hbiBRsD3qOheoOWzp2BFw6Mffw/exec";

// 2. ขา Hardware (ปรับได้ตามการต่อสายจริง)
const int RELAY_PIN = 26;
const int LED_PIN = 2;
const int RESET_PIN = 33;
const bool RELAY_ACTIVE_HIGH = false;

// เซ็นเซอร์นอกห้อง (ต่อตรงกับบอร์ดนี้)
const int DUST_LED_PIN = 25;
const int DUST_ANALOG_PIN = 34;
Adafruit_BMP085 bmp;
bool isBmpReady = false;

// 3. เกณฑ์การควบคุมพัดลม
float DP_OFF_HPA = 0.30;
float DP_ON_HPA = 0.15;
float DUST_MARGIN_UG = 3.0;
float PRESSURE_OFFSET_HPA = 0.0;

const unsigned long NODE_TIMEOUT_MS = 30000;
const unsigned long MIN_TOGGLE_MS = 30000;
const bool FAILSAFE_FAN_ON = true;

// ตัวแปรข้อมูล
NodeState inNode, outNode;
const float EMA_ALPHA = 0.4;
uint32_t localSeq = 0;
bool fanOn = true;
unsigned long lastToggleMs = 0;
String controlReason = "boot";

// ตัวจับเวลา
unsigned long lastReadMs = 0, lastEvalMs = 0, lastPrintMs = 0, lastUploadMs = 0, lastWifiRetryMs = 0;

// =============================================================
uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) crc ^= data[i];
  return crc;
}

void updateNode(NodeState &n, float dust, float press, float temp, uint32_t seq) {
  if (!n.hasData) {
    n.dustEMA = dust;
    n.pressEMA = press;
    n.tempEMA = temp;
    n.hasData = true;
  } else {
    n.dustEMA += EMA_ALPHA * (dust - n.dustEMA);
    n.pressEMA += EMA_ALPHA * (press - n.pressEMA);
    n.tempEMA += EMA_ALPHA * (temp - n.tempEMA);
  }
  n.lastSeq = seq;
  n.lastMs = millis();
}

// อ่านเซ็นเซอร์ "นอกห้อง" (ตัวเอง)
void readOutsideSensors() {
  // 1. อ่านค่าดิบ 5 ครั้งติดกันแล้วหาค่าเฉลี่ย เพื่อลดอาการ "ตัวเลขแกว่ง" (Noise filter)
  long sumRaw = 0;
  for (int i = 0; i < 5; i++) {
    digitalWrite(DUST_LED_PIN, LOW);
    delayMicroseconds(280);
    sumRaw += analogRead(DUST_ANALOG_PIN);
    delayMicroseconds(40);
    digitalWrite(DUST_LED_PIN, HIGH);
    delayMicroseconds(9680);
  }
  int rawValue = sumRaw / 5;

  float voltage = rawValue * (3.3 / 4095.0);

  // 2. ใช้สมการเดียวกับตัวในห้องเป๊ะ 100% (Baseline 0.20 และไม่มีการลบค่าออก)
  float baseline = 0.35;
  float dust = (voltage > baseline) ? (voltage - baseline) * 200.0 : 0.0;

  float pressure = 0.0, temp = 0.0;

  if (isBmpReady) {
    temp = bmp.readTemperature();
    // (ส่วนของความดันยังคงบวกชดเชย +1.80 ไว้ เพราะก่อนหน้านี้ปรับไว้แล้วค่าเท่ากันเป๊ะ)
    pressure = (bmp.readPressure() / 100.0F) + 1.80;
  }

  localSeq++;
  updateNode(outNode, dust, pressure, temp, localSeq);
}

// รับข้อมูล ESP-NOW จาก "ในห้อง"
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != (int)sizeof(SensorPacket)) return;
  SensorPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  uint8_t crcRecv = pkt.crc;
  pkt.crc = 0;
  if (crc8((const uint8_t *)&pkt, sizeof(pkt)) != crcRecv) return;

  if (strncmp(pkt.node_id, "INSIDE", sizeof(pkt.node_id)) == 0) {
    updateNode(inNode, pkt.dust, pkt.pressure, pkt.temperature, pkt.seq);
  }
}

// ควบคุมพัดลม
void applyRelay() {
  digitalWrite(RELAY_PIN, (RELAY_ACTIVE_HIGH == fanOn) ? HIGH : LOW);
  digitalWrite(LED_PIN, fanOn ? HIGH : LOW);
}

float currentDP() {
  return (inNode.pressEMA - outNode.pressEMA) - PRESSURE_OFFSET_HPA;
}

void updateControl(unsigned long now) {
  bool inFresh = inNode.hasData && (now - inNode.lastMs < NODE_TIMEOUT_MS);
  bool outFresh = outNode.hasData && (now - outNode.lastMs < NODE_TIMEOUT_MS);
  bool desired = fanOn;

  if (!inFresh || !outFresh) {
    desired = FAILSAFE_FAN_ON;
    controlReason = "sensor timeout -> failsafe";
  } else {
    float dp = currentDP();
    bool airClean = inNode.dustEMA < (outNode.dustEMA - DUST_MARGIN_UG);

    if (fanOn) {
      if (airClean && dp >= DP_OFF_HPA) {
        desired = false;
        controlReason = "clean + over-pressure -> fan OFF";
      } else {
        controlReason = "Maintaining ON (Normal)";  // <--- แก้บั๊กข้อความค้างตรงนี้
      }
    } else {
      if (!airClean) {
        desired = true;
        controlReason = "inside dust not better -> fan ON";
      } else if (dp <= DP_ON_HPA) {
        desired = true;
        controlReason = "pressure dropped -> fan ON";
      } else {
        controlReason = "Maintaining OFF (Clean)";  // <--- แก้บั๊กข้อความค้างตรงนี้
      }
    }
  }

  if (desired != fanOn && (now - lastToggleMs >= MIN_TOGGLE_MS)) {
    fanOn = desired;
    lastToggleMs = now;
    applyRelay();
  }
}

void sendRowToGoogleSheet(const char *sheet, const char *device, float dust, float press, float temp, const char *fan, const char *note) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] ส่งไม่ได้ เพราะ Wi-Fi หลุดอยู่");
    return;
  }

  // สร้าง Client แบบ Secure และข้ามการเช็คใบรับรอง (จำเป็นสำหรับ Google)
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, GOOGLE_SCRIPT_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"sheet_name\":\"" + String(sheet) + "\",\"device_id\":\"" + String(device) + "\",\"dust\":" + String(dust, 2) + ",\"pressure\":" + String(press, 2) + ",\"temperature\":" + String(temp, 2) + ",\"fan\":\"" + String(fan) + "\",\"note\":\"" + String(note) + "\"}";

  Serial.print("[HTTP] กำลังส่งข้อมูลไป Google Sheet (" + String(sheet) + ")... ");
  int httpCode = http.POST(jsonPayload);

  if (httpCode > 0) {
    Serial.printf("สำเร็จ! (Code: %d)\n", httpCode);
  } else {
    Serial.printf("ล้มเหลว! Error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void uploadAll(unsigned long now) {
  bool inFresh = inNode.hasData && (now - inNode.lastMs < NODE_TIMEOUT_MS);
  bool outFresh = outNode.hasData && (now - outNode.lastMs < NODE_TIMEOUT_MS);
  const char *fanTxt = fanOn ? "ON" : "OFF";

  if (inFresh) sendRowToGoogleSheet("Inside", "ESP32_INSIDE_NODE", inNode.dustEMA, inNode.pressEMA, inNode.tempEMA, fanTxt, controlReason.c_str());
  if (outFresh) sendRowToGoogleSheet("Outside", "ESP32_OUTSIDE_MASTER", outNode.dustEMA, outNode.pressEMA, outNode.tempEMA, fanTxt, "-");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n--- บอร์ดเริ่มทำงาน ---");

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);
  analogReadResolution(12);
  applyRelay();

  Serial.println("กำลังเช็คเซ็นเซอร์ BMP180...");
  Wire.begin(21, 22);
  if (bmp.begin()) {
    Serial.println("[SUCCESS] พบเซ็นเซอร์ BMP180");
    isBmpReady = true;
  } else {
    Serial.println("[ERROR] ไม่พบเซ็นเซอร์ BMP180");
  }

  // ---------------------------------------------------------
  // เพิ่มส่วนนี้: สแกนหา Wi-Fi รอบตัว
  // ---------------------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);

  Serial.println("\nกำลังสแกนหา Wi-Fi บริเวณนี้...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("-> ไม่พบ Wi-Fi ใดๆ เลย! (เสาอากาศอาจมีปัญหา หรือจุดนี้ไม่มี 2.4GHz)");
  } else {
    Serial.printf("-> พบ %d เครือข่าย:\n", n);
    bool foundKMITL = false;
    for (int i = 0; i < n; ++i) {
      Serial.printf("   %d: %s (ความแรง: %d dBm)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
      if (WiFi.SSID(i) == ssid) foundKMITL = true;
      delay(10);
    }
    if (!foundKMITL) Serial.println("\n[WARNING] *** บอร์ดมองไม่เห็น KMITL-WiFi ในบริเวณนี้! ***");
  }
  Serial.println("---------------------------------------------------------");

  // พยายามเชื่อมต่อ
  Serial.print("\nกำลังพยายามเชื่อมต่อ: ");
  Serial.println(ssid);
  WiFi.begin(ssid);

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 20) {
    delay(500);
    Serial.print(".");
    count++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] เชื่อมต่อ Wi-Fi สำเร็จ!");
  } else {
    Serial.println("\n[ERROR] เชื่อมต่อ Wi-Fi ไม่สำเร็จ!");
  }

  Serial.println("==============================================");
  Serial.println("  MAC Address ของ MASTER (ตัวนอกห้อง): " + WiFi.macAddress());
  Serial.println("==============================================");

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[SUCCESS] ESP-NOW พร้อมรับข้อมูลแล้ว");
  }
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && now - lastWifiRetryMs >= 30000) {
    lastWifiRetryMs = now;
    WiFi.disconnect();
    WiFi.begin(ssid);
  }

  if (now - lastReadMs >= 2000) {
    lastReadMs = now;
    readOutsideSensors();
  }
  if (now - lastEvalMs >= 1000) {
    lastEvalMs = now;
    updateControl(now);
  }
  if (now - lastUploadMs >= 30000) {
    lastUploadMs = now;
    uploadAll(now);
  }

  if (now - lastPrintMs >= 5000) {
    lastPrintMs = now;
    Serial.printf("IN  (Node)   : ฝุ่น %.1f | %.2f hPa | %.1f *C\n", inNode.dustEMA, inNode.pressEMA, inNode.tempEMA);
    Serial.printf("OUT (Master) : ฝุ่น %.1f | %.2f hPa | %.1f *C\n", outNode.dustEMA, outNode.pressEMA, outNode.tempEMA);
    Serial.printf("dP = %+.2f hPa | FAN = %s | %s\n-------------------\n", currentDP(), fanOn ? "ON" : "OFF", controlReason.c_str());
  }
}
