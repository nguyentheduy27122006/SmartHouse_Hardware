/*
 * ============================================================================
 * SMARTHOUSE V4 - FIRMWARE THỐNG NHẤT TẦNG 1 (ESP8266 NODEMCU)
 * ============================================================================
 * Danh sách 5 thiết bị ngoại vi Tầng 1 đã thống nhất theo đúng cấu trúc Firebase:
 *   1. Cửa Garage (L298N kênh A - OUT1/OUT2):
 *      - Chân D5 (GPIO14) -> L298N IN1, D6 (GPIO12) -> L298N IN2 (giữ jumper ENA)
 *      - Firebase: đọc lệnh "/home/floor_1/garage/command" ("open" / "close"),
 *                  ghi trạng thái "/home/floor_1/garage/status" ("open" / "closed")
 *   2. Cảm biến nhiệt độ, độ ẩm DHT11 & Màn hình LCD 16x2 I2C:
 *      - DHT11 nối chân D3 (GPIO0)
 *      - LCD 16x2 I2C nối D1 (GPIO5 - SCL), D2 (GPIO4 - SDA) (tự dò địa chỉ 0x27 / 0x3F / 0x26)
 *      - Firebase: ghi "/home/floor_1/temperature_1" (°C) và "/home/floor_1/humidity_1" (%)
 *   3. Dãy đèn LED trong nhà (qua MOSFET):
 *      - Chân D0 (GPIO16) điều khiển chung dãy đèn LED
 *      - Firebase: đọc "/home/floor_1/light_room_1" (true = sáng, false = tắt)
 *   4. Quạt thông qua OUTB (OUT3/OUT4) của L298N chung với cửa Garage:
 *      - Chân D7 (GPIO13) -> L298N IN3 (IN4 nối GND, giữ jumper ENB)
 *      - Firebase: đọc "/home/floor_1/fan_room_1" (true = quạt chạy, false = tắt)
 *   5. Báo trộm nhà xe (PIR / Hồng ngoại):
 *      - Chân D4 (GPIO2) đọc tín hiệu Digital
 *      - Firebase: đọc "/home/floor_1/alarm_enabled_1" (bật/tắt chế độ chống trộm),
 *                  ghi "/home/floor_1/alarm_triggered_1" (true = báo động khi có trộm)
 *
 * Giao tiếp Firebase:
 *   Dùng trực tiếp BearSSL::WiFiClientSecure + ESP8266HTTPClient (REST API GET/PUT)
 *   giúp ESP8266 hoạt động ổn định, không bị tràn RAM hay lỗi mConnectBasicClient.
 * ============================================================================
 */

#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

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

// ==== 2. Đường dẫn Firebase chuẩn theo đúng bảng thiết bị Tầng 1 ====
#define PATH_LED              "/home/floor_1/light_room_1"       // [light1]  Đèn tầng 1 (2 dây LED gộp chung)
#define PATH_FAN              "/home/floor_1/fan_room_1"         // [fan1]    Quạt tầng 1 (L298N OUTB)
#define PATH_GARAGE_STATUS    "/home/floor_1/garage/status"      // [garage]  Trạng thái cửa cuốn nhà xe ("open" / "closed")
#define PATH_GARAGE_CMD       "/home/floor_1/garage/command"     // [garage]  Lệnh cửa cuốn nhà xe ("open" / "close")
#define PATH_ALARM_ENABLED    "/home/floor_1/alarm_enabled_1"    // [alarm]   Bật/tắt báo trộm nhà xe (từ Web)
#define PATH_ALARM_TRIGGERED  "/home/floor_1/alarm_triggered_1"  // [alarm]   Trạng thái kích hoạt báo trộm nhà xe
#define PATH_TEMPERATURE      "/home/floor_1/temperature_1"      // [climate] Nhiệt độ DHT11 (°C)
#define PATH_HUMIDITY         "/home/floor_1/humidity_1"         // [climate] Độ ẩm DHT11 (%)

// ==== 3. Sơ đồ chân ESP8266 NodeMCU ====
// Lưu ý boot ESP8266: D3 (GPIO0), D4 (GPIO2) phải ở mức HIGH lúc cấp nguồn; D8 (GPIO15) ở mức LOW.
#define I2C_SCL_PIN         D1    // GPIO5  - SCL cho LCD 16x2 I2C
#define I2C_SDA_PIN         D2    // GPIO4  - SDA cho LCD 16x2 I2C
#define DHT_PIN             D3    // GPIO0  - Cảm biến nhiệt độ, độ ẩm DHT11
#define ANTITHEFT_PIN       D4    // GPIO2  - Cảm biến chống trộm (Digital)
#define GARAGE_IN1          D5    // GPIO14 - L298N IN1 (Kênh A - OUTA cho cửa Garage)
#define GARAGE_IN2          D6    // GPIO12 - L298N IN2 (Kênh A - OUTA cho cửa Garage)
#define FAN_PIN             D7    // GPIO13 - L298N IN3 (Kênh B - OUTB cho Quạt, IN4 nối GND)
#define LED_PIN             D0    // GPIO16 - MOSFET điều khiển dãy đèn LED trong nhà (light_room_1)

// Mức tín hiệu kích hoạt của cảm biến chống trộm tại chân D4:
// - Đặt LOW nếu dùng cảm biến hồng ngoại LM393 / công tắc từ (xuất LOW khi phát hiện)
// - Đặt HIGH nếu dùng cảm biến chuyển động PIR HC-SR501 (xuất HIGH khi phát hiện)
#define ANTITHEFT_ACTIVE_LEVEL LOW

// ==== 4. Thông số hoạt động ====
#define DHT_TYPE            DHT11
#define GARAGE_RUN_MS       300
#define GARAGE_SPEED_OPEN   90    // PWM 0..255
#define GARAGE_SPEED_CLOSE  85
#define POLL_MS             500   // Chu kỳ đọc lệnh từ Firebase (500ms)
#define DHT_READ_MS         2000  // Chu kỳ đọc DHT11 & cập nhật LCD (2000ms)
#define DHT_UPLOAD_MS       10000 // Chu kỳ tối thiểu đẩy nhiệt độ/độ ẩm lên Firebase (10s)
#define ANTITHEFT_DEBOUNCE  100   // Chống nhiễu cảm biến chống trộm (100ms)

// ==== 5. Đối tượng phần cứng & biến trạng thái ====
BearSSL::WiFiClientSecure tlsClient;
HTTPClient http;
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C *lcd = nullptr;
bool hasLcd = false;
uint8_t lcdAddr = 0x27;

String authToken = "";
unsigned long lastTokenTime = 0;
bool firebaseReadyFlag = false;
bool initialSyncDone = false;
unsigned long lastPoll = 0;

// [3] Trạng thái Đèn tầng 1 (chân D0 - /home/floor_1/light_room_1)
bool ledOn = false;

// [4] Trạng thái Quạt tầng 1 (điều khiển riêng qua OUTB L298N chân D7 - /home/floor_1/fan_room_1)
bool fanOn = false;

// Trạng thái Cửa Garage
bool garageRunning = false;
unsigned long garageStart = 0;
String lastCommand = "";   // "open" / "close" đã chạy gần nhất
bool garageStatusDirty = false;

// Trạng thái DHT11 & LCD
unsigned long lastDhtRead = 0;
unsigned long lastDhtUpload = 0;
float currentTemp = NAN, currentHumi = NAN;
float sentTemp = -1000.0f, sentHumi = -1000.0f;

// Trạng thái Cảm biến chống trộm
bool alarmEnabled = true;        // Đọc từ /home/floor_1/alarm_enabled_1
bool intrusionDetected = false;  // Đọc từ chân D4 -> ghi lên /home/floor_1/motion_detected_1
bool lastSyncedIntrusion = false;
bool lastSyncedAlarmTrig = false;
bool firstIntrusionSync = false;
int lastRawAntitheft = -1;
unsigned long lastAntitheftChange = 0;

// Theo dõi log Firebase
String seenFan = "", seenLed = "", seenGarage = "", seenAlarmEn = "";
unsigned long readOk = 0, readFail = 0, lastErrPrint = 0;
unsigned long lastWifiPrint = 0;
String lastHttpError = "";

// ==== 6. Các hàm Log & REST API Firebase (GET / PUT) ====
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

bool fbSetFloat(const char *path, float val) {
  return fbPutRaw(path, String(val, 1));
}

bool fbSetString(const char *path, const String &val) {
  return fbPutRaw(path, "\"" + val + "\"");
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

bool fbGetString(const char *path, String &outVal, const char *defaultIfNull = nullptr) {
  String raw;
  if (!fbGetRaw(path, raw)) return false;
  if (raw == "null") {
    if (defaultIfNull != nullptr) {
      Serial.printf("ℹ️ [Firebase] Node %s chua co (null) -> Tu dong khoi tao = \"%s\"\n", path, defaultIfNull);
      fbSetString(path, String(defaultIfNull));
      outVal = String(defaultIfNull);
      return true;
    }
    lastHttpError = "Node chua ton tai (null)";
    return false;
  }
  if (raw.length() >= 2 && raw.startsWith("\"") && raw.endsWith("\"")) {
    outVal = raw.substring(1, raw.length() - 1);
  } else {
    outVal = raw;
  }
  return true;
}

// Đăng nhập Email/Password qua REST API nếu USE_FIREBASE_TEST_MODE == false
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

// ==== 7. Điều khiển Thiết bị 1, 3, 4 (Garage, Dãy đèn LED gộp, Quạt) ====
void setFan(bool on) {
  if (on == fanOn) return;
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  Serial.printf("🌀 [Quat OUTB - %s] -> %s\n", PATH_FAN, on ? "BAT" : "TAT");
}

void setLedStrip(bool on) {
  if (on == ledOn) return;
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.printf("💡 [Day den LED (D0) - %s] -> %s\n", PATH_LED, on ? "BAT" : "TAT");
}

void stopGarage() {
  digitalWrite(GARAGE_IN1, LOW);
  digitalWrite(GARAGE_IN2, LOW);
}

void startGarage(const String &cmd) {
  Serial.printf("🚗 [Garage OUTA - %s] -> %s\n", PATH_GARAGE_CMD, cmd == "open" ? "MO CUA" : "DONG CUA");
  if (cmd == "open") {
    digitalWrite(GARAGE_IN2, LOW);
    analogWrite(GARAGE_IN1, GARAGE_SPEED_OPEN);
  } else {
    digitalWrite(GARAGE_IN1, LOW);
    analogWrite(GARAGE_IN2, GARAGE_SPEED_CLOSE);
  }
  garageStart = millis();
  garageRunning = true;
  lastCommand = cmd;
}

void runGarage() {
  if (garageRunning && millis() - garageStart >= GARAGE_RUN_MS) {
    stopGarage();
    garageRunning = false;
    garageStatusDirty = true;
    Serial.println(F("🚗 [Garage OUTA] Dung motor (het 300ms)"));
  }

  if (garageStatusDirty && WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    String st = (lastCommand == "open") ? "open" : "closed";
    if (fbSetString(PATH_GARAGE_STATUS, st)) {
      garageStatusDirty = false;
    }
  }
}

// ==== 8. Thiết bị 2 & 5: Cảm biến DHT11 + LCD 16x2 I2C + Cảm biến chống trộm ====
void updateLcdDisplay() {
  if (!hasLcd || lcd == nullptr) return;

  // Dòng 1: Hiển thị Nhiệt độ & Độ ẩm
  lcd->setCursor(0, 0);
  if (isnan(currentTemp) || isnan(currentHumi)) {
    lcd->print("DHT11: Loi doc  ");
  } else {
    char line1[17];
    snprintf(line1, sizeof(line1), "T:%.1fC H:%.0f%%   ", currentTemp, currentHumi);
    lcd->print(line1);
  }

  // Dòng 2: Hiển thị trạng thái An ninh (Chống trộm) & Quạt
  lcd->setCursor(0, 1);
  if (alarmEnabled && intrusionDetected) {
    lcd->print("!CANH BAO TROM! ");
  } else {
    char line2[17];
    snprintf(line2, sizeof(line2), "AnNinh:%s Q:%s", alarmEnabled ? "ON " : "OFF", fanOn ? "ON " : "OFF");
    lcd->print(line2);
  }
}

void handleAntiTheftSensor() {
  int raw = digitalRead(ANTITHEFT_PIN);
  unsigned long now = millis();

  if (raw != lastRawAntitheft) {
    lastRawAntitheft = raw;
    lastAntitheftChange = now;
  }

  if (now - lastAntitheftChange >= ANTITHEFT_DEBOUNCE) {
    bool detected = (raw == ANTITHEFT_ACTIVE_LEVEL);
    if (detected != intrusionDetected) {
      intrusionDetected = detected;
      Serial.printf("🚨 [Chong trom D4] -> %s (alarm_enabled_1 = %s)\n",
                    intrusionDetected ? "PHAT HIEN XAM NHAP!" : "Binh thuong",
                    alarmEnabled ? "true" : "false");
      updateLcdDisplay();
    }
  }

  // Đẩy trạng thái báo trộm lên Firebase (/home/floor_1/alarm_triggered_1)
  if (WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    bool alarmTrig = alarmEnabled && intrusionDetected;
    if (!firstIntrusionSync || alarmTrig != lastSyncedAlarmTrig) {
      if (fbSetBool(PATH_ALARM_TRIGGERED, alarmTrig)) {
        lastSyncedAlarmTrig = alarmTrig;
        firstIntrusionSync = true;
      }
    }
  }
}

void handleClimateAndLcd() {
  unsigned long now = millis();
  if (now - lastDhtRead < DHT_READ_MS) return;
  lastDhtRead = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    if (!isnan(currentTemp)) {
      Serial.println(F("⚠️ [DHT11] Khong doc duoc du lieu tai chan D3 (kiem tra day tin hieu)"));
    }
    currentTemp = NAN;
    currentHumi = NAN;
    updateLcdDisplay();
    return;
  }

  bool changedLocal = isnan(currentTemp) || abs(t - currentTemp) >= 0.1f || abs(h - currentHumi) >= 0.5f;
  currentTemp = t;
  currentHumi = h;

  if (changedLocal) {
    Serial.printf("🌡️ [DHT11] Nhiet do: %.1f C | Do am: %.1f %%\n", currentTemp, currentHumi);
    updateLcdDisplay();
  }

  // Đẩy lên Firebase nếu thay đổi đáng kể (>= 0.5°C hoặc >= 1% độ ẩm) và cách lần gửi trước >= 10s
  if (WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    bool changedCloud = abs(currentTemp - sentTemp) >= 0.5f || abs(currentHumi - sentHumi) >= 1.0f;
    if (changedCloud && (lastDhtUpload == 0 || now - lastDhtUpload >= DHT_UPLOAD_MS)) {
      lastDhtUpload = now;
      bool okT = fbSetFloat(PATH_TEMPERATURE, currentTemp);
      bool okH = fbSetFloat(PATH_HUMIDITY, currentHumi);
      if (okT && okH) {
        sentTemp = currentTemp;
        sentHumi = currentHumi;
      }
    }
  }
}

uint8_t scanLcdAddress(uint8_t sdaPin, uint8_t sclPin) {
  Wire.begin(sdaPin, sclPin);
  Wire.setClockStretchLimit(1500);
  // Ưu tiên 0x27, 0x3F, 0x26 trước, sau đó quét toàn bộ dải PCF8574 (0x20..0x27) & PCF8574A (0x38..0x3F)
  const uint8_t priorityAddrs[] = {0x27, 0x3F, 0x26};
  for (uint8_t addr : priorityAddrs) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  for (uint8_t addr = 0x20; addr <= 0x3F; addr++) {
    if (addr > 0x27 && addr < 0x38) continue;
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) return addr;
  }
  return 0;
}

void initLcdI2C() {
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delay(5);
  if (digitalRead(I2C_SCL_PIN) == LOW || digitalRead(I2C_SDA_PIN) == LOW) {
    Serial.println(F("⚠️ [I2C] Chan D1/D2 dang bi keo LOW -> Bo qua khoi tao LCD de tranh treo mach"));
    return;
  }

  // Thử chiều chuẩn (SDA = D2, SCL = D1), nếu không thấy thì thử đảo (SDA = D1, SCL = D2)
  uint8_t found = scanLcdAddress(I2C_SDA_PIN, I2C_SCL_PIN);
  if (found != 0) {
    lcdAddr = found;
    hasLcd = true;
    Serial.printf("🖥️ [LCD 16x2] Da ket noi tai dia chi I2C 0x%02X (SDA=D2, SCL=D1)\n", lcdAddr);
  } else {
    found = scanLcdAddress(I2C_SCL_PIN, I2C_SDA_PIN);
    if (found != 0) {
      lcdAddr = found;
      hasLcd = true;
      Serial.printf("🖥️ [LCD 16x2] Da ket noi tai dia chi I2C 0x%02X (phat hien cam dao SDA=D1, SCL=D2)\n", lcdAddr);
    }
  }

  if (hasLcd) {
    lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
    lcd->init();
    lcd->backlight();
    lcd->setCursor(0, 0);
    lcd->print("SmartHouse Tang1");
    lcd->setCursor(0, 1);
    lcd->print("Dang khoi dong..");
  } else {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.println(F("ℹ️ [LCD 16x2] Khong tim thay LCD I2C tren D1/D2 (bo qua hien thi LCD)"));
  }
}

// ==== 9. Kết nối Wi-Fi & Đồng bộ lệnh từ Firebase ====
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

  // Đồng bộ lần đầu: đọc vị trí cửa garage hiện tại để không tự chạy lại lệnh cũ
  if (!initialSyncDone) {
    initialSyncDone = true;
    Serial.println(F("[Firebase] Dang doc trang thai ban dau cua tung thiet bi..."));
    String st;
    if (fbGetString(PATH_GARAGE_STATUS, st, "closed")) {
      if (st == "open") lastCommand = "open";
      else if (st == "closed") lastCommand = "close";
      Serial.printf("📥 [Firebase DOC] %s = %s (vi tri cua hien tai)\n", PATH_GARAGE_STATUS, st.c_str());
    } else {
      logReadError(PATH_GARAGE_STATUS);
    }
  }

  // 1. Đọc đường dẫn Quạt (L298N OUTB): /home/floor_1/fan_room_1 (true = quạt chạy, false = tắt)
  bool vFan;
  if (fbGetBool(PATH_FAN, vFan, false)) {
    logRead(PATH_FAN, seenFan, vFan ? "true" : "false");
    if (vFan != fanOn) {
      setFan(vFan);
      updateLcdDisplay();
    }
  } else {
    logReadError(PATH_FAN);
  }

  // 2. Đọc đường dẫn Dãy đèn LED (gộp thành 1): /home/floor_1/light_room_1 (true = sáng, false = tắt)
  bool vLed;
  if (fbGetBool(PATH_LED, vLed, false)) {
    logRead(PATH_LED, seenLed, vLed ? "true" : "false");
    setLedStrip(vLed);
  } else {
    logReadError(PATH_LED);
  }

  // 3. Đọc công tắc bật/tắt chế độ chống trộm: /home/floor_1/alarm_enabled_1
  bool vAlarmEn;
  if (fbGetBool(PATH_ALARM_ENABLED, vAlarmEn, true)) {
    logRead(PATH_ALARM_ENABLED, seenAlarmEn, vAlarmEn ? "true" : "false");
    if (vAlarmEn != alarmEnabled) {
      alarmEnabled = vAlarmEn;
      Serial.printf("🛡️ [Chong trom - %s] -> %s\n", PATH_ALARM_ENABLED, alarmEnabled ? "BAT CHE DO AN NINH" : "TAT CHE DO AN NINH");
      updateLcdDisplay();
    }
  } else {
    logReadError(PATH_ALARM_ENABLED);
  }

  // 4. Đọc đường dẫn Cửa Garage (L298N OUTA): /home/floor_1/garage/command
  if (!garageRunning) {
    String cmd;
    if (fbGetString(PATH_GARAGE_CMD, cmd, "close")) {
      logRead(PATH_GARAGE_CMD, seenGarage, cmd);
      if ((cmd == "open" || cmd == "close") && cmd != lastCommand) {
        startGarage(cmd);
      }
    } else {
      logReadError(PATH_GARAGE_CMD);
    }
  }
}

// ==== 10. Setup & Loop ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n========================================================"));
  Serial.println(F("   SMARTHOUSE V4 - TANG 1 THONG NHAT 5 THIET BI NGOAI VI"));
  Serial.println(F("========================================================"));

  // Khởi tạo L298N: Kênh A (Garage: D5, D6) & Kênh B (Quạt: D7)
  pinMode(GARAGE_IN1, OUTPUT);
  pinMode(GARAGE_IN2, OUTPUT);
  stopGarage();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  // Khởi tạo Dãy đèn LED trong nhà (D0)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Khởi tạo Cảm biến chống trộm (D4)
  pinMode(ANTITHEFT_PIN, INPUT_PULLUP);

  // Khởi tạo Cảm biến DHT11 (D3) & Màn hình LCD 16x2 I2C (D1/D2)
  dht.begin();
  initLcdI2C();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  pollFirebase();          // Nhận lệnh Quạt, Dãy đèn LED, Chống trộm, Cửa Garage từ Firebase
  runGarage();             // Dừng motor Garage sau 300ms & đồng bộ trạng thái
  handleAntiTheftSensor(); // Đọc cảm biến chống trộm (D4) & đẩy cảnh báo lên Firebase
  handleClimateAndLcd();   // Đọc DHT11 (D3), hiển thị LCD (D1/D2) & đẩy lên Firebase
}
