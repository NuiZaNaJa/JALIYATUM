# Positive Pressure Room Controller

ระบบห้องความดันบวกกันฝุ่น PM2.5 อัตโนมัติ ด้วย 3 บอร์ดไมโครคอน트롤러สื่อสารกันแบบ Board-to-Board

> โครงงานมหาวิทยาลัย — สถาบันเทคโนโลยีพระจอมเกล้าเจ้าคุณทหารลาดกระบัง (KMITL)

---

## ภาพรวมระบบ

ระบบวัดค่าฝุ่น PM2.5 และความดันอากาศภายใน/ภายนอกห้อง แล้วสั่งเปิด-ปิดพัดลมอัตโนมัติเพื่อสร้างความดันบวก ป้องกันฝุ่นจากภายนอกเข้าสู่ห้อง

```
┌─────────────────────────────────────────────────────────────────┐
│                     สถาปัตยกรรมระบบ                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   [OUTSIDE]                  [MASTER]                  [INSIDE] │
│   ESP32U                     ESP32 Dev                UNO R4    │
│   ┌──────────┐              ┌──────────┐             ┌─────────┐│
│   │ GP2Y1014 │──ESP-NOW──▶  │          │◀──UDP────   │ GP2Y1014││
│   │ BMP180   │              │  Relay   │── Relay ──▶ │  [พัดลม] ││
│   │          │              │  WiFi    │── HTTP ───▶ │  Google ││
│   └──────────┘              │  Google  │             │ BMP180  ││
│       ฝั่งนอก                 └──────────┘             └─────────┘│
│                               ตัวกลาง                   ฝั่งในห้อง  │
│                                                                 │
│   ช่องทางสื่อสาร:                                                  │
│     ESP32U  → MASTER  : ESP-NOW (Board-to-Board)                │
│     UNO R4  → MASTER  : UDP Broadcast (ผ่าน WiFi บ้าน)            │
│     MASTER  → Internet: WiFi STA → Google Sheets (HTTP POST)    │
└─────────────────────────────────────────────────────────────────┘
```

---

## อุปกรณ์ที่ต้องใช้

| บอร์ด | รุ่น | หน้าที่ | ต่อ WiFi |
|-------|------|--------|---------|
| ESP32U | ESP32 Dev Module | วัดค่านอกห้อง + ส่ง ESP-NOW | ไม่ต้อง |
| ESP32 (Master) | ESP32 Dev Module | รับข้อมูล + ควบคุม Relay + ส่ง Google Sheets | ใช่ (ตัวเดียว) |
| UNO R4 WiFi | Arduino UNO R4 WiFi | วัดค่าในห้อง + ส่ง UDP | ใช่ |

### เซ็นเซอร์ (ทุกบอร์ด)

- **GP2Y1014** — วัดฝุ่น PM2.5 (วัดได้ทั้งในและนอก)
- **BMP180 (GY-68)** — วัดความดันอากาศ + อุณหภูมิ

### อุปกรณ์เพิ่มเติม

- **Relay Module 1 ช่อง** — ต่อพัดลม
- **LED** — แสดงสถานะพัดลม (บนบอร์ด Master)

---

## การต่อสาย

เปิดไฟล์ `manual.html` ในโฟลเดอร์โปรเจกต์เพื่อดูแผนผัง SVG ขนาดใหญ่พร้อมสี区分ขาทุกจุด

### สรุปขาต่อ

| ขา | ESP32U (นอกห้อง) | UNO R4 (ในห้อง) | ESP32 Master |
|----|------------------|-----------------|-------------|
| GP2Y1014 LED | GPIO 25 | D7 | — |
| GP2Y1014 Vo | GPIO 34 | A0 | — |
| GP2Y1014 VCC | 3.3V | 5V (GY-68 มี regulator) | — |
| BMP180 SDA | GPIO 21 | SDA | — |
| BMP180 SCL | GPIO 22 | SCL | — |
| BMP180 VCC | 3.3V | 5V (GY-68 มี regulator) | — |
| Relay IN | — | — | GPIO 26 |
| LED (สถานะ) | — | — | GPIO 2 |
| ปุ่ม Reset Wi-Fi | GPIO 33 | — | GPIO 33 |

---

## ตรรกะควบคุมพัดลม

```
สถานะเริ่มต้นบูต: พัดลม ON (ปลอดภัยเรื่องฝุ่น)

   ┌──────────────────────────────────────────────────────┐
   │                                                      │
   │  พัดลม ON อยู่                                          │
   │  ├─ ปิดพัดลม เมื่อ: ฝุ่นในห้อง < นอกห้อง                     │
   │  │          ความดันต่าง > 0.30 hPa (คนเริ่มอึดอัด)         │
   │  │                                                   │
   │  พัดลม OFF อยู่                                         │
   │  ├─ เปิดพัดลมกลับ เมื่อ: ฝุ่นในห้อง >= นอกห้อง                │
   │  │               หรือ ความดันต่าง < 0.15 hPa            │
   │  │                                                   │
   │  Fail-safe: เซ็นเซอร์เงียบ > 30 วินาที                    │
   │  └─ เปิดพัดลมไว้ (เสี่ยงฝุ่นน้อยกว่าเสี่ยงรั่ว)                   │
   └──────────────────────────────────────────────────────┘

ค่าที่ปรับแต่งได้ในโค้ด:
  DP_OFF_HPA  = 0.30  (ปิดพัดลมเมื่อความดันต่างเกินนี้)
  DP_ON_HPA   = 0.15  (เปิดพัดลมกลับเมื่อความดันตกลง)
  DUST_MARGIN = 3.0   (ฝุ่นต้องน้อยกว่าเท่านี้ถึงถือว่าสะอาด)
  PRESSURE_OFFSET = 0.0 (คาริเบรตความดัน baseline)
```

---

## การติดตั้ง

### 1. ติดตั้ง Arduino Core

- **ESP32U + Master**: ติดตั้ง **esp32** by Espressif Systems ผ่าน Boards Manager
- **UNO R4 WiFi**: ติดตั้ง **Arduino Renesas boards** ผ่าน Boards Manager

### 2. ติดตั้งไลบรารี

| ไลบรารี | ผู้สร้าง | ใช้กับ |
|----------|---------|--------|
| Adafruit BMP085 Library | Adafruit | ทุกบอร์ด |

### 3. แก้ไขค่าในโค้ด

**esp32_master.ino** (ส่วนที่ต้องแก้):
```cpp
const char *ssid     = "ชื่อ WiFi ของคุณ";
const char *password = "รหัส WiFi";

// URL จาก Google Apps Script (ขั้นตอนที่ 5)
const String GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/.../exec";
```

**esp32u_node.ino**:
```cpp
const char *WIFI_SSID = "ชื่อ WiFi เดียวกับ Master";  // ใช้สำหรับสแกน channel
uint8_t MASTER_MAC[] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX};  // ใส่ MAC ของ Master
```

**unor4_node.ino**:
```cpp
const char *ssid     = "ชื่อ WiFi เดียวกับ Master";
const char *password = "รหัส WiFi";  // ใส่ "" สำหรับ WiFi สาธารณะ (open)
```

### 4. อัปโหลดโค้ด (ตามลำดับ)

```
ขั้นตอนที่ 1: อัปโหลด esp32_master.ino
  - เลือกบอร์ด: ESP32 Dev Module
  - เปิด Serial Monitor (115200 baud) ตอนบูต
  - จด MAC Address ที่พิมพ์ออกมา
  - จด Wi-Fi Channel ที่ต่อสำเร็จ

ขั้นตอนที่ 2: อัปโหลด esp32u_node.ino
  - เลือกบอร์ด: ESP32 Dev Module
  - ใส่ MASTER_MAC จากขั้นตอนที่ 1

ขั้นตอนที่ 3: อัปโหลด unor4_node.ino
  - เลือกบอร์ด: Arduino UNO R4 WiFi
```

### 5. ตั้งค่า Google Sheets

1. เปิด Google Sheet ใหม่ → **Extensions → Apps Script**
2. ลบโค้ดเก่าทั้งหมด แล้ววางเนื้อหาจากไฟล์ `code.gs` ในโปรเจกต์
3. **Save**
4. **Deploy → New deployment → Web app**
   - Execute as: **Me**
   - Who has access: **Anyone**
5. คัดลอก URL ที่ได้ไปใส่ใน `GOOGLE_SCRIPT_URL` ใน `esp32_master.ino`
6. อัปโหลด master ซ้ำอีกครั้ง

> สำคัญ: เมื่อแก้โค้ด Apps Script ต้อง Deploy → Manage deployments → **New version** ทุกครั้ง ไม่งั้น Web App จะยังรันโค้ดเก่า

---

## การคาริเบรต

### ความดันอากาศ (BMP180)

1. ปิดพัดลม เปิดประตูให้ความดันในห้อง = นอกห้อง
2. ดูค่า `dP` ใน Serial Monitor ของ Master
3. เอาค่าที่เห็นมาใส่ใน `PRESSURE_OFFSET_HPA` ให้ dP กลายเป็น `~0.00`

### ค่าฝุ่น (GP2Y1014)

ค่า baseline (แรงดันตอนไม่มีฝุ่น) ต่างกันตามตัวเซ็นเซอร์ วิธีปรับ:

1. วางเซ็นเซอร์ในที่อากาศสะอาด (เช่นหน้าเครื่องฟอกอากาศ)
2. แก้โค้ดชั่วคราวให้พิมพ์ raw voltage ออกมา
3. เอาค่า voltage ที่อ่านได้มาใส่ใน `baseline` ของฟังก์ชัน `readDustDensity()`

---

## การแก้ไขปัญหา

| อาการ | สาเหตุที่เป็นไปได้ | วิธีแก้ |
|-------|------------------|--------|
| BMP180 = 0.00 hPa | สาย SDA/SCL สลับ / ไม่มีไฟเลี้ยง / บัดกรีไม่ติด | ดูผล I2C scan ตอนบูต (address 0x77) |
| ESP-NOW ส่งไม่สำเร็จทุกครั้ง | MASTER_MAC ผิด / Channel ไม่ตรง / Master ยังไม่ได้ flash | จด MAC จาก Master → เทียบในโค้ด node |
| WiFi ต่อไม่ได้ (UNO R4) | WiFi สาธารณะ (open) ต้องใช้ `WiFi.begin(ssid)` โดยไม่ส่ง password | เปลี่ยนเป็น `WiFi.begin(ssid)` (ไม่ต้องใส่ "" ว่าง) |
| Google Sheets ค่าว่าง | Apps Script ยังเป็นเวอร์ชันเก่า | วาง code.gs ใหม่ → Deploy New version |
| ค่าฝุ่นสูงผิดปกติ ทั้งที่อากาศดี | baseline ไม่ตรงกับตัวจริง / A0 ลอย (ไม่ต่อเซ็นเซอร์) | Calibrate baseline ตามวิธีด้านบน |

---

## โครงสร้างไฟล์

```
JALIYATUM/
├── esp32_master/
│   └── esp32_master.ino      # ตัวกลาง: รับข้อมูล + ควบคุม Relay + ส่ง Google Sheets
├── esp32u_node/
│   └── esp32u_node.ino        # ตัววัดนอกห้อง: ส่ง ESP-NOW
├── unor4_node/
│   └── unor4_node.ino         # ตัววัดในห้อง: ส่ง UDP Broadcast
├── code.gs                    # Google Apps Script สำหรับรับข้อมูลลงชีต
├── esp32_inandoutsite.ino     # โค้ดเดิมอ้างอิง (ตัวเดียววัดทั้งในและนอก)
├── manual.html                # คู่มือพร้อมแผนผัง SVG
└── README.md
```

---

## ไฟล์ที่เกี่ยวข้อง

- **`code.gs`** — Google Apps Script สำหรับรับ JSON จาก Master ลง Google Sheet (สร้างชีต + หัวตารางอัตโนมัติ)
- **`manual.html`** — คู่มือต่อสาย hardware แบบละเอียดพร้อมแผนผัง SVG ขนาดใหญ่ (เปิดในเบราว์เซอร์)

---

## License

MIT License — ใช้ แก้ไข และแจกจ่ายได้โดยไม่มีค่าใช้จ่าย
