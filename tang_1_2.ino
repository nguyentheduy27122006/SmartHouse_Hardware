/*
 * ============================================================================
 * FILE: tang_1_2.ino
 * MẠCH ESP8266 SỐ 2 (TẦNG 1.2) - QUẢN LÝ CỬA CHÍNH & HỆ THỐNG THANG MÁY
 * ============================================================================
 * 1. MỤC ĐÍCH:
 *    Vi điều khiển ESP8266 NodeMCU phụ trách cụm khóa mật khẩu Cửa chính Tầng 1
 *    (Keypad 4x4 + LCD 16x2 + Servo SG90) và Động cơ Thang máy 3 tầng (L298N).
 *
 * 2. DANH SÁCH THIẾT BỊ & SƠ ĐỒ ĐẤU NỐI CHÂN (ESP8266 NODEMCU):
 *    +-----------------------------------------------------------------------+
 *    | Chân ESP8266 | GPIO   | Thiết bị ngoại vi       | Chân trên Module    |
 *    +--------------+--------+-------------------------+---------------------+
 *    | D1           | GPIO5  | LCD 16x2 + Keypad 4x4   | SCL (Bus I2C chung) |
 *    | D2           | GPIO4  | LCD 16x2 + Keypad 4x4   | SDA (Bus I2C chung) |
 *    | D4           | GPIO2  | Servo SG90 Cửa chính    | Dây Cam/Vàng (PWM)  |
 *    | D5           | GPIO14 | Động cơ Thang máy       | IN1 (L298N - Đi lên)|
 *    | D6           | GPIO12 | Động cơ Thang máy       | IN2 (L298N - Xuống) |
 *    +-----------------------------------------------------------------------+
 *    * Lưu ý đấu nối & vận hành:
 *      - Keypad 4x4 (PCF8574 địa chỉ 0x20) chỉ dùng nhập mật khẩu mở Cửa chính
 *        vào nhà tại chỗ (mặc định "111#", nhấn A mở cửa, C xóa, D đổi mật khẩu).
 *      - Thang máy đấu vào OUT1 & OUT2 của L298N (giữ Jumper ENA), chỉ điều khiển
 *        gọi tầng từ giao diện Web thông qua Firebase.
 *      - Servo SG90: dây Cam/Vàng nối D4, dây Đỏ nối cổng 5V của L298N (hoặc VIN),
 *        dây Nâu/Đen nối GND chung.
 *
 * 3. ĐƯỜNG DẪN ĐỒNG BỘ FIREBASE REALTIME DATABASE:
 *    - [door1]    /home/floor_1/door_opened_1   (Ghi bool: true khi cửa mở 3s, false khi đóng)
 *    - [light1]   /home/floor_1/light_room_1    (Ghi bool: true để tự bật đèn Tầng 1 khi mở cửa)
 *    - [elevator] /home/elevator/target_floor   (Đọc int: Tầng đích 1, 2, 3 từ Web)
 *                 /home/elevator/current_floor  (Ghi int: Tầng hiện tại 1, 2, 3)
 *                 /home/elevator/status         (Ghi string: "idle", "moving_up", "moving_down")
 * ============================================================================
 */

// LƯU Ý QUAN TRỌNG: ESP8266WiFi và HTTPClient phải được include TRƯỚC Keypad.h
// vì Keypad.h có "#define CLOSED HIGH" trùng tên với hằng CLOSED trong lwIP.
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include "Keypad_I2C.h"
#include <Keypad.h>

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

// ==== 2. Đường dẫn Firebase cho Cửa chính & Thang máy ====
#define PATH_DOOR_OPENED      "/home/floor_1/door_opened_1"    // [door1] Trạng thái cửa chính (true = đang mở)
#define PATH_LIGHT_ROOM_1     "/home/floor_1/light_room_1"     // [light1] Tự động bật đèn tầng 1 khi mở cửa chính
#define PATH_ELEV_TARGET      "/home/elevator/target_floor"    // [elevator] Tầng đích nhận từ Web (1, 2, 3)
#define PATH_ELEV_CURRENT     "/home/elevator/current_floor"   // [elevator] Tầng hiện tại của thang máy (1, 2, 3)
#define PATH_ELEV_STATUS      "/home/elevator/status"          // [elevator] Trạng thái ("idle", "moving_up", "moving_down")

// ==== 3. Sơ đồ chân ESP8266 NodeMCU (Mạch Tầng 1.2) ====
#define I2C_SCL_PIN           D1    // GPIO5  - SCL chung cho LCD 16x2 & Keypad PCF8574
#define I2C_SDA_PIN           D2    // GPIO4  - SDA chung cho LCD 16x2 & Keypad PCF8574
#define SERVO_DOOR_PIN        D4    // GPIO2  - Động cơ Servo SG90 mở cửa chính
#define ELEV_IN1_PIN          D5    // GPIO14 - L298N IN1 (Thang máy đi LÊN)
#define ELEV_IN2_PIN          D6    // GPIO12 - L298N IN2 (Thang máy đi XUỐNG)

// (Tùy chọn) Công tắc hành trình dừng tầng 1, 2, 3 (nối GND, tích cực mức LOW).
// Nếu không gắn công tắc hành trình, thang máy sẽ tự dừng theo thời gian ELEV_TIME_PER_FLOOR_MS.
#define USE_LIMIT_SWITCHES    false
#define LIMIT_SW_FLOOR1_PIN   D0    // GPIO16 - Công tắc hành trình tầng 1
#define LIMIT_SW_FLOOR2_PIN   D3    // GPIO0  - Công tắc hành trình tầng 2
#define LIMIT_SW_FLOOR3_PIN   D7    // GPIO13 - Công tắc hành trình tầng 3

// Địa chỉ I2C
#define KEYPAD_I2C_ADDR       0x20  // Địa chỉ module mở rộng PCF8574 của Keypad 4x4
#define DEFAULT_LCD_ADDR      0x27  // Địa chỉ mặc định của LCD 16x2 (tự động quét 0x27/0x3F/0x26)

// ==== 4. Thông số hoạt động Cửa chính & Thang máy ====
#define SERVO_CLOSED_ANGLE    100   // Góc Servo khi đóng cửa chính
#define SERVO_OPEN_ANGLE      20    // Góc Servo khi mở cửa chính
#define DOOR_OPEN_MS          3000  // Thời gian giữ mở cửa chính (3000ms = 3 giây)

#define ELEV_SPEED_UP         200   // Tốc độ PWM (0..255) khi thang máy đi lên (khớp thang_may.ino gốc: 200)
#define ELEV_SPEED_DOWN       70    // Tốc độ PWM (0..255) khi thang máy đi xuống (khớp thang_may.ino gốc: 70)
#define ELEV_TIME_PER_FLOOR_MS 1800 // Thời gian di chuyển qua mỗi tầng (1.8 giây / tầng)
#define POLL_MS               500   // Chu kỳ đồng bộ Firebase (500ms)

// ==== 5. Khởi tạo đối tượng phần cứng ====
BearSSL::WiFiClientSecure tlsClient;
HTTPClient http;
Servo doorServo;
LiquidCrystal_I2C *lcd = nullptr;
bool hasLcd = false;
bool hasKeypad = false;
uint8_t lcdAddr = DEFAULT_LCD_ADDR;

const byte ROWS = 4;
const byte COLS = 4;
char hexaKeys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {0, 1, 2, 3};
byte colPins[COLS] = {4, 5, 6, 7};
Keypad_I2C kpad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS, KEYPAD_I2C_ADDR, 1, &Wire);

// ==== 6. Biến trạng thái hệ thống ====
String authToken = "";
unsigned long lastTokenTime = 0;
bool firebaseReadyFlag = false;
bool initialSyncDone = false;
unsigned long lastPoll = 0;

// Trạng thái Bàn phím & Mật khẩu cửa chính
String password = "111#";
String input = "";
bool changingPass = false;
bool confirmedOld = false;
char lastKey = 0;

// Trạng thái Cửa chính
bool doorOpen = false;
unsigned long doorCloseAt = 0;
unsigned long lcdRestoreAt = 0;
bool doorOpenDirty = false;
bool doorOpenTargetVal = false;
bool turnOnRoomLightDirty = false;

// Trạng thái Thang máy (Tầng 1..3)
enum ElevatorState { ELEV_IDLE, ELEV_MOVING_UP, ELEV_MOVING_DOWN };
ElevatorState elevState = ELEV_IDLE;
int currentFloor = 1;
int targetFloor = 1;
unsigned long elevMoveStartTime = 0;
unsigned long elevMoveDuration = 0;
bool elevStatusDirty = false;

// Theo dõi log Firebase
String seenElevTarget = "";
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

bool fbSetString(const char *path, const String &val) {
  return fbPutRaw(path, "\"" + val + "\"");
}

bool fbGetInt(const char *path, int &outVal, int defaultIfNull = 1) {
  String raw;
  if (!fbGetRaw(path, raw)) return false;
  if (raw == "null") {
    Serial.printf("ℹ️ [Firebase] Node %s chua co (null) -> Tu dong khoi tao = %d\n", path, defaultIfNull);
    fbSetInt(path, defaultIfNull);
    outVal = defaultIfNull;
    return true;
  }
  if (raw.startsWith("\"") && raw.endsWith("\"") && raw.length() >= 2) {
    raw = raw.substring(1, raw.length() - 1);
  }
  int parsed = raw.toInt();
  if (parsed >= 1 && parsed <= 3) {
    outVal = parsed;
    return true;
  }
  lastHttpError = "Gia tri tang khong hop le (" + raw + ")";
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

// ==== 8. Giao diện hiển thị LCD 16x2 (Cửa chính) ====
void showPrompt() {
  if (!hasLcd || lcd == nullptr) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(changingPass ? (confirmedOld ? "Nhap MK moi:" : "Nhap MK cu:") : "Nhap mat khau:");

  lcd->setCursor(0, 1);
  if (input.length() > 0) {
    lcd->print("Nhap: ");
    for (unsigned int i = 0; i < input.length() && i < 10; i++) lcd->print('*');
  } else if (!changingPass) {
    lcd->print("A:Mo cua  D:Doi");
  }
  lcdRestoreAt = 0;
}

void showMessage(const char *line1, const char *line2, unsigned long durationMs) {
  if (!hasLcd || lcd == nullptr) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(line1);
  lcd->setCursor(0, 1);
  lcd->print(line2);
  lcdRestoreAt = millis() + durationMs;
}

// ==== 9. Điều khiển Cửa chính (Servo D4) ====
void openMainDoor() {
  doorServo.write(SERVO_OPEN_ANGLE);
  doorOpen = true;
  doorCloseAt = millis() + DOOR_OPEN_MS;
  showMessage("Mat khau dung!", "Cua chinh: MO", DOOR_OPEN_MS);
  Serial.println(F("🚪 [Cua chinh] MO CUA (3 giay) -> Bat den Tang 1"));

  doorOpenDirty = true;
  doorOpenTargetVal = true;
  turnOnRoomLightDirty = true;
}

void handleMainDoor() {
  if (doorOpen && (long)(millis() - doorCloseAt) >= 0) {
    doorServo.write(SERVO_CLOSED_ANGLE);
    doorOpen = false;
    Serial.println(F("🚪 [Cua chinh] DONG CUA (het 3 giay)"));
    showPrompt();

    doorOpenDirty = true;
    doorOpenTargetVal = false;
  }

  // Đồng bộ trạng thái cửa chính & bật đèn tầng 1 lên Firebase
  if (WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    if (doorOpenDirty) {
      if (fbSetBool(PATH_DOOR_OPENED, doorOpenTargetVal)) {
        doorOpenDirty = false;
      }
    }
    if (turnOnRoomLightDirty) {
      if (fbSetBool(PATH_LIGHT_ROOM_1, true)) {
        turnOnRoomLightDirty = false;
      }
    }
  }
}

// ==== 10. Điều khiển Thang máy (L298N D5/D6 - chỉ điều khiển qua Web/Firebase) ====
void stopElevatorMotor() {
  digitalWrite(ELEV_IN1_PIN, LOW);
  digitalWrite(ELEV_IN2_PIN, LOW);
}

void moveElevatorToFloor(int destFloor) {
  if (destFloor < 1 || destFloor > 3) return;
  if (elevState != ELEV_IDLE) {
    Serial.printf("⚠️ [Thang may] Dang di chuyen, bo qua lenh goi tang %d\n", destFloor);
    return;
  }
  if (destFloor == currentFloor) {
    Serial.printf("ℹ️ [Thang may] Da o san Tang %d\n", destFloor);
    return;
  }

  targetFloor = destFloor;
  int floorsToMove = abs(targetFloor - currentFloor);
  elevMoveDuration = (unsigned long)floorsToMove * ELEV_TIME_PER_FLOOR_MS;
  elevMoveStartTime = millis();

  if (targetFloor > currentFloor) {
    elevState = ELEV_MOVING_UP;
    digitalWrite(ELEV_IN2_PIN, LOW);
    analogWrite(ELEV_IN1_PIN, ELEV_SPEED_UP);
    Serial.printf("🛗 [Thang may - Web] DI LEN: Tang %d -> Tang %d (%lu ms)\n",
                  currentFloor, targetFloor, elevMoveDuration);
  } else {
    elevState = ELEV_MOVING_DOWN;
    digitalWrite(ELEV_IN1_PIN, LOW);
    analogWrite(ELEV_IN2_PIN, ELEV_SPEED_DOWN);
    Serial.printf("🛗 [Thang may - Web] DI XUONG: Tang %d -> Tang %d (%lu ms)\n",
                  currentFloor, targetFloor, elevMoveDuration);
  }

  elevStatusDirty = true;
}

void handleElevator() {
  if (elevState != ELEV_IDLE) {
    bool reached = false;

    #if USE_LIMIT_SWITCHES
      if (targetFloor == 1 && digitalRead(LIMIT_SW_FLOOR1_PIN) == LOW) reached = true;
      if (targetFloor == 2 && digitalRead(LIMIT_SW_FLOOR2_PIN) == LOW) reached = true;
      if (targetFloor == 3 && digitalRead(LIMIT_SW_FLOOR3_PIN) == LOW) reached = true;
    #endif

    if (millis() - elevMoveStartTime >= elevMoveDuration) {
      reached = true;
    }

    if (reached) {
      stopElevatorMotor();
      currentFloor = targetFloor;
      elevState = ELEV_IDLE;
      elevStatusDirty = true;
      Serial.printf("✅ [Thang may] Da den Tang %d -> Dung motor\n", currentFloor);
    }
  }

  // Đồng bộ trạng thái thang máy lên Firebase
  if (elevStatusDirty && WiFi.status() == WL_CONNECTED && firebaseReadyFlag) {
    const char *stStr = (elevState == ELEV_MOVING_UP) ? "moving_up"
                      : (elevState == ELEV_MOVING_DOWN) ? "moving_down" : "idle";
    bool ok1 = fbSetInt(PATH_ELEV_CURRENT, currentFloor);
    bool ok2 = fbSetInt(PATH_ELEV_TARGET, targetFloor);
    bool ok3 = fbSetString(PATH_ELEV_STATUS, stStr);
    if (ok1 && ok2 && ok3) {
      seenElevTarget = String(targetFloor);
      elevStatusDirty = false;
    }
  }
}

// ==== 11. Xử lý Bàn phím 4x4 I2C (PCF8574 - 0x20): Chỉ nhập mật khẩu vào nhà ====
void handleKey(char key) {
  Serial.printf("⌨️ [Keypad] Phim: %c\n", key);

  if (key == 'A') {
    if (!changingPass) {
      if (input == password) {
        openMainDoor();
      } else {
        Serial.println(F("❌ [Cua chinh] Sai mat khau!"));
        showMessage("Sai mat khau!", "Vui long thu lai", 1500);
      }
      input = "";
    } else if (!confirmedOld) {
      bool ok = (input == password);
      input = "";
      if (ok) {
        confirmedOld = true;
        showPrompt();
      } else {
        changingPass = false;
        showMessage("Sai MK cu!", "Huy doi MK", 2000);
      }
    } else {
      if (input.length() > 0) {
        password = input;
        changingPass = false;
        confirmedOld = false;
        input = "";
        Serial.printf("✅ [Cua chinh] Da doi mat khau moi: %s\n", password.c_str());
        showMessage("Doi MK thanh cong", "", 2000);
      }
    }
  } else if (key == 'B') {
    // Phím B không dùng điều khiển thang máy (chỉ dùng Web)
    return;
  } else if (key == 'C') {
    input = "";
    changingPass = false;
    confirmedOld = false;
    showPrompt();
  } else if (key == 'D') {
    changingPass = true;
    confirmedOld = false;
    input = "";
    showPrompt();
  } else {
    input += key;
    if (input.length() > 10) input.remove(0, 1);
    showPrompt();
  }
}

void readKeypad() {
  if (!hasKeypad) return;
  char key = kpad.getKey();
  if (key && key != lastKey) {
    lastKey = key;
    handleKey(key);
  } else if (!key) {
    lastKey = 0;
  }
}

// ==== 12. Khởi tạo Bus I2C (LCD 16x2 + Keypad PCF8574) ====
bool checkI2CAddress(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

void initI2CDevices() {
  pinMode(I2C_SCL_PIN, INPUT_PULLUP);
  pinMode(I2C_SDA_PIN, INPUT_PULLUP);
  delay(5);
  if (digitalRead(I2C_SCL_PIN) == LOW || digitalRead(I2C_SDA_PIN) == LOW) {
    Serial.println(F("⚠️ [I2C] Chan D1/D2 dang bi keo LOW -> Bo qua LCD & Keypad de tranh treo mach!"));
    return;
  }

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClockStretchLimit(1500);

  // 1. Kiểm tra Keypad 4x4 PCF8574 (0x20)
  if (checkI2CAddress(KEYPAD_I2C_ADDR)) {
    hasKeypad = true;
    kpad.begin();
    Serial.printf("✅ [Keypad 4x4] Da ket noi tai I2C 0x%02X\n", KEYPAD_I2C_ADDR);
  } else {
    Serial.printf("ℹ️ [Keypad 4x4] Khong tim thay PCF8574 tai 0x%02X (bo qua Keypad)\n", KEYPAD_I2C_ADDR);
  }

  // 2. Tự động dò địa chỉ LCD 16x2 (0x27, 0x3F, 0x26)
  const uint8_t lcdCandidates[] = {0x27, 0x3F, 0x26};
  for (uint8_t addr : lcdCandidates) {
    if (checkI2CAddress(addr)) {
      lcdAddr = addr;
      hasLcd = true;
      break;
    }
  }

  if (hasLcd) {
    lcd = new LiquidCrystal_I2C(lcdAddr, 16, 2);
    lcd->init();
    lcd->backlight();
    showPrompt();
    Serial.printf("🖥️ [LCD 16x2] Da ket noi tai I2C 0x%02X\n", lcdAddr);
  } else {
    Serial.println(F("ℹ️ [LCD 16x2] Khong tim thay LCD I2C tren D1/D2 (bo qua LCD)"));
  }
}

// ==== 13. Kết nối Wi-Fi & Đồng bộ Firebase ====
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

  // Đồng bộ lần đầu: đọc tầng hiện tại của thang máy trên Firebase
  if (!initialSyncDone) {
    initialSyncDone = true;
    Serial.println(F("[Firebase] Dang dong bo trang thai ban dau cho Tang 1.2..."));
    int curFl = 1;
    if (fbGetInt(PATH_ELEV_CURRENT, curFl, 1)) {
      currentFloor = curFl;
      targetFloor = curFl;
      seenElevTarget = String(curFl);
      Serial.printf("📥 [Firebase DOC] %s = %d (tang hien tai)\n", PATH_ELEV_CURRENT, currentFloor);
      showPrompt();
    }
    fbSetBool(PATH_DOOR_OPENED, doorOpen);
  }

  // Đọc lệnh gọi tầng Thang máy từ Web: /home/elevator/target_floor
  if (elevState == ELEV_IDLE) {
    int webTargetFloor = currentFloor;
    if (fbGetInt(PATH_ELEV_TARGET, webTargetFloor, currentFloor)) {
      logRead(PATH_ELEV_TARGET, seenElevTarget, String(webTargetFloor));
      if (webTargetFloor != currentFloor) {
        moveElevatorToFloor(webTargetFloor);
      }
    } else {
      logReadError(PATH_ELEV_TARGET);
    }
  }
}

// ==== 14. Setup & Loop ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n============================================================"));
  Serial.println(F("   SMARTHOUSE V4 - TANG 1.2: CUA CHINH & THANG MAY          "));
  Serial.println(F("============================================================"));

  // Khởi tạo L298N điều khiển động cơ Thang máy (D5, D6)
  pinMode(ELEV_IN1_PIN, OUTPUT);
  pinMode(ELEV_IN2_PIN, OUTPUT);
  stopElevatorMotor();

  #if USE_LIMIT_SWITCHES
    pinMode(LIMIT_SW_FLOOR1_PIN, INPUT_PULLUP);
    pinMode(LIMIT_SW_FLOOR2_PIN, INPUT_PULLUP);
    pinMode(LIMIT_SW_FLOOR3_PIN, INPUT_PULLUP);
  #endif

  // Khởi tạo Servo cửa chính (D4)
  doorServo.attach(SERVO_DOOR_PIN);
  doorServo.write(SERVO_CLOSED_ANGLE);

  // Khởi tạo LCD 16x2 I2C & Keypad 4x4 I2C trên D1/D2
  initI2CDevices();

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  readKeypad();     // Quét phím bấm mật khẩu mở Cửa chính
  handleMainDoor(); // Tự động đóng cửa chính sau 3 giây & đồng bộ Firebase
  handleElevator(); // Điều khiển motor Thang máy L298N & đồng bộ tầng lên Firebase
  pollFirebase();   // Nhận lệnh gọi Thang máy từ Web Firebase

  if (lcdRestoreAt != 0 && !doorOpen && (long)(millis() - lcdRestoreAt) >= 0) {
    showPrompt();
  }
}
