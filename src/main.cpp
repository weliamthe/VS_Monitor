#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <DallasTemperature.h>
#include <FirebaseESP32.h>
#include <OneWire.h>
#include <PZEM004Tv30.h>

#include "addons/RTDBHelper.h"
#include "addons/TokenHelper.h"

// Kredensial koneksi jaringan WiFi yang dipakai ESP32 untuk akses internet.
const char *WIFI_SSID = "SSID";
const char *WIFI_PASSWORD = "Password";

// Konfigurasi autentikasi Firebase Realtime Database.
const char *API_KEY = "AIzaSyBATQH6JMIHjLL6Zn5VkZ9FqnUQ_b63yGI";
const char *DATABASE_URL = "https://voltsafe-8ead5-default-rtdb.firebaseio.com";
const char *USER_EMAIL = "esp32_1@device.local";
const char *USER_PASSWORD = "12345678";

// Identitas perangkat dan interval kerja utama aplikasi.
const char *DEVICE_ID = "esp32_1";
const uint32_t SEND_INTERVAL_MS = 5000;
const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
const uint32_t NTP_CHECK_INTERVAL_MS = 1000;
const uint32_t TEMP_CONVERSION_MS = 750;
const long GMT_OFFSET_SEC = 7 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

#define PZEM_RX_PIN 32
#define PZEM_TX_PIN 33
#define DS18B20_PIN 14

// Inisialisasi antarmuka perangkat keras:
// - UART2 untuk komunikasi ke modul PZEM004T
// - OneWire untuk sensor suhu DS18B20
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// Objek utama Firebase untuk proses autentikasi dan kirim data.
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Variabel penjadwalan berbasis millis()
unsigned long lastSendMs = 0;
unsigned long lastWiFiAttemptMs = 0;
unsigned long lastNtpCheckMs = 0;
unsigned long lastTempRequestMs = 0;

// Penanda status untuk mencegah inisialisasi atau log yang berulang-ulang.
bool firebaseReadyLogged = false;
bool firebaseStarted = false;
bool wifiLoggedConnected = false;
bool ntpStarted = false;
bool timeSynced = false;

// Jumlah sampel untuk rekap 30 menit.
const uint16_t LOG_SAMPLES = 360;  // 360 x 5dtk = 30 menit

// Menyimpan satu paket data pembacaan sensor yang siap ditampilkan atau di-upload.
struct MeterData {
  float voltage, current, power, energy, frequency, pf, temperature;
  String timestamp, dateKey, timeKey;
};

// Menyimpan akumulasi sementara untuk membuat rata-rata log 30 menit.
struct Acc {
  float sumV = 0, sumC = 0, sumP = 0, sumE = 0, sumF = 0, sumPF = 0, sumT = 0;
  uint16_t n = 0;
  String startTs;
} acc;

// Membentuk path dasar Firebase
// di bawah node /devices/{DEVICE_ID}/...
String buildDevicePath(const String &suffix) {
  return "/devices/" + String(DEVICE_ID) + suffix;
}

// Memulai konversi suhu DS18B20 
// Hasil suhu baru akan diambil setelah jeda TEMP_CONVERSION_MS tercapai.
void startTemperatureConversion() {
  ds18b20.requestTemperatures();
  lastTempRequestMs = millis();
}

// Mengelola koneksi WiFi:
// - Jika sudah tersambung, cukup tampilkan IP satu kali.
// - Jika putus, reset status waktu/NTP lalu coba sambung ulang
//   dengan jeda agar tidak spam request koneksi.
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiLoggedConnected) {
      Serial.printf("WiFi terhubung. IP: %s\n", WiFi.localIP().toString().c_str());
      wifiLoggedConnected = true;
    }
    return;
  }

  wifiLoggedConnected = false;
  timeSynced = false;
  ntpStarted = false;
  lastNtpCheckMs = 0;

  unsigned long now = millis();
  if (lastWiFiAttemptMs != 0 && now - lastWiFiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;

  lastWiFiAttemptMs = now;
  Serial.printf("Menghubungkan ke WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Menjalankan sinkronisasi waktu melalui NTP.
// Waktu harus valid terlebih dulu agar timestamp yang dikirim ke Firebase akurat.
void syncTimeWithNTP() {
  if (timeSynced || WiFi.status() != WL_CONNECTED) return;

  if (!ntpStarted) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");
    ntpStarted = true;
    Serial.println("Sinkronisasi NTP dimulai...");
  }

  unsigned long now = millis();
  if (lastNtpCheckMs != 0 && now - lastNtpCheckMs < NTP_CHECK_INTERVAL_MS) return;
  lastNtpCheckMs = now;

  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 10)) {
    timeSynced = true;
    Serial.println("Sinkronisasi NTP OK");
  }
}

// Inisialisasi Firebase dilakukan sekali setelah WiFi dan waktu siap.
// Library akan menangani token autentikasi berdasarkan email/password perangkat.
void initFirebase() {
  if (firebaseStarted) return;

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  Firebase.begin(&config, &auth);
  firebaseStarted = true;
  Serial.println("Firebase dimulai, menunggu autentikasi...");
}

// Mengambil waktu lokal lalu memecahnya menjadi beberapa format:
// - timestamp penuh untuk isi data
// - dateKey untuk folder tanggal
// - timeKey untuk nama node/jam tertentu
bool getTimestampParts(String &timestamp, String &dateKey, String &timeKey) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10)) return false;

  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeInfo);
  timestamp = buf;

  strftime(buf, sizeof(buf), "%Y-%m-%d", &timeInfo);
  dateKey = buf;

  strftime(buf, sizeof(buf), "%H-%M-%S", &timeInfo);
  timeKey = buf;

  return true;
}

// Membaca seluruh parameter listrik dan temparatur
bool readPzemData(MeterData &data) {
  data.voltage   = pzem.voltage();
  data.current   = pzem.current();
  data.power     = pzem.power();
  data.energy    = pzem.energy();
  data.frequency = pzem.frequency();
  data.pf        = pzem.pf();

  // Tunggu sampai konversi suhu selesai agar pembacaan DS18B20 tidak prematur.
  if (millis() - lastTempRequestMs < TEMP_CONVERSION_MS) return false;

  data.temperature = ds18b20.getTempCByIndex(0);
  startTemperatureConversion();

  // jika data tidak sah akan menunggu berikutnya
  if (isnan(data.voltage)) return false;
  if (data.temperature == DEVICE_DISCONNECTED_C || isnan(data.temperature)) return false;

  return getTimestampParts(data.timestamp, data.dateKey, data.timeKey);
}

// data sensor ke serial monitor
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

// Mengisi objek JSON sesuai struktur yang akan dikirim ke Firebase.
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

// Mengirim satu snapshot data ke path Firebase yang diberikan.
// Dipakai untuk data realtime dan bisa dipakai ulang untuk path lain bila diperlukan.
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

// Menambahkan pembacaan terbaru ke akumulator untuk perhitungan rata-rata periodik.
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

// Mengirim ringkasan rata-rata 30 menit ke Firebase.
// Node disimpan berdasarkan tanggal dan waktu akhir agregasi.
void uploadLog30m() {
  if (acc.n == 0) return;

  MeterData avg;
  if (!getTimestampParts(avg.timestamp, avg.dateKey, avg.timeKey)) {
    Serial.println("Timestamp log 30 menit belum siap.");
    return;
  }

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

// Menjaga urutan dependensi koneksi:
// 1. WiFi harus aktif
// 2. Waktu NTP harus sinkron
// 3. Firebase baru boleh diinisialisasi
void ensureConnections() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  syncTimeWithNTP();
  if (!timeSynced) return;

  initFirebase();

  if (firebaseStarted && Firebase.ready() && !firebaseReadyLogged) {
    Serial.println("Firebase siap.");
    firebaseReadyLogged = true;
  } else if (firebaseStarted && !Firebase.ready()) {
    firebaseReadyLogged = false;
  }
}

// Setup hanya dipakai untuk inisialisasi awal perangkat.
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 PZEM004T v4.0 -> Firebase RTDB");

  // DS18B20 diset non-blocking agar loop utama tetap ringan.
  ds18b20.begin();
  ds18b20.setWaitForConversion(false);
  startTemperatureConversion();
  connectWiFi();
}

// Loop utama menjalankan:
// - penjagaan koneksi
// - pembacaan sensor berkala
// - upload realtime
// - pembentukan log rata-rata 30 menit
void loop() {
  ensureConnections();

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (millis() - lastSendMs < SEND_INTERVAL_MS) {
    return;
  }

  if (!timeSynced) {
    Serial.println("Waktu belum sinkron, tunda upload.");
    return;
  }

  if (!Firebase.ready()) {
    Serial.println("Firebase belum ready, tunda upload.");
    return;
  }

  MeterData d;
  if (!readPzemData(d)) {
    Serial.println("Data belum siap. Cek sensor atau tunggu pembacaan berikutnya.");
    return;
  }

  lastSendMs = millis();
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
