#ifndef WIFI_SHARE_H
#define WIFI_SHARE_H

#include <Arduino.h>
#include <ESP8266WiFi.h>

class WiFiShare {
public:
  void begin(const char* deviceName, const char* ssid, const char* password) {
    _ssid = ssid;
    _password = password;
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    if (deviceName && deviceName[0] != '\0') {
      WiFi.hostname(deviceName);
    }
    WiFi.setAutoReconnect(true);
    WiFi.begin(_ssid, _password);
    unsigned long now = millis();
    _lastReconnectAttempt = now;
    _lastStatusPrint = now;
    Serial.printf("[WiFi] Dang ket noi Wi-Fi '%s'...\n", _ssid);
  }

  void loop() {
    wl_status_t st = WiFi.status();
    bool connected = (st == WL_CONNECTED);
    unsigned long now = millis();

    if (connected && !_wasConnected) {
      _wasConnected = true;
      Serial.printf("[WiFi] -> THANH CONG! IP: %s | Song: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else if (!connected && _wasConnected) {
      _wasConnected = false;
      Serial.println(F("[WiFi] -> CANH BAO: Mat ket noi Wi-Fi!"));
      _lastReconnectAttempt = now;
      _lastStatusPrint = now;
    } else if (!connected) {
      if (now - _lastStatusPrint >= 2000) {
        _lastStatusPrint = now;
        if (st == WL_NO_SSID_AVAIL) {
          Serial.printf("[WiFi] Dang cho '%s'... (Khong tim thay ten Wi-Fi nay! Kiem tra bang tan 2.4GHz)\n", _ssid);
        } else if (st == WL_WRONG_PASSWORD || st == WL_CONNECT_FAILED) {
          Serial.printf("[WiFi] Dang cho '%s'... (Loi ket noi / Sai mat khau!)\n", _ssid);
        } else {
          Serial.printf("[WiFi] Dang cho ket noi Wi-Fi '%s'...\n", _ssid);
        }
      }
      if (now - _lastReconnectAttempt >= 15000) {
        _lastReconnectAttempt = now;
        Serial.printf("[WiFi] Thu ket noi lai '%s'...\n", _ssid);
        WiFi.disconnect();
        WiFi.begin(_ssid, _password);
      }
    }
  }

  bool online() const {
    return WiFi.status() == WL_CONNECTED;
  }

private:
  const char* _ssid = "";
  const char* _password = "";
  bool _wasConnected = false;
  unsigned long _lastReconnectAttempt = 0;
  unsigned long _lastStatusPrint = 0;
};

#endif // WIFI_SHARE_H
