#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <DallasTemperature.h>
#include <FirebaseESP32.h>
#include <OneWire.h>
#include <PZEM004Tv30.h>

#include "addons/RTDBHelper.h"
#include "addons/TokenHelper.h"

const char *WIFI_SSID = "CEIOT";
const char *WIFI_PASSWORD = "CE-1OT@!";

const char *API_KEY = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
const char *DATABASE_URL = "https://voltsafe-8ead5-default-rtdb.firebaseio.com";
const char *USER_EMAIL = "esp32_1@device.local";
const char *USER_PASSWORD = "12345678";

const char *DEVICE_ID = "esp32_1";
const uint32_t SEND_INTERVAL_MS = 5000;
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

#define PZEM_RX_PIN 32
#define PZEM_TX_PIN 33
#define DS18B20_PIN 14

HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastSendMs = 0;
bool firebaseReadyLogged = false;
const uint16_t LOG_SAMPLES = 360;  // 360 x 5dtk = 30 menit

struct MeterData {
  float voltage, current, power, energy, frequency, pf, temperature;
  String timestamp, dateKey, timeKey;
};

struct Acc {
  float sumV = 0, sumC = 0, sumP = 0, sumE = 0, sumF = 0, sumPF = 0, sumT = 0;
  uint16_t n = 0;
  String startTs;
} acc;

String buildDevicePath(const String &suffix) {
  return "/devices/" + String(DEVICE_ID) + suffix;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (uint8_t i = 0; WiFi.status() != WL_CONNECTED && i < 40; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi terhubung. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi gagal, akan coba lagi nanti.");
  }
}

void syncTimeWithNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");
  Serial.print("Sinkronisasi NTP");

  struct tm timeInfo;
  for (uint8_t i = 0; i < 30; i++) {
    if (getLocalTime(&timeInfo, 500)) {
      Serial.println(" OK");
      return;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println(" GAGAL");
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
  Serial.println("Firebase dimulai, menunggu autentikasi...");
}

bool getTimestampParts(String &timestamp, String &dateKey, String &timeKey) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo)) return false;

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
  timestamp = buf;

  strftime(buf, sizeof(buf), "%Y-%m-%d", &timeInfo);
  dateKey = buf;

  strftime(buf, sizeof(buf), "%H-%M-%S", &timeInfo);
  timeKey = buf;

  return true;
}

bool readPzemData(MeterData &data) {
  data.voltage   = pzem.voltage();
  data.current   = pzem.current();
  data.power     = pzem.power();
  data.energy    = pzem.energy();
  data.frequency = pzem.frequency();
  data.pf        = pzem.pf();

  ds18b20.requestTemperatures();
  data.temperature = ds18b20.getTempCByIndex(0);

  if (isnan(data.voltage)) return false;
  if (data.temperature == DEVICE_DISCONNECTED_C || isnan(data.temperature)) return false;

  return getTimestampParts(data.timestamp, data.dateKey, data.timeKey);
}

void printMeterData(const MeterData &data) {
  Serial.println("========== PZEM DATA ==========");
  Serial.printf("Voltage   : %.2f V\n", data.voltage);
  Serial.printf("Current   : %.2f A\n", data.current);
  Serial.printf("Power     : %.2f W\n", data.power);
  Serial.printf("Energy    : %.2f kWh\n", data.energy);
  Serial.printf("Frequency : %.2f Hz\n", data.frequency);
  Serial.printf("PF        : %.2f\n", data.pf);
  Serial.printf("Temp      : %.2f C\n", data.temperature);
  Serial.printf("Timestamp : %s\n", data.timestamp.c_str());
}

void fillJson(FirebaseJson &json, const MeterData &data) {
  json.clear();
  json.set("voltage", data.voltage);
  json.set("current", data.current);
  json.set("power", data.power);
  json.set("energy", data.energy);
  json.set("frequency", data.frequency);
  json.set("pf", data.pf);
  json.set("Temperature", data.temperature);
  json.set("timestamp", data.timestamp);
}

bool uploadToFirebase(const MeterData &data, const String &suffix) {
  FirebaseJson json;
  fillJson(json, data);

  String path = buildDevicePath(suffix);
  bool ok = Firebase.setJSON(fbdo, path.c_str(), json);

  if (ok)
    Serial.printf("Firebase OK : %s\n", path.c_str());
  else
    Serial.printf("Firebase GAGAL: %s | %s\n", path.c_str(), fbdo.errorReason().c_str());

  return ok;
}

void accumulate(const MeterData &d) {
  if (acc.n == 0) acc.startTs = d.timestamp;
  acc.sumV  += d.voltage;
  acc.sumC  += d.current;
  acc.sumP  += d.power;
  acc.sumE  += d.energy;
  acc.sumF  += d.frequency;
  acc.sumPF += d.pf;
  acc.sumT  += d.temperature;
  acc.n++;
}

void uploadLog30m() {
  if (acc.n == 0) return;

  MeterData avg;
  getTimestampParts(avg.timestamp, avg.dateKey, avg.timeKey);

  avg.voltage     = acc.sumV  / acc.n;
  avg.current     = acc.sumC  / acc.n;
  avg.power       = acc.sumP  / acc.n;
  avg.energy      = acc.sumE  / acc.n;
  avg.frequency   = acc.sumF  / acc.n;
  avg.pf          = acc.sumPF / acc.n;
  avg.temperature = acc.sumT  / acc.n;

  FirebaseJson json;
  fillJson(json, avg);
  json.set("sampleCount", acc.n);
  json.set("startTimestamp", acc.startTs);
  json.set("endTimestamp", avg.timestamp);

  String path = buildDevicePath("/logs_30m/" + avg.dateKey + "/" + avg.timeKey);
  bool ok = Firebase.setJSON(fbdo, path.c_str(), json);

  if (ok)
    Serial.printf("Firebase logs_30m OK : %s (%d sample)\n", path.c_str(), acc.n);
  else
    Serial.printf("Firebase logs_30m GAGAL: %s | %s\n", path.c_str(), fbdo.errorReason().c_str());

  acc = Acc();
}

void ensureConnections() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (Firebase.ready() && !firebaseReadyLogged) {
    Serial.println("Firebase siap.");
    firebaseReadyLogged = true;
  } else if (!Firebase.ready()) {
    firebaseReadyLogged = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 PZEM004T v4.0 -> Firebase RTDB");

  ds18b20.begin();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) syncTimeWithNTP();
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
    Serial.println("Firebase belum ready, tunda upload.");
    return;
  }

  MeterData d;
  if (!readPzemData(d)) {
    Serial.println("Gagal baca sensor! Cek wiring PZEM/DS18B20.");
    return;
  }

  printMeterData(d);

  bool ok = uploadToFirebase(d, "/realtime");
  accumulate(d);

  if (acc.n >= LOG_SAMPLES) {
    uploadLog30m();
  }

  Serial.printf("Realtime: %s | Akumulator: %d/%d\n",
                ok ? "OK" : "GAGAL", acc.n, LOG_SAMPLES);
  Serial.println();
}
