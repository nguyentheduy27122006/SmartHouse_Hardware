/*
 * Tầng 1 trên một ESP8266 NodeMCU: gộp cua_chinh + nha_xe_moi_nhat + phần DHT11/LCD của bao_trom_va_do_am.
 *
 *   - Cửa chính: bàn phím 4x4 (I2C), LCD 16x2 thứ nhất, servo, còi bíp
 *   - Cửa garage: motor qua kênh A của L298N, mở/đóng bằng phím B hoặc từ web/app
 *   - Quạt tầng 1: kênh B của L298N
 *   - Đèn trong nhà tầng 1: 2 dây LED qua 2 module MOSFET
 *   - Nhiệt độ, độ ẩm: DHT11, hiện trên LCD 16x2 thứ hai
 *
 * Sơ đồ chân (không đổi chỗ D3, D4, D8: lúc cấp nguồn D3, D4 phải ở mức cao, D8 ở mức thấp,
 * sai thì mạch không khởi động hoặc không nạp được code):
 *   D1 (GPIO5)  SCL chung: LCD cửa (0x27), LCD nhiệt độ (0x26, hàn cầu A0), bàn phím PCF8574 (0x20)
 *   D2 (GPIO4)  SDA chung
 *   D5 (GPIO14) L298N IN1   motor garage (băm xung để chạy chậm)
 *   D6 (GPIO12) L298N IN2   motor garage
 *   D7 (GPIO13) L298N IN3   quạt. IN4 nối GND. Giữ jumper ENA và ENB trên L298N
 *   D0 (GPIO16) MOSFET dây LED 1 (chân này chỉ bật/tắt)
 *   D8 (GPIO15) MOSFET dây LED 2
 *   D3 (GPIO0)  DHT11
 *   D4 (GPIO2)  servo cửa chính
 *   RX (GPIO3)  còi bíp. Serial chỉ còn chiều in ra (TX)
 * Nguồn: motor, quạt và dây LED lấy nguồn 12V riêng; servo lấy 5V riêng, không lấy từ chân 3V3 của
 * NodeMCU; mọi nguồn nối chung GND. MOSFET phải là loại logic-level (IRLZ44N, AO3400...).
 *
 * Firebase, tính từ /home/floor_1 (một stream cho cả nhánh, ESP8266 không đủ RAM cho nhiều stream):
 *   light_room_1     quạt. Đảo logic giống firmware cũ: false = quạt chạy (web, app dùng quy ước này)
 *   led_strip_1, led_strip_2   dây LED: true = sáng
 *   garage/command   lệnh "open" / "close" từ web, app. garage/status mạch ghi "open" / "closed"
 *   door_opened_1    true trong 3 giây cửa chính mở
 *   temperature_1, humidity_1  nhiệt độ, độ ẩm
 *
 * Bàn phím: số, # và * để nhập mật khẩu; A mở cửa chính; B mở cửa garage (cần mật khẩu) hoặc đóng
 * (không cần); C xóa; D đổi mật khẩu. Mật khẩu đổi chỉ giữ tới lần khởi động lại, như cua_chinh.
 */

// ESP8266WiFi, WiFiShare và Firebase phải đứng trước Keypad: Keypad.h có "#define CLOSED HIGH", trùng
// tên với hằng CLOSED trong thư viện mạng lwip và làm lỗi biên dịch nếu include sau.
#include <ESP8266WiFi.h>
#include "WiFiShare.h"
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <Wire.h>
#include "Keypad_I2C.h"
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <DHT.h>

// ==== Cấu hình Wi-Fi + Firebase (project Smart House: smart-house-fcd4f) ====
// Wi-Fi nhà do WiFiShare lo (firmware/libraries/WiFiShare). Mọi mạch dùng cùng SETUP_SSID và
// SETUP_PASSWORD. Không commit mật khẩu thật lên GitHub.
#define DEVICE_NAME "tang_1"
#define SETUP_SSID "Iphone"
#define SETUP_PASSWORD "112345678"
#define API_KEY "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY"
#define DATABASE_URL "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app"
// Đặt true nếu Rules trên Firebase đang mở (.read: true, .write: true) -> bỏ qua đăng nhập Email/Password.
// Đặt false nếu Rules yêu cầu đăng nhập tài khoản bên dưới.
#define USE_FIREBASE_TEST_MODE true
#define USER_EMAIL "nvtngang@hcmus.edu.vn"
#define USER_PASSWORD "12345678"

// ==== Đường dẫn Firebase cụ thể cho từng thiết bị Tầng 1 ====
#define PATH_FAN            "/home/floor_1/light_room_1"    // Quạt Tầng 1 (false = quạt chạy, true = quạt tắt)
#define PATH_LED_1          "/home/floor_1/led_strip_1"     // Dây đèn LED 1 (true = sáng, false = tắt)
#define PATH_LED_2          "/home/floor_1/led_strip_2"     // Dây đèn LED 2 (true = sáng, false = tắt)
#define PATH_GARAGE_CMD     "/home/floor_1/garage/command"  // Lệnh cửa garage ("open" / "close")
#define PATH_GARAGE_STATUS  "/home/floor_1/garage/status"   // Trạng thái cửa garage ("open" / "closed")
#define PATH_DOOR_OPENED    "/home/floor_1/door_opened_1"   // Trạng thái cửa chính mở (true / false)
#define PATH_TEMPERATURE    "/home/floor_1/temperature_1"   // Nhiệt độ DHT11 (float)
#define PATH_HUMIDITY       "/home/floor_1/humidity_1"      // Độ ẩm DHT11 (float)

// ==== Chân ====
#define GARAGE_IN1 D5
#define GARAGE_IN2 D6
#define FAN_PIN D7
#define LED1_PIN D0
#define LED2_PIN D8
#define DHT_PIN D3
#define SERVO_PIN D4
#define BUZZER_PIN 3  // RX

#define KEYPAD_ADDR 0x20
#define LCD_DOOR_ADDR 0x27
#define LCD_CLIMATE_ADDR 0x26

// ==== Thông số ====
#define SERVO_CLOSED 100
#define SERVO_OPEN 20
#define DOOR_OPEN_MS 3000
#define GARAGE_RUN_MS 300       // motor garage chạy 300 ms mỗi lần, như nha_xe_moi_nhat
#define GARAGE_SPEED_OPEN 90    // độ rộng xung 0..255. ENA cắm jumper nên tốc độ hơi khác bản cũ, chỉnh lại nếu cần
#define GARAGE_SPEED_CLOSE 85

LiquidCrystal_I2C lcdDoor(LCD_DOOR_ADDR, 16, 2);
LiquidCrystal_I2C lcdClimate(LCD_CLIMATE_ADDR, 16, 2);
Servo doorServo;
DHT dht(DHT_PIN, DHT11);

const byte ROWS = 4;
const byte COLS = 4;
char hexaKeys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {0, 1, 2, 3};
byte colPins[COLS] = {4, 5, 6, 7};
Keypad_I2C kpad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS, KEYPAD_ADDR, 1, &Wire);

WiFiShare wifiShare;
FirebaseData fbdo;    // đọc & ghi trực tiếp từng đường dẫn thiết bị
FirebaseAuth auth;
FirebaseConfig config;
bool firebaseStarted = false;
bool initialSyncDone = false;
unsigned long lastPollTime = 0;
const unsigned long FIREBASE_POLL_MS = 500; // Chu kỳ đọc lệnh từ Firebase (500ms)

// ==== Trạng thái ====
String password = "111#";
String input = "";
bool changingPass = false;
bool confirmedOld = false;
char lastKey = 0;

bool doorOpen = false;
unsigned long doorCloseAt = 0;
unsigned long lcdDoorRestoreAt = 0;  // 0: LCD cửa đang hiện lời nhắc, không có thông báo chờ xóa

bool fanOn = false;
bool ledOn[2] = {false, false};

bool garageRunning = false;
unsigned long garageStart = 0;
String lastCommand = "";     // lần chạy gần nhất: "open" / "close". Rỗng: chưa biết cửa đang ở đâu
String pendingCommand = "";  // lệnh từ web đến lúc motor đang chạy, xử lý khi motor dừng
bool garageFromKeypad = false;
bool statusDirty = false;        // trạng thái cửa garage chưa ghi được lên Firebase
bool statusHasCommand = false;   // ghi cả lệnh, vì lần chạy đó do bàn phím
unsigned long lastStatusTry = 0;
int statusFails = 0;

unsigned long lastSensorRead = 0;
unsigned long lastUpload = 0;
float lastTemp = -1000, lastHumi = -1000;
float sentTemp = -1000, sentHumi = -1000;

bool hasKeypad = false;
bool hasLcdDoor = false;
bool hasLcdClimate = false;
#define ENABLE_RX_BUZZER false  // Đặt true nếu gắn còi vào chân RX (GPIO3)

uint8_t beepsLeft = 0;
unsigned int beepMs = 0;
unsigned long nextBeepAt = 0;

// ==== Còi: bíp không chặn loop(), để motor, servo và Firebase vẫn chạy ====
void beep(unsigned int ms = 100, uint8_t times = 1) {
  #if !ENABLE_RX_BUZZER
    return;
  #endif
  beepMs = ms;
  beepsLeft = times;
  nextBeepAt = millis();
}

void runBeeper() {
  #if !ENABLE_RX_BUZZER
    return;
  #endif
  if (beepsLeft == 0 || (long)(millis() - nextBeepAt) < 0) return;
  tone(BUZZER_PIN, 1000, beepMs);
  beepsLeft--;
  nextBeepAt = millis() + beepMs + 200;
}

// ==== LCD cửa ====
void showPrompt() {
  if (!hasLcdDoor) return;
  lcdDoor.clear();
  lcdDoor.print(changingPass ? (confirmedOld ? "MK moi:" : "MK cu:") : "Nhap mat khau:");
  lcdDoor.setCursor(0, 1);
  if (input.length() > 0) {
    lcdDoor.print("Nhap: ");
    for (unsigned int i = 0; i < input.length(); i++) lcdDoor.print('*');
  } else if (!changingPass) {
    lcdDoor.print("A:Cua  B:Garage");
  }
  lcdDoorRestoreAt = 0;
}

// Hiện thông báo một lúc rồi quay lại lời nhắc
void showMessage(const char *line1, const char *line2, unsigned long ms) {
  if (!hasLcdDoor) return;
  lcdDoor.clear();
  lcdDoor.print(line1);
  lcdDoor.setCursor(0, 1);
  lcdDoor.print(line2);
  lcdDoorRestoreAt = millis() + ms;
}

// ==== Firebase ====
bool firebaseReadyLogged = false;
unsigned long lastFirebaseWaitPrint = 0;

void initFirebase() {
  Serial.println(F("[Firebase] Bat dau khoi tao Firebase..."));
  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(2048);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  #if USE_FIREBASE_TEST_MODE
    Serial.println(F("[Firebase] Che do: TEST MODE (khong can dang nhap Email/Password)"));
    config.signer.test_mode = true;
  #else
    Serial.printf("[Firebase] Che do: Xac thuc Email (%s)\n", USER_EMAIL);
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
  #endif
  config.token_status_callback = tokenStatusCallback;
  Firebase.reconnectNetwork(false);
  Firebase.begin(&config, &auth);
  firebaseStarted = true;
  lastFirebaseWaitPrint = millis();
}

// Chỉ dùng Firebase khi đã vào mạng nhà. Lần đầu vào được mạng thì khởi động Firebase.
// Mất mạng thì bàn phím, cửa, garage và LCD vẫn chạy, chỉ không ghi lên Firebase.
bool firebaseReady() {
  if (!wifiShare.online()) {
    firebaseReadyLogged = false;
    initialSyncDone = false;
    return false;
  }
  if (!firebaseStarted) initFirebase();
  bool rdy = Firebase.ready();
  if (rdy && !firebaseReadyLogged) {
    firebaseReadyLogged = true;
    Serial.println(F("[Firebase] -> THANH CONG! Da xac thuc & ket noi Firebase san sang!"));
  } else if (!rdy && (millis() - lastFirebaseWaitPrint >= 3000)) {
    lastFirebaseWaitPrint = millis();
    Serial.println(F("[Firebase] Dang cho xac thuc Token / ket noi Server..."));
  }
  return rdy;
}

// ==== Quạt, đèn ====
void setFan(bool on) {
  if (on == fanOn) return;
  fanOn = on;
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  Serial.printf("🌀 [Quat - %s] -> %s\n", PATH_FAN, on ? "BAT" : "TAT");
}

void setLed(uint8_t i, bool on) {
  if (on == ledOn[i]) return;
  ledOn[i] = on;
  digitalWrite(i == 0 ? LED1_PIN : LED2_PIN, on ? HIGH : LOW);
  Serial.printf("💡 [Den LED %u - %s] -> %s\n", i + 1, i == 0 ? PATH_LED_1 : PATH_LED_2, on ? "BAT" : "TAT");
}

// ==== Log Firebase: in mỗi lần đọc thấy giá trị đổi, mỗi lần ghi, mỗi lần lỗi ====
// Giá trị đọc gần nhất của từng đường dẫn, để chỉ in khi trên Firebase có thay đổi (không spam mỗi 500ms)
String seenFan = "", seenLed1 = "", seenLed2 = "", seenGarageCmd = "";
unsigned long lastReadErrPrint = 0;

void logRead(const char *path, String &seen, const String &val) {
  if (val == seen) return;
  Serial.printf("📥 [Firebase DOC] %s : %s -> %s\n", path, seen.length() ? seen.c_str() : "(chua co)", val.c_str());
  seen = val;
}

void logReadError(const char *path) {
  if (millis() - lastReadErrPrint < 5000) return;  // lỗi đọc in tối đa 5 giây một lần
  lastReadErrPrint = millis();
  Serial.printf("⚠️ [Firebase DOC] LOI %s : %s\n", path, fbdo.errorReason().c_str());
}

bool fbSetBool(const char *path, bool v) {
  bool ok = Firebase.RTDB.setBool(&fbdo, path, v);
  if (ok) Serial.printf("📤 [Firebase GHI] %s = %s -> OK\n", path, v ? "true" : "false");
  else Serial.printf("❌ [Firebase GHI] %s = %s -> LOI: %s\n", path, v ? "true" : "false", fbdo.errorReason().c_str());
  return ok;
}

bool fbSetString(const char *path, const String &v) {
  bool ok = Firebase.RTDB.setString(&fbdo, path, v);
  if (ok) Serial.printf("📤 [Firebase GHI] %s = \"%s\" -> OK\n", path, v.c_str());
  else Serial.printf("❌ [Firebase GHI] %s = \"%s\" -> LOI: %s\n", path, v.c_str(), fbdo.errorReason().c_str());
  return ok;
}

bool fbSetFloat(const char *path, float v) {
  bool ok = Firebase.RTDB.setFloat(&fbdo, path, v);
  if (ok) Serial.printf("📤 [Firebase GHI] %s = %.1f -> OK\n", path, v);
  else Serial.printf("❌ [Firebase GHI] %s = %.1f -> LOI: %s\n", path, v, fbdo.errorReason().c_str());
  return ok;
}

// ==== Đọc trực tiếp từng đường dẫn thiết bị trên Firebase ====
void pollFirebaseDevices() {
  if (!firebaseReady()) return;

  unsigned long now = millis();
  if (now - lastPollTime < FIREBASE_POLL_MS) return;
  lastPollTime = now;

  // Lần đầu kết nối: đọc trạng thái cửa garage đang lưu để không tự chạy lại lệnh cũ lúc mới bật mạch
  if (!initialSyncDone) {
    initialSyncDone = true;
    Serial.println(F("[Firebase] Dang dong bo trang thai ban dau cua tung thiet bi..."));
    if (Firebase.RTDB.getString(&fbdo, PATH_GARAGE_STATUS)) {
      String st = fbdo.stringData();
      if (st == "open") lastCommand = "open";
      else if (st == "closed") lastCommand = "close";
      Serial.printf("📥 [Firebase DOC] %s = %s (vi tri cua hien tai)\n", PATH_GARAGE_STATUS, st.c_str());
    } else {
      logReadError(PATH_GARAGE_STATUS);
    }
  }

  // 1. Quạt: /home/floor_1/light_room_1 (false = quạt chạy, true = quạt tắt)
  if (Firebase.RTDB.getBool(&fbdo, PATH_FAN)) {
    bool rawVal = fbdo.boolData();
    logRead(PATH_FAN, seenFan, rawVal ? "true" : "false");
    setFan(!rawVal);
  } else {
    logReadError(PATH_FAN);
  }

  // 2. Đèn LED 1: /home/floor_1/led_strip_1 (true = sáng)
  if (Firebase.RTDB.getBool(&fbdo, PATH_LED_1)) {
    bool v = fbdo.boolData();
    logRead(PATH_LED_1, seenLed1, v ? "true" : "false");
    setLed(0, v);
  } else {
    logReadError(PATH_LED_1);
  }

  // 3. Đèn LED 2: /home/floor_1/led_strip_2 (true = sáng)
  if (Firebase.RTDB.getBool(&fbdo, PATH_LED_2)) {
    bool v = fbdo.boolData();
    logRead(PATH_LED_2, seenLed2, v ? "true" : "false");
    setLed(1, v);
  } else {
    logReadError(PATH_LED_2);
  }

  // 4. Lệnh cửa garage: /home/floor_1/garage/command ("open" / "close")
  if (!garageRunning) {
    if (Firebase.RTDB.getString(&fbdo, PATH_GARAGE_CMD)) {
      String cmd = fbdo.stringData();
      logRead(PATH_GARAGE_CMD, seenGarageCmd, cmd);
      if ((cmd == "open" || cmd == "close") && cmd != lastCommand) {
        Serial.printf("🚗 [Garage] Nhan lenh tu Web: %s -> chuan bi chay motor\n", cmd.c_str());
        pendingCommand = cmd;
      }
    } else {
      logReadError(PATH_GARAGE_CMD);
    }
  }
}

// ==== Cửa chính ====
void openDoor() {
  doorServo.write(SERVO_OPEN);
  doorOpen = true;
  doorCloseAt = millis() + DOOR_OPEN_MS;
  showMessage("Mo cua...", "", DOOR_OPEN_MS);
  beep(300);
  Serial.println(F("🚪 [Cua chinh] MO CUA (mat khau dung)"));

  // Vào nhà thì bật đèn tầng 1, như cua_chinh bật đèn khi mở cửa
  setLed(0, true);
  setLed(1, true);
  if (firebaseReady()) {
    fbSetBool(PATH_DOOR_OPENED, true);
    if (fbSetBool(PATH_LED_1, true)) seenLed1 = "true";  // tránh in lại "đổi" ở lần đọc sau
    if (fbSetBool(PATH_LED_2, true)) seenLed2 = "true";
  } else {
    Serial.println(F("⚠️ [Cua chinh] Firebase chua san sang -> khong ghi duoc trang thai"));
  }
}

void closeDoorIfDue() {
  if (!doorOpen || (long)(millis() - doorCloseAt) < 0) return;
  doorServo.write(SERVO_CLOSED);
  doorOpen = false;
  Serial.println(F("🚪 [Cua chinh] DONG CUA (het 3 giay)"));
  if (firebaseReady()) fbSetBool(PATH_DOOR_OPENED, false);
}

// ==== Cửa garage ====
void stopGarage() {
  digitalWrite(GARAGE_IN1, LOW);
  digitalWrite(GARAGE_IN2, LOW);
}

void startGarage(const String &cmd, bool fromKeypad) {
  Serial.printf("🚗 [Garage] %s (%s)\n", cmd == "open" ? "MO CUA" : "DONG CUA", fromKeypad ? "Ban phim" : "Web");
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
  garageFromKeypad = fromKeypad;
}

// Cập nhật trực tiếp vào đường dẫn /home/floor_1/garage/status và /home/floor_1/garage/command
void syncGarageStatus() {
  if (!statusDirty || !firebaseReady()) return;
  if (lastStatusTry != 0 && millis() - lastStatusTry < 5000) return;
  lastStatusTry = millis();

  const char* newStatus = (lastCommand == "open") ? "open" : "closed";
  bool ok = fbSetString(PATH_GARAGE_STATUS, newStatus);
  if (ok && statusHasCommand) {
    ok = fbSetString(PATH_GARAGE_CMD, lastCommand);
    if (ok) seenGarageCmd = lastCommand;
  }

  if (ok) {
    statusDirty = false;
    statusHasCommand = false;
    statusFails = 0;
  } else {
    statusFails++;
    Serial.printf("⚠️ [Garage] Ghi trang thai that bai lan %d, thu lai sau 5 giay\n", statusFails);
  }
}

void runGarage() {
  if (!garageRunning && pendingCommand.length() > 0) {
    String cmd = pendingCommand;
    pendingCommand = "";
    if (cmd != lastCommand) startGarage(cmd, false);
  }

  if (garageRunning && millis() - garageStart >= GARAGE_RUN_MS) {
    stopGarage();
    garageRunning = false;
    // Ghi trạng thái sau khi motor đã dừng
    statusDirty = true;
    if (garageFromKeypad) statusHasCommand = true;
    lastStatusTry = 0;
    statusFails = 0;
  }
  syncGarageStatus();
}

// ==== Bàn phím ====
void handleKey(char key) {
  beep();
  Serial.println(key);

  if (key == 'A') {
    if (!changingPass) {
      if (input == password) openDoor();
      else {
        showMessage("Sai! Thu lai", "", 1500);
        beep(200, 3);
      }
      input = "";
    } else if (!confirmedOld) {
      bool ok = input == password;
      input = "";
      if (ok) {
        confirmedOld = true;
        showPrompt();
      } else {
        changingPass = false;
        showMessage("Sai MK cu!", "", 2000);
      }
    } else {
      password = input;
      changingPass = false;
      confirmedOld = false;
      input = "";
      showMessage("Da doi MK", "", 2000);
    }
  } else if (key == 'B') {
    // Đóng garage không cần mật khẩu, mở thì cần
    if (garageRunning) return;
    if (lastCommand == "open") {
      startGarage("close", true);
      showMessage("Dong garage", "", 1500);
    } else if (input == password) {
      startGarage("open", true);
      showMessage("Mo garage", "", 1500);
    } else {
      showMessage("Sai! Thu lai", "", 1500);
      beep(200, 3);
    }
    input = "";
  } else if (key == 'C') {
    input = "";
    showPrompt();
  } else if (key == 'D') {
    changingPass = true;
    confirmedOld = false;
    input = "";
    showPrompt();
  } else {
    input += key;
    if (input.length() > 12) input.remove(0, 1);  // vừa một dòng LCD
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

// ==== DHT11 + LCD nhiệt độ ====
// Đọc mỗi 2 giây. Gửi lên Firebase khi đổi đủ nhiều, cách nhau ít nhất 10 giây, như bao_trom_va_do_am.
void readClimate() {
  unsigned long now = millis();
  if (now - lastSensorRead < 2000) return;
  lastSensorRead = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    if (hasLcdClimate) {
      lcdClimate.setCursor(0, 0);
      lcdClimate.print("Loi cam bien DHT");
      lcdClimate.setCursor(0, 1);
      lcdClimate.print("                ");
    }
    if (lastTemp != -999) {
      Serial.println(F("[DHT11] -> Bo qua doc DHT11 (Chua cam hoac khong co tin hieu tai D3)"));
      lastTemp = -999;
    }
    lastHumi = -1000;
    return;
  }

  if (abs(t - lastTemp) >= 0.1 || abs(h - lastHumi) >= 0.1) {
    if (hasLcdClimate) {
      lcdClimate.setCursor(0, 0);
      lcdClimate.print("Nhiet do: ");
      lcdClimate.print(t, 1);
      lcdClimate.write(223);
      lcdClimate.print("C  ");
      lcdClimate.setCursor(0, 1);
      lcdClimate.print("Do am  : ");
      lcdClimate.print(h, 1);
      lcdClimate.print("%   ");
    }
    lastTemp = t;
    lastHumi = h;
    Serial.printf("[DHT11] Nhiet do: %.1f C | Do am: %.1f %%\n", t, h);
  }

  bool changed = abs(t - sentTemp) >= 0.5 || abs(h - sentHumi) >= 1;
  if (changed && (lastUpload == 0 || now - lastUpload >= 10000) && firebaseReady()) {
    lastUpload = now;
    bool okT = fbSetFloat(PATH_TEMPERATURE, t);
    bool okH = fbSetFloat(PATH_HUMIDITY, h);
    if (okT && okH) {
      sentTemp = t;
      sentHumi = h;
    }
  }
}

bool checkI2CDevice(uint8_t addr, const char* name) {
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();
  if (err == 0) {
    Serial.printf("   + [I2C 0x%02X] %s: OK (Da ket noi)\n", addr, name);
    return true;
  } else {
    Serial.printf("   - [I2C 0x%02X] %s: Khong phan hoi -> BO QUA\n", addr, name);
    return false;
  }
}

// ==== Setup, loop ====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n===================================================="));
  Serial.println(F("   KHOI DONG SMARTHOUSE TANG 1 (ESP8266 NODEMCU)    "));
  Serial.println(F("===================================================="));

  // 1. BẮT BUỘC KIỂM TRA WI-FI & FIREBASE TRƯỚC TIÊN
  Serial.println(F("[1/3] KIEM TRA KET NOI WI-FI & FIREBASE (BAT BUOC)..."));
  wifiShare.begin(DEVICE_NAME, SETUP_SSID, SETUP_PASSWORD);
  int waitCount = 0;
  while (!wifiShare.online() && waitCount < 20) {
    delay(500);
    Serial.print(".");
    waitCount++;
  }
  Serial.println();
  wifiShare.loop();
  if (wifiShare.online()) {
    initFirebase();
  } else {
    Serial.println(F("[WiFi] -> Chua ket noi duoc ngay, se tiep tuc thu ket noi ngam trong loop()..."));
  }

  // 2. KHỞI TẠO CÁC CHÂN ĐIỀU KHIỂN CƠ BẢN
  Serial.println(F("[2/3] Khoi tao cac chan dieu khien (Garage D5/D6, Quat D7, LED D0/D8)..."));
  #if ENABLE_RX_BUZZER
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  #endif
  pinMode(GARAGE_IN1, OUTPUT);
  pinMode(GARAGE_IN2, OUTPUT);
  stopGarage();
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);

  // 3. KIỂM TRA NGOẠI VI (NẾU KHÔNG VÀO ĐƯỢC THÌ TỰ ĐỘNG BỎ QUA KHÔNG TREO MẠCH)
  Serial.println(F("[3/3] Kiem tra ngoai vi I2C / Cam bien (Tu dong bo qua neu chua cam)..."));
  pinMode(D1, INPUT_PULLUP);
  pinMode(D2, INPUT_PULLUP);
  delay(5);
  if (digitalRead(D1) == LOW || digitalRead(D2) == LOW) {
    Serial.println(F("   - Bus I2C (D1/D2) dang bi keo LOW -> BO QUA toan bo LCD & Keypad!"));
  } else {
    Wire.begin(D2, D1);
    Wire.setClockStretchLimit(1500);
    hasKeypad = checkI2CDevice(KEYPAD_ADDR, "Ban phim PCF8574");
    hasLcdDoor = checkI2CDevice(LCD_DOOR_ADDR, "LCD Cua chinh");
    hasLcdClimate = checkI2CDevice(LCD_CLIMATE_ADDR, "LCD Nhiet do");

    if (hasKeypad) kpad.begin();
    if (hasLcdDoor) {
      lcdDoor.init();
      lcdDoor.backlight();
      showPrompt();
    }
    if (hasLcdClimate) {
      lcdClimate.init();
      lcdClimate.backlight();
      lcdClimate.print("Doc cam bien...");
    }
  }

  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_CLOSED);
  dht.begin();
  Serial.println(F("=== HOAN TAT KHOI DONG! BAT DAU VONG LAP CHINH ===\n"));
}

void loop() {
  wifiShare.loop();
  pollFirebaseDevices();
  readKeypad();
  runBeeper();
  closeDoorIfDue();
  runGarage();
  readClimate();

  if (lcdDoorRestoreAt != 0 && !doorOpen && (long)(millis() - lcdDoorRestoreAt) >= 0) showPrompt();
}