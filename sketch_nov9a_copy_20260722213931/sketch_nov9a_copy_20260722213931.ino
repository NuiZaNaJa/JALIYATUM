#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

// =============================================================
// 1. ตั้งค่าประจำเครื่อง (ระบุให้ชัดเจนสำหรับแต่ละตัว)
// =============================================================
// *** สำหรับตัวในห้อง ให้ตั้งเป็น: "ESP32_INSIDE" และ "Inside" ***
// *** สำหรับตัวนอกห้อง ให้ตั้งเป็น: "ESP32_OUTSIDE" และ "Outside" ***

const char* DEVICE_ID    = "ESP32_INSIDE"; // ชื่ออุปกรณ์
const char* TARGET_SHEET = "Inside";       // ชื่อหน้า Sheet ใน Google Sheet (Inside หรือ Outside)

// =============================================================
// 2. ตั้งค่า Wi-Fi และ Google Web App URL
// =============================================================
const char* ssid     = "KMITL-WiFi";     // ใส่ชื่อ Wi-Fi
const char* password = ""; // ใส่รหัสผ่าน Wi-Fi

// วาง URL ที่ได้จากการ Deploy Google Apps Script ตรงนี้
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbzyTPUSDX67G7uUKfZM2-_xwCeX4T6apPWHLiZJ_1EKXglCNGWf1VY6UCopUEL8lEO_VA/exec";

// Hardware Pins
const int RESET_PIN       = 33;
const int DUST_LED_PIN    = 25;
const int DUST_ANALOG_PIN = 34;

// AB129 (BMP180)
Adafruit_BMP085 bmp;
bool isBmpReady = false;

// ตัวแปรจับเวลาส่งข้อมูลขึ้น Google Sheet (ส่งทุกๆ 15 วินาที เพื่อไม่ให้เกิน Quota)
unsigned long previousMillis = 0;
const long interval = 15000; 

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
// ส่งข้อมูลไปยัง Google Sheets
// -------------------------------------------------------------
void sendDataToGoogleSheet(float dust, float pressure, float temp) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    // ตั้งค่า URL โดยเปิดติดตาม Redirect (Google Apps Script จะ Redirect 302)
    http.begin(GOOGLE_SCRIPT_URL);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/json");

    // สร้างข้อมูลรูปแบบ JSON
    String jsonPayload = "{";
    jsonPayload += "\"sheet_name\":\"" + String(TARGET_SHEET) + "\",";
    jsonPayload += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
    jsonPayload += "\"dust\":" + String(dust, 2) + ",";
    jsonPayload += "\"pressure\":" + String(pressure, 2) + ",";
    jsonPayload += "\"temperature\":" + String(temp, 2);
    jsonPayload += "}";

    Serial.println("\n[HTTP] กำลังส่งข้อมูลไปยัง Google Sheets (" + String(TARGET_SHEET) + ")...");
    int httpCode = http.POST(jsonPayload);

    if (httpCode > 0) {
      Serial.printf("[HTTP] ตอบกลับรหัส: %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK || httpCode == 201) {
        Serial.println("[SUCCESS] บันทึกข้อมูลลง Google Sheet เรียบร้อย!");
      }
    } else {
      Serial.printf("[ERROR] ส่งข้อมูลไม่สำเร็จ, Error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
  } else {
    Serial.println("[WARNING] Wi-Fi หลุด! ไม่สามารถส่งข้อมูลได้");
  }
}

// -------------------------------------------------------------
// เชื่อมต่อ Wi-Fi
// -------------------------------------------------------------
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
    Serial.println("\n [SUCCESS] เชื่อมต่อ Wi-Fi สำเร็จ! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n [ERROR] เชื่อมต่อ Wi-Fi ไม่สำเร็จ!");
  }
}

// -------------------------------------------------------------
// SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);

  if (!bmp.begin()) {
    Serial.println("[ERROR] ไม่พบโมดูล AB129 (BMP180)");
    isBmpReady = false;
  } else {
    Serial.println("[SUCCESS] เชื่อมต่อ AB129 (BMP180) สำเร็จ!");
    isBmpReady = true;
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);
  analogReadResolution(12);

  connectToWiFi();
}

// -------------------------------------------------------------
// LOOP
// -------------------------------------------------------------
void loop() {
  // ปุ่ม Reset Wi-Fi
  if (digitalRead(RESET_PIN) == LOW) {
    delay(100);
    if (digitalRead(RESET_PIN) == LOW) {
      connectToWiFi();
      while (digitalRead(RESET_PIN) == LOW) delay(10);
    }
  }

  // อ่านค่าและส่งข้อมูลทุกๆ 15 วินาที
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

    // แสดงผลทาง Serial Monitor
    Serial.println("------------------------------------------");
    Serial.printf("Device ID   : %s\n", DEVICE_ID);
    Serial.printf("Target Sheet: %s\n", TARGET_SHEET);
    Serial.printf("Dust        : %.2f ug/m3\n", dust);
    Serial.printf("Pressure    : %.2f hPa\n", pressure);
    Serial.printf("Temperature : %.2f *C\n", temp);

    // ส่งเข้า Google Sheet
    sendDataToGoogleSheet(dust, pressure, temp);
  }
}