#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_now.h>

// =============================================================
//  Type definitions — ต้องอยู่บนสุดของไฟล์ ก่อน config ทั้งหมด
//  เหตุผล: Arduino IDE จะสร้าง function prototype อัตโนมัติ
//  แล้วแทรกไว้เหนือโค้ดผู้ใช้ทั้งหมด ถ้า struct ไม่อยู่บนสุด
//  ฟังก์ชันที่รับ struct (เช่น NodeState&) จะ compile ไม่ผ่าน
// =============================================================

// โครงสร้าง Packet ESP-NOW (ต้องเหมือนฝั่ง esp32u_node เป๊ะ)
typedef struct __attribute__((packed)) {
  char node_id[10];   // "OUTSIDE" หรือ "INSIDE"
  float dust;         // ug/m3
  float pressure;     // hPa
  float temperature;  // *C
  uint32_t seq;       // เลขรันแพ็กเก็ต
  uint8_t crc;        // checksum ง่ายๆ (XOR ทุก byte)
} SensorPacket;

// ที่เก็บข้อมูล node (กรอง noise ด้วย EMA)
struct NodeState {
  bool hasData = false;
  unsigned long lastMs = 0;
  float dustEMA = 0, pressEMA = 0, tempEMA = 0;
  uint32_t lastSeq = 0;
};

// Prototype ชัดเจนสำหรับฟังก์ชันที่รับ struct เป็น parameter
// เมื่อ IDE เจอ prototype ที่เขียนไว้เอง จะไม่สร้างใหม่อัตโนมัติ
// (แก้ error: 'NodeState' was not declared in this scope)
void updateNode(NodeState &n, float dust, float press, float temp, uint32_t seq);

// =============================================================
//  ESP32 MASTER (ตัวกลาง) — ระบบห้องความดันบวกกันฝุ่น PM2.5
// -------------------------------------------------------------
//  หน้าที่:
//   1. รับค่าฝุ่น PM2.5 + ความดันอากาศ จากบอร์ดตัววัด 2 ตัว
//        - ESP32U       -> ส่งผ่าน ESP-NOW (สื่อสารตรง board-to-board)
//        - UNO R4 WiFi  -> ส่งผ่าน UDP Broadcast (ผ่าน WiFi บ้าน)
//   2. คำนวณและสั่ง Relay เปิด/ปิด "พัดลม" อัตโนมัติ
//   3. ส่งข้อมูลทั้งหมดขึ้น Google Sheet (ตัวเดียวที่ออกอินเทอร์เน็ต)
//
//  ตรรกะควบคุมพัดลม:
//   - ปกติ: เปิดพัดลม = สร้างความดันบวก กันฝุ่นจากนอกเข้าห้อง
//   - ปิดพัดลม: ครบ 2 เงื่อนไขพร้อมกัน
//        (1) ฝุ่นในห้อง "น้อยกว่า" นอกห้อง (อากาศในห้องสะอาดแล้ว)
//        (2) ความดันในห้อง "สูงกว่า" นอกห้องมากเกินไป (คนเริ่มอึดอัด)
//   - เปิดกลับ: ฝุ่นในห้องไม่ดีกว่านอกห้อง หรือความดันตกลง
//   - Fail-safe: ถ้าเซ็นเซอร์ตัวใดเงียบเกิน 30 วินาที -> เปิดพัดลมไว้
// =============================================================

// =============================================================
// 1. ตั้งค่า Wi-Fi และ Google Web App URL
// =============================================================
const char *ssid = "KMITL-WiFi";   // ใส่ชื่อ Wi-Fi (ตัวเดียวกับ UNO R4 ใช้)
const char *password = ""; // ใส่รหัสผ่าน Wi-Fi

// วาง URL ที่ได้จากการ Deploy Google Apps Script ตรงนี้
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbxcmGw3BYhbpe0Lh9iT59iEPWHb6lTW3sfFZ6AQLef-hbiBRsD3qOheoOWzp2BFw6Mffw/exec";
// =============================================================
// 2. ตั้งค่า Hardware
// =============================================================
const int RELAY_PIN = 26;       // ขาควบคุม Relay พัดลม
const int LED_PIN = 2;          // LED แสดงสถานะพัดลม (บนบอร์ด)
const int RESET_PIN = 33;       // ปุ่ม Reset Wi-Fi (กดค้าง = reconnect)
const bool RELAY_ACTIVE_HIGH = true; // โมดูล Relay ส่วนมากเป็น Active LOW
                                     // ถ้าพัดลมทำงานสลับกัน ให้แก้เป็น false

// =============================================================
// 3. ตั้งค่าเกณฑ์ควบคุม (ปรับตามหน้างานจริง)
// =============================================================
float DP_OFF_HPA = 0.30; // dP (hPa) ที่เริ่มอึดอัด -> สั่งปิดพัดลม (0.30 hPa ≈ 30 Pa)
float DP_ON_HPA  = 0.15; // dP (hPa) ที่ความดันตกจนเปิดพัดลมกลับ (Hysteresis)
float DUST_MARGIN_UG = 3.0; // ฝุ่นในต้องน้อยกว่านอกเกินค่านี้ (ug/m3) ถึงถือว่า "สะอาดกว่า"

// ค่าชดเชยความดัน (คาริเบรต 2 จุด):
// วิธีทำ: ปิดพัดลม + เปิดประตูให้ความดันใน=นอก แล้วดูค่า dP ใน Serial Monitor
//         เอาค่านั้นมาใส่ตรงนี้ ให้ dP ที่อ่านได้กลายเป็น ~0.00
float PRESSURE_OFFSET_HPA = 0.0;

const unsigned long NODE_TIMEOUT_MS = 30000; // node เงียบเกินนี้ = ข้อมูลหมดอายุ
const unsigned long MIN_TOGGLE_MS   = 30000; // สลับพัดลมถี่สุดทุก 30 วิ (กัน Relay พัง)
const bool FAILSAFE_FAN_ON = true;  // เซ็นเซอร์ตาย/ข้อมูลขาด -> เปิดพัดลมไว้ (ปลอดภัยเรื่องฝุ่น)

// =============================================================
// 4.-5. SensorPacket และ NodeState ถูกย้ายไปนิยามไว้บนสุดของไฟล์
//       (เหนือหัวข้อ ESP32 MASTER) เพื่อเลี่ยง Arduino IDE
//       auto-prototype bug — ห้ามย้ายกลับลงมา
// =============================================================
NodeState inNode, outNode;            // INSIDE (UNO R4) / OUTSIDE (ESP32U)
const float EMA_ALPHA = 0.4;          // 0-1 ยิ่งน้อยยิ่งนิ่ง (กรอง noise BMP180)

uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) crc ^= data[i];
  return crc;
}

void updateNode(NodeState &n, float dust, float press, float temp, uint32_t seq) {
  if (!n.hasData) {
    n.dustEMA = dust; n.pressEMA = press; n.tempEMA = temp;
    n.hasData = true;
  } else {
    n.dustEMA  += EMA_ALPHA * (dust  - n.dustEMA);
    n.pressEMA += EMA_ALPHA * (press - n.pressEMA);
    n.tempEMA  += EMA_ALPHA * (temp  - n.tempEMA);
  }
  n.lastSeq = seq;
  n.lastMs = millis();
}

// =============================================================
// 6. รับข้อมูลจาก ESP-NOW (ESP32U)
// =============================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
// Arduino ESP32 Core เวอร์ชัน 3.x ขึ้นไป
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
// Arduino ESP32 Core เวอร์ชัน 2.x
void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != (int)sizeof(SensorPacket)) return;

  SensorPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  uint8_t crcRecv = pkt.crc;
  pkt.crc = 0;
  if (crc8((const uint8_t *)&pkt, sizeof(pkt)) != crcRecv) return; // ข้อมูลเสีย

  if (strncmp(pkt.node_id, "INSIDE", sizeof(pkt.node_id)) == 0) {
    updateNode(inNode, pkt.dust, pkt.pressure, pkt.temperature, pkt.seq);
  } else if (strncmp(pkt.node_id, "OUTSIDE", sizeof(pkt.node_id)) == 0) {
    updateNode(outNode, pkt.dust, pkt.pressure, pkt.temperature, pkt.seq);
  }
}

// =============================================================
// 7. รับข้อมูลจาก UDP (UNO R4 WiFi) รูปแบบ: "NODE,dust,pressure,temp,seq"
// =============================================================
WiFiUDP udp;
const uint16_t UDP_PORT = 4210;

void pollUdp() {
  int size = udp.parsePacket();
  if (size <= 0) return;

  char buf[96];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  char *node = strtok(buf, ",");
  char *d = strtok(NULL, ",");
  char *p = strtok(NULL, ",");
  char *t = strtok(NULL, ",");
  char *s = strtok(NULL, ",");
  if (!node || !d || !p || !t || !s) return;

  if (strcmp(node, "INSIDE") == 0) {
    updateNode(inNode, atof(d), atof(p), atof(t), (uint32_t)atol(s));
  } else if (strcmp(node, "OUTSIDE") == 0) {
    updateNode(outNode, atof(d), atof(p), atof(t), (uint32_t)atol(s));
  }
}

// =============================================================
// 8. ตรรกะควบคุมพัดลม
// =============================================================
bool fanOn = true;                       // เริ่มต้นเปิดไว้ก่อน (ปลอดภัยเรื่องฝุ่น)
unsigned long lastToggleMs = 0;
String controlReason = "boot";

void applyRelay() {
  digitalWrite(RELAY_PIN, (RELAY_ACTIVE_HIGH == fanOn) ? HIGH : LOW);
  digitalWrite(LED_PIN, fanOn ? HIGH : LOW);
}

float currentDP() { // ความดันต่าง (ในห้อง - นอกห้อง) หลังหัก offset
  return (inNode.pressEMA - outNode.pressEMA) - PRESSURE_OFFSET_HPA;
}

void updateControl(unsigned long now) {
  bool inFresh  = inNode.hasData  && (now - inNode.lastMs  < NODE_TIMEOUT_MS);
  bool outFresh = outNode.hasData && (now - outNode.lastMs < NODE_TIMEOUT_MS);

  bool desired = fanOn;

  if (!inFresh || !outFresh) {
    // ข้อมูลไม่ครบ -> ใช้นโยบาย Fail-safe
    desired = FAILSAFE_FAN_ON;
    controlReason = "sensor timeout -> failsafe";
  } else {
    float dp = currentDP();
    bool airClean = inNode.dustEMA < (outNode.dustEMA - DUST_MARGIN_UG);

    if (fanOn) {
      // ปิดพัดลมเมื่อ: ในห้องสะอาดกว่านอก และความดันในสูงเกินคนอึดอัด
      if (airClean && dp >= DP_OFF_HPA) {
        desired = false;
        controlReason = "clean + over-pressure -> fan OFF";
      }
    } else {
      // เปิดกลับเมื่อ: ฝุ่นในไม่ดีกว่านอก หรือ ความดันตกต่ำกว่าเกณฑ์
      if (!airClean) {
        desired = true;
        controlReason = "inside dust not better -> fan ON";
      } else if (dp <= DP_ON_HPA) {
        desired = true;
        controlReason = "pressure dropped -> fan ON";
      }
    }
  }

  if (desired != fanOn && (now - lastToggleMs >= MIN_TOGGLE_MS)) {
    fanOn = desired;
    lastToggleMs = now;
    applyRelay();
    Serial.printf("[CONTROL] FAN -> %s (%s)\n", fanOn ? "ON" : "OFF",
                  controlReason.c_str());
  }
}

// =============================================================
// 9. ส่งข้อมูลไป Google Sheets (ผ่าน MASTER ตัวเดียว)
// =============================================================
void sendRowToGoogleSheet(const char *sheet, const char *device, float dust,
                          float press, float temp, const char *fan,
                          const char *note) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARNING] Wi-Fi หลุด! ข้ามการส่ง Google Sheet");
    return;
  }

  HTTPClient http;
  http.begin(GOOGLE_SCRIPT_URL);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{";
  jsonPayload += "\"sheet_name\":\"" + String(sheet) + "\",";
  jsonPayload += "\"device_id\":\"" + String(device) + "\",";
  jsonPayload += "\"dust\":" + String(dust, 2) + ",";
  jsonPayload += "\"pressure\":" + String(press, 2) + ",";
  jsonPayload += "\"temperature\":" + String(temp, 2) + ",";
  jsonPayload += "\"fan\":\"" + String(fan) + "\",";
  jsonPayload += "\"note\":\"" + String(note) + "\"";
  jsonPayload += "}";

  int httpCode = http.POST(jsonPayload);
  if (httpCode > 0) {
    Serial.printf("[HTTP] %s -> รหัสตอบกลับ: %d\n", sheet, httpCode);
  } else {
    Serial.printf("[ERROR] ส่ง %s ไม่สำเร็จ: %s\n", sheet,
                  http.errorToString(httpCode).c_str());
  }
  http.end();
}

void uploadAll(unsigned long now) {
  bool inFresh  = inNode.hasData  && (now - inNode.lastMs  < NODE_TIMEOUT_MS);
  bool outFresh = outNode.hasData && (now - outNode.lastMs < NODE_TIMEOUT_MS);
  const char *fanTxt = fanOn ? "ON" : "OFF";

  if (inFresh) {
    sendRowToGoogleSheet("Inside", "UNOR4_INSIDE", inNode.dustEMA,
                         inNode.pressEMA, inNode.tempEMA, fanTxt,
                         controlReason.c_str());
  }
  if (outFresh) {
    sendRowToGoogleSheet("Outside", "ESP32U_OUTSIDE", outNode.dustEMA,
                         outNode.pressEMA, outNode.tempEMA, fanTxt, "-");
  }
}

// =============================================================
// 10. เชื่อมต่อ Wi-Fi
// =============================================================
void connectToWiFi() {
  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_STA);

  Serial.print("กำลังเชื่อมต่อ Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 30) {
    delay(500);
    Serial.print(".");
    count++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] เชื่อมต่อ Wi-Fi สำเร็จ! IP: " +
                   WiFi.localIP().toString());
    Serial.println("Wi-Fi Channel: " + String(WiFi.channel()) +
                   "  <- ตัว ESP32U จะสแกนหา channel นี้เองตอนบูต");
  } else {
    Serial.println("\n[ERROR] เชื่อมต่อ Wi-Fi ไม่สำเร็จ!");
  }
}

// =============================================================
// SETUP
// =============================================================
unsigned long lastEvalMs = 0, lastPrintMs = 0, lastUploadMs = 0;
unsigned long lastWifiRetryMs = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_PIN, INPUT_PULLUP);
  applyRelay(); // fanOn = true ตั้งแต่บูต

  connectToWiFi();

  // *** พิมพ์ MAC Address ออกมา -> เอาไปใส่ในไฟล์ esp32u_node ***
  Serial.println("==============================================");
  Serial.println("  MAC Address ของ MASTER: " + WiFi.macAddress());
  Serial.println("  (เอาค่านี้ไปใส่ MASTER_MAC ใน esp32u_node.ino)");
  Serial.println("==============================================");

  // เริ่ม ESP-NOW (รับอย่างเดียว ไม่ต้อง add peer)
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW init ไม่สำเร็จ");
  } else {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("[SUCCESS] ESP-NOW พร้อมรับข้อมูล");
  }

  // เริ่ม UDP listener (รับจาก UNO R4 WiFi)
  if (udp.begin(UDP_PORT)) {
    Serial.printf("[SUCCESS] UDP พร้อมรับข้อมูลที่ port %d\n", UDP_PORT);
  }
}

// =============================================================
// LOOP
// =============================================================
void loop() {
  unsigned long now = millis();

  // ปุ่ม Reset Wi-Fi
  if (digitalRead(RESET_PIN) == LOW) {
    delay(100);
    if (digitalRead(RESET_PIN) == LOW) {
      connectToWiFi();
      while (digitalRead(RESET_PIN) == LOW) delay(10);
    }
  }

  // WiFi หลุด -> reconnect แบบไม่ block (ESP-NOW ยังทำงานต่อได้)
  if (WiFi.status() != WL_CONNECTED && now - lastWifiRetryMs >= 30000) {
    lastWifiRetryMs = now;
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }

  // รับ UDP ตลอดเวลา
  pollUdp();

  // ประเมินเงื่อนไขควบคุมทุก 1 วินาที
  if (now - lastEvalMs >= 1000) {
    lastEvalMs = now;
    updateControl(now);
  }

  // พิมพ์สถานะทุก 5 วินาที
  if (now - lastPrintMs >= 5000) {
    lastPrintMs = now;
    bool inFresh  = inNode.hasData  && (now - inNode.lastMs  < NODE_TIMEOUT_MS);
    bool outFresh = outNode.hasData && (now - outNode.lastMs < NODE_TIMEOUT_MS);
    Serial.println("------------------------------------------");
    Serial.printf("IN  (UNO R4) : %s ฝุ่น %.1f ug/m3 | %.2f hPa | %.1f *C | seq %lu\n",
                  inFresh ? "OK " : "STALE", inNode.dustEMA, inNode.pressEMA,
                  inNode.tempEMA, (unsigned long)inNode.lastSeq);
    Serial.printf("OUT (ESP32U) : %s ฝุ่น %.1f ug/m3 | %.2f hPa | %.1f *C | seq %lu\n",
                  outFresh ? "OK " : "STALE", outNode.dustEMA, outNode.pressEMA,
                  outNode.tempEMA, (unsigned long)outNode.lastSeq);
    Serial.printf("dP = %+.2f hPa | FAN = %s | %s\n",
                  (inNode.hasData && outNode.hasData) ? currentDP() : 0.0f,
                  fanOn ? "ON" : "OFF", controlReason.c_str());
  }

  // ส่ง Google Sheet ทุก 30 วินาที
  if (now - lastUploadMs >= 30000) {
    lastUploadMs = now;
    uploadAll(now);
  }
}
