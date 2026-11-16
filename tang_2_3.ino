/*
 * ============================================================================
 * FILE: tang_2_3.ino
 * MẠCH ESP8266 SỐ 3 (TẦNG 2 & 3) - GỘP NGOẠI VI TẦNG 2 VÀ TẦNG 3 TRÊN 1 ESP8266
 * ============================================================================
 * 1. MỤC ĐÍCH:
 *    Vi điều khiển ESP8266 NodeMCU tích hợp toàn bộ thiết bị ngoại vi của cả
 *    Tầng 2 (Chiếu sáng, Báo cháy khí Gas/Khói MQ-2, Còi Buzzer, Màn hình OLED)
 *    và Tầng 3 (Khóa cửa vân tay AS608 + Động cơ Servo SG90).
 *
 * 2. DANH SÁCH THIẾT BỊ & SƠ ĐỒ ĐẤU NỐI CHÂN (ESP8266 NODEMCU):
 *    +-----------------------------------------------------------------------+
 *    | Chân ESP8266 | GPIO   | Thiết bị ngoại vi       | Chân trên Module    |
 *    +--------------+--------+-------------------------+---------------------+
 *    | A0           | ADC0   | [Tầng 2] Cảm biến MQ-2  | AO (Analog Output)  |
 *    | D1           | GPIO5  | [Chung]  Còi Buzzer     | Chân Dương (+) Còi  |
 *    | D2           | GPIO4  | [Tầng 2] OLED 0.96" I2C | SDA (0x3C, tùy chọn)|
 *    | D3           | GPIO0  | [Tầng 2] OLED 0.96" I2C | SCL (0x3C, tùy chọn)|
 *    | D4           | GPIO2  | [Tầng 3] Servo Cửa T3   | Dây Cam/Vàng (PWM)  |
 *    | D5           | GPIO14 | [Tầng 2] Nút nhấn đèn   | Nút nhấn (nối GND)  |
 *    | D6           | GPIO12 | [Tầng 2] Đèn LED tầng 2 | SIG/IN (MOSFET) / + |
 *    | D7           | GPIO13 | [Tầng 3] Vân tay AS608  | TX của cảm biến     |
 *    | D8           | GPIO15 | [Tầng 3] Vân tay AS608  | RX của cảm biến     |
 *    +-----------------------------------------------------------------------+
 *    * Mẹo đăng ký vân tay AS608:
 *      Mở Serial Monitor (115200 baud) và gõ lệnh "ENROLL <id>" (ví dụ: ENROLL 1).
 *
 * 3. ĐƯỜNG DẪN ĐỒNG BỘ FIREBASE REALTIME DATABASE:
 *    - [light2] /home/floor_2/light_room_2     (Đọc/Ghi bool: true = Sáng, false = Tắt)
 *    - [smoke]  /home/floor_2/smoke_level_2    (Ghi int: Giá trị Analog MQ-2 từ 0..1023)
 *               /home/floor_2/smoke_detected_2 (Ghi bool: true khi vượt ngưỡng khói 120)
 *               /home/floor_2/buzzer_mute_2    (Đọc bool: true = Tắt tiếng còi từ Web)
 *    - [door3]  /home/door_status              (Ghi bool: true khi mở cửa vân tay 2s)
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <Adafruit_Fingerprint.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==== 1. Cấu hình Wi-Fi + Firebase ====
#define WIFI_SSID      "Iphone"
#define WIFI_PASSWORD  "112345678"
#define API_KEY        "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY"
#define DATABASE_URL   "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_HOST  "smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app"

// true: Rules nhánh "home" đang mở (.read/.write = true), không cần đăng nhập.
// false: Rules yêu cầu đăng nhập -> điền đúng email + mật khẩu THẬT bên dưới.
#define USE_FIREBASE_TEST_MODE true
#define USER_EMAIL     "nvtngang@hcmus.edu.vn"
#define USER_PASSWORD  "12345678"

// ==== 2. Đường dẫn Firebase chuẩn theo bảng thiết bị Tầng 2 & Tầng 3 ====
#define PATH_LIGHT_2          "/home/floor_2/light_room_2"     // [light2] Đèn LED tầng 2 (bool)
#define PATH_SMOKE_LEVEL_2    "/home/floor_2/smoke_level_2"    // [smoke]  Mức khói MQ-2 tầng 2 (int)
#define PATH_SMOKE_DETECTED_2 "/home/floor_2/smoke_detected_2" // [smoke]  Cờ phát hiện khói tầng 2 (bool)
#define PATH_BUZZER_MUTE_2    "/home/floor_2/buzzer_mute_2"    // [smoke]  Cờ tắt tiếng còi báo khói từ Web (bool)
#define PATH_DOOR_3_STATUS    "/home/door_status"              // [door3]  Trạng thái khóa cửa vân tay tầng 3 (bool)

// ==== 3. Sơ đồ chân trên 1 ESP8266 NodeMCU (Gộp Tầng 2 + Tầng 3, không trùng chân) ====
#define SMOKE_ANALOG_PIN      A0    // ADC0   - Chân Analog đọc cảm biến khói MQ-2 (Tầng 2)
#define BUZZER_PIN            D1    // GPIO5  - Còi Buzzer chung (Báo khói Tầng 2 + Báo vân tay Tầng 3)
#define I2C_SDA_PIN           D2    // GPIO4  - SDA cho màn hình OLED 0.96" Tầng 2 (nếu lắp kèm)
#define I2C_SCL_PIN           D3    // GPIO0  - SCL cho màn hình OLED 0.96" Tầng 2 (nếu lắp kèm)
#define SERVO_DOOR_PIN        D4    // GPIO2  - Servo SG90 mở khóa cửa vân tay (Tầng 3)
#define SWITCH_BTN_PIN        D5    // GPIO14 - Nút nhấn vật lý bật/tắt đèn Tầng 2 (nối GND)
#define LIGHT_RELAY_PIN       D6    // GPIO12 - Đèn LED / Relay Tầng 2
#define FINGER_RX_PIN         D7    // GPIO13 - SoftwareSerial RX (nối chân TX của AS608 Tầng 3)
#define FINGER_TX_PIN         D8    // GPIO15 - SoftwareSerial TX (nối chân RX của AS608 Tầng 3)

// ==== 4. Thông số hoạt động ====
#define SMOKE_THRESHOLD       120   // Ngưỡng báo khói MQ-2 (0..1023)
#define SERVO_OPEN_POS        180   // Góc mở cửa vân tay Tầng 3
#define SERVO_CLOSE_POS       0     // Góc đóng cửa vân tay Tầng 3
#define DOOR_OPEN_DURATION    2000  // Giữ cửa vân tay mở 2 giây rồi tự đóng

#define POLL_MS               500   // Chu kỳ đọc lệnh từ Firebase (500ms)
#define SENSOR_READ_MS        500   // Chu kỳ đọc cảm biến khói MQ-2 (500ms)
#define FINGER_SCAN_MS        150   // Chu kỳ quét vân tay AS608 (150ms)
#define DEBOUNCE_DELAY        50    // Chống dội nút nhấn vật lý (50ms)

#define OLED_I2C_ADDR         0x3C
#define SCREEN_WIDTH          128
#define SCREEN_HEIGHT         64

// ==== 5. Khởi tạo đối tượng phần cứng ====
BearSSL::WiFiClientSecure tlsClient;
HTTPClient http;

SoftwareSerial fingerSerial(FINGER_RX_PIN, FINGER_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
Servo doorServo;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledAvailable = false;

// ==== 6. Biến trạng thái hệ thống ====
String authToken = "";
unsigned long lastTokenTime = 0;
bool firebaseReadyFlag = false;
bool initialSyncDone = false;
unsigned long lastPoll = 0;

// --- [1] Đèn LED Tầng 2 & Nút nhấn vật lý ---
bool roomLight2State = false;
bool light2SyncDirty = false;
int buttonState = HIGH;
int lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;

// --- [2] Cảm biến khói MQ-2 & Còi Buzzer ---
int currentSmokeLevel = 0;
int lastSyncedSmokeLevel = -100;
bool smokeDetected = false;
bool lastSyncedSmokeDetected = false;
bool firstSmokeSync = false;
bool buzzerMuteFromWeb = false;
unsigned long lastSensorReadTime = 0;

// Còi kêu ngắn (khi quét vân tay) kết hợp còi báo khói
bool shortBeepActive = false;
unsigned long shortBeepStart = 0;
unsigned long shortBeepDuration = 0;

// --- [3] Cảm biến vân tay AS608 & Cửa Tầng 3 ---
bool fingerprintSensorReady = false;
int lastFingerprintID = -1;
bool isDoor3Open = false;
unsigned long door3OpenedAt = 0;
unsigned long lastFingerScanTime = 0;
bool door3StatusDirty = false;
bool door3TargetVal = false;

// --- Hiệu ứng màn hình OLED 0.96" ---
int oledState = 0;
unsigned long oledStateStartTime = 0;
int oledProgress = 0;
int quoteIndex = 0;
unsigned long lastOledFrameTime = 0;
const char* quotes[] = {
  "Chao mung ban den voi khong gian song ly tuong.",
  "Hay yeu ngoi nha cua ban nhu yeu chinh ban than minh."
};
const int quoteCount = sizeof(quotes) / sizeof(quotes[0]);

// Theo dõi log Firebase
String seenLight2 = "", seenMute2 = "";
unsigned long readOk = 0, readFail = 0, lastErrPrint = 0;
unsigned long lastWifiPrint = 0;
String lastHttpError = "";

// ==== 7. Các hàm Log & REST API Firebase (GET / PUT) ====
void logRead(const char *path, String &seen, const String &val) {
  readOk++;
  if (val == seen) return;
  Serial.printf("📥 [Firebase DOC] %s : %s -> %s (OK %lu)\n",
                path, seen.length() ? seen.c_str() : "(chua co)", val.c_str(), readOk);
  seen = val;
}

void logReadError(const char *path) {
  readFail++;
  if (millis() - lastErrPrint < 3000) return;
  lastErrPrint = millis();
  Serial.printf("⚠️ [Firebase LOI] %s : %s | OK %lu / loi %lu | RAM %u\n",
                path, lastHttpError.c_str(), readOk, readFail, ESP.getFreeHeap());
}

String buildFirebaseUrl(const char *path) {
  String url = String(DATABASE_URL) + String(path) + ".json";
  if (!USE_FIREBASE_TEST_MODE && authToken.length() > 0) {
    url += "?auth=" + authToken;
  }
  return url;
}

bool fbGetRaw(const char *path, String &outPayload) {
  String url = buildFirebaseUrl(path);
  http.useHTTP10(true);
  http.setReuse(true);
  http.setTimeout(5000);

  if (!http.begin(tlsClient, url)) {
    lastHttpError = "http.begin failed";
    return false;
  }

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    outPayload = http.getString();
    outPayload.trim();
    http.end();
    return true;
  } else if (code > 0) {
    String errBody = http.getString();
    errBody.trim();
    lastHttpError = "HTTP " + String(code) + " " + errBody;
    http.end();
    return false;
  } else {
    lastHttpError = http.errorToString(code);
    http.end();
    return false;
  }
}

bool fbPutRaw(const char *path, const String &jsonPayload) {
  if (WiFi.status() != WL_CONNECTED || !firebaseReadyFlag) return false;
  String url = buildFirebaseUrl(path);
  http.useHTTP10(true);
  http.setReuse(true);
  http.setTimeout(5000);

  if (!http.begin(tlsClient, url)) {
    lastHttpError = "http.begin failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(jsonPayload);
  http.end();

  if (code == HTTP_CODE_OK) {
    Serial.printf("📤 [Firebase GHI] %s = %s -> OK\n", path, jsonPayload.c_str());
    return true;
  } else {
    lastHttpError = (code > 0) ? ("HTTP " + String(code)) : http.errorToString(code);
    Serial.printf("❌ [Firebase GHI] %s = %s -> LOI (%s)\n", path, jsonPayload.c_str(), lastHttpError.c_str());
    return false;
  }
}

bool fbSetBool(const char *path, bool val) {
  return fbPutRaw(path, val ? "true" : "false");
}

bool fbSetInt(const char *path, int val) {
  return fbPutRaw(path, String(val));
}

bool fbGetBool(const char *path, bool &outVal, bool defaultIfNull = false) {
  String raw;
  if (!fbGetRaw(path, raw)) return false;
  if (raw == "true") {
    outVal = true;
    return true;
  } else if (raw == "false") {
    outVal = false;
    return true;
  } else if (raw == "null") {
    Serial.printf("ℹ️ [Firebase] Node %s chua co (null) -> Tu dong khoi tao = %s\n",
                  path, defaultIfNull ? "true" : "false");
    fbSetBool(path, defaultIfNull);
    outVal = defaultIfNull;
    return true;
  }
  lastHttpError = "Gia tri khong phai bool (" + raw + ")";
  return false;
}

bool loginFirebaseEmail() {
  Serial.printf("[Firebase] Dang dang nhap Email: %s ...\n", USER_EMAIL);
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(API_KEY);
  String body = "{\"email\":\"" + String(USER_EMAIL) + "\",\"password\":\"" + String(USER_PASSWORD) + "\",\"returnSecureToken\":true}";

  http.useHTTP10(true);
  http.setTimeout(8000);
  if (!http.begin(tlsClient, url)) {
    Serial.println(F("❌ [Firebase Auth] Khong mo duoc ket noi toi identitytoolkit"));
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  if (code == HTTP_CODE_OK) {
    int idx = resp.indexOf("\"idToken\"");
    if (idx >= 0) {
      int colon = resp.indexOf(':', idx);
      int startQuote = resp.indexOf('"', colon + 1);
      int endQuote = resp.indexOf('"', startQuote + 1);
      if (startQuote >= 0 && endQuote > startQuote) {
        authToken = resp.substring(startQuote + 1, endQuote);
        lastTokenTime = millis();
        Serial.println(F("✅ [Firebase Auth] Dang nhap Email THANH CONG! Da lay duoc idToken."));
        return true;
      }
    }
  }
  Serial.printf("❌ [Firebase Auth] Loi dang nhap (HTTP %d): %s\n", code, resp.c_str());
  return false;
}

// ==== 8. Điều khiển Còi Buzzer (D1) & Đèn Tầng 2 (D6) ====
void updateBuzzerOutput() {
  bool smokeAlarmBeep = smokeDetected && !buzzerMuteFromWeb;
  bool shouldBeep = smokeAlarmBeep || shortBeepActive;
  digitalWrite(BUZZER_PIN, shouldBeep ? HIGH : LOW);
}

void triggerShortBeep(unsigned long durationMs = 300) {
  shortBeepActive = true;
  shortBeepStart = millis();
  shortBeepDuration = durationMs;
  updateBuzzerOutput();
}

void setRoomLight2(bool turnOn, bool markDirtyForFirebase = false) {
  if (roomLight2State == turnOn && !markDirtyForFirebase) return;
  roomLight2State = turnOn;
  digitalWrite(LIGHT_RELAY_PIN, turnOn ? HIGH : LOW);
  Serial.printf("💡 [Den Tang 2 (D6) - %s] -> %s\n", PATH_LIGHT_2, turnOn ? "BAT" : "TAT");
  if (markDirtyForFirebase) {
    light2SyncDirty = true;
  }
}

// ==== 9. Xử lý Nút nhấn vật lý (D5) & Cảm biến khói MQ-2 (A0) - Tầng 2 ====
void handlePhysicalSwitch() {
  int reading = digitalRead(SWITCH_BTN_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        bool newState = !roomLight2State;
        setRoomLight2(newState, true);
      }
    }
  }
  lastButtonReading = reading;

  if (light2SyncDirty && WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    if (fbSetBool(PATH_LIGHT_2, roomLight2State)) {
      seenLight2 = roomLight2State ? "true" : "false";
      light2SyncDirty = false;
    }
  }
}

void handleSmokeSensor() {
  unsigned long now = millis();
  if (now - lastSensorReadTime < SENSOR_READ_MS) return;
  lastSensorReadTime = now;

  currentSmokeLevel = analogRead(SMOKE_ANALOG_PIN);
  bool detected = (currentSmokeLevel > SMOKE_THRESHOLD);

  if (detected != smokeDetected) {
    smokeDetected = detected;
    if (!smokeDetected) {
      buzzerMuteFromWeb = false; // Tự động bỏ mute khi hết khói
    }
    Serial.printf("🚨 [MQ-2 Tang 2] %s (Muc khoi: %d)\n",
                  smokeDetected ? "PHAT HIEN KHOI/GAS!" : "Khong khi binh thuong", currentSmokeLevel);
  }

  updateBuzzerOutput();

  // Đồng bộ mức khói & trạng thái khói lên Firebase
  if (WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    if (!firstSmokeSync || smokeDetected != lastSyncedSmokeDetected) {
      if (fbSetBool(PATH_SMOKE_DETECTED_2, smokeDetected)) {
        lastSyncedSmokeDetected = smokeDetected;
        firstSmokeSync = true;
      }
    }
    if (abs(currentSmokeLevel - lastSyncedSmokeLevel) >= 5) {
      if (fbSetInt(PATH_SMOKE_LEVEL_2, currentSmokeLevel)) {
        lastSyncedSmokeLevel = currentSmokeLevel;
      }
    }
  }
}

// ==== 10. Xử lý Cảm biến vân tay AS608 (D7/D8) & Cửa Servo (D4) - Tầng 3 ====
void openDoor3ByFingerprint() {
  doorServo.write(SERVO_OPEN_POS);
  isDoor3Open = true;
  door3OpenedAt = millis();
  Serial.println(F("🔓 [Cua Van Tay Tang 3] DANG MO (2 giay)..."));

  door3StatusDirty = true;
  door3TargetVal = true;
}

void closeDoor3() {
  doorServo.write(SERVO_CLOSE_POS);
  isDoor3Open = false;
  Serial.println(F("🔒 [Cua Van Tay Tang 3] DA DONG."));

  door3StatusDirty = true;
  door3TargetVal = false;
}

int getFingerprintID() {
  if (!fingerprintSensorReady) return -1;

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    if (p == FINGERPRINT_NOTFOUND) {
      Serial.println(F("❌ [Van Tay Tang 3] Van tay KHONG HOP LE!"));
      triggerShortBeep(600);
    }
    return -1;
  }
  return finger.fingerID;
}

bool enrollFingerprint(uint8_t id) {
  if (!fingerprintSensorReady) {
    Serial.println(F("❌ Cam bien van tay chua san sang!"));
    return false;
  }
  Serial.printf("👉 Dang dang ky van tay ID #%d. Dat ngon tay len cam bien...\n", id);
  int p = -1;
  unsigned long startWait = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - startWait > 15000) {
      Serial.println(F("⏱️ Het thoi gian cho!"));
      return false;
    }
    p = finger.getImage();
    delay(50);
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) return false;

  Serial.println(F("☝️ Nhac ngon tay ra..."));
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(50);

  Serial.println(F("👉 Dat lai CUNG ngon tay do lan 2..."));
  startWait = millis();
  p = -1;
  while (p != FINGERPRINT_OK) {
    if (millis() - startWait > 15000) return false;
    p = finger.getImage();
    delay(50);
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) return false;
  if (finger.createModel() != FINGERPRINT_OK) {
    Serial.println(F("❌ Hai lan dat van tay khong khop!"));
    return false;
  }
  if (finger.storeModel(id) == FINGERPRINT_OK) {
    Serial.printf("✅ Da luu van tay ID #%d thanh cong!\n", id);
    triggerShortBeep(300);
    return true;
  }
  return false;
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toUpperCase();
  if (line.startsWith("ENROLL")) {
    int id = line.substring(6).toInt();
    if (id >= 1 && id <= 127) enrollFingerprint((uint8_t)id);
    else Serial.println(F("ℹ️ Cu phap: ENROLL <1..127> (VD: ENROLL 1)"));
  } else if (line == "OPEN") {
    openDoor3ByFingerprint();
  }
}

void handleFingerprintAndDoor3() {
  unsigned long now = millis();

  // Tắt tiếng bíp ngắn khi hết thời gian
  if (shortBeepActive && (now - shortBeepStart >= shortBeepDuration)) {
    shortBeepActive = false;
    updateBuzzerOutput();
  }

  // Tự đóng cửa Servo Tầng 3 sau 2 giây
  if (isDoor3Open && (now - door3OpenedAt >= DOOR_OPEN_DURATION)) {
    closeDoor3();
  }

  // Quét cảm biến vân tay AS608 mỗi 150ms
  if (fingerprintSensorReady && (now - lastFingerScanTime >= FINGER_SCAN_MS)) {
    lastFingerScanTime = now;
    int id = getFingerprintID();
    if (id >= 0 && id != lastFingerprintID) {
      lastFingerprintID = id;
      Serial.printf("✅ [Van Tay Tang 3] Hop le! ID #%d (Do tin cay: %d)\n", id, finger.confidence);
      triggerShortBeep(300);
      openDoor3ByFingerprint();
    } else if (id == -1) {
      lastFingerprintID = -1;
    }
  }

  // Đồng bộ trạng thái cửa Tầng 3 lên /home/door_status
  if (door3StatusDirty && WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    if (fbSetBool(PATH_DOOR_3_STATUS, door3TargetVal)) {
      door3StatusDirty = false;
    }
  }
}

// ==== 11. Hiển thị màn hình OLED 0.96" I2C (nếu có gắn trên D2/D3) ====
void drawOledQuote(const char* quote) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  String q = String(quote);
  const int maxCharsPerLine = 21;
  int lineHeight = 10, y = 6;
  String word = "", line = "";
  for (unsigned int i = 0; i <= q.length(); i++) {
    char c = q.charAt(i);
    if (c == ' ' || c == '\0') {
      if ((line.length() + word.length()) <= maxCharsPerLine) {
        line += word + ' ';
      } else {
        int16_t x1, y1; uint16_t w, h;
        display.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2, y);
        display.print(line);
        y += lineHeight;
        line = word + ' ';
      }
      word = "";
    } else {
      word += c;
    }
  }
  if (line.length() > 0) {
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, y);
    display.print(line);
  }
}

void handleOledDisplay() {
  if (!oledAvailable) return;
  unsigned long now = millis();
  if (now - lastOledFrameTime < 80) return;
  lastOledFrameTime = now;

  display.clearDisplay();
  if (smokeDetected) {
    display.setTextSize(2);
    display.setCursor(10, 8);
    display.print("CANH BAO!");
    display.setTextSize(1);
    display.setCursor(14, 34);
    display.print("PHAT HIEN KHOI/GAS");
    display.setCursor(22, 48);
    display.printf("Muc khoi: %d", currentSmokeLevel);
    display.display();
    return;
  }

  switch (oledState) {
    case 0: {
      int barWidth = 100, barHeight = 10;
      int barX = (SCREEN_WIDTH - barWidth) / 2, barY = 26;
      display.setTextSize(1);
      display.setCursor(16, 8);
      display.print("Tang 2 & Tang 3");
      display.drawRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);
      int filled = map(oledProgress, 0, 100, 0, barWidth - 2);
      display.fillRect(barX + 1, barY + 1, filled, barHeight - 2, SSD1306_WHITE);
      display.setCursor(8, 48);
      display.printf("Khoi:%d D2:%s C3:%s", currentSmokeLevel,
                     roomLight2State ? "ON" : "OF", isDoor3Open ? "MO" : "DG");
      oledProgress += 5;
      if (oledProgress > 100) {
        oledProgress = 0;
        oledState = 1;
        oledStateStartTime = now;
      }
      break;
    }
    case 1: {
      display.setTextSize(2);
      display.setCursor(4, 12);
      display.print("Smart Home");
      display.setCursor(16, 36);
      display.print("WELCOME!");
      if (now - oledStateStartTime > 3000) {
        oledState = 2;
        oledStateStartTime = now;
      }
      break;
    }
    default: {
      drawOledQuote(quotes[quoteIndex]);
      display.setCursor(8, 52);
      display.printf("Khoi:%d D2:%s C3:%s", currentSmokeLevel,
                     roomLight2State ? "ON" : "OF", isDoor3Open ? "MO" : "DG");
      if (now - oledStateStartTime > 3500) {
        quoteIndex = (quoteIndex + 1) % quoteCount;
        oledState = 0;
        oledStateStartTime = now;
      }
      break;
    }
  }
  display.display();
}

// ==== 12. Kết nối Wi-Fi & Đồng bộ lệnh từ Firebase ====
void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Dang ket noi '%s'", WIFI_SSID);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK | IP %s | song %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println(F("[WiFi] Chua vao duoc, se tu thu lai trong loop()"));
  }
}

void initFirebase() {
  tlsClient.setInsecure();
  tlsClient.setBufferSizes(4096, 1024);

#if USE_FIREBASE_TEST_MODE
  Serial.println(F("[Firebase] Che do TEST MODE (ket noi HTTPS truc tiep qua BearSSL)"));
  firebaseReadyFlag = true;
#else
  firebaseReadyFlag = loginFirebaseEmail();
#endif
}

void pollFirebase() {
  if (WiFi.status() != WL_CONNECTED) {
    initialSyncDone = false;
    firebaseReadyFlag = false;
    if (millis() - lastWifiPrint >= 5000) {
      lastWifiPrint = millis();
      Serial.println(F("[WiFi] Mat ket noi, dang cho..."));
    }
    return;
  }

  if (!firebaseReadyFlag) {
    if (millis() - lastPoll < 3000) return;
    lastPoll = millis();
    initFirebase();
    if (!firebaseReadyFlag) return;
  }

#if !USE_FIREBASE_TEST_MODE
  if (millis() - lastTokenTime >= 45UL * 60UL * 1000UL) {
    loginFirebaseEmail();
  }
#endif

  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();

  // Đồng bộ trạng thái khởi đầu khi mới kết nối Firebase
  if (!initialSyncDone) {
    initialSyncDone = true;
    Serial.println(F("[Firebase] Dang dong bo trang thai ban dau cho Tang 2 & Tang 3..."));
    fbSetBool(PATH_DOOR_3_STATUS, isDoor3Open);
  }

  // 1. Đọc công tắc Đèn LED Tầng 2: /home/floor_2/light_room_2
  if (!light2SyncDirty) {
    bool vLight2;
    if (fbGetBool(PATH_LIGHT_2, vLight2, false)) {
      logRead(PATH_LIGHT_2, seenLight2, vLight2 ? "true" : "false");
      setRoomLight2(vLight2, false);
    } else {
      logReadError(PATH_LIGHT_2);
    }
  }

  // 2. Đọc cờ tắt còi báo cháy từ Web: /home/floor_2/buzzer_mute_2
  bool vMute2;
  if (fbGetBool(PATH_BUZZER_MUTE_2, vMute2, false)) {
    logRead(PATH_BUZZER_MUTE_2, seenMute2, vMute2 ? "true" : "false");
    if (vMute2 != buzzerMuteFromWeb) {
      buzzerMuteFromWeb = vMute2;
      updateBuzzerOutput();
      Serial.printf("🔇 [Coi Bao Chay - %s] -> %s\n",
                    PATH_BUZZER_MUTE_2, buzzerMuteFromWeb ? "TAT TIENG (MUTE)" : "CHO PHEP KEU (UNMUTE)");
    }
  } else {
    logReadError(PATH_BUZZER_MUTE_2);
  }
}

// ==== 13. Setup & Loop ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n============================================================"));
  Serial.println(F("   SMARTHOUSE V4 - GOP TANG 2 & TANG 3 TREN 1 ESP8266       "));
  Serial.println(F("============================================================"));

  // Khởi tạo chân Đèn Tầng 2 (D6), Còi Buzzer (D1), Nút nhấn (D5), Cảm biến khói (A0)
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SWITCH_BTN_PIN, INPUT_PULLUP);
  pinMode(SMOKE_ANALOG_PIN, INPUT);
  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Khởi tạo Servo cửa vân tay Tầng 3 (D4)
  doorServo.attach(SERVO_DOOR_PIN);
  doorServo.write(SERVO_CLOSE_POS);

  // Khởi tạo cảm biến vân tay AS608 qua SoftwareSerial (D7 = RX, D8 = TX) ở 57600 baud
  fingerSerial.begin(57600);
  finger.begin(57600);
  delay(100);
  Serial.println(F("🔍 Dang kiem tra cam bien van tay AS608 (D7/D8)..."));
  if (finger.verifyPassword()) {
    fingerprintSensorReady = true;
    finger.getTemplateCount();
    Serial.printf("✅ [AS608 Tang 3] San sang! So van tay da luu: %d\n", finger.templateCount);
    Serial.println(F("💡 Meo: Go 'ENROLL <id>' vao Serial Monitor de dang ky van tay moi."));
  } else {
    fingerprintSensorReady = false;
    Serial.println(F("⚠️ [AS608 Tang 3] Khong tim thay cam bien van tay (bo qua quet van tay)."));
  }

  // Khởi tạo màn hình OLED 0.96" I2C (0x3C) trên D2 (SDA) / D3 (SCL) (nếu có gắn)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.beginTransmission(OLED_I2C_ADDR);
  if (Wire.endTransmission() == 0 && display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 24);
    display.print("ESP8266 Tang 2&3...");
    display.display();
    oledStateStartTime = millis();
    Serial.println(F("🖥️ [OLED 0.96\"] Da ket noi tai 0x3C (SDA=D2, SCL=D3)."));
  } else {
    Serial.println(F("ℹ️ [OLED 0.96\"] Khong tim thay OLED 0x3C (bo qua hien thi OLED)."));
  }

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  handleSerialCommands();      // Lệnh ENROLL <id> đăng ký vân tay qua Serial Monitor
  handlePhysicalSwitch();      // Quét nút nhấn vật lý (D5) bật/tắt đèn Tầng 2
  handleSmokeSensor();         // Đọc cảm biến khói MQ-2 (A0) & báo cháy tự động
  handleFingerprintAndDoor3(); // Quét vân tay AS608 (D7/D8) & đóng/mở cửa Servo (D4)
  handleOledDisplay();         // Cập nhật màn hình OLED 0.96" (nếu có gắn)
  pollFirebase();              // Đồng bộ Đèn Tầng 2 & Cờ tắt còi báo cháy từ Web Firebase
}
