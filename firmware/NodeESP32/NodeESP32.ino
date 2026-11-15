/* ============================================================
   PAMSIMAS Smart Water Meter - NODE (ESP32) 
   PERSIAPAN:
   1. Install library: PubSubClient, ArduinoJson, NTPClient,
      LiquidCrystal_I2C, WiFiManager (tzapu)
   2. Kalau nanti USE_AES diaktifkan: download aes.h + aes.c dari
      https://github.com/kokke/tiny-AES-c, taruh di folder sketch ini,
      lalu install library "Base64" (Densaugeo)
   ============================================================ */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* ============================================================
   PARALEL: jaringan (WiFi/MQTT/LoRa/kirim data) jalan di Task/core
   TERPISAH dari LCD+sensor+menu, supaya LCD tidak lag saat WiFi lambat.
   ============================================================ */
SemaphoreHandle_t dataMutex;
TaskHandle_t networkTaskHandle;

/* ============================================================
   SAKLAR AES — biarkan 0 dulu sampai pengujian keamanan selesai.
   Setelah itu tinggal ubah jadi 1 (butuh aes.h/aes.c + lib Base64).
   ============================================================ */
#define USE_AES 1

#if USE_AES
  extern "C" {
    #include "aes.h"
  }
  uint8_t aesKey[16] = {
    0x50,0x41,0x4D,0x53,0x49,0x4D,0x41,0x53,
    0x32,0x30,0x32,0x36,0x4B,0x45,0x59,0x21
  };
#endif

/* ============================================================
   GANTI BARIS-BARIS INI UNTUK TIAP BOARD SEBELUM UPLOAD!
   ============================================================ */
#define NODE_ID "01"                                 /* "01","02","03","4"dst */
#define TB_PROVISION_KEY    "xxxxx"       /* dari Device Profile -> Device Provisioning */
#define TB_PROVISION_SECRET "xxxxx"    /* SAMA untuk semua node, bukan per-board */
#define DEFAULT_MODE 1                                /* 0=LoRa, 1=WiFi */
#define USE_FAKE_SENSOR 0                              /* 1=data palsu (testing), 0=sensor asli */

/* ============================================================
   SAKLAR TLS — untuk keperluan forensik perbandingan MQTT vs MQTTS.
   1 = MQTTS (port 8883, terenkripsi TLS)
   0 = MQTT biasa (port 1883, plaintext — untuk demo/capture Wireshark)
   ============================================================ */
#define USE_TLS 1

/* ===== THINGSBOARD (untuk mode WiFi langsung) ===== */
const char* TB_HOST = "thingsboard.cloud";
#if USE_TLS
  const int TB_PORT = 8883;
#else
  const int TB_PORT = 1883;
#endif

/* ===== PIN MAPPING ===== */
#define SENSOR_PIN   4
#define LORA_M0_PIN  18
#define LORA_M1_PIN  19
#define LORA_AUX_PIN 23
#define BTN1_PIN     32
#define BTN2_PIN     33
#define BTN3_PIN     25
#define BTN4_PIN     26
#define RELAY_PIN    27
#define LED_PIN      2
#define LORA_SERIAL  Serial2   /* RX2=16, TX2=17 */

#define AP_PORTAL_NAME    "PAMSIMAS-Node" NODE_ID //Nama Wi-Fi alat
#define AP_PORTAL_PASS    "12345678" //Password dari Wi-Fi alat

LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;
WiFiManager wm;

#if USE_TLS
  WiFiClientSecure secureClient;
  PubSubClient mqttClient(secureClient);
#else
  WiFiClient plainClient;
  PubSubClient mqttClient(plainClient);
#endif
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200); /* UTC+7 */

#if USE_AES
/* Koneksi KEDUA, terpisah dari koneksi ThingsBoard di atas — khusus
   kirim data sensor terenkripsi ke Mosquitto lokal (broker perantara). */
#define MOSQUITTO_IP    "xxxxx"   /* GANTI sesuai IP server Mosquitto */
#define MOSQUITTO_PORT  1883
WiFiClient localWifiClient;
PubSubClient localMqttClient(localWifiClient);
#endif

/* ===== STATE ===== */
enum SystemState {
  STATE_NORMAL, STATE_MENU_ADMIN, STATE_KALIBRASI, STATE_KALIBRASI_UKUR,
  STATE_SETTING_KIRIM, STATE_SETTING_JAM, STATE_RESET_ODOMETER,
  STATE_KONTROL_VALVE, STATE_MODE_SELECT
};
SystemState sysState = STATE_NORMAL;

/* ===== SENSOR ===== */
volatile uint32_t pulseCount = 0;
float filteredFlow = 0.0f;
float totalLiter   = 0.0f;
float faktorKalibrasi = 1.0f / 7.5f;
float fakeFlowValue = 0.0f;

/* ===== VALVE + JADWAL ===== */
uint8_t valveState = 0;
struct ValveSchedule {
  uint8_t  enabled;
  uint16_t startMin, endMin;       /* jendela jam (menit dari 00:00), seperti sebelumnya */
  uint8_t  useDateRange;           /* 0 = jadwal harian biasa, 1 = HANYA aktif di rentang tanggal ini */
  uint32_t startDateCode;          /* format YYYYMMDD, misal 20260810 */
  uint32_t endDateCode;            /* format YYYYMMDD */
};
ValveSchedule schedule = {0, 0, 0};
uint8_t manualOverride = 0;

/* ===== KIRIM ===== */
uint32_t sendIntervalMs = 5000;  /* interval saat TIDAK ada aliran (diatur dari menu admin) */
uint32_t lastSendTick = 0;

#define FAST_SEND_INTERVAL_MS 5000   /* interval saat ADA aliran air */
#define FLOW_THRESHOLD 0.05f         /* L/menit, di atas ini dianggap "ada aliran" */

/* ===== MODE & WIFI ===== */
uint8_t commMode = DEFAULT_MODE;
uint8_t wifiStarted = 0;
uint8_t mqttReady = 0;
char tbToken[40] = "";              /* token hasil provisioning, tersimpan di Flash */
uint32_t lastMqttAttempt = 0;

/* ===== WAKTU (untuk mode LoRa via gateway) ===== */
uint8_t needTime = 1;
char rxBuf[220];
uint8_t rxIdx = 0;

/* ===== PIN ADMIN ===== */
const uint8_t adminPIN[4] = {1, 2, 3, 4};

/* ===== KALIBRASI ===== */
struct KalibData {
  uint8_t pelanDone, sedangDone, cepatDone;
  float pelanPulsa, pelanRef, sedangPulsa, sedangRef, cepatPulsa, cepatRef;
};
KalibData kalib;
uint8_t kalibSelectedPoint = 0;

/* ===== FLASH HARIAN + BACKLOG ===== */
uint8_t flashSavedToday = 0;
uint8_t snapshotDoneMorning = 0;
#define BACKLOG_MAX 20
struct BacklogEntry { float volume; uint8_t hour, minute, sec; };
BacklogEntry backlog[BACKLOG_MAX];
uint8_t backlogCount = 0;

#if USE_AES
/* ===== Base64 sendiri (tidak pakai library eksternal) ===== */
static const char b64_chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "0123456789+/";

static int b64_encode(const uint8_t *src, size_t len, char *out) {
  size_t i = 0, j = 0;
  int enc_len = 0;
  uint8_t buf[3], b64[4];
  while (len--) {
    buf[i++] = *(src++);
    if (i == 3) {
      b64[0] = (buf[0] & 0xfc) >> 2;
      b64[1] = ((buf[0] & 0x03) << 4) | ((buf[1] & 0xf0) >> 4);
      b64[2] = ((buf[1] & 0x0f) << 2) | ((buf[2] & 0xc0) >> 6);
      b64[3] = buf[2] & 0x3f;
      for (i = 0; i < 4; i++) out[enc_len++] = b64_chars[b64[i]];
      i = 0;
    }
  }
  if (i > 0) {
    for (j = i; j < 3; j++) buf[j] = 0;
    b64[0] = (buf[0] & 0xfc) >> 2;
    b64[1] = ((buf[0] & 0x03) << 4) | ((buf[1] & 0xf0) >> 4);
    b64[2] = ((buf[1] & 0x0f) << 2) | ((buf[2] & 0xc0) >> 6);
    for (j = 0; j < i + 1; j++) out[enc_len++] = b64_chars[b64[j]];
    while (i++ < 3) out[enc_len++] = '=';
  }
  out[enc_len] = '\0';
  return enc_len;
}

static int b64_char_index(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int b64_decode(const char *src, uint8_t *out) {
  size_t len = strlen(src);
  size_t i = 0, j = 0, in_ = 0;
  uint8_t b64[4], buf[3];
  int dec_len = 0;
  while (len-- && src[in_] != '=' && b64_char_index(src[in_]) != -1) {
    b64[i++] = src[in_++];
    if (i == 4) {
      for (i = 0; i < 4; i++) b64[i] = b64_char_index((char)b64[i]);
      buf[0] = (b64[0] << 2) | ((b64[1] & 0x30) >> 4);
      buf[1] = ((b64[1] & 0xf) << 4) | ((b64[2] & 0x3c) >> 2);
      buf[2] = ((b64[2] & 0x3) << 6) | b64[3];
      for (i = 0; i < 3; i++) out[dec_len++] = buf[i];
      i = 0;
    }
  }
  if (i > 0) {
    for (j = i; j < 4; j++) b64[j] = 0;
    for (j = 0; j < 4; j++) {
      int v = b64_char_index((char)b64[j]);
      b64[j] = (v == -1) ? 0 : v;
    }
    buf[0] = (b64[0] << 2) | ((b64[1] & 0x30) >> 4);
    buf[1] = ((b64[1] & 0xf) << 4) | ((b64[2] & 0x3c) >> 2);
    for (j = 0; j < i - 1; j++) out[dec_len++] = buf[j];
  }
  return dec_len;
}

/* ===== AES-128 CBC HELPER (dipakai hanya kalau USE_AES=1) ===== */
bool AES_EncryptToBase64(const char *plaintext, char *outBuf, size_t outBufSize) {
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)random(0, 256);
    size_t plen = strlen(plaintext);
    size_t padded = ((plen / 16) + 1) * 16;
    if (padded > 160) return false;
    uint8_t buf[176];
    memcpy(buf, plaintext, plen);
    uint8_t padByte = (uint8_t)(padded - plen);
    for (size_t i = plen; i < padded; i++) buf[i] = padByte;
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, aesKey, iv);
    AES_CBC_encrypt_buffer(&ctx, buf, padded);
    uint8_t combined[192];
    memcpy(combined, iv, 16);
    memcpy(combined + 16, buf, padded);
    int encLen = b64_encode(combined, 16 + padded, outBuf);
    return (encLen < (int)outBufSize && encLen > 0);
}
bool AES_DecryptFromBase64(const char *b64input, char *outBuf, size_t outBufSize) {
    uint8_t combined[192];
    int decLen = b64_decode(b64input, combined);
    if (decLen < 32) return false;
    uint8_t iv[16];
    memcpy(iv, combined, 16);
    uint8_t *cipher = combined + 16;
    size_t cipherLen = decLen - 16;
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, aesKey, iv);
    AES_CBC_decrypt_buffer(&ctx, cipher, cipherLen);
    uint8_t padByte = cipher[cipherLen - 1];
    if (padByte == 0 || padByte > 16 || padByte > cipherLen) return false;
    size_t plainLen = cipherLen - padByte;
    if (plainLen >= outBufSize) return false;
    memcpy(outBuf, cipher, plainLen);
    outBuf[plainLen] = '\0';
    return true;
}
#endif

/* ===== ISR ===== */
void IRAM_ATTR onPulse() { pulseCount++; }

/* ===== BUTTON ===== */
uint8_t Btn_Read(uint8_t pin) {
  if (digitalRead(pin) == LOW) {
    uint32_t start = millis();
    while (digitalRead(pin) == LOW) {
      if (millis() - start >= 2000) { while (digitalRead(pin) == LOW) {} return 2; }
    }
    if (millis() - start >= 50) return 1;
  }
  return 0;
}

/* ===== VALVE ===== */
void Valve_SetState(uint8_t open) {
  digitalWrite(RELAY_PIN, open ? LOW : HIGH);
  valveState = open;
}

void Schedule_Apply() {
  static int16_t lastDebugMin = -1;
  time_t now; time(&now);
  struct tm *tinfo = localtime(&now);
  uint16_t nowMin = tinfo->tm_hour * 60 + tinfo->tm_min;
  uint32_t todayCode = (uint32_t)(tinfo->tm_year + 1900) * 10000UL
                      + (uint32_t)(tinfo->tm_mon + 1) * 100UL
                      + (uint32_t)tinfo->tm_mday;

  xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);

  /* Debug: cetak status jadwal sekali tiap menit berganti */
  if ((int16_t)nowMin != lastDebugMin) {
    lastDebugMin = nowMin;
    Serial.printf("[Jadwal] tgl=%lu now=%02d:%02d | enabled=%d useDate=%d [%lu-%lu] start=%d end=%d | override=%d valve=%d\n",
                  (unsigned long)todayCode, tinfo->tm_hour, tinfo->tm_min,
                  schedule.enabled, schedule.useDateRange,
                  (unsigned long)schedule.startDateCode, (unsigned long)schedule.endDateCode,
                  schedule.startMin, schedule.endMin,
                  manualOverride, valveState);
  }

  if (manualOverride || !schedule.enabled) { xSemaphoreGiveRecursive(dataMutex); return; }

  /* Kalau pakai rentang tanggal, cek dulu apakah hari ini masuk rentangnya.
     Di luar rentang -> jadwal dianggap TIDAK berlaku, valve dibiarkan terbuka. */
  if (schedule.useDateRange) {
    bool inDateRange = (todayCode >= schedule.startDateCode && todayCode <= schedule.endDateCode);
    if (!inDateRange) {
      if (valveState != 1) Valve_SetState(1);
      xSemaphoreGiveRecursive(dataMutex);
      return;
    }
  }

  bool shouldClose;
  if (schedule.startMin <= schedule.endMin)
    shouldClose = (nowMin >= schedule.startMin && nowMin < schedule.endMin);
  else
    shouldClose = (nowMin >= schedule.startMin || nowMin < schedule.endMin);
  uint8_t desiredOpen = shouldClose ? 0 : 1;
  if (desiredOpen != valveState) Valve_SetState(desiredOpen);
  xSemaphoreGiveRecursive(dataMutex);
}

/* ===== LORA (dipakai hanya saat commMode==0) ===== */
void LoRa_SetMode(uint8_t m0, uint8_t m1) {
  digitalWrite(LORA_M0_PIN, m0);
  digitalWrite(LORA_M1_PIN, m1);
  delay(50);
  uint32_t t = millis() + 3000;
  while (digitalRead(LORA_AUX_PIN) == LOW) { if (millis() > t) break; }
  delay(10);
}
void LoRa_WaitReady() {
  uint32_t t = millis() + 2000;
  while (digitalRead(LORA_AUX_PIN) == LOW) { if (millis() > t) break; }
}

/* ===== FLASH ===== */
void Flash_Save() {
  prefs.begin("pamsimas", false);
  prefs.putFloat("totalLiter", totalLiter);
  prefs.putBytes("kalib", &kalib, sizeof(kalib));
  prefs.putUInt("sendInt", sendIntervalMs);
  prefs.putUChar("commMode", commMode);
  prefs.putBytes("schedule", &schedule, sizeof(schedule));
  prefs.putUChar("override", manualOverride);
  prefs.end();
}
void Flash_Load() {
  prefs.begin("pamsimas", true);
  totalLiter = prefs.getFloat("totalLiter", 0.0f);
  size_t len = prefs.getBytesLength("kalib");
  if (len == sizeof(kalib)) prefs.getBytes("kalib", &kalib, sizeof(kalib));
  sendIntervalMs = prefs.getUInt("sendInt", 5000);
  commMode = prefs.getUChar("commMode", DEFAULT_MODE);
  size_t schLen = prefs.getBytesLength("schedule");
  if (schLen == sizeof(schedule)) prefs.getBytes("schedule", &schedule, sizeof(schedule));
  manualOverride = prefs.getUChar("override", 0);
  prefs.end();
}
void Backlog_Save() {
  prefs.begin("pamsimas", false);
  prefs.putBytes("backlog", backlog, sizeof(backlog));
  prefs.putUChar("backlogCnt", backlogCount);
  prefs.end();
}
void Backlog_Load() {
  prefs.begin("pamsimas", true);
  size_t len = prefs.getBytesLength("backlog");
  if (len == sizeof(backlog)) prefs.getBytes("backlog", backlog, sizeof(backlog));
  backlogCount = prefs.getUChar("backlogCnt", 0);
  prefs.end();
}
void Backlog_Push(float volume, uint8_t h, uint8_t m, uint8_t s) {
  xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
  if (backlogCount >= BACKLOG_MAX) {
    for (uint8_t i = 0; i < BACKLOG_MAX - 1; i++) backlog[i] = backlog[i+1];
    backlogCount = BACKLOG_MAX - 1;
  }
  backlog[backlogCount].volume = volume;
  backlog[backlogCount].hour   = h;
  backlog[backlogCount].minute = m;
  backlog[backlogCount].sec    = s;
  backlogCount++;
  Backlog_Save();
  xSemaphoreGiveRecursive(dataMutex);
}

void Flash_CheckAndSave() {
  time_t now; time(&now);
  struct tm *tinfo = localtime(&now);

  if (tinfo->tm_hour == 11 && tinfo->tm_min == 59) {
    if (!snapshotDoneMorning) {
      Backlog_Push(totalLiter, 11, 59, tinfo->tm_sec);
      Flash_Save(); snapshotDoneMorning = 1;
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Snapshot 11:59!");
      delay(1500);
    }
  } else snapshotDoneMorning = 0;

  if (tinfo->tm_hour == 23 && tinfo->tm_min == 59) {
    if (!flashSavedToday) {
      Backlog_Push(totalLiter, 23, 59, tinfo->tm_sec);
      Flash_Save(); flashSavedToday = 1;
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Data Tersimpan!");
      lcd.setCursor(0,1); lcd.print("Flash OK 23:59");
      delay(2000);
    }
  } else flashSavedToday = 0;
}

/* ============================================================
   ===== DEVICE PROVISIONING — auto-register ke ThingsBoard =====
   Node belum punya token -> connect sebentar pakai username "provision",
   kirim nama device + kunci provisioning, dapat token baru, simpan ke Flash.
   Selanjutnya token itu dipakai terus, provisioning tidak diulang lagi.
   ============================================================ */
void loadTbToken() {
  prefs.begin("pamsimas", true);
  String t = prefs.getString("tbToken", "");
  prefs.end();
  strncpy(tbToken, t.c_str(), sizeof(tbToken) - 1);
}
void saveTbToken(const char* token) {
  prefs.begin("pamsimas", false);
  prefs.putString("tbToken", token);
  prefs.end();
  strncpy(tbToken, token, sizeof(tbToken) - 1);
}
void clearTbToken() {
  prefs.begin("pamsimas", false);
  prefs.remove("tbToken");
  prefs.end();
  tbToken[0] = '\0';
}

volatile bool provisionDone = false;
char provisionTokenBuf[40] = {0};

void provisionCallback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != "/provision/response") return;

  String payloadStr = "";
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];
  Serial.println("[Provisioning] Response: " + payloadStr);

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payloadStr) == DeserializationError::Ok) {
    const char* status  = doc["status"];
    const char* credVal = doc["credentialsValue"];
    if (status != nullptr && String(status) == "SUCCESS" && credVal != nullptr)
      strncpy(provisionTokenBuf, credVal, sizeof(provisionTokenBuf) - 1);
  }
  provisionDone = true;
}

bool doProvisioning() {
  if (mqttClient.connected()) mqttClient.disconnect();

  mqttClient.setServer(TB_HOST, TB_PORT);
  mqttClient.setCallback(provisionCallback);

  char clientId[28];
  snprintf(clientId, sizeof(clientId), "ESP32_Node%s_prov", NODE_ID);

  if (!mqttClient.connect(clientId, "provision", "")) {
    Serial.println("[Provisioning] Gagal connect ke endpoint provisioning");
    return false;
  }
  mqttClient.subscribe("/provision/response");

  StaticJsonDocument<256> reqDoc;
  reqDoc["deviceName"]            = "Node " NODE_ID;
  reqDoc["provisionDeviceKey"]    = TB_PROVISION_KEY;
  reqDoc["provisionDeviceSecret"] = TB_PROVISION_SECRET;
  char reqBuf[256];
  serializeJson(reqDoc, reqBuf);
  mqttClient.publish("/provision/request", reqBuf);
  Serial.println("[Provisioning] Request terkirim, menunggu balasan...");

  provisionDone = false;
  provisionTokenBuf[0] = '\0';
  uint32_t t0 = millis();
  while (!provisionDone && millis() - t0 < 8000) {
    mqttClient.loop();
    delay(50);
  }
  mqttClient.disconnect();

  if (strlen(provisionTokenBuf) > 0) {
    saveTbToken(provisionTokenBuf);
    Serial.println("[Provisioning] Token baru diterima dan disimpan!");
    return true;
  }
  Serial.println("[Provisioning] Timeout / gagal mendapatkan token");
  return false;
}

/* ============================================================
   ===== MODE WIFI: KONEKSI LANGSUNG KE THINGSBOARD =====
   ============================================================ */
void mqttCallbackDirect(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String payloadStr = "";
  for (unsigned int i = 0; i < length; i++) payloadStr += (char)payload[i];

  /* ===== Perintah RPC langsung (setValve) ===== */
  if (topicStr.startsWith("v1/devices/me/rpc/request/")) {
    String reqId = topicStr.substring(topicStr.lastIndexOf('/') + 1);

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payloadStr) != DeserializationError::Ok) return;

    const char* method = doc["method"];
    if (method != nullptr && String(method) == "setValve") {
      int v  = doc["params"]["valve"]    | 0;
      int ov = doc["params"]["override"] | -1;

      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      if (ov != -1) { manualOverride = ov; Flash_Save(); }
      if (manualOverride) Valve_SetState(v ? 1 : 0);
      else Schedule_Apply();
      xSemaphoreGiveRecursive(dataMutex);

      String replyTopic = "v1/devices/me/rpc/response/" + reqId;
      mqttClient.publish(replyTopic.c_str(), "{\"status\":\"ok\"}");
      Serial.printf("[WiFi] Perintah valve: %s, override:%d\n", v ? "OPEN" : "CLOSE", manualOverride);
    }
    return;
  }

  /* ===== Shared attribute langsung (jadwal) ===== */
  if (topicStr == "v1/devices/me/attributes") {
    StaticJsonDocument<384> doc;
    if (deserializeJson(doc, payloadStr) != DeserializationError::Ok) return;

    xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
    /* Pecah nilai lama jadi komponen jam:menit, supaya update PARSIAL
       (misal ThingsBoard cuma kirim start_m saja) tidak menghapus field lain */
    uint8_t sh = schedule.startMin / 60, sm = schedule.startMin % 60;
    uint8_t eh = schedule.endMin   / 60, em = schedule.endMin   % 60;

    if (doc.containsKey("enabled")) schedule.enabled = doc["enabled"];
    if (doc.containsKey("start_h")) sh = doc["start_h"];
    if (doc.containsKey("start_m")) sm = doc["start_m"];
    if (doc.containsKey("end_h"))   eh = doc["end_h"];
    if (doc.containsKey("end_m"))   em = doc["end_m"];

    schedule.startMin = (uint16_t)sh * 60 + sm;
    schedule.endMin   = (uint16_t)eh * 60 + em;

    /* ===== Rentang tanggal (opsional) — dikirim widget Date sebagai epoch ms ===== */
    if (doc.containsKey("use_date_range")) schedule.useDateRange = doc["use_date_range"] ? 1 : 0;
    if (doc.containsKey("start_date")) {
      int64_t epochMs = doc["start_date"];
      time_t t = (time_t)(epochMs / 1000);
      struct tm dtinfo; localtime_r(&t, &dtinfo);
      schedule.startDateCode = (uint32_t)(dtinfo.tm_year + 1900) * 10000UL
                              + (uint32_t)(dtinfo.tm_mon + 1) * 100UL
                              + (uint32_t)dtinfo.tm_mday;
    }
    if (doc.containsKey("end_date")) {
      int64_t epochMs = doc["end_date"];
      time_t t = (time_t)(epochMs / 1000);
      struct tm dtinfo; localtime_r(&t, &dtinfo);
      schedule.endDateCode = (uint32_t)(dtinfo.tm_year + 1900) * 10000UL
                            + (uint32_t)(dtinfo.tm_mon + 1) * 100UL
                            + (uint32_t)dtinfo.tm_mday;
    }

    Flash_Save();
    xSemaphoreGiveRecursive(dataMutex);

    Serial.printf("[WiFi] Jadwal diperbarui: enabled=%d start=%02d:%02d end=%02d:%02d | useDate=%d [%lu-%lu]\n",
                  schedule.enabled, sh, sm, eh, em,
                  schedule.useDateRange, (unsigned long)schedule.startDateCode, (unsigned long)schedule.endDateCode);
  }
}

#if USE_AES
/* Koneksi ke Mosquitto lokal — hanya dipakai untuk kirim data sensor
   terenkripsi. Terpisah total dari koneksi RPC/jadwal ke ThingsBoard. */
bool connectLocalMosquitto() {
  if (localMqttClient.connected()) return true;
  localMqttClient.setServer(MOSQUITTO_IP, MOSQUITTO_PORT);

  char clientId[28];
  snprintf(clientId, sizeof(clientId), "ESP32_Node%s_enc", NODE_ID);

  if (localMqttClient.connect(clientId)) {
    Serial.println("[AES] Terhubung ke Mosquitto lokal");
    return true;
  }
  Serial.print("[AES] Gagal connect Mosquitto lokal, rc=");
  Serial.println(localMqttClient.state());
  return false;
}
#endif

bool connectMQTTDirect() {
  if (mqttClient.connected()) return true;
  if (millis() - lastMqttAttempt < 5000) return false; /* jangan spam percobaan */
  lastMqttAttempt = millis();

  if (strlen(tbToken) == 0) {
    Serial.println("[Provisioning] Token belum ada, mencoba provisioning...");
    if (!doProvisioning()) return false;
  }

  mqttClient.setServer(TB_HOST, TB_PORT);
  mqttClient.setCallback(mqttCallbackDirect);

  char clientId[24];
  snprintf(clientId, sizeof(clientId), "ESP32_Node%s", NODE_ID);

  if (mqttClient.connect(clientId, tbToken, NULL)) {
    mqttClient.subscribe("v1/devices/me/rpc/request/+");
    mqttClient.subscribe("v1/devices/me/attributes");
    Serial.println("[WiFi] MQTT langsung ke ThingsBoard: connected!");
    return true;
  }

  int st = mqttClient.state();
  Serial.print("[WiFi] MQTT gagal, rc="); Serial.println(st);
  if (st == 4 || st == 5) { /* token ditolak server -> reset supaya provisioning ulang */
    Serial.println("[Provisioning] Token ditolak, reset untuk provisioning ulang");
    clearTbToken();
  }
  return false;
}

void startWiFiIfNeeded() {
  if (commMode == 1 && !wifiStarted) {
    WiFi.mode(WIFI_STA);
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Mode WiFi");
    lcd.setCursor(0,1); lcd.print("Konek/Portal...");
    wm.setConfigPortalTimeout(180);
    bool ok = wm.autoConnect(AP_PORTAL_NAME, AP_PORTAL_PASS);
    if (ok) {
      wifiStarted = 1;
#if USE_TLS
      secureClient.setInsecure();
#endif

      /* Sinkron waktu langsung via NTP, retry sampai berhasil (jaringan
         kadang belum siap tepat sesaat setelah WiFi baru konek) */
      timeClient.begin();
      bool ntpOk = false;
      for (uint8_t attempt = 0; attempt < 10; attempt++) {
        if (timeClient.forceUpdate()) { ntpOk = true; break; }
        delay(500);
      }

      if (ntpOk) {
        time_t epoch = timeClient.getEpochTime();
        struct timeval tv = { epoch, 0 };
        settimeofday(&tv, NULL);
        needTime = 0;
        Serial.println("[NTP] Sinkron berhasil: " + timeClient.getFormattedTime());
      } else {
        Serial.println("[NTP] GAGAL sinkron setelah 10x percobaan!");
      }

      lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi OK!");
      lcd.setCursor(0,1); lcd.print(ntpOk ? "NTP Sync OK" : "NTP GAGAL!");
      delay(2000);
    } else {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi gagal");
      lcd.setCursor(0,1); lcd.print("coba lagi nanti");
      delay(2000);
    }
    lcd.clear();
  }
}

/* Kirim telemetry langsung ke ThingsBoard (device API biasa) */
void Node_PublishDirect(uint8_t isBacklogEntry, float vol, uint8_t h, uint8_t m, uint8_t s) {
  if (!connectMQTTDirect()) return;

  time_t now; time(&now);
  struct tm *tinfo = localtime(&now);
  char nodeTs[9];
  if (isBacklogEntry) snprintf(nodeTs, sizeof(nodeTs), "%02d:%02d:%02d", h, m, s);
  else snprintf(nodeTs, sizeof(nodeTs), "%02d:%02d:%02d", tinfo->tm_hour, tinfo->tm_min, tinfo->tm_sec);

  StaticJsonDocument<256> doc;
  xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
  doc["flow"]    = isBacklogEntry ? 0.0f : filteredFlow;
  doc["volume"]  = isBacklogEntry ? vol  : totalLiter;
  doc["valve"]   = valveState;
  xSemaphoreGiveRecursive(dataMutex);
  doc["node_ts"] = nodeTs;
  doc["link"]    = "wifi_direct";
  doc["backlog"] = isBacklogEntry ? 1 : 0;

  char jsonBuf[256];
  serializeJson(doc, jsonBuf);

#if USE_AES
  /* Data sensor dienkripsi, dikirim ke Mosquitto lokal (bukan langsung
     ke ThingsBoard) — dibongkar nanti oleh bridge.py sebelum diteruskan.
     RPC/jadwal TIDAK lewat sini, tetap via mqttClient (ThingsBoard) di
     connectMQTTDirect() seperti biasa, tidak terpengaruh sama sekali. */
  if (connectLocalMosquitto()) {
    char encBuf[300];
    if (AES_EncryptToBase64(jsonBuf, encBuf, sizeof(encBuf))) {
      char topic[40];
      snprintf(topic, sizeof(topic), "sensor/encrypted/%s", NODE_ID);
      char sendBuf[320];
      snprintf(sendBuf, sizeof(sendBuf), "ENC:%s", encBuf);
      localMqttClient.publish(topic, sendBuf);
      Serial.println("[AES] Terenkripsi -> Mosquitto: " + String(jsonBuf));
    } else {
      Serial.println("[AES] Enkripsi gagal, data tidak dikirim");
    }
  }
#else
  /* Jalur normal (tanpa AES) — langsung ke ThingsBoard, seperti sebelumnya */
  if (!connectMQTTDirect()) return;
  mqttClient.publish("v1/devices/me/telemetry", jsonBuf);
  Serial.println("[WiFi] Published: " + String(jsonBuf));
#endif

  digitalWrite(LED_PIN, HIGH); delay(100); digitalWrite(LED_PIN, LOW);
}

/* ============================================================
   ===== MODE LORA: TETAP LEWAT GATEWAY (seperti sebelumnya) =====
   ============================================================ */
void LoRa_SendLine(const char *plaintext) {
#if USE_AES
  char encBuf[260]; char sendBuf[280];
  if (!AES_EncryptToBase64(plaintext, encBuf, sizeof(encBuf))) return;
  snprintf(sendBuf, sizeof(sendBuf), "ENC:%s\n", encBuf);
  LoRa_WaitReady();
  LORA_SERIAL.print(sendBuf);
#else
  LoRa_WaitReady();
  LORA_SERIAL.print(plaintext);
#endif
}

void Node_SendData_LoRa() {
  time_t now; time(&now);
  struct tm *tinfo = localtime(&now);

  xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
  float flowCopy = filteredFlow, volCopy = totalLiter;
  uint8_t valveCopy = valveState;
  xSemaphoreGiveRecursive(dataMutex);

  char txBuf[96];
  snprintf(txBuf, sizeof(txBuf),
    "NODE:" NODE_ID ",Q:%.2f,V:%.2f,VALVE:%d,TS:%02d:%02d:%02d,NEEDTIME:%d\r\n",
    flowCopy, volCopy, valveCopy,
    tinfo->tm_hour, tinfo->tm_min, tinfo->tm_sec, needTime);
  LoRa_SendLine(txBuf);
  digitalWrite(LED_PIN, HIGH); delay(100); digitalWrite(LED_PIN, LOW);
}

void Node_SendBacklog_LoRa() {
  xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
  if (backlogCount == 0) { xSemaphoreGiveRecursive(dataMutex); return; }
  BacklogEntry e = backlog[0];
  char txBuf[80];
  snprintf(txBuf, sizeof(txBuf),
    "NODE:" NODE_ID ",BACKLOG:1,V:%.2f,TS:%02d:%02d:%02d\r\n",
    e.volume, e.hour, e.minute, e.sec);
  LoRa_SendLine(txBuf);
  Serial.print("Kirim backlog (LoRa): "); Serial.println(txBuf);

  for (uint8_t i = 0; i < backlogCount - 1; i++) backlog[i] = backlog[i+1];
  backlogCount--;
  Backlog_Save();
  xSemaphoreGiveRecursive(dataMutex);
}

void handleIncomingLine(char *line) {
  char *timePtr = strstr(line, "TIME:");
  if (timePtr != NULL) {
    int h, m, s;
    if (sscanf(timePtr + 5, "%d:%d:%d", &h, &m, &s) == 3 &&
        h>=0&&h<24 && m>=0&&m<60 && s>=0&&s<60) {
      time_t now; time(&now);
      struct tm tinfo; localtime_r(&now, &tinfo);
      tinfo.tm_hour=h; tinfo.tm_min=m; tinfo.tm_sec=s; tinfo.tm_isdst=-1;
      time_t t = mktime(&tinfo);
      struct timeval tv = { t, 0 };
      settimeofday(&tv, NULL);
      needTime = 0;
      Serial.println("Waktu diterima dari gateway!");
    }
    return;
  }

  char *cmdPtr = strstr(line, "CMD:");
  if (cmdPtr != NULL) {
    char nid[3] = {0};
    strncpy(nid, cmdPtr + 4, 2);
    if (strcmp(nid, NODE_ID) != 0) return;

    char *valPtr   = strstr(cmdPtr, "VALVE:");
    char *ovrPtr   = strstr(cmdPtr, "OVERRIDE:");
    char *schedPtr = strstr(cmdPtr, "SCHED:");

    if (schedPtr) {
      int en, sh, sm, eh, em;
      if (sscanf(schedPtr, "SCHED:%d,SH:%d,SM:%d,EH:%d,EM:%d", &en, &sh, &sm, &eh, &em) == 5) {
        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        schedule.enabled  = en;
        schedule.startMin = sh * 60 + sm;
        schedule.endMin   = eh * 60 + em;
        Flash_Save();
        xSemaphoreGiveRecursive(dataMutex);
        Serial.println("Jadwal valve diperbarui!");
      }
      return;
    }

    if (valPtr) {
      int v = 0; sscanf(valPtr, "VALVE:%d", &v);
      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      if (ovrPtr) {
        int ov = 0; sscanf(ovrPtr, "OVERRIDE:%d", &ov);
        manualOverride = ov;
        Flash_Save();
      }
      if (manualOverride) Valve_SetState(v ? 1 : 0);
      else Schedule_Apply();
      xSemaphoreGiveRecursive(dataMutex);
      Serial.printf("Perintah valve: %s, override:%d\n", v ? "OPEN" : "CLOSE", manualOverride);
    }
    return;
  }
}

void checkIncomingLoRa() {
  while (LORA_SERIAL.available()) {
    char c = LORA_SERIAL.read();
    if (c == '\n') {
      rxBuf[rxIdx]='\0'; rxIdx=0;
#if USE_AES
      if (strncmp(rxBuf, "ENC:", 4) == 0) {
        char decBuf[128];
        if (AES_DecryptFromBase64(rxBuf + 4, decBuf, sizeof(decBuf)))
          handleIncomingLine(decBuf);
        else
          Serial.println("SECURITY: gagal dekripsi paket masuk, diabaikan");
      }
#else
      handleIncomingLine(rxBuf);
#endif
    }
    else if (c != '\r') { if (rxIdx < sizeof(rxBuf)-1) rxBuf[rxIdx++]=c; else rxIdx=0; }
  }
}

/* ===== LCD ===== */
void LCD_Update() {
  char buf[17];
  lcd.setCursor(0,0);
  snprintf(buf, sizeof(buf), "Q:%.2f L/min ", filteredFlow);
  lcd.print(buf);
  lcd.setCursor(0,1);
  snprintf(buf, sizeof(buf), "V:%.2f L %s", totalLiter, valveState ? "[O]" : "[C]");
  lcd.print(buf);
}

/* ===== PIN INPUT ===== */
uint8_t PIN_Input() {
  uint8_t pinInput[4] = {0}; uint8_t idx=0, digit=0;
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Masukkan PIN:");
  auto showPin = [&]() {
    lcd.setCursor(0,1);
    for (uint8_t i=0;i<4;i++){ if(i<idx)lcd.print("*"); else if(i==idx)lcd.print(String(digit)); else lcd.print("_"); }
    lcd.print("    ");
  };
  showPin();
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ digit=(digit+1)%10; showPin(); }
    if (Btn_Read(BTN3_PIN)==1){ digit=(digit==0)?9:digit-1; showPin(); }
    if (Btn_Read(BTN4_PIN)==1){
      pinInput[idx]=digit; idx++; digit=0;
      if (idx>=4){
        lcd.clear(); lcd.setCursor(0,0);
        if (memcmp(pinInput,adminPIN,4)==0){ lcd.print("PIN Benar!"); delay(1000); return 1; }
        else { lcd.print("PIN Salah!"); delay(1000); return 0; }
      }
      showPin();
    }
    if (Btn_Read(BTN1_PIN)==1){ if(idx>0){idx--;digit=pinInput[idx];} showPin(); }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return 2; }
  }
}

/* ===== MENU ADMIN ===== */
void Menu_ShowAdmin(uint8_t sel) {
  lcd.clear(); lcd.setCursor(0,0); lcd.print("== Menu Admin ==");
  lcd.setCursor(0,1);
  switch (sel) {
    case 0: lcd.print(">1.Kalibrasi"); break;
    case 1: lcd.print(">2.Set Kirim"); break;
    case 2: lcd.print(">3.Set Jam"); break;
    case 3: lcd.print(">4.Reset Odom"); break;
    case 4: lcd.print(">5.Kontrol Valve"); break;
    case 5: lcd.print(">6.Mode Kirim"); break;
  }
}
uint8_t Menu_Admin() {
  uint8_t sel=0; Menu_ShowAdmin(sel);
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ sel=(sel==0)?5:sel-1; Menu_ShowAdmin(sel); }
    if (Btn_Read(BTN3_PIN)==1){ sel=(sel+1)%6; Menu_ShowAdmin(sel); }
    if (Btn_Read(BTN4_PIN)==1) return sel;
    if (Btn_Read(BTN1_PIN)==2) return 255;
  }
}

/* ===== MODE SELECT + RESET WIFI ===== */
void Menu_ModeSelect() {
  uint8_t mode = commMode;
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Mode: 4=simpan");
  auto show = [&](){ lcd.setCursor(0,1); lcd.print(mode==0 ? "< LoRa > 3L:rst " : "< WiFi > 3L:rst "); };
  show();
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ mode=!mode; show(); }
    if (Btn_Read(BTN3_PIN)==2){
      wm.resetSettings();
      wifiStarted = 0;
      lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi direset!");
      lcd.setCursor(0,1); lcd.print("Setup ulang nanti");
      delay(2000);
      show();
    }
    if (Btn_Read(BTN4_PIN)==1){
      commMode=mode; Flash_Save();
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Mode Tersimpan!");
      lcd.setCursor(0,1); lcd.print(commMode==0 ? "Mode: LoRa" : "Mode: WiFi");
      delay(1500);
      startWiFiIfNeeded();
      return;
    }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return; }
  }
}

/* ===== SETTING KIRIM (per 15 menit) ===== */
void Menu_SettingKirim() {
  uint8_t step = sendIntervalMs / 900000UL; if (step<1) step=1;
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Interval Kirim:");
  auto show=[&](){ lcd.setCursor(0,1); char b[17]; snprintf(b,sizeof(b),"< %3d menit >",step*15); lcd.print(b); lcd.print(" "); };
  show();
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ step=(step>=96)?1:step+1; show(); }
    if (Btn_Read(BTN3_PIN)==1){ step=(step<=1)?96:step-1; show(); }
    if (Btn_Read(BTN4_PIN)==1){
      sendIntervalMs=(uint32_t)step*900000UL; Flash_Save();
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Tersimpan!"); delay(1500); return;
    }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return; }
  }
}

/* ===== SETTING JAM ===== */
void Menu_SettingJam() {
  time_t now; time(&now);
  struct tm tinfo; localtime_r(&now,&tinfo);
  uint8_t h=tinfo.tm_hour,m=tinfo.tm_min,s=tinfo.tm_sec; uint8_t cur=0;
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Setting Jam:");
  auto show=[&](){ lcd.setCursor(0,1); char b[17]; snprintf(b,sizeof(b),"%c%02d:%02d:%02d%c",cur==0?'[':' ',h,m,s,cur==2?']':' '); lcd.print(b); lcd.print("   "); };
  show();
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ if(cur==0)h=(h+1)%24; else if(cur==1)m=(m+1)%60; else s=(s+1)%60; show(); }
    if (Btn_Read(BTN3_PIN)==1){ if(cur==0)h=(h==0)?23:h-1; else if(cur==1)m=(m==0)?59:m-1; else s=(s==0)?59:s-1; show(); }
    if (Btn_Read(BTN4_PIN)==1){
      if (cur<2){ cur++; show(); }
      else {
        struct tm nt=tinfo; nt.tm_hour=h; nt.tm_min=m; nt.tm_sec=s;
        time_t t=mktime(&nt); struct timeval tv={t,0}; settimeofday(&tv,NULL);
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Waktu Tersimpan!"); delay(1500); return;
      }
    }
    if (Btn_Read(BTN1_PIN)==1){ if(cur>0){cur--;show();} }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return; }
  }
}

/* ===== KONTROL VALVE MANUAL (otomatis set override) ===== */
void Menu_KontrolValve() {
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Kontrol Valve:");
  lcd.setCursor(0,1); lcd.print(valveState ? "Status: OPEN " : "Status: CLOSE");
  while (1) {
    if (Btn_Read(BTN4_PIN)==1){
      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      manualOverride = 1;
      Valve_SetState(!valveState);
      Flash_Save();
      xSemaphoreGiveRecursive(dataMutex);
      lcd.setCursor(0,1); lcd.print(valveState ? "Status: OPEN " : "Status: CLOSE");
    }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return; }
  }
}

/* ===== KALIBRASI ===== */
void Menu_ShowKalibrasi(uint8_t sel) {
  lcd.clear(); lcd.setCursor(0,0); lcd.print("== Kalibrasi ==");
  lcd.setCursor(0,1);
  const char *label; uint8_t done;
  switch (sel){ case 0:label="Pelan";done=kalib.pelanDone;break; case 1:label="Sedang";done=kalib.sedangDone;break; default:label="Cepat";done=kalib.cepatDone;break; }
  char b[17]; snprintf(b,sizeof(b),">%-8s [%s]",label,done?"OK":"--"); lcd.print(b);
}
uint8_t Menu_Kalibrasi() {
  uint8_t sel=0; Menu_ShowKalibrasi(sel);
  while (1) {
    if (Btn_Read(BTN2_PIN)==1){ sel=(sel==0)?2:sel-1; Menu_ShowKalibrasi(sel); }
    if (Btn_Read(BTN3_PIN)==1){ sel=(sel+1)%3; Menu_ShowKalibrasi(sel); }
    if (Btn_Read(BTN4_PIN)==1) return sel;
    if (Btn_Read(BTN1_PIN)==2) return 255;
  }
}
void Menu_KalibrasiUkur(uint8_t titik) {
  const char *label; switch(titik){case 0:label="Pelan";break;case 1:label="Sedang";break;default:label="Cepat";break;}
  lcd.clear(); lcd.setCursor(0,0); char b[17]; snprintf(b,sizeof(b),"Kalib: %s",label); lcd.print(b);
  uint8_t refDigit[3]={0,0,0}; uint8_t refIdx=0; float pulsaMentah=0.0f; uint32_t lastCalc=millis();
  while (1) {
    if (millis()-lastCalc>=1000){ lastCalc=millis(); noInterrupts(); uint32_t pc=pulseCount; pulseCount=0; interrupts(); pulsaMentah=(float)pc; }
    lcd.setCursor(0,1); snprintf(b,sizeof(b),"P:%-5.1f R:%d%d.%d ",pulsaMentah,refDigit[0],refDigit[1],refDigit[2]); lcd.print(b);
    if (Btn_Read(BTN2_PIN)==1) refDigit[refIdx]=(refDigit[refIdx]+1)%10;
    if (Btn_Read(BTN3_PIN)==1) refDigit[refIdx]=(refDigit[refIdx]==0)?9:refDigit[refIdx]-1;
    if (Btn_Read(BTN4_PIN)==1) refIdx=(refIdx+1)%3;
    if (Btn_Read(BTN4_PIN)==2){
      float refValue=(refDigit[0]*10)+refDigit[1]+(refDigit[2]/10.0f);
      switch(titik){
        case 0: kalib.pelanPulsa=pulsaMentah; kalib.pelanRef=refValue; kalib.pelanDone=1; break;
        case 1: kalib.sedangPulsa=pulsaMentah; kalib.sedangRef=refValue; kalib.sedangDone=1; break;
        default: kalib.cepatPulsa=pulsaMentah; kalib.cepatRef=refValue; kalib.cepatDone=1; break;
      }
      if (kalib.pelanDone && kalib.sedangDone && kalib.cepatDone){
        float f1=(kalib.pelanPulsa>0)?(kalib.pelanRef/kalib.pelanPulsa):0;
        float f2=(kalib.sedangPulsa>0)?(kalib.sedangRef/kalib.sedangPulsa):0;
        float f3=(kalib.cepatPulsa>0)?(kalib.cepatRef/kalib.cepatPulsa):0;
        uint8_t cnt=(f1>0)+(f2>0)+(f3>0);
        faktorKalibrasi=(cnt>0)?((f1+f2+f3)/cnt):(1.0f/7.5f);
      }
      Flash_Save();
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Titik Tersimpan!");
      lcd.setCursor(0,1); snprintf(b,sizeof(b),"Ref: %.1f L/min",refValue); lcd.print(b);
      delay(2000); return;
    }
    if (Btn_Read(BTN1_PIN)==2){ lcd.clear(); return; }
  }
}

/* ===== SETUP ===== */
/* ============================================================
   ===== NETWORK TASK — jalan di CORE TERPISAH (core 0) =====
   Semua urusan WiFi/MQTT/LoRa/kirim data ada di sini, supaya LCD dan
   sensor di loop() utama (core 1) TIDAK PERNAH nunggu jaringan lambat.
   ============================================================ */
void NetworkTask(void *pvParameters) {
  for (;;) {
    if (commMode == 0) {
      checkIncomingLoRa();
    } else {
      if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) connectMQTTDirect();
        mqttClient.loop();
        timeClient.update();
#if USE_AES
        if (!localMqttClient.connected()) connectLocalMosquitto();
        localMqttClient.loop();
#endif
      }
    }

    if (sysState == STATE_NORMAL) {
      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      float flowNow = filteredFlow;
      xSemaphoreGiveRecursive(dataMutex);

      uint32_t effectiveInterval = (flowNow > FLOW_THRESHOLD) ? FAST_SEND_INTERVAL_MS : sendIntervalMs;

      if (millis() - lastSendTick >= effectiveInterval) {
        lastSendTick = millis();
        if (commMode == 0) Node_SendData_LoRa();
        else Node_PublishDirect(0, 0, 0, 0, 0);
      }

      static uint32_t lastBacklogTry = 0;
      if (millis() - lastBacklogTry >= sendIntervalMs) {
        lastBacklogTry = millis();
        xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
        uint8_t hasBacklog = (backlogCount > 0);
        BacklogEntry e = hasBacklog ? backlog[0] : BacklogEntry{0,0,0,0};
        xSemaphoreGiveRecursive(dataMutex);

        if (hasBacklog) {
          if (commMode == 0) {
            Node_SendBacklog_LoRa();
          } else if (mqttClient.connected()) {
            Node_PublishDirect(1, e.volume, e.hour, e.minute, e.sec);
            xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
            for (uint8_t i = 0; i < backlogCount - 1; i++) backlog[i] = backlog[i+1];
            backlogCount--;
            Backlog_Save();
            xSemaphoreGiveRecursive(dataMutex);
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10)); /* kasih jatah waktu ke task lain */
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  pinMode(LORA_M0_PIN, OUTPUT); pinMode(LORA_M1_PIN, OUTPUT); pinMode(LORA_AUX_PIN, INPUT_PULLUP);
  pinMode(BTN1_PIN, INPUT_PULLUP); pinMode(BTN2_PIN, INPUT_PULLUP);
  pinMode(BTN3_PIN, INPUT_PULLUP); pinMode(BTN4_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT); pinMode(LED_PIN, OUTPUT);

  Valve_SetState(0);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, FALLING);
  LORA_SERIAL.begin(9600, SERIAL_8N1, 16, 17);
  LoRa_SetMode(0, 0);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  lcd.setCursor(0,0); lcd.print("PAMSIMAS Node");
  lcd.setCursor(0,1); lcd.print("Initializing...");
  delay(1500); lcd.clear();

  struct timeval tv = { 0, 0 }; settimeofday(&tv, NULL);
  randomSeed(analogRead(35));

  Flash_Load();
  Backlog_Load();
  loadTbToken();

  dataMutex = xSemaphoreCreateRecursiveMutex();

  startWiFiIfNeeded();

  lastSendTick = millis();

  /* Task jaringan jalan di CORE 0, loop() utama (LCD/sensor/menu) di CORE 1 */
  xTaskCreatePinnedToCore(NetworkTask, "NetworkTask", 8192, NULL, 1, &networkTaskHandle, 0);
}

/* ===== LOOP (CORE 1) — murni sensor + LCD + menu, TIDAK PERNAH nunggu jaringan ===== */
void loop() {
  if (sysState == STATE_NORMAL) {
#if USE_FAKE_SENSOR
    static uint32_t lastFake = 0;
    if (millis() - lastFake >= 5000) {
      lastFake = millis();
      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      fakeFlowValue += 2.0f;
      if (fakeFlowValue > 40.0f) fakeFlowValue = 0.0f;
      filteredFlow = fakeFlowValue;
      totalLiter += (filteredFlow / 60.0f) * 5.0f;
      xSemaphoreGiveRecursive(dataMutex);
    }
#else
    static uint32_t lastCalc = 0;
    if (millis() - lastCalc >= 1000) {
      lastCalc = millis();
      noInterrupts(); uint32_t pc = pulseCount; pulseCount = 0; interrupts();
      float rawFlow = (float)pc * faktorKalibrasi;
      xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY);
      filteredFlow = (0.7f * filteredFlow) + (0.3f * rawFlow);
      totalLiter += (filteredFlow / 60.0f);
      xSemaphoreGiveRecursive(dataMutex);
    }
#endif

    LCD_Update();
    Flash_CheckAndSave();
    Schedule_Apply();

    if (Btn_Read(BTN1_PIN) == 2) { if (PIN_Input() == 1) sysState = STATE_MENU_ADMIN; }
  }

  if (sysState == STATE_MENU_ADMIN) {
    uint8_t p = Menu_Admin();
    if (p==0) sysState=STATE_KALIBRASI;
    else if (p==1) sysState=STATE_SETTING_KIRIM;
    else if (p==2) sysState=STATE_SETTING_JAM;
    else if (p==3) sysState=STATE_RESET_ODOMETER;
    else if (p==4) sysState=STATE_KONTROL_VALVE;
    else if (p==5) sysState=STATE_MODE_SELECT;
    else sysState=STATE_NORMAL;
  }
  if (sysState == STATE_KALIBRASI) {
    uint8_t t = Menu_Kalibrasi();
    if (t==255) sysState=STATE_MENU_ADMIN;
    else { sysState=STATE_KALIBRASI_UKUR; kalibSelectedPoint=t; }
  }
  if (sysState == STATE_KALIBRASI_UKUR) { Menu_KalibrasiUkur(kalibSelectedPoint); sysState=STATE_KALIBRASI; }
  if (sysState == STATE_SETTING_KIRIM) { Menu_SettingKirim(); sysState=STATE_MENU_ADMIN; }
  if (sysState == STATE_SETTING_JAM) { Menu_SettingJam(); sysState=STATE_MENU_ADMIN; }
  if (sysState == STATE_RESET_ODOMETER) {
    totalLiter = 0.0f; Flash_Save();
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Odometer Reset!");
    lcd.setCursor(0,1); lcd.print("V: 0.00 L"); delay(1500); lcd.clear();
    sysState=STATE_MENU_ADMIN;
  }
  if (sysState == STATE_KONTROL_VALVE) { Menu_KontrolValve(); sysState=STATE_MENU_ADMIN; }
  if (sysState == STATE_MODE_SELECT) { Menu_ModeSelect(); sysState=STATE_MENU_ADMIN; }
}
