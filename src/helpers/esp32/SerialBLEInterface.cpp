#include "SerialBLEInterface.h"
#include "esp_mac.h"
#if defined(BLE_CONN_INTERVAL_MIN) && !defined(USE_NIMBLE)
  #include <esp_gap_ble_api.h>
#endif

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define ADVERT_RESTART_DELAY  1000   // millis
#define BLE_WRITE_MIN_INTERVAL   60

#ifdef USE_NIMBLE
// ============================ NimBLE host ============================

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin_code = pin_code;

  if (strcmp(name, "@@MAC") == 0) {
    uint8_t addr[8];
    memset(addr, 0, sizeof(addr));
    esp_efuse_mac_get_default(addr);
    sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  char dev_name[32+16];
  sprintf(dev_name, "%s%s", prefix, name);

  NimBLEDevice::init(dev_name);
  NimBLEDevice::setSecurityCallbacks(this);
  NimBLEDevice::setMTU(MAX_FRAME_SIZE);
#ifdef BLE_CLEAR_BONDS_ON_BOOT
  NimBLEDevice::deleteAllBonds();   // troubleshooting: start from a clean bond store
#endif

  // static PIN + secure connections with MITM, bonded (same policy as the
  // Bluedroid path: the device shows the pin, the phone enters it)
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(pin_code);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(this);

  pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  // encrypted + authenticated (MITM) write: the phone must be bonded via the pin
  NimBLECharacteristic * pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_AUTHEN);
  pRxCharacteristic->setCallbacks(this);

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
}

// -------- NimBLESecurityCallbacks methods

uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");
  return _pin_code;
}

void SerialBLEInterface::onPassKeyNotify(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onPassKeyNotify(%u)", pass_key);
}

bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}

bool SerialBLEInterface::onSecurityRequest() {
  BLE_DEBUG_PRINTLN("onSecurityRequest()");
  return true;  // allow
}

void SerialBLEInterface::onAuthenticationComplete(ble_gap_conn_desc* desc) {
  if (desc != nullptr && desc->sec_state.encrypted && desc->sec_state.authenticated) {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Success");
    deviceConnected = true;
  } else {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Failure*");
    if (pServer != nullptr) {
      pServer->disconnect(desc ? desc->conn_handle : last_conn_id);
    }
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- NimBLEServerCallbacks methods

void SerialBLEInterface::onConnect(NimBLEServer* pServer) {
}

void SerialBLEInterface::onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
  last_conn_id = desc->conn_handle;
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d, mtu=%d", desc->conn_handle,
                    ble_att_mtu(desc->conn_handle));

#if defined(BLE_CONN_INTERVAL_MIN) && defined(BLE_CONN_INTERVAL_MAX)
  // ask the central for a longer connection interval plus slave latency, so
  // the radio/CPU only wake every interval*(latency+1) instead of every
  // phone-default (~15-30ms) event. Latency only skips events we have nothing
  // to send/receive -- responses still go out at the next event.
#ifdef BLE_CONN_SLAVE_LATENCY
  uint16_t latency = BLE_CONN_SLAVE_LATENCY;
#else
  uint16_t latency = 0;
#endif
  pServer->updateConnParams(desc->conn_handle, BLE_CONN_INTERVAL_MIN, BLE_CONN_INTERVAL_MAX,
                            latency, 600 /* 6s supervision, 10ms units */);
  BLE_DEBUG_PRINTLN("requested conn params: int=%d-%d lat=%d",
                    BLE_CONN_INTERVAL_MIN, BLE_CONN_INTERVAL_MAX, latency);
#endif
}

void SerialBLEInterface::onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) {
  BLE_DEBUG_PRINTLN("onMTUChange(), mtu=%d", MTU);
}

void SerialBLEInterface::onDisconnect(NimBLEServer* pServer) {
  BLE_DEBUG_PRINTLN("onDisconnect()");
  deviceConnected = false;
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- NimBLECharacteristicCallbacks methods

void SerialBLEInterface::onWrite(NimBLECharacteristic* pCharacteristic) {
  NimBLEAttValue val = pCharacteristic->getValue();
  const uint8_t* rxValue = val.data();
  int len = val.length();

  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("ERROR: onWrite(), frame too big, len=%d", len);
  } else {
    Frame frame = {};
    frame.len = len;
    memcpy(frame.buf, rxValue, len);

    if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
      BLE_DEBUG_PRINTLN("ERROR: onWrite(), recv_queue is full!");
    }
  }
}

// ---------- public methods

void SerialBLEInterface::clearBuffers() {
  xQueueReset(recv_queue);
  send_queue_len = 0;
}

void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  pService->start();

#if defined(BLE_ADV_INTERVAL_MIN) && defined(BLE_ADV_INTERVAL_MAX)
  // slower advertising cadence saves standby power at the cost of slightly
  // slower (re)discovery by clients; units are 0.625ms
  pServer->getAdvertising()->setMinInterval(BLE_ADV_INTERVAL_MIN);
  pServer->getAdvertising()->setMaxInterval(BLE_ADV_INTERVAL_MAX);
#endif
  pServer->getAdvertising()->start();
  adv_restart_time = 0;
}

void SerialBLEInterface::disable() {
  _isEnabled = false;

  BLE_DEBUG_PRINTLN("SerialBLEInterface::disable");

  pServer->getAdvertising()->stop();
  pServer->disconnect(last_conn_id);
  // (NimBLE 1.4 has no service stop; disconnect + adv stop is enough)
  oldDeviceConnected = deviceConnected = false;
  adv_restart_time = 0;
}

bool SerialBLEInterface::isConnected() const {
  return deviceConnected;
}

bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d", (uint32_t)send_queue[0].len, (uint32_t) send_queue[0].buf[0]);

    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
  }

  Frame frame;
  if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
    memcpy(dest, frame.buf, frame.len);
    BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", (uint32_t) frame.len, (uint32_t) dest[0]);
    return frame.len;
  }

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();

      BLE_DEBUG_PRINTLN("SerialBLEInterface -> disconnecting...");
      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> stopping advertising");
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> connecting...");
      pServer->getAdvertising()->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> re-starting advertising");
      pServer->getAdvertising()->start();  // re-Start advertising
    }
    adv_restart_time = 0;
  }

  // watchdog: a missed disconnect callback can leave us believing a client is
  // attached and advertising stopped -- the device then becomes invisible to
  // every client. Periodically trust the real connection count instead.
  {
    static unsigned long next_adv_check = 0;
    if (millis() > next_adv_check) {
      next_adv_check = millis() + 10000;
      if (_isEnabled && pServer->getConnectedCount() == 0) {
        if (deviceConnected) {
          BLE_DEBUG_PRINTLN("SerialBLEInterface -> stale client state, clearing");
          deviceConnected = false;
          clearBuffers();
        }
        if (!pServer->getAdvertising()->isAdvertising()) {
          BLE_DEBUG_PRINTLN("SerialBLEInterface -> watchdog re-starting advertising");
          pServer->getAdvertising()->start();
        }
      }
    }
  }
  return 0;
}

#else
// ============================ Bluedroid ============================

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  _pin_code = pin_code;

  if (strcmp(name, "@@MAC") == 0) {
    uint8_t addr[8];
    memset(addr, 0, sizeof(addr));
    esp_efuse_mac_get_default(addr);
    sprintf(name, "%02X%02X%02X%02X%02X%02X",    // modify (IN-OUT param)
          addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
  }
  char dev_name[32+16];
  sprintf(dev_name, "%s%s", prefix, name);

  // Create the BLE Device
  BLEDevice::init(dev_name);
  BLEDevice::setSecurityCallbacks(this);
  BLEDevice::setMTU(MAX_FRAME_SIZE);

  BLESecurity  sec;
  sec.setStaticPIN(pin_code);
  sec.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);

  // Create the BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(this);

  // Create the BLE Service
  pService = pServer->createService(SERVICE_UUID);

  // Create a BLE Characteristic
  pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pTxCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  pRxCharacteristic->setCallbacks(this);

  pServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
}

// -------- BLESecurityCallbacks methods

uint32_t SerialBLEInterface::onPassKeyRequest() {
  BLE_DEBUG_PRINTLN("onPassKeyRequest()");
  return _pin_code;
}

void SerialBLEInterface::onPassKeyNotify(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onPassKeyNotify(%u)", pass_key);
}

bool SerialBLEInterface::onConfirmPIN(uint32_t pass_key) {
  BLE_DEBUG_PRINTLN("onConfirmPIN(%u)", pass_key);
  return true;
}

bool SerialBLEInterface::onSecurityRequest() {
  BLE_DEBUG_PRINTLN("onSecurityRequest()");
  return true;  // allow
}

void SerialBLEInterface::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) {
  if (cmpl.success) {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Success");
    deviceConnected = true;
  } else {
    BLE_DEBUG_PRINTLN(" - SecurityCallback - Authentication Failure*");

    //pServer->removePeerDevice(pServer->getConnId(), true);
    pServer->disconnect(pServer->getConnId());
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLEServerCallbacks methods

void SerialBLEInterface::onConnect(BLEServer* pServer) {
}

void SerialBLEInterface::onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) {
  BLE_DEBUG_PRINTLN("onConnect(), conn_id=%d, mtu=%d", param->connect.conn_id, pServer->getPeerMTU(param->connect.conn_id));
  last_conn_id = param->connect.conn_id;

#if defined(BLE_CONN_INTERVAL_MIN) && defined(BLE_CONN_INTERVAL_MAX)
  // ask the central for a longer connection interval plus slave latency, so
  // the radio/CPU only wake every interval*(latency+1) instead of every
  // phone-default (~15-30ms) event. Latency only skips events we have nothing
  // to send/receive -- responses still go out at the next event.
  {
    esp_ble_conn_update_params_t params = {};
    memcpy(params.bda, param->connect.remote_bda, sizeof(params.bda));
    params.min_int = BLE_CONN_INTERVAL_MIN;   // 1.25ms units
    params.max_int = BLE_CONN_INTERVAL_MAX;
#ifdef BLE_CONN_SLAVE_LATENCY
    params.latency = BLE_CONN_SLAVE_LATENCY;
#else
    params.latency = 0;
#endif
    params.timeout = 600;   // 6s supervision timeout (10ms units)
    esp_ble_gap_update_conn_params(&params);
    BLE_DEBUG_PRINTLN("requested conn params: int=%d-%d lat=%d",
                      params.min_int, params.max_int, params.latency);
  }
#endif
}

void SerialBLEInterface::onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) {
  BLE_DEBUG_PRINTLN("onMtuChanged(), mtu=%d", pServer->getPeerMTU(param->mtu.conn_id));
}

void SerialBLEInterface::onDisconnect(BLEServer* pServer) {
  BLE_DEBUG_PRINTLN("onDisconnect()");
  deviceConnected = false;
  if (_isEnabled) {
    adv_restart_time = millis() + ADVERT_RESTART_DELAY;
  }
}

// -------- BLECharacteristicCallbacks methods

void SerialBLEInterface::onWrite(BLECharacteristic* pCharacteristic, esp_ble_gatts_cb_param_t* param) {
  uint8_t* rxValue = pCharacteristic->getData();
  int len = pCharacteristic->getLength();

  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("ERROR: onWrite(), frame too big, len=%d", len);
  } else {
    Frame frame = {};
    frame.len = len;
    memcpy(frame.buf, rxValue, len);

    if (xQueueSend(recv_queue, &frame, 0) != pdTRUE) {
      BLE_DEBUG_PRINTLN("ERROR: onWrite(), recv_queue is full!");
    }
  }
}

// ---------- public methods

void SerialBLEInterface::clearBuffers() {
  xQueueReset(recv_queue);
  send_queue_len = 0;
}

void SerialBLEInterface::enable() {
  if (_isEnabled) return;

  _isEnabled = true;
  clearBuffers();

  // Start the service
  pService->start();

  // Start advertising
#if defined(BLE_ADV_INTERVAL_MIN) && defined(BLE_ADV_INTERVAL_MAX)
  // slower advertising cadence saves standby power at the cost of slightly
  // slower (re)discovery by clients; units are 0.625ms
  pServer->getAdvertising()->setMinInterval(BLE_ADV_INTERVAL_MIN);
  pServer->getAdvertising()->setMaxInterval(BLE_ADV_INTERVAL_MAX);
#endif
  pServer->getAdvertising()->start();
  adv_restart_time = 0;
}

void SerialBLEInterface::disable() {
  _isEnabled = false;

  BLE_DEBUG_PRINTLN("SerialBLEInterface::disable");

  pServer->getAdvertising()->stop();
  pServer->disconnect(last_conn_id);
  // (NimBLE 1.4 has no service stop; disconnect + adv stop is enough)
  oldDeviceConnected = deviceConnected = false;
  adv_restart_time = 0;
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%d", len);
    return 0;
  }

  if (deviceConnected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;  // add to send queue
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}


bool SerialBLEInterface::isWriteBusy() const {
  return millis() < _last_write + BLE_WRITE_MIN_INTERVAL;   // still too soon to start another write?
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  if (send_queue_len > 0   // first, check send queue
    && millis() >= _last_write + BLE_WRITE_MIN_INTERVAL    // space the writes apart
  ) {
    _last_write = millis();
    pTxCharacteristic->setValue(send_queue[0].buf, send_queue[0].len);
    pTxCharacteristic->notify();

    BLE_DEBUG_PRINTLN("writeBytes: sz=%d, hdr=%d", (uint32_t)send_queue[0].len, (uint32_t) send_queue[0].buf[0]);

    send_queue_len--;
    for (int i = 0; i < send_queue_len; i++) {   // delete top item from queue
      send_queue[i] = send_queue[i + 1];
    }
  }

  Frame frame;
  if (xQueueReceive(recv_queue, &frame, 0) == pdTRUE) {
    memcpy(dest, frame.buf, frame.len);
    BLE_DEBUG_PRINTLN("readBytes: sz=%d, hdr=%d", (uint32_t) frame.len, (uint32_t) dest[0]);
    return frame.len;
  }

  if (deviceConnected != oldDeviceConnected) {
    if (!deviceConnected) {    // disconnecting
      clearBuffers();

      BLE_DEBUG_PRINTLN("SerialBLEInterface -> disconnecting...");

      //pServer->getAdvertising()->setMinInterval(500);
      //pServer->getAdvertising()->setMaxInterval(1000);

      adv_restart_time = millis() + ADVERT_RESTART_DELAY;
    } else {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> stopping advertising");
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> connecting...");
      // connecting
      // do stuff here on connecting
      pServer->getAdvertising()->stop();
      adv_restart_time = 0;
    }
    oldDeviceConnected = deviceConnected;
  }

  if (adv_restart_time && millis() >= adv_restart_time) {
    if (pServer->getConnectedCount() == 0) {
      BLE_DEBUG_PRINTLN("SerialBLEInterface -> re-starting advertising");
      pServer->getAdvertising()->start();  // re-Start advertising
    }
    adv_restart_time = 0;
  }
  return 0;
}

bool SerialBLEInterface::isConnected() const {
  return deviceConnected;  //pServer != NULL && pServer->getConnectedCount() > 0;
}

#endif // USE_NIMBLE
