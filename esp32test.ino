#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =====================================================
// BLE configuration
// =====================================================

static const char* DEVICE_NAME = "EV_SIM_001";
static const uint32_t BLE_PASSKEY = 123456;

static const char* SERVICE_UUID =
  "12345678-1234-1234-1234-123456789000";

static const char* COMMAND_UUID =
  "12345678-1234-1234-1234-123456789001";

static const char* RESPONSE_UUID =
  "12345678-1234-1234-1234-123456789002";

// =====================================================
// Connection settings
// =====================================================

static const unsigned long WIFI_TIMEOUT_MS = 20000;

// =====================================================
// BLE objects
// =====================================================

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* commandCharacteristic = nullptr;
NimBLECharacteristic* responseCharacteristic = nullptr;
NimBLEAdvertising* bleAdvertising = nullptr;

// =====================================================
// Storage
// =====================================================

Preferences preferences;

// =====================================================
// Runtime states
// =====================================================

bool bleClientConnected = false;
bool bleConnectionEncrypted = false;
bool responseNotificationsEnabled = false;

bool wifiConnectionRequested = false;
bool wifiConnecting = false;

String pendingSsid;
String pendingPassword;

unsigned long wifiConnectionStartTime = 0;

// =====================================================
// Send JSON response over BLE
// =====================================================

void sendBleJson(JsonDocument& document) {
  String response;
  serializeJson(document, response);

  responseCharacteristic->setValue(response);

  if (bleClientConnected && responseNotificationsEnabled) {
    responseCharacteristic->notify();
  }

  Serial.print("BLE TX: ");
  Serial.println(response);
}

void sendSimpleResponse(
  const char* command,
  const char* status,
  const char* message
) {
  JsonDocument response;

  response["cmd"] = command;
  response["status"] = status;
  response["message"] = message;

  sendBleJson(response);
}

// =====================================================
// Wi-Fi credential storage
// =====================================================

bool saveWifiCredentials(
  const String& ssid,
  const String& password
) {
  if (!preferences.begin("wifi-config", false)) {
    Serial.println("Failed to open Preferences");
    return false;
  }

  size_t ssidWritten =
    preferences.putString("ssid", ssid);

  size_t passwordWritten =
    preferences.putString("password", password);

  preferences.putBool("configured", true);
  preferences.end();

  return ssidWritten > 0 && passwordWritten > 0;
}

bool loadWifiCredentials(
  String& ssid,
  String& password
) {
  if (!preferences.begin("wifi-config", true)) {
    return false;
  }

  bool configured =
    preferences.getBool("configured", false);

  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");

  preferences.end();

  return configured && !ssid.isEmpty();
}

void clearWifiCredentials() {
  if (preferences.begin("wifi-config", false)) {
    preferences.clear();
    preferences.end();
  }

  WiFi.disconnect(true, true);

  Serial.println("Stored Wi-Fi credentials deleted");
}

// =====================================================
// Wi-Fi response functions
// =====================================================

void sendWifiConnectingResponse() {
  JsonDocument response;

  response["cmd"] = "WIFI_CONNECTION_STATUS";
  response["status"] = "CONNECTING";
  response["ssid"] = pendingSsid;

  sendBleJson(response);
}

void sendWifiSuccessResponse() {
  JsonDocument response;

  response["cmd"] = "WIFI_CREDENTIALS_ACK";
  response["status"] = "SUCCESS";
  response["ssid"] = WiFi.SSID();
  response["ip_address"] = WiFi.localIP().toString();
  response["gateway"] = WiFi.gatewayIP().toString();
  response["rssi"] = WiFi.RSSI();

  sendBleJson(response);

  Serial.println();
  Serial.println("Wi-Fi connected successfully");

  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());

  Serial.print("Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
}

void sendWifiFailureResponse(const char* reason) {
  JsonDocument response;

  response["cmd"] = "WIFI_CREDENTIALS_ACK";
  response["status"] = "FAILED";
  response["reason"] = reason;

  sendBleJson(response);
}

// =====================================================
// Start Wi-Fi connection
// =====================================================

void startWifiConnection(
  const String& ssid,
  const String& password
) {
  if (ssid.isEmpty()) {
    sendWifiFailureResponse("SSID is empty");
    return;
  }

  pendingSsid = ssid;
  pendingPassword = password;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(300);

  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  Serial.print("SSID: ");
  Serial.println(pendingSsid);

  // Do not print the password.
  WiFi.begin(
    pendingSsid.c_str(),
    pendingPassword.c_str()
  );

  wifiConnectionStartTime = millis();
  wifiConnecting = true;

  sendWifiConnectingResponse();
}

// =====================================================
// Non-blocking Wi-Fi connection manager
// =====================================================

void processWifiConnection() {
  if (!wifiConnecting) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;

    if (!saveWifiCredentials(
          pendingSsid,
          pendingPassword
        )) {
      Serial.println(
        "Warning: connected, but credentials were not saved"
      );
    }

    sendWifiSuccessResponse();

    // Remove the password from temporary RAM.
    pendingPassword = "";

    return;
  }

  if (millis() - wifiConnectionStartTime >=
      WIFI_TIMEOUT_MS) {
    wifiConnecting = false;

    WiFi.disconnect(false, false);

    sendWifiFailureResponse(
      "Wi-Fi connection timed out"
    );

    pendingPassword = "";

    Serial.println("Wi-Fi connection timed out");
  }
}

// =====================================================
// Command handlers
// =====================================================

void handleWifiConfigCheck() {
  String storedSsid;
  String storedPassword;

  bool configured =
    loadWifiCredentials(storedSsid, storedPassword);

  JsonDocument response;

  response["cmd"] = "WIFI_CONFIG_STATUS";
  response["configured"] = configured;

  if (configured) {
    response["ssid"] = storedSsid;
  }

  response["wifi_connected"] =
    WiFi.status() == WL_CONNECTED;

  if (WiFi.status() == WL_CONNECTED) {
    response["ip_address"] =
      WiFi.localIP().toString();
  }

  sendBleJson(response);

  // Remove password copied into RAM.
  storedPassword = "";
}

void handleWifiCredentialsWrite(
  JsonDocument& request
) {
  const char* ssid = request["ssid"];
  const char* password = request["password"];

  if (ssid == nullptr || strlen(ssid) == 0) {
    sendWifiFailureResponse(
      "Missing or empty ssid"
    );
    return;
  }

  if (password == nullptr) {
    sendWifiFailureResponse(
      "Missing password"
    );
    return;
  }

  if (wifiConnecting) {
    sendWifiFailureResponse(
      "Wi-Fi connection already in progress"
    );
    return;
  }

  startWifiConnection(
    String(ssid),
    String(password)
  );
}

void handleGetIpAddress() {
  JsonDocument response;

  response["cmd"] = "IP_ADDRESS_RESPONSE";

  if (WiFi.status() == WL_CONNECTED) {
    response["status"] = "SUCCESS";
    response["ip_address"] =
      WiFi.localIP().toString();
    response["ssid"] = WiFi.SSID();
  } else {
    response["status"] = "FAILED";
    response["reason"] = "Wi-Fi is not connected";
  }

  sendBleJson(response);
}

void handleClearWifiConfig() {
  clearWifiCredentials();

  JsonDocument response;

  response["cmd"] = "WIFI_CONFIG_CLEAR_ACK";
  response["status"] = "SUCCESS";

  sendBleJson(response);
}

// =====================================================
// Process BLE commands
// =====================================================

void processBleCommand(
  const std::string& receivedValue,
  NimBLEConnInfo& connectionInfo
) {
  Serial.print("BLE RX: ");
  Serial.println(receivedValue.c_str());

  if (!connectionInfo.isEncrypted()) {
    sendSimpleResponse(
      "ERROR",
      "UNAUTHORIZED",
      "BLE connection is not encrypted"
    );
    return;
  }

  JsonDocument request;

  DeserializationError error =
    deserializeJson(request, receivedValue);

  if (error) {
    sendSimpleResponse(
      "ERROR",
      "FAILED",
      "Invalid JSON"
    );
    return;
  }

  const char* command = request["cmd"];

  if (command == nullptr) {
    sendSimpleResponse(
      "ERROR",
      "FAILED",
      "Missing cmd field"
    );
    return;
  }

  if (strcmp(command, "WIFI_CONFIG_CHECK") == 0) {
    handleWifiConfigCheck();
    return;
  }

  if (strcmp(command, "WIFI_CREDENTIALS_WRITE") == 0) {
    handleWifiCredentialsWrite(request);
    return;
  }

  if (strcmp(command, "GET_IP_ADDRESS") == 0) {
    handleGetIpAddress();
    return;
  }

  if (strcmp(command, "WIFI_CONFIG_CLEAR") == 0) {
    handleClearWifiConfig();
    return;
  }

  if (strcmp(command, "PING") == 0) {
    sendSimpleResponse(
      "PONG",
      "SUCCESS",
      "ESP32 is connected"
    );
    return;
  }

  sendSimpleResponse(
    command,
    "FAILED",
    "Unknown command"
  );
}

// =====================================================
// BLE characteristic callback
// =====================================================

class CommandCallbacks
  : public NimBLECharacteristicCallbacks {

  void onWrite(
    NimBLECharacteristic* characteristic,
    NimBLEConnInfo& connectionInfo
  ) override {
    std::string value = characteristic->getValue();

    if (value.empty()) {
      sendSimpleResponse(
        "ERROR",
        "FAILED",
        "Empty command"
      );
      return;
    }

    processBleCommand(value, connectionInfo);
  }
};

class ResponseCallbacks
  : public NimBLECharacteristicCallbacks {

  void onSubscribe(
    NimBLECharacteristic* characteristic,
    NimBLEConnInfo& connectionInfo,
    uint16_t subscriptionValue
  ) override {
    responseNotificationsEnabled =
      subscriptionValue == 1 ||
      subscriptionValue == 3;

    Serial.print("Notifications: ");
    Serial.println(
      responseNotificationsEnabled
        ? "ENABLED"
        : "DISABLED"
    );
  }
};

// =====================================================
// BLE server callbacks
// =====================================================

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(
    NimBLEServer* server,
    NimBLEConnInfo& connectionInfo
  ) override {
    bleClientConnected = true;
    bleConnectionEncrypted =
      connectionInfo.isEncrypted();

    Serial.println();
    Serial.println("BLE client connected");

    Serial.print("Client: ");
    Serial.println(
      connectionInfo
        .getAddress()
        .toString()
        .c_str()
    );
  }

  void onDisconnect(
    NimBLEServer* server,
    NimBLEConnInfo& connectionInfo,
    int reason
  ) override {
    bleClientConnected = false;
    bleConnectionEncrypted = false;
    responseNotificationsEnabled = false;

    Serial.print(
      "BLE client disconnected. Reason: "
    );
    Serial.println(reason);

    delay(200);
    NimBLEDevice::startAdvertising();

    Serial.println("Advertising restarted");
  }

  uint32_t onPassKeyDisplay() override {
    Serial.print("Enter this passkey: ");
    Serial.println(BLE_PASSKEY);

    return BLE_PASSKEY;
  }

  void onAuthenticationComplete(
    NimBLEConnInfo& connectionInfo
  ) override {
    bleConnectionEncrypted =
      connectionInfo.isEncrypted();

    Serial.println();
    Serial.println("BLE authentication completed");

    Serial.print("Encrypted: ");
    Serial.println(
      connectionInfo.isEncrypted()
        ? "YES"
        : "NO"
    );

    Serial.print("Bonded: ");
    Serial.println(
      connectionInfo.isBonded()
        ? "YES"
        : "NO"
    );
  }
};

// =====================================================
// BLE initialization
// =====================================================

void startBle() {
  NimBLEDevice::init(DEVICE_NAME);

  NimBLEDevice::setSecurityAuth(
    true,  // bonding
    true,  // MITM protection
    true   // secure connections
  );

  NimBLEDevice::setSecurityIOCap(
    BLE_HS_IO_DISPLAY_ONLY
  );

  NimBLEDevice::setSecurityPasskey(
    BLE_PASSKEY
  );

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService* service =
    bleServer->createService(SERVICE_UUID);

  commandCharacteristic =
    service->createCharacteristic(
      COMMAND_UUID,
      NIMBLE_PROPERTY::WRITE |
      NIMBLE_PROPERTY::WRITE_ENC
    );

  responseCharacteristic =
    service->createCharacteristic(
      RESPONSE_UUID,
      NIMBLE_PROPERTY::READ |
      NIMBLE_PROPERTY::NOTIFY |
      NIMBLE_PROPERTY::READ_ENC
    );

  commandCharacteristic->setCallbacks(
    new CommandCallbacks()
  );

  responseCharacteristic->setCallbacks(
    new ResponseCallbacks()
  );

  responseCharacteristic->setValue(
    "{\"cmd\":\"DEVICE_STATUS\",\"status\":\"READY\"}"
  );

  service->start();

  bleAdvertising =
    NimBLEDevice::getAdvertising();

  bleAdvertising->clearData();
  bleAdvertising->enableScanResponse(true);
  bleAdvertising->addServiceUUID(SERVICE_UUID);
  bleAdvertising->setName(DEVICE_NAME);
  bleAdvertising->start();

  Serial.println("BLE advertising started");
  Serial.print("Device name: ");
  Serial.println(DEVICE_NAME);
}

// =====================================================
// Arduino setup and loop
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("EV Charger BLE Wi-Fi Provisioning");
  Serial.println("======================================");

  startBle();
}

void loop() {
  processWifiConnection();
  delay(10);
}