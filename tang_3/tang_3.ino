/**
 * ============================================================================
 * SMARTHOUSE V4 - FIRMWARE TỔNG HỢP TẦNG 3 (ESP8266 NODEMCU)
 * ============================================================================
 * Vi điều khiển: ESP8266 NodeMCU (ESP-12E)
 * Tích hợp toàn bộ các thiết bị ngoại vi Tầng 3 theo Khóa Luận Tốt Nghiệp (KLTN):
 *  1. Khóa cửa thông minh bằng cảm biến vân tay AS608 (van_tay.ino - KLTN mục 3.1.6):
 *     - Cảm biến vân tay quang học AS608 giao tiếp qua SoftwareSerial (D7, D8) ở
 *       tốc độ 57600 baud + Động cơ Servo SG90 (D4) + Còi Buzzer (D5).
 *     - Khi vân tay hợp lệ: Còi kêu 300ms, Servo mở cửa 2 giây rồi tự đóng,
 *       đồng thời TỰ ĐỘNG BẬT ĐÈN PHÒNG TẦNG 3 (D6) và đẩy trạng thái cửa
 *       (home/door_status) lên giao diện Web theo thời gian thực.
 *     - Hỗ trợ đăng ký vân tay mới trực tiếp qua Serial Monitor (gõ: ENROLL <id>).
 *  2. Hệ thống chiếu sáng Tầng 3 (KLTN mục 3.2.2, 3.2.3 & 3.3.3):
 *     - Điều khiển Relay/LED đèn phòng ngủ & phòng làm việc Tầng 3 từ xa qua Web
 *       (nhánh Firebase: home/light_room_3) hoặc tự động sáng khi mở cửa.
 *
 * Thông số Firebase (từ SmartHouseV4/sdk.doc):
 *  - Project ID:   smart-house-fcd4f
 *  - Database URL: https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app/
 *  - Link Web:     https://smart-house-orpin.vercel.app/nha-iot.html
 * ============================================================================
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <Adafruit_Fingerprint.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ============================================================================
// 1. CẤU HÌNH WI-FI & FIREBASE (THEO FILE SmartHouseV4/sdk.doc)
// ============================================================================
#define WIFI_SSID       "Tesla_physics"   // Đổi tên Wi-Fi 2.4GHz của bạn tại đây
#define WIFI_PASSWORD   "top102102"       // Đổi mật khẩu Wi-Fi của bạn tại đây

#define API_KEY         "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY"
#define DATABASE_URL    "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app/"

// Đặt true nếu Realtime Database đang bật "Test Mode" (.read: true, .write: true)
// Đặt false nếu bạn dùng Firebase Authentication (Email/Password)
#define USE_FIREBASE_TEST_MODE true
#define USER_EMAIL      "truongkhanhlinh22102003@gmail.com"
#define USER_PASSWORD   "Khanhlinh2210@@"

// ============================================================================
// 2. ĐỊNH NGHĨA SƠ ĐỒ CHÂN TRÊN ESP8266 NODEMCU (THEO van_tay.ino & KLTN HÌNH 3.13)
// ============================================================================
#define LIGHT_RELAY_PIN   D6    // GPIO12 - Relay điều khiển đèn Tầng 3
#define BUZZER_PIN        D5    // GPIO14 - Còi Buzzer báo vân tay
#define SERVO_DOOR_PIN    D4    // GPIO2  - Servo SG90 mở khóa cửa Tầng 3
#define FINGER_RX_PIN     D7    // GPIO13 - SoftwareSerial RX (nối chân TX của AS608)
#define FINGER_TX_PIN     D8    // GPIO15 - SoftwareSerial TX (nối chân RX của AS608)

// ============================================================================
// 3. KHỞI TẠO ĐỐI TƯỢNG PHẦN CỨNG & FIREBASE
// ============================================================================
SoftwareSerial fingerSerial(FINGER_RX_PIN, FINGER_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

Servo doorServo;
const int SERVO_OPEN_POS  = 180; // Góc mở cửa theo van_tay.ino
const int SERVO_CLOSE_POS = 0;   // Góc đóng cửa theo van_tay.ino

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ============================================================================
// 4. BIẾN TRẠNG THÁI HỆ THỐNG (NON-BLOCKING TIMERS)
// ============================================================================
bool fingerprintSensorReady = false;
int lastFingerprintID = -1;
bool isDoorOpen = false;
unsigned long doorOpenedAt = 0;
const unsigned long DOOR_OPEN_DURATION = 2000; // Giữ cửa mở 2 giây theo van_tay.ino

unsigned long lastFingerScanTime = 0;
const unsigned long FINGER_SCAN_INTERVAL = 150; // Quét vân tay mỗi 150ms

bool roomLightState = false;

bool buzzerBeeping = false;
unsigned long buzzerStartedAt = 0;
unsigned long buzzerDuration = 0;

unsigned long lastFirebasePollTime = 0;
const unsigned long FIREBASE_POLL_INTERVAL = 500;

// ============================================================================
// 5. CÁC HÀM ĐIỀU KHIỂN ĐÈN, CỬA, CÒI & ĐĂNG KÝ VÂN TAY
// ============================================================================

void triggerBuzzer(unsigned long durationMs = 300) {
  digitalWrite(BUZZER_PIN, HIGH);
  buzzerBeeping = true;
  buzzerStartedAt = millis();
  buzzerDuration = durationMs;
}

void setRoomLight(bool turnOn, bool syncToFirebase = true) {
  roomLightState = turnOn;
  digitalWrite(LIGHT_RELAY_PIN, turnOn ? HIGH : LOW);

  if (syncToFirebase && Firebase.ready()) {
    Firebase.RTDB.setBool(&fbdo, "/home/light_room_3", turnOn);
    Firebase.RTDB.setBool(&fbdo, "/home/floor_3/light", turnOn);
  }
}

void updateDoorStatusOnFirebase(bool open) {
  if (Firebase.ready()) {
    Firebase.RTDB.setBool(&fbdo, "/home/door_status", open);
    Firebase.RTDB.setBool(&fbdo, "/home/floor_3/door", open);
  }
}

void openDoorByFingerprint() {
  doorServo.write(SERVO_OPEN_POS);
  isDoorOpen = true;
  doorOpenedAt = millis();
  Serial.println("🔓 Cửa Tầng 3: ĐANG MỞ (2 giây)...");

  // Khi cửa mở đồng thời đèn trong phòng Tầng 3 tự động bật sáng (KLTN mục 3.1.6 trang 36)
  setRoomLight(true, true);
  updateDoorStatusOnFirebase(true);
}

void closeDoor() {
  doorServo.write(SERVO_CLOSE_POS);
  isDoorOpen = false;
  Serial.println("🔒 Cửa Tầng 3: ĐÃ ĐÓNG.");
  updateDoorStatusOnFirebase(false);
}

// Hàm đọc ID vân tay từ cảm biến AS608 (theo van_tay.ino)
int getFingerprintID() {
  if (!fingerprintSensorReady) return -1;

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    if (p == FINGERPRINT_NOTFOUND) {
      Serial.println("❌ Vân tay KHÔNG HỢP LỆ! Cửa vẫn đóng.");
      triggerBuzzer(600);
    }
    return -1;
  }

  return finger.fingerID;
}

// Hàm đăng ký mẫu vân tay mới qua Serial Monitor (Lệnh: ENROLL <id>)
bool enrollFingerprint(uint8_t id) {
  if (!fingerprintSensorReady) {
    Serial.println("❌ Cảm biến vân tay chưa sẵn sàng!");
    return false;
  }
  Serial.printf("👉 Đang đăng ký vân tay ID #%d. Vui lòng đặt ngón tay lên cảm biến...\n", id);
  int p = -1;
  unsigned long startWait = millis();
  while (p != FINGERPRINT_OK) {
    if (millis() - startWait > 15000) {
      Serial.println("⏱️ Hết thời gian chờ đặt ngón tay!");
      return false;
    }
    p = finger.getImage();
    delay(50);
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("❌ Lỗi chuyển đổi ảnh vân tay lần 1.");
    return false;
  }
  Serial.println("☝️ Hãy nhấc ngón tay ra...");
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }
  Serial.println("👉 Đặt lại CÙNG ngón tay đó một lần nữa...");
  startWait = millis();
  p = -1;
  while (p != FINGERPRINT_OK) {
    if (millis() - startWait > 15000) {
      Serial.println("⏱️ Hết thời gian chờ đặt ngón tay lần 2!");
      return false;
    }
    p = finger.getImage();
    delay(50);
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    Serial.println("❌ Lỗi chuyển đổi ảnh vân tay lần 2.");
    return false;
  }
  if (finger.createModel() != FINGERPRINT_OK) {
    Serial.println("❌ Hai lần đặt vân tay không khớp nhau!");
    return false;
  }
  if (finger.storeModel(id) == FINGERPRINT_OK) {
    Serial.printf("✅ Đã lưu thành công vân tay ID #%d vào bộ nhớ AS608!\n", id);
    triggerBuzzer(300);
    return true;
  }
  Serial.println("❌ Lỗi lưu mẫu vân tay vào bộ nhớ.");
  return false;
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toUpperCase();
  if (line.startsWith("ENROLL")) {
    int id = line.substring(6).toInt();
    if (id >= 1 && id <= 127) {
      enrollFingerprint((uint8_t)id);
    } else {
      Serial.println("ℹ️ Cú pháp đăng ký vân tay: ENROLL <1..127> (Ví dụ: ENROLL 1)");
    }
  } else if (line == "OPEN") {
    openDoorByFingerprint();
  }
}

// ============================================================================
// 6. KẾT NỐI WI-FI & KHỞI TẠO FIREBASE REALTIME DATABASE
// ============================================================================

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("🔌 Đang kết nối Wi-Fi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Đã kết nối Wi-Fi! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n⚠️ Không kết nối được Wi-Fi, khóa vân tay vẫn hoạt động Offline.");
  }
}

void initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  #if USE_FIREBASE_TEST_MODE
    config.signer.test_mode = true;
  #else
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
  #endif

  Firebase.begin(&config, &auth);
  Firebase.reconnectNetwork(true);

  Serial.println("🔄 Đang kết nối Firebase: " + String(DATABASE_URL));
  unsigned long timeout = millis();
  while (!Firebase.ready() && millis() - timeout < 10000) {
    delay(100);
  }

  if (Firebase.ready()) {
    Serial.println("✅ Kết nối Firebase SmartHouseV4 (ESP8266 Tầng 3) thành công!");
    Firebase.RTDB.setString(&fbdo, "/home/thongbao", "ESP8266 Tầng 3 đã kết nối (SmartHouseV4)");

    if (Firebase.RTDB.getBool(&fbdo, "/home/light_room_3")) {
      setRoomLight(fbdo.boolData(), false);
    } else {
      Firebase.RTDB.setBool(&fbdo, "/home/light_room_3", false);
    }

    updateDoorStatusOnFirebase(false);
  } else {
    Serial.print("❌ Lỗi kết nối Firebase: ");
    Serial.println(fbdo.errorReason());
  }
}

// ============================================================================
// 7. XỬ LÝ QUÉT VÂN TAY, TỰ ĐÓNG CỬA & ĐỒNG BỘ LỆNH TỪ WEB
// ============================================================================

void handleLocalPeripherals() {
  unsigned long now = millis();

  // 1. Tắt còi Buzzer sau khi hết thời gian kêu (non-blocking)
  if (buzzerBeeping && (now - buzzerStartedAt >= buzzerDuration)) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerBeeping = false;
  }

  // 2. Tự động đóng cửa Servo Tầng 3 sau 2 giây mở
  if (isDoorOpen && (now - doorOpenedAt >= DOOR_OPEN_DURATION)) {
    closeDoor();
  }

  // 3. Quét cảm biến vân tay AS608 định kỳ mỗi 150ms
  if (fingerprintSensorReady && (now - lastFingerScanTime >= FINGER_SCAN_INTERVAL)) {
    lastFingerScanTime = now;
    int id = getFingerprintID();

    if (id >= 0 && id != lastFingerprintID) {
      lastFingerprintID = id;
      Serial.printf("✅ Vân tay hợp lệ! ID #%d (Độ tin cậy: %d)\n", id, finger.confidence);

      triggerBuzzer(300);
      openDoorByFingerprint();
    } else if (id == -1) {
      lastFingerprintID = -1;
    }
  }
}

void pollFirebaseCommands() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  unsigned long now = millis();
  if (now - lastFirebasePollTime < FIREBASE_POLL_INTERVAL) return;
  lastFirebasePollTime = now;

  // Lắng nghe công tắc Đèn Tầng 3 từ Web: home/light_room_3
  if (Firebase.RTDB.getBool(&fbdo, "/home/light_room_3")) {
    bool webLight = fbdo.boolData();
    if (webLight != roomLightState) {
      setRoomLight(webLight, false);
      Firebase.RTDB.setBool(&fbdo, "/home/floor_3/light", webLight);
      Serial.print("💡 Đèn Tầng 3 (từ Web): ");
      Serial.println(webLight ? "BẬT" : "TẮT");
    }
  }
}

// ============================================================================
// 8. SETUP & LOOP CHÍNH
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n====================================================");
  Serial.println("   SMARTHOUSE V4 - ESP8266 NODEMCU TANG 3           ");
  Serial.println("====================================================");

  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  doorServo.attach(SERVO_DOOR_PIN);
  doorServo.write(SERVO_CLOSE_POS);

  // Khởi tạo cảm biến vân tay AS608 qua SoftwareSerial (D7, D8) ở 57600 baud
  fingerSerial.begin(57600);
  finger.begin(57600);
  delay(100);

  Serial.println("🔍 Đang kiểm tra cảm biến vân tay AS608...");
  if (finger.verifyPassword()) {
    fingerprintSensorReady = true;
    finger.getTemplateCount();
    Serial.printf("✅ Cảm biến vân tay AS608 sẵn sàng! Số vân tay đã lưu: %d\n", finger.templateCount);
    Serial.println("💡 Mẹo: Gõ 'ENROLL <id>' vào Serial Monitor để đăng ký vân tay mới.");
  } else {
    fingerprintSensorReady = false;
    Serial.println("⚠️ Không tìm thấy cảm biến vân tay AS608 (kiểm tra dây D7/D8). Hệ thống vẫn chạy Wi-Fi/Đèn.");
  }

  connectToWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  // 1. Nhận lệnh đăng ký vân tay từ Serial Monitor (ENROLL <id>)
  handleSerialCommands();

  // 2. Quét vân tay AS608 & tự đóng cửa Servo
  handleLocalPeripherals();

  // 3. Đồng bộ nhận lệnh bật/tắt đèn Tầng 3 từ Web Firebase
  pollFirebaseCommands();
}
