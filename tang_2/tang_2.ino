/**
 * ============================================================================
 * SMARTHOUSE V4 - FIRMWARE TỔNG HỢP TẦNG 2 (ESP8266 NODEMCU)
 * ============================================================================
 * Vi điều khiển: ESP8266 NodeMCU (ESP-12E)
 * Tích hợp toàn bộ các thiết bị ngoại vi Tầng 2 theo Khóa Luận Tốt Nghiệp (KLTN):
 *  1. Hệ thống báo cháy & rò rỉ khí gas phòng bếp (baochay_denl2.ino - KLTN mục 3.1.5):
 *     - Cảm biến khí/khói MQ-2 đọc mức khói qua chân Analog A0 (0..1023, ngưỡng 120).
 *     - Khi nồng độ khói vượt ngưỡng an toàn: tự động kích hoạt còi Buzzer báo động
 *       và gửi cảnh báo thời gian thực lên Web ("PHÁT HIỆN KHÓI").
 *  2. Điều khiển Còi cảnh báo Tầng 2 từ xa qua Web (KLTN mục 3.2.3 - Hình 3.24):
 *     - Bật/tắt còi cảnh báo chủ động hoặc ngắt còi từ giao diện Web Firebase.
 *  3. Hệ thống chiếu sáng thông minh Tầng 2 (KLTN mục 3.1.1 & 3.3.2):
 *     - Đèn phòng làm việc & phòng bếp điều khiển song song qua công tắc trên Web
 *       (home/floor_2/light) VÀ nút nhấn vật lý tại chân D5 (có chống dội phím).
 *     - Khi bấm nút vật lý tại phòng, trạng thái đèn tự động đồng bộ ngược lên Web.
 *  4. Màn hình OLED 0.96" SSD1306 I2C (0x3C) khu vực sinh hoạt Tầng 2 (led_oled.ino):
 *     - Hiển thị lời chào "Smart Home WELCOME!", thông điệp không gian sống, mức khói
 *       hiện tại và chuyển sang màn hình cảnh báo khẩn cấp khi phát hiện cháy.
 *
 * Thông số Firebase (từ SmartHouseV4/sdk.doc):
 *  - Project ID:   smart-house-fcd4f
 *  - Database URL: https://smart-house-fcd4f-default-rtdb.asia-southeast1.firebasedatabase.app/
 *  - Link Web:     https://smart-house-orpin.vercel.app/nha-iot.html
 * ============================================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

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
// 2. ĐỊNH NGHĨA SƠ ĐỒ CHÂN TRÊN ESP8266 NODEMCU (THEO baochay_denl2.ino)
// ============================================================================
#define OLED_I2C_ADDR       0x3C
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64

#define SMOKE_ANALOG_PIN    A0    // ADC0   - Chân Analog đọc mức khói MQ-2
#define BUZZER_PIN          D1    // GPIO5  - Còi báo khói Tầng 2
#define SWITCH_BTN_PIN      D5    // GPIO14 - Nút nhấn vật lý bật/tắt đèn (nối GND)
#define LIGHT_RELAY_PIN     D6    // GPIO12 - Relay / LED đèn phòng Tầng 2
#define I2C_SDA_PIN         D2    // GPIO4  - SDA cho màn hình OLED 0.96" (nếu lắp kèm)
#define I2C_SCL_PIN         D3    // GPIO0  - SCL cho màn hình OLED 0.96" (nếu lắp kèm)

#define SMOKE_THRESHOLD     120   // Ngưỡng ADC 10-bit (0..1023) theo baochay_denl2.ino

// ============================================================================
// 3. KHỞI TẠO ĐỐI TƯỢNG PHẦN CỨNG & FIREBASE
// ============================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledAvailable = false;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ============================================================================
// 4. BIẾN TRẠNG THÁI HỆ THỐNG (NON-BLOCKING TIMERS)
// ============================================================================
// --- Đèn phòng Tầng 2 & Nút nhấn vật lý ---
bool roomLightState = false;
int buttonState = HIGH;
int lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

// --- Cảm biến khói MQ-2 & Còi báo cháy ---
int currentSmokeLevel = 0;
int lastSyncedSmokeLevel = -100;
bool smokeDetected = false;
bool lastSyncedSmokeStatus = false;

bool webBuzzerState = false;  // Công tắc "Còi cảnh báo" trên Web (home/floor_2/buzzer)
bool webBuzzerMute = false;   // Cờ tắt tiếng còi khi có khói (home/floor_2/buzzer_mute_2)

unsigned long lastSensorReadTime = 0;
const unsigned long SENSOR_READ_INTERVAL = 500; // Đọc MQ-2 mỗi 500ms

unsigned long lastFirebasePollTime = 0;
const unsigned long FIREBASE_POLL_INTERVAL = 500; // Đồng bộ lệnh Web mỗi 500ms

// --- Trạng thái hiệu ứng màn hình OLED (từ led_oled.ino) ---
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

// ============================================================================
// 5. HÀM CẬP NHẬT NGOẠI VI & HIỂN THỊ OLED
// ============================================================================

void updateBuzzerOutput() {
  bool shouldBeep = webBuzzerState || (smokeDetected && !webBuzzerMute);
  digitalWrite(BUZZER_PIN, shouldBeep ? HIGH : LOW);
}

void setRoomLight(bool turnOn, bool syncToFirebase = true) {
  roomLightState = turnOn;
  digitalWrite(LIGHT_RELAY_PIN, turnOn ? HIGH : LOW);

  if (syncToFirebase && Firebase.ready()) {
    Firebase.RTDB.setBool(&fbdo, "/home/floor_2/light", turnOn);
    Firebase.RTDB.setBool(&fbdo, "/home/floor_2/light_room_2", turnOn);
  }
}

void drawOledQuote(const char* quote) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  String q = String(quote);
  const int maxCharsPerLine = 21;
  int lineHeight = 10;
  int y = 6;

  String word = "", line = "";
  for (unsigned int i = 0; i <= q.length(); i++) {
    char c = q.charAt(i);
    if (c == ' ' || c == '\0') {
      if ((line.length() + word.length()) <= maxCharsPerLine) {
        line += word + ' ';
      } else {
        int16_t x1, y1;
        uint16_t w, h;
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
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, y);
    display.print(line);
  }
}

void handleOledDisplay() {
  if (!oledAvailable) return;
  unsigned long now = millis();
  if (now - lastOledFrameTime < 60) return;
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
      int barX = (SCREEN_WIDTH - barWidth) / 2, barY = 28;
      display.setTextSize(1);
      display.setCursor((SCREEN_WIDTH - 66) / 2, 10);
      display.print("SmartHouse V4");
      display.drawRect(barX, barY, barWidth, barHeight, SSD1306_WHITE);
      int filled = map(oledProgress, 0, 100, 0, barWidth - 2);
      display.fillRect(barX + 1, barY + 1, filled, barHeight - 2, SSD1306_WHITE);
      display.setCursor(20, 48);
      display.printf("Khoi: %d | Den: %s", currentSmokeLevel, roomLightState ? "ON" : "OFF");
      oledProgress += 4;
      if (oledProgress > 100) {
        oledProgress = 0;
        oledState = 1;
        oledStateStartTime = now;
      }
      break;
    }
    case 1: {
      String line1 = "Smart Home";
      String line2 = "WELCOME!";
      int len1 = (now - oledStateStartTime) / 120;
      int len2 = (int)(now - oledStateStartTime - 700) / 120;
      if (len1 > (int)line1.length()) len1 = line1.length();
      if (len2 < 0) len2 = 0;
      if (len2 > (int)line2.length()) len2 = line2.length();

      display.setTextSize(2);
      display.setCursor(4, 12);
      display.print(line1.substring(0, len1));
      display.setCursor(16, 36);
      display.print(line2.substring(0, len2));

      if (now - oledStateStartTime > 4000) {
        oledState = 2;
        oledStateStartTime = now;
      }
      break;
    }
    default: {
      drawOledQuote(quotes[quoteIndex]);
      display.setCursor(18, 52);
      display.printf("Khoi:%d Den:%s", currentSmokeLevel, roomLightState ? "ON" : "OFF");
      if (now - oledStateStartTime > 4000) {
        quoteIndex++;
        if (quoteIndex >= quoteCount) {
          quoteIndex = 0;
          oledState = 0;
        }
        oledStateStartTime = now;
      }
      break;
    }
  }

  display.display();
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
    Serial.println("\n⚠️ Không kết nối được Wi-Fi, hệ thống báo cháy & nút nhấn vẫn hoạt động Offline.");
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
    Serial.println("✅ Kết nối Firebase SmartHouseV4 (ESP8266 Tầng 2) thành công!");
    Firebase.RTDB.setString(&fbdo, "/home/thongbao", "ESP8266 Tầng 2 đã kết nối (SmartHouseV4)");

    if (Firebase.RTDB.getBool(&fbdo, "/home/floor_2/light")) {
      setRoomLight(fbdo.boolData(), false);
    } else {
      Firebase.RTDB.setBool(&fbdo, "/home/floor_2/light", false);
    }

    Firebase.RTDB.setBool(&fbdo, "/home/floor_2/smoke_status", false);
    Firebase.RTDB.setBool(&fbdo, "/home/floor_2/smoke_detected_2", false);
    Firebase.RTDB.setInt(&fbdo, "/home/floor_2/smoke_level_2", 0);
  } else {
    Serial.print("❌ Lỗi kết nối Firebase: ");
    Serial.println(fbdo.errorReason());
  }
}

// ============================================================================
// 7. XỬ LÝ NÚT NHẤN VẬT LÝ & CẢM BIẾN KHÓI MQ-2 (KLTN MỤC 3.1.5)
// ============================================================================

void handlePhysicalSwitch() {
  int reading = digitalRead(SWITCH_BTN_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        bool newState = !roomLightState;
        setRoomLight(newState, true);
        Serial.print("🔘 Nút nhấn vật lý Tầng 2 -> Đèn: ");
        Serial.println(newState ? "BẬT" : "TẮT");
      }
    }
  }
  lastButtonReading = reading;
}

void handleSmokeSensor() {
  unsigned long now = millis();
  if (now - lastSensorReadTime < SENSOR_READ_INTERVAL) return;
  lastSensorReadTime = now;

  currentSmokeLevel = analogRead(SMOKE_ANALOG_PIN);
  smokeDetected = (currentSmokeLevel > SMOKE_THRESHOLD);

  updateBuzzerOutput();

  if (Firebase.ready()) {
    if (smokeDetected != lastSyncedSmokeStatus) {
      lastSyncedSmokeStatus = smokeDetected;
      Firebase.RTDB.setBool(&fbdo, "/home/floor_2/smoke_status", smokeDetected);
      Firebase.RTDB.setBool(&fbdo, "/home/floor_2/smoke_detected_2", smokeDetected);
      Firebase.RTDB.setBool(&fbdo, "/home/floor_2/buzzer", smokeDetected || webBuzzerState);

      if (smokeDetected) {
        Serial.printf("🚨 PHÁT HIỆN KHÓI/GAS TẦNG 2! Mức đo: %d\n", currentSmokeLevel);
      } else {
        Serial.println("✅ Không khí Tầng 2 đã trở lại bình thường.");
      }
    }

    if (abs(currentSmokeLevel - lastSyncedSmokeLevel) >= 5) {
      lastSyncedSmokeLevel = currentSmokeLevel;
      Firebase.RTDB.setInt(&fbdo, "/home/floor_2/smoke_level_2", currentSmokeLevel);
      Serial.printf("🌫️ Giá trị cảm biến khói MQ-2: %d\n", currentSmokeLevel);
    }
  }
}

// ============================================================================
// 8. ĐỒNG BỘ LỆNH ĐIỀU KHIỂN TỪ WEB FIREBASE
// ============================================================================

void pollFirebaseCommands() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  unsigned long now = millis();
  if (now - lastFirebasePollTime < FIREBASE_POLL_INTERVAL) return;
  lastFirebasePollTime = now;

  // 1. Lắng nghe công tắc Đèn phòng Tầng 2 trên Web: home/floor_2/light
  if (Firebase.RTDB.getBool(&fbdo, "/home/floor_2/light")) {
    bool webLight = fbdo.boolData();
    if (webLight != roomLightState) {
      setRoomLight(webLight, false);
      Firebase.RTDB.setBool(&fbdo, "/home/floor_2/light_room_2", webLight);
      Serial.print("💡 Đèn phòng Tầng 2 (từ Web): ");
      Serial.println(webLight ? "BẬT" : "TẮT");
    }
  }

  // 2. Lắng nghe công tắc Còi cảnh báo Tầng 2 trên Web: home/floor_2/buzzer
  if (Firebase.RTDB.getBool(&fbdo, "/home/floor_2/buzzer")) {
    bool webBuzzer = fbdo.boolData();
    if (webBuzzer != webBuzzerState) {
      webBuzzerState = webBuzzer;
      if (!webBuzzerState && smokeDetected) {
        webBuzzerMute = true;
      } else if (webBuzzerState) {
        webBuzzerMute = false;
      }
      updateBuzzerOutput();
      Serial.print("⚠️ Còi cảnh báo Tầng 2 (từ Web): ");
      Serial.println(webBuzzerState ? "BẬT" : "TẮT");
    }
  }
}

// ============================================================================
// 9. SETUP & LOOP CHÍNH
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n====================================================");
  Serial.println("   SMARTHOUSE V4 - ESP8266 NODEMCU TANG 2           ");
  Serial.println("====================================================");

  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SWITCH_BTN_PIN, INPUT_PULLUP);
  pinMode(SMOKE_ANALOG_PIN, INPUT);

  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 24);
    display.print("ESP8266 Tang 2...");
    display.display();
    oledStateStartTime = millis();
    Serial.println("🖥️ Đã kết nối màn hình OLED 0.96\" (0x3C).");
  } else {
    Serial.println("ℹ️ Không tìm thấy OLED 0x3C (bỏ qua hiển thị OLED).");
  }

  connectToWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    initFirebase();
  }
}

void loop() {
  // 1. Quét nút nhấn vật lý bật/tắt đèn phòng Tầng 2
  handlePhysicalSwitch();

  // 2. Đọc cảm biến khói/gas MQ-2 và kích hoạt báo cháy tự động
  handleSmokeSensor();

  // 3. Cập nhật hiệu ứng màn hình OLED (nếu có gắn OLED)
  handleOledDisplay();

  // 4. Đồng bộ lệnh điều khiển từ Web Firebase
  pollFirebaseCommands();
}
