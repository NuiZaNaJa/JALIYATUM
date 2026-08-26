#include <WiFiS3.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

// =============================================================
//  UNO R4 WiFi NODE (ตัววัด "ในห้อง") — ส่งค่าให้ ESP32 MASTER ผ่าน UDP
// -------------------------------------------------------------
//  * UNO R4 WiFi ใช้ ESP-NOW ไม่ได้ (ชิป ESP32-S3 บนบอร์ดเป็นแค่
//    WiFi bridge ผ่าน firmware ของ Arduino ไม่เปิด ESP-NOW ให้ใช้)
//    จึงส่งแบบ "UDP Broadcast" ผ่าน WiFi บ้านแทน
//    -> ไม่ต้องรู้ IP ของ MASTER, MASTER ที่อยู่วงเดียวกันรับได้เลย
//  * ถ้าอยากเอาตัวนี้ไปวัด "นอกห้อง" แทน แค่เปลี่ยน NODE_ID เป็น "OUTSIDE"
// =============================================================

const char *NODE_ID = "INSIDE"; // ชื่อ node ("INSIDE" หรือ "OUTSIDE")

// Wi-Fi บ้าน (ตัวเดียวกับที่ ESP32 MASTER ต่อ)
const char *ssid = "KMITL-WiFi";   // ใส่ชื่อ Wi-Fi
const char *password = ""; // ใส่รหัสผ่าน Wi-Fi

const uint16_t UDP_PORT = 4210; // ต้องตรงกับ UDP_PORT ฝั่ง MASTER

// Hardware Pins
const int DUST_LED_PIN = 7;     // ขาคุม LED ของ GP2Y1014
const int DUST_ANALOG_PIN = A0; // ขาอ่านค่า Analog

// AB129 (BMP180) — ต่อขา SDA/SCL ของบอร์ด (โมดูล GY-68 มี level shifter ในตัว ใช้ 5V ได้)
Adafruit_BMP085 bmp;
bool isBmpReady = false;

WiFiUDP udp;
uint32_t seq = 0;

// ส่งข้อมูลทุกๆ 5 วินาที
unsigned long previousMillis = 0;
const long interval = 5000;

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

  // UNO R4 อ้างอิงแรงดัน ADC ที่ 5V (ต่างจาก ESP32 ที่ใช้ 3.3V)
  float voltage = rawValue * (5.0 / 4095.0);

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
// หา Broadcast IP ของวงแลน (เช่น 192.168.1.255) อัตโนมัติ
// -------------------------------------------------------------
IPAddress getBroadcastIP() {
  IPAddress ip = WiFi.localIP();
  IPAddress sn = WiFi.subnetMask();
  return IPAddress((uint8_t)(ip[0] | ~sn[0]), (uint8_t)(ip[1] | ~sn[1]),
                   (uint8_t)(ip[2] | ~sn[2]), (uint8_t)(ip[3] | ~sn[3]));
}

// -------------------------------------------------------------
// เชื่อมต่อ Wi-Fi (เรียกซ้ำได้ ถ้าต่ออยู่แล้วจะข้าม)
// -------------------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("กำลังเชื่อมต่อ Wi-Fi: ");
  Serial.println(ssid);

  // เครือข่าย open (ไม่มีรหัส) ต้องเรียก WiFi.begin(ssid) "โดยไม่ส่ง password"
  // — บน UNO R4 ถ้าส่ง password ว่าง "" จะเชื่อมต่อไม่ได้ (ต่างจาก ESP32)
  if (strlen(password) == 0) {
    WiFi.begin(ssid);
  } else {
    WiFi.begin(ssid, password);
  }

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 30) {
    delay(500);
    Serial.print(".");
    count++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SUCCESS] เชื่อมต่อ Wi-Fi สำเร็จ! IP: " +
                   WiFi.localIP().toString());
    Serial.println("Broadcast IP: " + getBroadcastIP().toString());
  } else {
    Serial.print("\n[ERROR] เชื่อมต่อ Wi-Fi ไม่สำเร็จ! status = ");
    Serial.println(WiFi.status());
    Serial.println("(1=หา SSID ไม่เจอ 4=CONNECT_FAILED 6=DISCONNECTED)");
  }
}

// -------------------------------------------------------------
// SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(); // ใช้ขา SDA/SCL ของบอร์ด UNO R4

  if (!bmp.begin()) {
    Serial.println("[ERROR] ไม่พบโมดูล AB129 (BMP180)");
    isBmpReady = false;
  } else {
    Serial.println("[SUCCESS] เชื่อมต่อ AB129 (BMP180) สำเร็จ!");
    isBmpReady = true;
  }

  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);
  analogReadResolution(12); // อ่านแบบ 12-bit (0-4095) ให้เหมือน ESP32

  connectWiFi();
  udp.begin(UDP_PORT);
}

// -------------------------------------------------------------
// LOOP
// -------------------------------------------------------------
void loop() {
  // WiFi หลุด -> ต่อใหม่อัตโนมัติ
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    float dust = readDustDensity();
    float pressure = 0.0;
    float temp = 0.0;

    if (isBmpReady) {
      temp = bmp.readTemperature();
      pressure = bmp.readPressure() / 100.0F; // hPa
    }
    seq++;

    // ส่งแบบ CSV: "NODE,dust,pressure,temp,seq"
    String msg = String(NODE_ID) + "," + String(dust, 2) + "," +
                 String(pressure, 2) + "," + String(temp, 2) + "," +
                 String(seq);

    if (WiFi.status() == WL_CONNECTED) {
      udp.beginPacket(getBroadcastIP(), UDP_PORT);
      udp.write((const uint8_t *)msg.c_str(), msg.length());
      udp.endPacket();
    }

    // แสดงผลทาง Serial Monitor
    // (UNO R4 ไม่มี Serial.printf — ใช้ print ธรรมดา)
    Serial.println("------------------------------------------");
    Serial.print("Node ID     : ");
    Serial.println(NODE_ID);
    Serial.print("Dust        : ");
    Serial.print(dust, 2);
    Serial.println(" ug/m3");
    Serial.print("Pressure    : ");
    Serial.print(pressure, 2);
    Serial.println(" hPa");
    Serial.print("Temperature : ");
    Serial.print(temp, 1);
    Serial.println(" *C");
    Serial.println("[UDP] ส่ง: " + msg);
  }
}
