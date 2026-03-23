#include <SPI.h>
#include <LoRa.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <GY521.h>
#include <QMC5883LCompass.h>
#include <WebServer.h>
#include <Update.h>
#include <WiFi.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>
#define SD_CS 15 

// ================== Konfigurasi PIN & LoRa ==================
#define SS 5
#define RST 4
#define DIO0 25
// #define LED_RX 2
// #define LED_TX 12
// #define LED_RUN 2
// #define LED_SETTING 12
#define LED_RX_1 2
#define LED_RX_2 26
#define LED_TX_1 12
#define LED_TX_2 27
#define LED_RUN_1 2
#define LED_RUN_2 26
#define LED_SETTING_1 12
#define LED_SETTING_2 27

#define BTN_MODE 0
#define GPS_RX 16
#define GPS_TX 17
#define LORA_SF 7
#define LORA_BAND 125E3
#define LORA_FREQ 915E6
#define LORA_TXPOW 8
//board hijau
#define MODEM_RX 13
#define MODEM_TX 14

//board biru
// #define MODEM_RX 16
// #define MODEM_TX 17
HardwareSerial ModemSerial(1);
TinyGsm modem(ModemSerial);
TinyGsmClient simClient(modem);
PubSubClient mqtt(simClient);

bool simReady = false;
bool mqttReady = false;
unsigned long lastMqttReconnect = 0;
bool backlogBusy = false;

const unsigned long HOLD_MS = 3000;
unsigned long lastLoraSent = 0;
unsigned long lastGsmSent = 0;
// ================== Variabel Global ==================
WebServer server(80);
GY521 mpu(0x68);
QMC5883LCompass compass;
TinyGPSPlus gps;
HardwareSerial GPSSerial(2);

struct AppConfig {
  String mmsi = "525000100";
  String shipName = "DefaultShip";
  String apn = "internet";
  String simUser = "";
  String simPass = "";
  String mqttHost = "aisport.ppns.ac.id";
  int mqttPort = 1883;
  String mqttUser = "mosquitto";
  String mqttPass = "mosquitto";
  String mqttTopic = "lentera/GATE6D9FE8";
  unsigned long loraInterval = 60000;   // <--- Tambahan
  unsigned long gsmInterval  = 10000; 
  bool enableLoRa = true;   // <--- Tambahan
  bool enableGSM  = true;   // <--- Tambahan

} config;

uint64_t MAC_FULL;
uint32_t NODE_ID;
bool IS_GATEWAY = false;
bool loraReady = false;
bool gpsReady = false;
bool CAN_SEE_GATEWAY = true;

String lastPinkRoute = "";
String myPrevHop = "";
unsigned long lastMsgSent = 0;
const unsigned long msgInterval = 60000;
unsigned long gpsLastFixMs = 0;
bool gpsValid = false;
const unsigned long VALID_TIMEOUT_MS = 2000;

static bool sosActive = false;
static unsigned long sosPressStart = 0;
bool sosSentOnce = false;
// void blinkLED(int pin, int t = 100) {
//   digitalWrite(pin, HIGH);
//   delay(t);
//   digitalWrite(pin, LOW);
// }
unsigned long lastPinkSeen = 0;
const unsigned long pinkTimeout = 120000;

unsigned long lastSimCheck = 0;
const unsigned long simRetryInterval = 45000; // 45 detik

String topicStatus = "lentera/status/" + config.mmsi;
void publishStatus(String status) {
  if (mqtt.connected()) {
    mqtt.publish(("lentera/status/" + config.mmsi).c_str(), status.c_str());
  }
}

void sendBacklogSafe() {
  if (backlogBusy) return;  // hindari konflik akses SD
  backlogBusy = true;

  if (!SD.exists("/temp.csv")) { backlogBusy = false; return; }

  // SD.remove("/temp_send.csv");
  // SD.rename("/temp.csv", "/temp_send.csv");
  if (SD.exists("/temp_send.csv")) {
    SD.remove("/temp_send.csv");
  }
  SD.rename("/temp.csv", "/temp_send.csv");


  File tempFile = SD.open("/temp_send.csv", FILE_READ);
  File retryFile = SD.open("/temp.csv", FILE_WRITE);
  if (!tempFile || !retryFile) {
    Serial.println("❌ Gagal buka file backlog.");
    backlogBusy = false;
    return;
  }

  Serial.println("📤 Kirim backlog dengan resume support...");
  int sentCount = 0, failCount = 0;

  while (tempFile.available()) {
    String line = tempFile.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    if (!mqtt.connected() || !simReady) {
      Serial.println("⚠️ MQTT/SIM terputus, hentikan kirim.");
      retryFile.println(line);
      failCount++;
      break;
    }

    if (mqtt.publish(config.mqttTopic.c_str(), line.c_str())) {
      Serial.printf("✅ Backlog terkirim: %s\n", line.c_str());
      sentCount++;
      delay(250);
    } else {
      Serial.println("⚠️ MQTT gagal kirim, simpan kembali ke backlog.");
      retryFile.println(line);
      failCount++;
    }
  }

  tempFile.close();
  retryFile.close();
  SD.remove("/temp_send.csv");

  backlogBusy = false;

  if (failCount == 0)
    Serial.printf("✅ Semua backlog (%d baris) berhasil dikirim.\n", sentCount);
  else
    Serial.printf("⚠️ %d baris gagal, tersimpan ulang di temp.csv.\n", failCount);
}

// ================= SIM800L + MQTT ==================
void initSIM800L() {
  simReady = false;
  mqttReady = false;
  Serial.println("🔌 Inisialisasi SIM800L...");

  ModemSerial.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modem.restart();
  delay(1000);

  unsigned long startAttempt = millis();
  const unsigned long maxWait = 30000; // timeout total 30 detik

  unsigned long lastBlink = 0;
  const unsigned long blinkInterval = 200;

  Serial.println("📡 Menunggu registrasi jaringan seluler...");
  while (!modem.waitForNetwork(1000)) {
    unsigned long now = millis();
    if (now - lastBlink >= blinkInterval) {
      digitalWrite(LED_RUN_1, !digitalRead(LED_RUN_1));
      digitalWrite(LED_RUN_2, !digitalRead(LED_RUN_2));
      lastBlink = now;
    }
    yield();

    // Timeout aman
    if (millis() - startAttempt > maxWait) {
      Serial.println("❌ Timeout: tidak bisa registrasi jaringan!");
      simReady = false;
      mqttReady = false;
      return;
    }
  }

  Serial.println("✅ Jaringan seluler terdeteksi, aktifkan GPRS...");
  startAttempt = millis();
  while (!modem.gprsConnect(config.apn.c_str(), config.simUser.c_str(), config.simPass.c_str())) {
    unsigned long now = millis();
    if (now - lastBlink >= 400) {
      digitalWrite(LED_RUN_1, !digitalRead(LED_RUN_1));
      digitalWrite(LED_RUN_2, !digitalRead(LED_RUN_2));
      lastBlink = now;
    }
    yield();

    if (millis() - startAttempt > maxWait) {
      Serial.println("❌ Timeout: gagal aktifkan GPRS!");
      simReady = false;
      mqttReady = false;
      return;
    }
  }

  if (!modem.isGprsConnected()) {
    Serial.println("❌ GPRS masih belum aktif setelah percobaan!");
    simReady = false;
    mqttReady = false;
    return;
  }

  Serial.println("🌐 GPRS aktif, mencoba koneksi MQTT...");
  mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);

  startAttempt = millis();
  while (!mqtt.connect("ESP32SIM", config.mqttUser.c_str(), config.mqttPass.c_str())) {
    unsigned long now = millis();
    if (now - lastBlink >= 500) {
      digitalWrite(LED_RUN_1, !digitalRead(LED_RUN_1));
      digitalWrite(LED_RUN_2, !digitalRead(LED_RUN_2));
      lastBlink = now;
    }
    yield();

    if (millis() - startAttempt > maxWait) {
      Serial.println("❌ Timeout: MQTT gagal connect!");
      simReady = true;   // modem masih OK
      mqttReady = false;
      return;
    }
  }

  // === Bagian baru di bawah ini ===
  Serial.println("✅ MQTT via GPRS tersambung!");
  digitalWrite(LED_RUN_1, LOW);
  digitalWrite(LED_RUN_2, LOW);
  simReady = true;
  mqttReady = true;

  // 🔥 Kirim backlog jika ada data offline
  if (SD.exists("/temp.csv")) {
    Serial.println("📤 MQTT aktif — kirim backlog temp.csv...");
    sendBacklogSafe();  // sinkronisasi otomatis backlog
  }
}


bool mqttConnect() {
  Serial.print("🔗 MQTT connect ke ");
  Serial.println(config.mqttHost);

  mqtt.setServer(config.mqttHost.c_str(), config.mqttPort);
  if (mqtt.connect("ESP32SIM", config.mqttUser.c_str(), config.mqttPass.c_str())) {
    Serial.println("✅ MQTT terhubung");
    mqttReady = true;
    return true;
  } else {
    Serial.println("⚠️ MQTT gagal connect");
    mqttReady = false;
    return false;
  }
}

void logToTemp(String payload) {
  File f = SD.open("/temp.csv", FILE_APPEND);
  if (f) {
    f.println(payload);
    f.close();
    Serial.println("📁 Simpan offline ke temp.csv");
  } else {
    Serial.println("❌ Tidak bisa tulis temp.csv");
  }
}

void sendBacklog() {
  if (!SD.exists("/temp.csv")) return;
  File f = SD.open("/temp.csv", FILE_READ);
  if (!f) return;

  Serial.println("📤 Mengirim backlog temp.csv...");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (mqtt.publish(config.mqttTopic.c_str(), line.c_str())) {
      Serial.printf("✅ Kirim backlog: %s\n", line.c_str());
      delay(300);
    } else {
      Serial.println("⚠️ MQTT gagal kirim backlog, hentikan.");
      break;
    }
  }
  f.close();

  // Hapus backlog setelah terkirim
  SD.remove("/temp.csv");
  Serial.println("🗑️ temp.csv dihapus setelah sinkronisasi.");
}


void blinkLED(int pin1, int pin2, int t = 100) {
  digitalWrite(pin1, HIGH);
  digitalWrite(pin2, HIGH);
  delay(t);
  digitalWrite(pin1, LOW);
  digitalWrite(pin2, LOW);
}

// ================= AIS ENCODE =================
struct AISMessage18 {
  uint8_t message_id = 18;      // 6 bits
  uint8_t repeat_indicator = 0; // 2 bits
  uint32_t mmsi;                // 30 bits
  uint8_t reserved = 0;         // 8 bits
  uint16_t sog;                 // 10 bits (Speed Over Ground * 10)
  bool position_accuracy = false; // 1 bit
  float longitude;              // 28 bits (1/10000 minute = 1/600000 derajat)
  float latitude;               // 27 bits
  uint16_t cog;                 // 12 bits (Course Over Ground * 10)
  uint16_t heading;             // 9 bits (True Heading)
  uint8_t utc_second;           // 6 bits
  uint8_t regional = 0;         // 2 bits
  bool cs_flag = false;         // 1 bit
  bool display_flag = false;    // 1 bit
  bool dsc_flag = false;        // 1 bit
  bool band_flag = false;       // 1 bit
  bool msg22_flag = false;      // 1 bit
  bool assigned_flag = false;   // 1 bit
  bool raim_flag = false;       // 1 bit
  uint32_t radio_status = 0;    // 20 bits
};

void writeBits(uint8_t *buffer, uint32_t value, uint16_t startBit, uint8_t numBits, bool isSigned = false)
{
  if (isSigned && (value & (1UL << (numBits - 1))))
    value |= 0xFFFFFFFFUL << numBits;
  for (uint8_t i = 0; i < numBits; i++)
  {
    uint16_t pos = startBit + i;
    uint8_t byteIdx = pos / 8;
    uint8_t bitIdx = 7 - (pos % 8);
    if (value & (1UL << (numBits - 1 - i)))
      buffer[byteIdx] |= (1 << bitIdx);
    else
      buffer[byteIdx] &= ~(1 << bitIdx);
  }
}

void bitsToAISString(const uint8_t *bits, uint16_t bitCount, char *output)
{
  uint16_t charCount = (bitCount + 5) / 6;
  for (uint16_t i = 0; i < charCount; i++)
  {
    uint8_t sixBits = 0;
    for (uint8_t j = 0; j < 6; j++)
    {
      uint16_t bitIdx = i * 6 + j;
      if (bitIdx < bitCount)
        sixBits = (sixBits << 1) | ((bits[bitIdx / 8] >> (7 - (bitIdx % 8))) & 1);
      else
        sixBits <<= 1;
    }
    output[i] = (sixBits < 40) ? (sixBits + 48) : (sixBits + 56);
  }
  output[charCount] = '\0';
}

String encodeMessage18(const AISMessage18 &msg) {
  uint8_t bits[21] = {0};  // 168 bits = 21 bytes

  writeBits(bits, msg.message_id, 0, 6);
  writeBits(bits, msg.repeat_indicator, 6, 2);
  writeBits(bits, msg.mmsi, 8, 30);
  writeBits(bits, msg.reserved, 38, 8);
  writeBits(bits, msg.sog, 46, 10);
  writeBits(bits, msg.position_accuracy ? 1 : 0, 56, 1);
  int32_t lng_enc = (int32_t)round(msg.longitude * 600000.0);
  writeBits(bits, lng_enc, 57, 28, true);
  int32_t lat_enc = (int32_t)round(msg.latitude * 600000.0);
  writeBits(bits, lat_enc, 85, 27, true);
  writeBits(bits, msg.cog, 112, 12);
  writeBits(bits, msg.heading, 124, 9);
  writeBits(bits, msg.utc_second, 133, 6);
  writeBits(bits, msg.regional, 139, 2);
  writeBits(bits, msg.cs_flag ? 1 : 0, 141, 1);
  writeBits(bits, msg.display_flag ? 1 : 0, 142, 1);
  writeBits(bits, msg.dsc_flag ? 1 : 0, 143, 1);
  writeBits(bits, msg.band_flag ? 1 : 0, 144, 1);
  writeBits(bits, msg.msg22_flag ? 1 : 0, 145, 1);
  writeBits(bits, msg.assigned_flag ? 1 : 0, 146, 1);
  writeBits(bits, msg.raim_flag ? 1 : 0, 147, 1);
  writeBits(bits, msg.radio_status, 148, 20);

  char payload[29];
  bitsToAISString(bits, 168, payload);
  return String(payload);
}

String nmeaChecksum(const String &sentence)
{
  int chk = 0;
  for (int i = 1; i < sentence.length(); i++)
    chk ^= sentence[i];
  char buf[5];
  sprintf(buf, "*%02X", chk);
  return String(buf);
}
String makeAivdm(const String &payload)
{
  String base = "!AIVDM,1,1,,A," + payload + ",0";
  return base + nmeaChecksum(base);
}

// ================== Log SD CARD ==================
// void logToSD(String data) {
//   File logFile = SD.open("/log.csv", FILE_APPEND);
//   if (logFile) {
//     logFile.print(millis());
//     logFile.print(",");
//     logFile.println(data);
//     logFile.close();
//   } else {
//     Serial.println("Tidak bisa tulis ke SD Card!");
//   }
// }
void logToSD(String sensorData, String loraData = "") {
  File logFile = SD.open("/log.csv", FILE_APPEND);
  if (logFile) {
    logFile.print(millis());
    logFile.print('\t');           // Kolom waktu
    logFile.print(sensorData);     // Kolom sensor
    logFile.print('\t');           // Pisah dengan LoRa
    logFile.println(loraData);     // Kolom LoRa
    logFile.close();
  } else {
    Serial.println("Tidak bisa tulis ke SD Card!");
  }
}

void handleDownloadLog() {
  if (!SD.begin(SD_CS)) {
    server.send(500, "text/plain", "SD Card tidak terdeteksi!");
    return;
  }

  File logFile = SD.open("/log.csv", FILE_READ);
  if (!logFile) {
    server.send(404, "text/plain", "File log.csv tidak ditemukan!");
    return;
  }

  // Header agar browser otomatis mendownload
  server.sendHeader("Content-Type", "text/csv");
  server.sendHeader("Content-Disposition", "attachment; filename=log.csv");
  server.sendHeader("Connection", "close");

  // Kirim isi file ke browser
  server.streamFile(logFile, "text/csv");
  logFile.close();

  Serial.println("📤 File log.csv dikirim, akan restart...");
  server.send(200, "text/html",
  "<html><body style='font-family:sans-serif;text-align:center;padding-top:40px;'>"
  "<h3>📥 File log.csv sedang dikirim...</h3>"
  "<p>ESP32 akan restart setelah unduhan selesai.</p>"
  "</body></html>");
  delay(2000);
  ESP.restart();

}


void saveConfig() {
  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card belum siap, tidak bisa save config!");
    return;
  }
  StaticJsonDocument<512> doc;
  doc["mmsi"] = config.mmsi;
  doc["ship"] = config.shipName;
  doc["apn"] = config.apn;
  doc["simUser"] = config.simUser;
  doc["simPass"] = config.simPass;
  doc["mqttHost"] = config.mqttHost;
  doc["mqttPort"] = config.mqttPort;
  doc["mqttUser"] = config.mqttUser;
  doc["mqttPass"] = config.mqttPass;
  doc["mqttTopic"] = config.mqttTopic;
  doc["loraInterval"] = config.loraInterval;
  doc["gsmInterval"] = config.gsmInterval; 
  doc["enableLoRa"] = config.enableLoRa;
  doc["enableGSM"]  = config.enableGSM;
  File f = SD.open("/config.json", FILE_WRITE);
  if (!f) {
    Serial.println("Gagal buka file config.json untuk ditulis!");
    return;
  }
  serializeJsonPretty(doc, f);
  f.flush();
  f.close();
  Serial.println("Config berhasil disimpan ke SD Card.");
}

void loadConfig() {
  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card belum siap, tidak bisa load config!");
    return;
  }
  File f = SD.open("/config.json", FILE_READ);
  if (!f) {
    Serial.println("config.json belum ada, pakai default!");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("Gagal baca config.json! Pakai default.");
    return;
  }

  if (doc.containsKey("mmsi")) config.mmsi = (const char*)doc["mmsi"];
  if (doc.containsKey("ship")) config.shipName = (const char*)doc["ship"];
  if (doc.containsKey("apn")) config.apn = (const char*)doc["apn"];
  if (doc.containsKey("simUser")) config.simUser = (const char*)doc["simUser"];
  if (doc.containsKey("simPass")) config.simPass = (const char*)doc["simPass"];
  if (doc.containsKey("mqttHost")) config.mqttHost = (const char*)doc["mqttHost"];
  if (doc.containsKey("mqttPort")) config.mqttPort = doc["mqttPort"];
  if (doc.containsKey("mqttUser")) config.mqttUser = (const char*)doc["mqttUser"];
  if (doc.containsKey("mqttPass")) config.mqttPass = (const char*)doc["mqttPass"];
  if (doc.containsKey("mqttTopic")) config.mqttTopic = (const char*)doc["mqttTopic"];
  if (doc.containsKey("loraInterval")) config.loraInterval = doc["loraInterval"];
  if (doc.containsKey("gsmInterval"))  config.gsmInterval  = doc["gsmInterval"];
  if (doc.containsKey("enableLoRa")) config.enableLoRa = doc["enableLoRa"];
  if (doc.containsKey("enableGSM"))  config.enableGSM  = doc["enableGSM"];


  Serial.println("Config loaded dari SD:");
  Serial.printf("  MMSI: %s | Kapal: %s\n", config.mmsi.c_str(), config.shipName.c_str());
  Serial.printf("  APN: %s | MQTT: %s:%d\n", config.apn.c_str(), config.mqttHost.c_str(), config.mqttPort);
}


// ================== Fungsi OTA & WebConfig ==================
const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"/>
<title>ESP32 OTA & Config</title>
<style>
body{font-family:sans-serif;background:#0f1226;color:#e9ecf1;padding:20px}
.card{background:#1b1e3a;padding:16px;border-radius:10px;max-width:650px;margin:auto}
input,button{width:100%;padding:10px;margin-top:5px;border-radius:6px;border:1px solid #444;background:#222;color:#eee}
button{background:#3b82f6;border:none;cursor:pointer}
h2{color:#60a5fa}
</style></head><body>
<div class="card">
<h2>📦 OTA Firmware Update</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<label>Upload Firmware (.bin)</label>
<input type="file" name="firmware" accept=".bin">
<button type="submit">Upload</button>
</form>
</div>

<div class="card">
<h2>⚙️ Konfigurasi Node & Koneksi</h2>
<form method="POST" action="/config">
<h3>🛰️ Identitas Kapal</h3>
<label>MMSI</label><input name="mmsi" value="%MMSI%">
<label>Nama Kapal</label><input name="ship" value="%SHIP%">

<h3>📶 Koneksi SIM800L</h3>
<label>APN</label><input name="apn" value="%APN%">
<label>SIM User</label><input name="simUser" value="%SIMUSER%">
<label>SIM Pass</label><input name="simPass" value="%SIMPASS%">

<h3>☁️ Server MQTT</h3>
<label>Host</label><input name="mqttHost" value="%MQTTHOST%">
<label>Port</label><input name="mqttPort" value="%MQTTPORT%">
<label>User</label><input name="mqttUser" value="%MQTTUSER%">
<label>Pass</label><input name="mqttPass" value="%MQTTPASS%">
<label>Topic</label><input name="mqttTopic" value="%MQTTTOPIC%">

<h3>⏱️ Interval Pengiriman</h3>
<label>LoRa Interval (ms)</label><input name="loraInterval" value="%LORAINTERVAL%">
<label>GSM Interval (ms)</label><input name="gsmInterval" value="%GSMINTERVAL%">

<h3>🚀 Mode Pengiriman</h3>
<label><input type="checkbox" name="enableLoRa" %LORA_CHECKED%> Aktifkan LoRa</label><br>
<label><input type="checkbox" name="enableGSM" %GSM_CHECKED%> Aktifkan GSM/MQTT</label>

<button type="submit">💾 Simpan & Restart</button>
</form>
</div>

<div class="card">
<h2>📥 Unduh Log Aktivitas</h2>
<a href="/download"><button type="button">Download log.csv</button></a>
</div>
</body></html>
)HTML";



void sendRoot() {
  String html = PAGE_HTML;
  html.replace("%MMSI%", config.mmsi);
  html.replace("%SHIP%", config.shipName);
  html.replace("%APN%", config.apn);
  html.replace("%SIMUSER%", config.simUser);
  html.replace("%SIMPASS%", config.simPass);
  html.replace("%MQTTHOST%", config.mqttHost);
  html.replace("%MQTTPORT%", String(config.mqttPort));
  html.replace("%MQTTUSER%", config.mqttUser);
  html.replace("%MQTTPASS%", config.mqttPass);
  html.replace("%MQTTTOPIC%", config.mqttTopic);
  html.replace("%LORAINTERVAL%", String(config.loraInterval));
  html.replace("%GSMINTERVAL%", String(config.gsmInterval));
  html.replace("%LORA_CHECKED%", config.enableLoRa ? "checked" : "");
  html.replace("%GSM_CHECKED%", config.enableGSM ? "checked" : "");

  server.send(200, "text/html", html);
}


void handleUpdateUpload()
{
  HTTPUpload &upload = server.upload();
  static bool started = false;
  if (upload.status == UPLOAD_FILE_START) {
    started = Update.begin(UPDATE_SIZE_UNKNOWN);
    Serial.println("[OTA] Mulai upload firmware...");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (started) Update.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Selesai upload: %u bytes\n", upload.totalSize);
    } else {
      Serial.println("[OTA] Gagal update!");
    }
  }
}

void handleUpdateFinish()
{
  server.send(200, "text/plain", "OTA Selesai — perangkat akan reboot...");
  delay(500);
  ESP.restart();
}

void handleConfigPost() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("mmsi")) config.mmsi = server.arg("mmsi");
    if (server.hasArg("ship")) config.shipName = server.arg("ship");
    if (server.hasArg("apn")) config.apn = server.arg("apn");
    if (server.hasArg("simUser")) config.simUser = server.arg("simUser");
    if (server.hasArg("simPass")) config.simPass = server.arg("simPass");
    if (server.hasArg("mqttHost")) config.mqttHost = server.arg("mqttHost");
    if (server.hasArg("mqttPort")) config.mqttPort = server.arg("mqttPort").toInt();
    if (server.hasArg("mqttUser")) config.mqttUser = server.arg("mqttUser");
    if (server.hasArg("mqttPass")) config.mqttPass = server.arg("mqttPass");
    if (server.hasArg("mqttTopic")) config.mqttTopic = server.arg("mqttTopic");
    if (server.hasArg("loraInterval")) config.loraInterval = server.arg("loraInterval").toInt();
    if (server.hasArg("gsmInterval"))  config.gsmInterval  = server.arg("gsmInterval").toInt();
        config.enableLoRa = server.hasArg("enableLoRa");
        config.enableGSM  = server.hasArg("enableGSM");

    saveConfig();

    Serial.println("Konfigurasi baru disimpan:");
    Serial.printf("  MMSI   : %s\n", config.mmsi.c_str());
    Serial.printf("  Kapal  : %s\n", config.shipName.c_str());
    Serial.printf("  APN    : %s\n", config.apn.c_str());
    Serial.printf("  SIMUsr : %s\n", config.simUser.c_str());
    Serial.printf("  SIMPwd : %s\n", config.simPass.c_str());
    Serial.printf("  MQTT   : %s:%d\n", config.mqttHost.c_str(), config.mqttPort);
    Serial.printf("  Topic  : %s\n", config.mqttTopic.c_str());

    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;text-align:center;padding-top:50px;'>"
      "<h2>Konfigurasi disimpan!</h2>"
      "<p>Perangkat akan restart dalam 3 detik...</p>"
      "</body></html>");

    delay(3000);
    ESP.restart(); 
  }
}

// void readIMU(float *pitch, float *roll) {
//   mpu.read();
//   float ax = mpu.getAccelX() * 9.80665;
//   float ay = mpu.getAccelY() * 9.80665;
//   float az = mpu.getAccelZ() * 9.80665;

//   float tPitch = (180.0 * atan2(ax, sqrt(ay * ay + az * az)) / PI) + 90.0;
//   float tRoll  = (180.0 * atan2(ay, sqrt(ax * ax + az * az)) / PI) + 90.0;

//   // clamp ke 0–180
//   if (tPitch < 0) tPitch = 0; if (tPitch > 180) tPitch = 180;
//   if (tRoll < 0) tRoll = 0;   if (tRoll > 180) tRoll = 180;

//   *pitch = tPitch;
//   *roll = tRoll;
// }

void readIMU(float *pitch, float *roll) {
  mpu.read();
  float ax = mpu.getAccelX() * 9.80665;
  float ay = mpu.getAccelY() * 9.80665;
  float az = mpu.getAccelZ() * 9.80665;

  float tPitch = (180.0 * atan2(ax, sqrt(ay * ay + az * az)) / PI);
  float tRoll  = (180.0 * atan2(ay, sqrt(ax * ax + az * az)) / PI);

  // clamp ke -90 – +90
  if (tPitch < -90) tPitch = -90;
  if (tPitch > 90)  tPitch = 90;
  if (tRoll < -90)  tRoll = -90;
  if (tRoll > 90)   tRoll = 90;

  *pitch = tPitch;  
  *roll = tRoll;
}

float readHeading() {
  compass.read();
  return compass.getAzimuth();
}

// ================== Handler Mesh & LoRa ==================
void sendPacket(String pkt) {
  if (LoRa.beginPacket() == 0) {
    Serial.println("TX fail");
    return;
  }
  LoRa.print(pkt);
  LoRa.endPacket();
  // blinkLED(LED_TX);
  blinkLED(LED_TX_1, LED_TX_2);
  Serial.printf("[TX] %s\n", pkt.c_str());
} 
void node_handlePacket(String data, int rssi) {
  // --- 1️⃣ Re-broadcast data AIS BROADCAST ---
  if (data.startsWith("MSG:BROADCAST-")) {
    delay(random(500, 1500));  // supaya tidak tabrakan antar node
    sendPacket(data);          // kirim ulang persis seperti diterima
    Serial.println("📡 Re-broadcast MSG:BROADCAST untuk memperluas jangkauan");
    return;                    // selesai, tidak perlu proses PINK
  }

  // --- 2️⃣ Tangani paket PINK (routing) ---
  if (!data.startsWith("PINK:")) return;

  bool simulateFarFromGateway = false;  // ubah ke true kalau mau test jarak jauh

  if (simulateFarFromGateway && data.startsWith("PINK:GATE")) {
    if (data.indexOf('-') == -1) {
      Serial.println("Simulasi aktif: Abaikan ping langsung dari gateway (jarak jauh)");
      Serial.println("========================================");
      return;
    }
  }

  // --- sisanya tetap sama ---
  String chain = data.substring(5);
  String temp = chain;
  temp.replace("-", "");
  int hopCount = chain.length() - temp.length();
  Serial.println("========================================");
  Serial.printf("PINK diterima: %s\n", chain.c_str());
  Serial.printf("Panjang rute: %d karakter | Jumlah hop: %d | RSSI: %d dBm\n",
                chain.length(), hopCount, rssi);

  if (lastPinkRoute != "")
    Serial.printf("Rute tersimpan saat ini: %s (len=%d)\n",
                  lastPinkRoute.c_str(), lastPinkRoute.length());
  else
    Serial.println("Belum ada rute tersimpan sebelumnya.");

  if (chain.indexOf(config.mmsi) != -1) {
    Serial.printf("MMSI %s sudah ada di rantai — abaikan (loop prevention)\n",
                  config.mmsi.c_str());
    Serial.println("========================================");
    return;
  }

  // ======== PRIORITAS RUTE TERPENDEK + SELALU RELAY ========
  static int lastRssi = -999;
  int newLen = chain.length();
  int oldLen = lastPinkRoute.length();

  if (lastPinkRoute == "") {
    lastPinkRoute = chain;
    lastRssi = rssi;
    int lastDash = chain.lastIndexOf('-');
    myPrevHop = (lastDash >= 0) ? chain.substring(lastDash + 1) : "";
    String newChain = chain + "-" + config.mmsi;
    delay(random(1500, 4000));
    sendPacket("PINK:" + newChain);
    Serial.printf("Rute baru disimpan & dikirim: %s (RSSI %d)\n", newChain.c_str(), rssi);
  } 
  else if (newLen < oldLen) {
    Serial.printf("Rute lebih pendek (%d < %d), ganti rute.\n", newLen, oldLen);
    lastPinkRoute = chain;
    lastRssi = rssi;
    int lastDash = chain.lastIndexOf('-');
    myPrevHop = (lastDash >= 0) ? chain.substring(lastDash + 1) : "";
    String newChain = chain + "-" + config.mmsi;
    delay(random(1500, 4000));
    sendPacket("PINK:" + newChain);
    Serial.printf("Relay %s (RSSI %d)\n", newChain.c_str(), rssi);
  } 
  else if (newLen == oldLen) {
    if (rssi > lastRssi + 3) {
      Serial.printf("Rute sama panjang (%d == %d) tapi sinyal lebih kuat (%d > %d), GANTI.\n",
                    newLen, oldLen, rssi, lastRssi);
      lastPinkRoute = chain;
      lastRssi = rssi;
    } else {
      Serial.printf("Rute sama panjang (%d == %d) — relay ulang tanpa ubah rute. (RSSI %d, sebelumnya %d)\n",
                    newLen, oldLen, rssi, lastRssi);
    }
    String newChain = chain + "-" + config.mmsi;
    delay(random(1500, 4000));
    sendPacket("PINK:" + newChain);
    Serial.printf("Relay ulang: %s (RSSI %d)\n", newChain.c_str(), rssi);
  } 
  else {
    Serial.printf("Abaikan rute lebih panjang (%d > %d)\n", newLen, oldLen);
  }

  Serial.printf("Rute aktif sekarang: %s (len=%d, RSSI terakhir=%d)\n",
                lastPinkRoute.c_str(), lastPinkRoute.length(), lastRssi);
  lastPinkSeen = millis();
  Serial.println("========================================");
}

// int mapAngleToDigit(float angle) {
//   angle = constrain(angle, -90.0, 90.0);
//   float scaled = ((angle + 90.0) / 180.0) * 9.0;
//   return round(scaled);
// }
int mapAngleToDigit(float angle) {
  angle = constrain(angle, -90.0, 90.0);

  // 0° → 4
  // -90° → 0
  // +90° → 9
  float scaled = ((angle + 90.0) / 180.0) * 9.0;
  int digit = round(scaled);

  // Geser sedikit supaya 0° tepat di tengah (4)
  if (digit > 4) digit -= 1;

  return constrain(digit, 0, 9);
}

// ================== SETUP NODE ==================
void setup() {
  Serial.begin(115200);
  pinMode(LED_RUN_1, OUTPUT);
  pinMode(LED_RUN_2, OUTPUT);
  pinMode(LED_TX_1, OUTPUT);
  pinMode(LED_TX_2, OUTPUT);
  pinMode(LED_RX_1, OUTPUT);
  pinMode(LED_RX_2, OUTPUT);
  pinMode(LED_SETTING_1, OUTPUT);
  pinMode(LED_SETTING_2, OUTPUT);
  pinMode(BTN_MODE, INPUT_PULLUP);
  digitalWrite(LED_SETTING_1, HIGH);
  digitalWrite(LED_SETTING_2, HIGH);
  digitalWrite(LED_RUN_1, HIGH);
  digitalWrite(LED_RUN_2, HIGH);
  delay(2000);
  digitalWrite(LED_RUN_1, LOW);
  digitalWrite(LED_RUN_2, LOW);
  digitalWrite(LED_SETTING_1, LOW);
  digitalWrite(LED_SETTING_2, LOW);
  Serial.print("🗄️ SD Card...");
  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card gagal");
  } else {
    Serial.println("SD Card ready");
    loadConfig();
    if (!SD.exists("/log.csv")) {
      File logFile = SD.open("/log.csv", FILE_WRITE);
      logFile.println("time_ms\tsensor_data\tlora_data");
      logFile.close();
    }
  }

  // MAC_FULL = ESP.getEfuseMac();
  // NODE_ID = (uint32_t)(MAC_FULL & 0xFFFF);

  // Tombol Mode Setting (OTA/config)
  unsigned long t0 = millis();
  bool hold = false;
  while (millis() - t0 < HOLD_MS) {
    if (digitalRead(BTN_MODE) == LOW) hold = true;
    else { hold = false; break; }
  }
  if (hold) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32-OTA-NODE", "mesh12345");
    server.on("/", HTTP_GET, sendRoot);
    server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
    server.on("/config", HTTP_POST, handleConfigPost);
    server.on("/download", HTTP_GET, handleDownloadLog);
    server.begin();
    while (true) {
      server.handleClient();
      // digitalWrite(LED_SETTING, (millis() / 300) % 2);
      digitalWrite(LED_SETTING_1, (millis() / 300) % 2);
      digitalWrite(LED_SETTING_2, (millis() / 300) % 2);
    }
  }

  // Inisialisasi LoRa
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init gagal");
  } else {
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BAND);
    LoRa.setTxPower(LORA_TXPOW);
    LoRa.enableCrc();
    loraReady = true;
    Serial.println("LoRa siap digunakan"); 
  }

  // GPS
  // gpsReady = true;

  GPSSerial.begin(115200, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("Menunggu GPS fix...");
  while (!gpsReady) {
    while (GPSSerial.available()) gps.encode(GPSSerial.read());
    if (gps.location.isValid() && gps.satellites.value() >= 3) {
      gpsReady = true;
      Serial.printf("GPS Fix! Lat: %.6f, Lng: %.6f | Sats: %u\n",
        gps.location.lat(), gps.location.lng(), gps.satellites.value());
      break;
    }
    // digitalWrite(LED_RUN, (millis() / 300) % 2);
    digitalWrite(LED_RUN_1, (millis() / 300) % 2);
    digitalWrite(LED_RUN_2, (millis() / 300) % 2);
  }

  // IMU & Kompas
  Wire.begin();
  while (!mpu.wakeup()) {
    Serial.println("Tidak bisa konek ke GY-521, retry...");
    delay(1000);
  }
  mpu.setAccelSensitivity(3);  
  mpu.setGyroSensitivity(0);   
  mpu.setThrottle(false);
  Serial.println("GY-521 Siap");
  compass.init();
  Serial.println("QMC5883L Siap");
  initSIM800L();
  if (simReady) {
    mqttConnect();
  }
  Serial.printf("setup done! | Mode: Node | MMSI: %s\n", config.mmsi.c_str());
}

void loop() {
  server.handleClient(); 
  bool timeToSendLora = (millis() - lastLoraSent >= config.loraInterval);
  bool timeToSendGsm  = (millis() - lastGsmSent  >= config.gsmInterval);

  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  if (gps.location.isValid()) {
    gpsValid = true;
    gpsLastFixMs = millis();
  } else if (millis() - gpsLastFixMs > VALID_TIMEOUT_MS) {
    gpsValid = false;
  }
  // --- Tombol SOS dicek setiap loop ---
  if (digitalRead(BTN_MODE) == LOW) {
    if (sosPressStart == 0) sosPressStart = millis();
    if (millis() - sosPressStart > 2000) {
      sosActive = !sosActive;        // toggle on/off
      sosSentOnce = false;           // reset supaya kirim lagi kalau baru aktif
      sosPressStart = millis();      // hindari retrigger terus-menerus
      Serial.printf("🚨 SOS toggled: %s\n", sosActive ? "AKTIF" : "NONAKTIF");
    }
  } else {
    sosPressStart = 0;
  }

  // --- LED indikator SOS ---
  if (sosActive) {
    digitalWrite(LED_SETTING_1, (millis() / 300) % 2);
    digitalWrite(LED_SETTING_2, (millis() / 300) % 2);
  } else {
    digitalWrite(LED_SETTING_1, LOW);
    digitalWrite(LED_SETTING_2, LOW);
  }

  // --- Pengiriman AIS ---
  bool timeToSend = (millis() - lastMsgSent >= msgInterval);
  bool sosToSend  = (sosActive && !sosSentOnce);  // kirim segera saat SOS baru aktif

  // if (gpsValid && (timeToSend || sosToSend)) {
  // if (gps.location.isUpdated() && gpsValid && (timeToSend || sosToSend)) {
  // if (gps.location.isUpdated() && gpsValid && millis() - lastMsgSent >= msgInterval) {
  if (gps.location.isUpdated() && gpsValid && (timeToSendLora || timeToSendGsm)) {  

    double lat = gps.location.lat();
    double lng = gps.location.lng();
    double spd = gps.speed.kmph();
    double cog = gps.course.deg();
    float pitch = 0, roll = 0; readIMU(&pitch, &roll);
    float heading = readHeading();

    AISMessage18 msg;
    msg.mmsi = config.mmsi.toInt();
    msg.latitude = lat;
    msg.longitude = lng;
    msg.sog = (uint16_t)(gps.speed.knots() * 10);
    // msg.cog = (uint16_t)(gps.course.deg() * 10);
    // Gunakan waktu GPS sebagai pengganti COG (format HHMM)
    if (gps.time.isValid()) {
      int jam = gps.time.hour();
      int menit = gps.time.minute();
      int hhmm = jam * 100 + menit;
      msg.cog = (uint16_t) hhmm;  // simpan dalam 12 bit (cukup untuk 0–4095)
    } else {
      msg.cog = 0;  // fallback jika GPS belum valid
    }

    msg.heading = (uint16_t)heading;
    // msg.reserved = (uint8_t)roll;
    // msg.utc_second = (uint8_t) round((pitch / 180.0) * 63); 
    int roll_digit = mapAngleToDigit(roll);
    int pitch_digit = mapAngleToDigit(pitch);
    msg.reserved = (uint8_t)(roll_digit * 10 + pitch_digit);

    // UTC second tetap dari GPS time
    // if (gps.time.isValid()) {
    //   msg.utc_second = gps.time.second();
    // } else {
    //   msg.utc_second = 0;
    // }
    if (gps.date.isValid()) {
      int day = gps.date.day();    // 1–31
      msg.utc_second = day;        // simpan tanggal (bukan detik)
    } else {
      msg.utc_second = 0;
    }

    msg.repeat_indicator = 0;
    msg.position_accuracy = false;
    msg.regional = 0;
    // msg.cs_flag = false;
    // =================== SOS Flag ===================
    // if (digitalRead(BTN_MODE) == LOW) {
    //   if (sosPressStart == 0) sosPressStart = millis();
    //   if (millis() - sosPressStart > 2000) {
    //     sosActive = true; // Tombol ditekan lama → aktifkan SOS
    //   }
    // } else {
    //   sosPressStart = 0;
    // }

    // cs_flag digunakan sebagai indikator SOS
    msg.cs_flag = sosActive;

    msg.display_flag = false;
    msg.dsc_flag = false;
    msg.band_flag = false;
    msg.msg22_flag = false;
    msg.assigned_flag = false;
    msg.raim_flag = false;
    msg.radio_status = 0;
    Serial.printf("GPS Time: %02d:%02d → COG-field=%d\n",
                  gps.time.hour(), gps.time.minute(), msg.cog);
    Serial.printf("🚨 SOS FLAG: %s\n", msg.cs_flag ? "AKTIF" : "normal");

    String payload = encodeMessage18(msg);
    Serial.printf("Roll=%.1f Pitch=%.1f → reserved=%02d | UTC=%d\n",
              roll, pitch, msg.reserved, msg.utc_second);

    String aisFull = makeAivdm(payload);
    String fullPath = (lastPinkRoute.length() > 0) 
      ? (lastPinkRoute + "-" + config.mmsi) 
      : ("BROADCAST-" + config.mmsi);

    // Kirim via LoRa
    if (config.enableLoRa && timeToSendLora && loraReady) {
        sendPacket("MSG:" + fullPath + ":" + payload);
        lastLoraSent = millis();
        Serial.println("📡 Kirim via LoRa");
    }

    // --- 2️⃣ Kirim via MQTT ---
    if (config.enableGSM && timeToSendGsm && simReady && mqtt.connected()) {
      if (mqtt.publish(config.mqttTopic.c_str(), aisFull.c_str())) {
        Serial.println("☁️ Kirim via MQTT OK");
        // publishStatus("SEND_VIA_MQTT");
      } else {
        Serial.println("⚠️ MQTT gagal, simpan backlog");
        logToTemp(aisFull); 
        // publishStatus("SAVE_OFFLINE");
      }
      lastGsmSent = millis();
    } else {
      logToTemp(aisFull);
      Serial.println("💾 SIM tidak siap, disimpan offline");
      // publishStatus("SAVE_OFFLINE");
    }

    // --- Logging ke SD ---
    String sensorStr = "lat=" + String(lat, 6) + 
                      ",lng=" + String(lng, 6) +
                      ",spd=" + String(spd, 2) +
                      ",cog=" + String(cog, 2) +
                      ",pitch=" + String(pitch, 1) +
                      ",roll=" + String(roll, 1) +
                      ",hdg=" + String(heading, 1);

    String loraStr = "TX_MSG," + fullPath + "," + payload;
    logToSD(sensorStr, loraStr);


        // String logStr = String(lat, 6) + "," + String(lng, 6) + "," +
        //                 String(spd, 2) + "," + String(cog, 2) + "," +
        //                 String(pitch, 1) + "," + String(roll, 1) + "," +
        //                 String(heading, 1);
        // logToSD(logStr);
        if (sosActive) {
          sosSentOnce = true;  // sudah kirim SOS
          // jangan ubah lastMsgSent biar interval AIS tetap normal
        } else {
          lastMsgSent = millis(); // normal update
        }
        // lastMsgSent = millis();
      } 

  // LoRa Receive
  int sz = LoRa.parsePacket();
  if (sz) {
    String data = "";
    while (LoRa.available()) data += (char)LoRa.read();
    int rssi = LoRa.packetRssi();
    // blinkLED(LED_RX);
    blinkLED(LED_RX_1, LED_RX_2);
    if (data.startsWith("MSG:")) {
      String loraStr = "RX_MSG," + data + ",RSSI=" + String(rssi);
      logToSD("-", loraStr);   // tanda "-" kalau tidak ada data sensor
    }
    node_handlePacket(data, rssi);
  }
    // --- Reconnect MQTT tiap 30 detik ---
  // if (simReady && !mqtt.connected() && millis() - lastMqttReconnect > 30000) {
  //   lastMqttReconnect = millis();
  //   if (mqttConnect()) sendBacklog();
  // }
  // if (simReady && mqtt.connected() && millis() - lastMqttReconnect > 30000) {
  //   lastMqttReconnect = millis();
  //   sendBacklogSafe();
  // }
  // if (simReady && !mqtt.connected() && millis() - lastMqttReconnect > 30000) {
  //   lastMqttReconnect = millis();
  //   if (mqttConnect()) {
  //     sendBacklogSafe();   // langsung kirim backlog setelah koneksi sukses
  //   }
  // }
  if (simReady && mqtt.connected() && millis() - lastMqttReconnect > 30000) {
    lastMqttReconnect = millis();
    if (SD.exists("/temp.csv")) {
      Serial.println("📤 MQTT reconnect — kirim backlog tertunda...");
      sendBacklogSafe();
    }
  }


  if (millis() - lastSimCheck >= simRetryInterval) {
  lastSimCheck = millis();

  // 🔹 Cek kondisi SIM/modem
  bool gprsOK = modem.isGprsConnected();
  bool netOK  = modem.isNetworkConnected() || gprsOK;


  if (!netOK || !gprsOK) {
    Serial.println("⚠️ SIM800L terputus — lakukan restart penuh modem...");
    simReady = false;
    mqttReady = false;

    modem.gprsDisconnect();
    modem.restart();
    delay(2000);

    // Coba registrasi ulang jaringan
    if (modem.waitForNetwork(60000)) {
      Serial.println("✅ Registrasi jaringan OK");
      if (modem.gprsConnect(config.apn.c_str(), config.simUser.c_str(), config.simPass.c_str())) {
        Serial.println("✅ GPRS tersambung ulang");
        simReady = true;
      } else {
        Serial.println("❌ Gagal aktifkan GPRS");
      }
    } else {
      Serial.println("❌ Tidak bisa registrasi jaringan seluler");
    }

    // Coba MQTT lagi kalau modem OK
    if (simReady) {
      mqttReady = mqttConnect();
      if (mqttReady) sendBacklogSafe();
    }
  }
}

  mqtt.loop();
  // --- Cek apakah jalur mesh (gateway) masih aktif ---
  if (lastPinkRoute.length() > 0 && (millis() - lastPinkSeen > pinkTimeout)) {
    Serial.println("⚠️ Gateway tidak terdeteksi >2 menit, hapus rute mesh.");
    lastPinkRoute = "";
  }

}
