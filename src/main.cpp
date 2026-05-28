#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <FirebaseESP32.h>
#include <PZEM004Tv30.h>

#include "addons/RTDBHelper.h"
#include "addons/TokenHelper.h"

// WiFi
static const char *WIFI_SSID = "TP-Link_F060 - 6307";
static const char *WIFI_PASSWORD = "6307310706";

// Firebase
static const char *API_KEY = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
static const char *DATABASE_URL = "https://voltsafe-8ead5-default-rtdb.firebaseio.com";
static const char *USER_EMAIL = "esp32_1@device.local";
static const char *USER_PASSWORD = "12345678";

// Device
static const char *DEVICE_ID = "esp32_1";
static const uint32_t SEND_INTERVAL_MS = 5000;
static const long GMT_OFFSET_SEC = 7 * 3600;
static const int DAYLIGHT_OFFSET_SEC = 0;

// Pin ESP32 ke PZEM004T
// ESP32 RX menerima dari TX PZEM
// ESP32 TX mengirim ke RX PZEM
#define PZEM_RX_PIN 32
#define PZEM_TX_PIN 33

HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastSendMs = 0;
bool firebaseReadyLogged = false;

struct MeterData {
  float voltage;
  float current;
  float power;
  float energy;
  float frequency;
  float pf;
  String timestamp;
  String dateKey;
  String timeKey;
};

String buildDevicePath(const String &suffix) {
  String path = "/devices/";
  path += DEVICE_ID;
  path += suffix;
  return path;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint8_t retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi terhubung. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi belum terhubung, akan dicoba lagi.");
  }
}

bool syncTimeWithNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");

  Serial.print("Sinkronisasi waktu via NTP");
  struct tm timeInfo;
  for (uint8_t i = 0; i < 30; i++) {
    if (getLocalTime(&timeInfo, 500)) {
      Serial.println("\nWaktu NTP berhasil disinkronkan.");
      return true;
    }

    Serial.print(".");
    delay(500);
  }

  Serial.println("\nGagal sinkronisasi NTP.");
  return false;
}

void initFirebase() {
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  config.token_status_callback = tokenStatusCallback;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  Firebase.begin(&config, &auth);

  Serial.println("Inisialisasi Firebase selesai, menunggu autentikasi.");
}

bool getTimestampParts(String &timestamp, String &dateKey, String &timeKey) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) {
    return false;
  }

  char tsBuffer[24];
  char dateBuffer[11];
  char timeBuffer[9];

  strftime(tsBuffer, sizeof(tsBuffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
  strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &timeInfo);
  strftime(timeBuffer, sizeof(timeBuffer), "%H-%M-%S", &timeInfo);

  timestamp = String(tsBuffer);
  dateKey = String(dateBuffer);
  timeKey = String(timeBuffer);
  return true;
}

bool readPzemData(MeterData &data) {
  data.voltage = pzem.voltage();
  data.current = pzem.current();
  data.power = pzem.power();
  data.energy = pzem.energy();
  data.frequency = pzem.frequency();
  data.pf = pzem.pf();

  if (isnan(data.voltage)) {
    return false;
  }

  return getTimestampParts(data.timestamp, data.dateKey, data.timeKey);
}

void printMeterData(const MeterData &data) {
  Serial.println("========== PZEM DATA ==========");
  Serial.print("Voltage   : ");
  Serial.print(data.voltage);
  Serial.println(" V");

  Serial.print("Current   : ");
  Serial.print(data.current);
  Serial.println(" A");

  Serial.print("Power     : ");
  Serial.print(data.power);
  Serial.println(" W");

  Serial.print("Energy    : ");
  Serial.print(data.energy);
  Serial.println(" kWh");

  Serial.print("Frequency : ");
  Serial.print(data.frequency);
  Serial.println(" Hz");

  Serial.print("PF        : ");
  Serial.println(data.pf);

  Serial.print("Timestamp : ");
  Serial.println(data.timestamp);
}

void fillJson(FirebaseJson &json, const MeterData &data) {
  json.clear();
  json.set("voltage", data.voltage);
  json.set("current", data.current);
  json.set("power", data.power);
  json.set("energy", data.energy);
  json.set("frequency", data.frequency);
  json.set("pf", data.pf);
  json.set("timestamp", data.timestamp);
}

bool uploadRealtime(const MeterData &data) {
  FirebaseJson json;
  fillJson(json, data);

  const String path = buildDevicePath("/realtime");
  const bool ok = Firebase.setJSON(fbdo, path.c_str(), json);

  if (ok) {
    Serial.printf("Firebase realtime OK: %s\n", path.c_str());
  } else {
    Serial.printf("Firebase realtime GAGAL: %s | %s\n", path.c_str(), fbdo.errorReason().c_str());
  }

  return ok;
}

bool uploadLog5s(const MeterData &data) {
  FirebaseJson json;
  fillJson(json, data);

  String suffix = "/logs_5s/";
  suffix += data.dateKey;
  suffix += "/";
  suffix += data.timeKey;

  const String path = buildDevicePath(suffix);
  const bool ok = Firebase.setJSON(fbdo, path.c_str(), json);

  if (ok) {
    Serial.printf("Firebase logs_5s OK: %s\n", path.c_str());
  } else {
    Serial.printf("Firebase logs_5s GAGAL: %s | %s\n", path.c_str(), fbdo.errorReason().c_str());
  }

  return ok;
}

void ensureConnections() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (Firebase.ready() && !firebaseReadyLogged) {
    Serial.println("Firebase siap digunakan.");
    firebaseReadyLogged = true;
  } else if (!Firebase.ready()) {
    firebaseReadyLogged = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 PZEM004T v4.0 -> Firebase RTDB");
  Serial.println("Starting...");

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeWithNTP();
  }

  initFirebase();
}

void loop() {
  ensureConnections();

  if (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    return;
  }

  if (millis() - lastSendMs < SEND_INTERVAL_MS) {
    delay(50);
    return;
  }
  lastSendMs = millis();

  if (!Firebase.ready()) {
    Serial.println("Firebase belum ready, upload ditunda.");
    return;
  }

  MeterData meterData;
  if (!readPzemData(meterData)) {
    Serial.println("Gagal membaca data dari PZEM!");
    Serial.println("Cek wiring RX/TX, power PZEM, koneksi AC, atau NTP.");
    return;
  }

  printMeterData(meterData);

  const bool realtimeOk = uploadRealtime(meterData);
  const bool logOk = uploadLog5s(meterData);

  if (realtimeOk && logOk) {
    Serial.println("Upload Firebase selesai.");
  } else {
    Serial.println("Sebagian upload Firebase gagal.");
  }

  Serial.println();
}
