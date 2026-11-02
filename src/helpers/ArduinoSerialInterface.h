#pragma once

#include "BaseSerialInterface.h"
#include <Arduino.h>

class ArduinoSerialInterface : public BaseSerialInterface {
  bool _isEnabled;
  bool _is_usbcdc;    // true when _serial is the native USB CDC (connection state is queryable)
  uint8_t _state;
  uint16_t _frame_len;
  uint16_t rx_len;
  Stream* _serial;
  uint8_t rx_buf[MAX_FRAME_SIZE];

public:
  ArduinoSerialInterface() { _isEnabled = false; _is_usbcdc = false; _state = 0; }

  void begin(Stream& serial) { 
    _serial = &serial; 
    _is_usbcdc = false;
#if defined(ESP32) && ARDUINO_USB_CDC_ON_BOOT
    if (&serial == (Stream *)&Serial) _is_usbcdc = true;   // native USB CDC interface
#endif  
#ifdef RAK_4631
    pinMode(WB_IO2, OUTPUT);
#endif  
  }

  // BaseSerialInterface methods
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }

  bool isConnected() const override;

  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;
};