/**
 * Thư viện Keypad_I2C tích hợp sẵn cho module mở rộng I2C PCF8574 (0x20)
 * Tương thích với thư viện chuẩn <Keypad.h> của Arduino / PlatformIO
 */
#ifndef KEYPAD_I2C_H
#define KEYPAD_I2C_H

#include <Arduino.h>
#include <Keypad.h>
#include <Wire.h>

class Keypad_I2C : public Keypad {
public:
  Keypad_I2C(char* userKeymap, byte* row, byte* col, byte numRows, byte numCols,
             byte address, byte width = 1, TwoWire* awire = &Wire)
      : Keypad(userKeymap, row, col, numRows, numCols),
        i2caddr(address),
        i2cwidth(width),
        _wire(awire),
        pinState(0xFF) {}

  void begin() {
    _wire->begin();
    pinState = 0xFF;
    port_write(pinState);
  }

  void pin_mode(byte pinNum, byte mode) override {}

  void pin_write(byte pinNum, boolean level) override {
    word mask = 1 << pinNum;
    if (level == HIGH) {
      pinState |= mask;
    } else {
      pinState &= ~mask;
    }
    port_write(pinState);
  }

  int pin_read(byte pinNum) override {
    word mask = 1 << pinNum;
    _wire->requestFrom((int)i2caddr, (int)i2cwidth);
    word pinVal = 0xFFFF; // Mặc định HIGH (không bấm phím) nếu chưa gắn PCF8574
    if (_wire->available()) {
      pinVal = _wire->read();
      if (i2cwidth > 1 && _wire->available()) {
        pinVal |= (_wire->read() << 8);
      }
    }
    pinVal &= mask;
    return (pinVal == mask) ? HIGH : LOW;
  }

  void port_write(word i2cportval) {
    _wire->beginTransmission((int)i2caddr);
    _wire->write((byte)(i2cportval & 0xFF));
    if (i2cwidth > 1) {
      _wire->write((byte)(i2cportval >> 8));
    }
    _wire->endTransmission();
    pinState = i2cportval;
  }

private:
  byte i2caddr;
  byte i2cwidth;
  TwoWire* _wire;
  word pinState;
};

#endif // KEYPAD_I2C_H

