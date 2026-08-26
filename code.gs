// =============================================================
//  Google Apps Script — รับข้อมูลจาก ESP32 MASTER ลง Google Sheet
// -------------------------------------------------------------
//  วิธีใช้:
//   1. เปิด Google Sheet > Extensions > Apps Script แล้ววางโค้ดนี้
//   2. Deploy > New deployment > เลือก "Web app"
//      - Execute as: Me
//      - Who has access: Anyone
//   3. เอา URL ที่ได้ไปใส่ GOOGLE_SCRIPT_URL ใน esp32_master.ino
//
//  รูปแบบ JSON ที่รับ (POST):
//   { "sheet_name": "Inside", "device_id": "UNOR4_INSIDE",
//     "dust": 12.3, "pressure": 1013.25, "temperature": 30.5,
//     "fan": "ON", "note": "-" }
//  (fan/note ไม่ส่งมาก็ได้ — รองรับ payload แบบเก่าด้วย)
// =============================================================

function doPost(e) {
  try {
    var data = JSON.parse(e.postData.contents);

    var ss = SpreadsheetApp.getActiveSpreadsheet();
    var sheetName = data.sheet_name || "Log";
    var sheet = ss.getSheetByName(sheetName);

    // ถ้ายังไม่มีชีตชื่อนั้น ให้สร้างใหม่อัตโนมัติ
    if (!sheet) {
      sheet = ss.insertSheet(sheetName);
    }

    // ใส่หัวตารางให้แถวแรกอัตโนมัติ
    if (sheet.getLastRow() === 0) {
      sheet.appendRow([
        "Timestamp",
        "Device ID",
        "Dust (ug/m3)",
        "Pressure (hPa)",
        "Temperature (C)",
        "Fan",
        "Note"
      ]);
    }

    sheet.appendRow([
      new Date(),
      data.device_id || "",
      data.dust != null ? data.dust : "",
      data.pressure != null ? data.pressure : "",
      data.temperature != null ? data.temperature : "",
      data.fan || "",
      data.note || ""
    ]);

    return ContentService
      .createTextOutput(JSON.stringify({ status: "success" }))
      .setMimeType(ContentService.MimeType.JSON);

  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "error", message: String(err) }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

// เอาไว้ทดสอบว่า Web App ทำงานอยู่ (เปิด URL ใน browser ได้เลย)
function doGet() {
  return ContentService.createTextOutput("OK - PM2.5 Positive Pressure Logger");
}
