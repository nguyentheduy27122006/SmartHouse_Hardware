/*
 * ============================================================================
 * FILE TEST RIÊNG: ESP8266 NODEMCU + MẠCH L298N ĐIỀU KHIỂN QUẠT QUA FIREBASE
 * ============================================================================
 * Đường dẫn Firebase:
 *   - Chính: /home/floor1/light_room_1   (true = BẬT quạt, false = TẮT quạt)
 *   - Phụ:   /home/floor_1/light_room_1  (hỗ trợ song song nếu Web dùng floor_1)
 *
 * Sơ đồ đấu dây (ESP8266 NodeMCU <-> L298N <-> Quạt DC):
 *   - Chân D1 (GPIO 5)  ---> IN1 (trên L298N)
 *   - Chân D2 (GPIO 4)  ---> IN2 (trên L298N)
 *   - Chân D5 (GPIO 14) ---> ENA (trên L298N) [Hoặc giữ nguyên Jumper ENA nối 5V]
 *   - Chân GND NodeMCU  ---> GND (trên L298N) - BẮT BUỘC nối chung GND!
 *   - Cổng OUT1 & OUT2  ---> 2 dây của Quạt DC (Motor)
 *   - Cổng 12V & GND    ---> Nguồn cấp cho L298N & Quạt (5V - 12V tùy quạt)
 * ============================================================================
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ============================================================================
// 1. CẤU HÌNH WI-FI & FIREBASE
// ============================================================================
#define WIFI_SSID       "Iphone"
#define WIFI_PASSWORD   "112345678"

#define API_KEY         "AIzaSyBom2FOJvMisw2NcE89Zkr-rP_dgOSqbMY"
#define DATABASE_URL    "https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app"

// Đặt true nếu Firebase Realtime Database đang để Rules Test Mode (.read: true, .write: true)
// Đặt false nếu dùng Firebase Authentication (Email/Password)
#define USE_FIREBASE_TEST_MODE true
#define USER_EMAIL      "nguyentheduy07private@gmail.com"
#define USER_PASSWORD   "nguyentheduy2712D@"

// Đường dẫn trên Firebase Realtime Database
#define FIREBASE_PATH_MAIN "/home/floor1/light_room_1"
#define FIREBASE_PATH_ALT  "/home/floor_1/light_room_1"

// ============================================================================
// 2. ĐỊNH NGHĨA CHÂN L298N TRÊN ESP8266 NODEMCU
// ============================================================================
#define FAN_IN1_PIN     D1    // GPIO5  - Nối IN1 của L298N
#define FAN_IN2_PIN     D2    // GPIO4  - Nối IN2 của L298N
#define FAN_ENA_PIN     D5    // GPIO14 - Nối ENA của L298N (hoặc cắm Jumper ENA trên L298N)

// ============================================================================
// 3. BIẾN TOÀN CỤC
// ============================================================================
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

bool fanState = false;
bool lastMainVal = false;
bool lastAltVal = false;
bool isFirstSync = true;

unsigned long lastPollTime = 0;
const unsigned long POLL_INTERVAL = 500; // Đọc Firebase mỗi 500ms

// ============================================================================
// 4. HÀM ĐIỀU KHIỂN QUẠT QUA MẠCH L298N
// ============================================================================
void setFan(bool turnOn, const char* sourcePath) {
  fanState = turnOn;

  if (turnOn) {
    // Quay thuận: IN1 = HIGH, IN2 = LOW, ENA = HIGH
    digitalWrite(FAN_IN1_PIN, HIGH);
    digitalWrite(FAN_IN2_PIN, LOW);
    digitalWrite(FAN_ENA_PIN, HIGH);
    digitalWrite(LED_BUILTIN, LOW);  // Sáng đèn LED xanh trên mạch ESP8266 để báo hiệu
    Serial.printf("🌀 [QUẠT L298N] BẬT (ON)  <-- Nhận từ %s = true\n", sourcePath);
  } else {
    // Dừng quạt: IN1 = LOW, IN2 = LOW, ENA = LOW
    digitalWrite(FAN_IN1_PIN, LOW);
    digitalWrite(FAN_IN2_PIN, LOW);
    digitalWrite(FAN_ENA_PIN, LOW);
    digitalWrite(LED_BUILTIN, HIGH); // Tắt đèn LED trên mạch ESP8266
    Serial.printf("🛑 [QUẠT L298N] TẮT (OFF) <-- Nhận từ %s = false\n", sourcePath);
  }
}

// ============================================================================
// 5. KẾT NỐI WI-FI & FIREBASE
// ============================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("🔌 Đang kết nối Wi-Fi: ");
  Serial.print(WIFI_SSID);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Đã kết nối Wi-Fi!");
    Serial.print("📡 Địa chỉ IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ Không kết nối được Wi-Fi! Kiểm tra lại tên Wi-Fi / mật khẩu (băng tần 2.4GHz).");
  }
}

void initFirebase() {
  // Giảm bộ nhớ đệm SSL (BearSSL) để ESP8266 không bị lỗi mConnectBasicClient
  fbdo.setBSSLBufferSize(4096 /* Rx */, 1024 /* Tx */);
  fbdo.setResponseSize(2048);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;
  config.timeout.serverResponse = 10 * 1000;

  #if USE_FIREBASE_TEST_MODE
    config.signer.test_mode = true;
  #else
    auth.user.email = USER_EMAIL;
    auth.user.password = USER_PASSWORD;
  #endif

  Firebase.begin(&config, &auth);
  Firebase.reconnectNetwork(true);

  Serial.println("🔄 Đang kết nối Firebase: " + String(DATABASE_URL));
  Serial.printf("ℹ️ Free Heap: %u bytes\n", ESP.getFreeHeap());
}

// ============================================================================
// 6. SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n====================================================");
  Serial.println("   TEST ESP8266 + L298N QUAT + FIREBASE (TANG 1)    ");
  Serial.println("====================================================");

  // Cấu hình các chân điều khiển L298N
  pinMode(FAN_IN1_PIN, OUTPUT);
  pinMode(FAN_IN2_PIN, OUTPUT);
  pinMode(FAN_ENA_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // Ban đầu tắt quạt
  digitalWrite(FAN_IN1_PIN, LOW);
  digitalWrite(FAN_IN2_PIN, LOW);
  digitalWrite(FAN_ENA_PIN, LOW);
  digitalWrite(LED_BUILTIN, HIGH);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  unsigned long now = millis();
  if (now - lastPollTime < POLL_INTERVAL) return;
  lastPollTime = now;

  // Khởi tạo node /home/floor1/light_room_1 lần đầu nếu trên Firebase chưa có
  if (isFirstSync) {
    isFirstSync = false;
    Serial.println("✅ Firebase đã sẵn sàng! Đang đồng bộ trạng thái ban đầu...");
    if (Firebase.RTDB.getBool(&fbdo, FIREBASE_PATH_MAIN)) {
      lastMainVal = fbdo.boolData();
      setFan(lastMainVal, FIREBASE_PATH_MAIN);
    } else {
      // Nếu trên Firebase chưa có nhánh /home/floor1/light_room_1 thì tạo sẵn = false
      if (Firebase.RTDB.setBool(&fbdo, FIREBASE_PATH_MAIN, false)) {
        Serial.println("📝 Đã tạo sẵn node /home/floor1/light_room_1 = false trên Firebase.");
      } else {
        Serial.println("⚠️ Lỗi ghi Firebase: " + fbdo.errorReason());
      }
    }
    return;
  }

  // 1. Đọc nhánh chính: /home/floor1/light_room_1
  if (Firebase.RTDB.getBool(&fbdo, FIREBASE_PATH_MAIN)) {
    bool val = fbdo.boolData();
    if (val != lastMainVal) {
      lastMainVal = val;
      setFan(val, FIREBASE_PATH_MAIN);
    }
  } else {
    Serial.println("⚠️ Lỗi đọc Firebase [" + String(FIREBASE_PATH_MAIN) + "]: " + fbdo.errorReason());
  }

  // 2. Đọc nhánh phụ (nếu Web bật/tắt ở /home/floor_1/light_room_1)
  if (Firebase.RTDB.getBool(&fbdo, FIREBASE_PATH_ALT)) {
    bool valAlt = fbdo.boolData();
    if (valAlt != lastAltVal) {
      lastAltVal = valAlt;
      setFan(valAlt, FIREBASE_PATH_ALT);
    }
  }
}
