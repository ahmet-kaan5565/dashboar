/*
 * IoT Dashboard düğümü — ESP-01 / ESP-01S (1MB flash)
 *
 * esp8266_node ile aynı mantık, ESP-01'e göre kısıtlanmış:
 *   - Sadece GPIO0 ve GPIO2 serbest -> DHT22 DATA = GPIO2 (10k pull-up ile 3.3V'a)
 *   - Yerleşik LED yok (TX LED'i GPIO1, Serial ile çakışır) -> blink yok
 *   - ArduinoOTA varsayılan KAPALI (mDNS flash yer, 1MB'ta OTA için ~500KB sınırı var).
 *     Güncelleme İNTERNET OTA ile: GitHub'daki firmware/esp01.json + esp01.bin
 *     (+ dashboard'daki "Güncelle" butonu hemen tetikler)
 *
 * !! ÖNEMLİ: OTA için 1MB flash GEREKİR. Eski siyah ESP-01'lerin çoğu 512KB'tır -> OTA ÇALIŞMAZ,
 *    sadece kabloyla yüklersin. ESP-01S (mavi) genelde 1MB.
 *
 * Arduino IDE ayarı: Kart: "Generic ESP8266 Module"
 *   Flash Size: "1MB (FS:none OTA:~502KB)"     <- OTA için şart
 *   Flash Mode: DOUT (ESP-01'lerde QIO sorun çıkarabilir)
 *   Yüklerken GPIO0'ı GND'ye çek (flash modu), sonra çıkar.
 *
 * Gerekli kütüphaneler: "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#define USE_ARDUINO_OTA 0   // 1 yaparsan LAN OTA da açılır (flash'a ~40KB ekler, 1MB'ta sığmayabilir)
#if USE_ARDUINO_OTA
#include <ArduinoOTA.h>
#endif
#include <time.h>
#include <DHT.h>

#include "secrets.h"   // WIFI_SSID, WIFI_PASS, OTA_PASS  (secrets.example.h'den kopyala)

// ======================= AYARLAR =======================
#define DEVICE_ID        "esp01-1"       // Firestore'da cihaz adı (dashboard'da görünür)
#define FW_VERSION       1               // Her yeni .bin için ARTIR, esp01.json'daki "version" ile aynı mantık

#define FB_PROJECT       "dashboardveritabani"
#define FB_API_KEY       "AIzaSyAaSlbKLJxQIqGzaUXsIt8RwGM8XM9fg7w"

#define OTA_JSON_URL     "https://raw.githubusercontent.com/ahmet-kaan5565/dashboar/main/firmware/esp01.json"

#define DHT_PIN          2               // ESP-01: GPIO2
#define DHT_TYPE         DHT22           // DHT11 ise DHT11 yap

// Firestore ücretsiz kota: 20.000 yazma/gün. 30sn canlı = 2880/gün, 5dk geçmiş = 288/gün (cihaz başına)
const unsigned long SEND_INTERVAL_MS  = 30UL * 1000;
const unsigned long HIST_INTERVAL_MS  = 5UL * 60 * 1000;
const unsigned long CMD_INTERVAL_MS   = 60UL * 1000;        // dashboard'dan "güncelle" komutu kontrolü
const unsigned long OTA_CHECK_MS      = 30UL * 60 * 1000;   // otomatik sürüm kontrolü
// =======================================================

DHT dht(DHT_PIN, DHT_TYPE);

unsigned long lastSend = 0, lastHist = 0, lastCmd = 0, lastOta = 0, wifiLostAt = 0;
bool firstRun = true;
int failCount = 0;

// ---------- Yardımcılar ----------
bool timeValid() { return time(nullptr) > 1700000000; }

String tsMs() { return String((unsigned long)time(nullptr)) + "000"; }

// ESP-01'de kullanılabilir LED pini yok (GPIO1 = TX).
void blink() {}

// Firestore REST çağrısı. Dönüş: HTTP kodu (<=0 = bağlantı hatası)
int fsRequest(const char* method, const String& path, const String& query,
              const String& body, String* out = nullptr) {
  BearSSL::WiFiClientSecure client;
  client.setInsecure();   // bkz. README notu: sertifika doğrulanmıyor
  HTTPClient http;
  String url = String("https://firestore.googleapis.com/v1/projects/") + FB_PROJECT +
               "/databases/(default)/documents/" + path + "?key=" + FB_API_KEY + query;
  if (!http.begin(client, url)) return -1;
  http.useHTTP10(true);          // chunked cevap yok, parse kolay
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (out && code > 0) *out = http.getString();
  http.end();
  return code;
}

String fStr(const char* k, const String& v) { return String("\"") + k + "\":{\"stringValue\":\"" + v + "\"}"; }
String fInt(const char* k, const String& v) { return String("\"") + k + "\":{\"integerValue\":\"" + v + "\"}"; }
String fDbl(const char* k, float v)         { return String("\"") + k + "\":{\"doubleValue\":" + String(v, 1) + "}"; }
String fBool(const char* k, bool v)         { return String("\"") + k + "\":{\"booleanValue\":" + (v ? "true" : "false") + "}"; }

// ---------- Firestore: cihaz kaydı (parent doküman) ----------
// Alt koleksiyonu olan ama kendisi olmayan doküman "hayalet"tir ve collection listesinde görünmez.
// Dashboard cihazı bulsun diye parent dokümanı burada yazıyoruz. cfg alanına dokunmuyoruz (updateMask).
bool registerDevice() {
  String body = String("{\"fields\":{") +
    fStr("tip", "esp01") + "," +
    fInt("fw", String(FW_VERSION)) + "," +
    fStr("ip", WiFi.localIP().toString()) + "," +
    fInt("rssi", String(WiFi.RSSI())) + "," +
    fInt("boot", timeValid() ? tsMs() : String("0")) + "}}";
  String q = "&updateMask.fieldPaths=tip&updateMask.fieldPaths=fw&updateMask.fieldPaths=ip"
             "&updateMask.fieldPaths=rssi&updateMask.fieldPaths=boot";
  int code = fsRequest("PATCH", String("cihazlar/") + DEVICE_ID, q, body);
  Serial.printf("[FS] register -> %d\n", code);
  return code == 200;
}

// ---------- Sensör okuma ----------
void readSensor(float& t, float& h) {
  t = dht.readTemperature();
  h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {          // DHT bazen ilk okumada NaN verir
    delay(2100);
    t = dht.readTemperature();
    h = dht.readHumidity();
  }
}

// ---------- Firestore: canlı veri ----------
bool sendLive(float t, float h) {
  time_t now = time(nullptr);
  struct tm tmv; localtime_r(&now, &tmv);
  char saat[12], tarih[16];
  strftime(saat,  sizeof saat,  "%H:%M:%S", &tmv);
  strftime(tarih, sizeof tarih, "%d.%m.%Y", &tmv);

  String f = "";
  if (!isnan(t)) f += fDbl("sicaklik", t) + ",";
  if (!isnan(h)) f += fDbl("nem", h) + ",";
  f += fStr("saat", saat) + "," + fStr("tarih", tarih) + "," +
       fStr("durum", "online") + "," + fInt("ts", tsMs());

  int code = fsRequest("PATCH", String("cihazlar/") + DEVICE_ID + "/canli/veri", "",
                       String("{\"fields\":{") + f + "}}");
  Serial.printf("[FS] canli -> %d\n", code);
  return code == 200;
}

// ---------- Firestore: geçmiş kaydı ----------
bool sendHistory(float t, float h) {
  if (isnan(t) && isnan(h)) return false;
  String f = "";
  if (!isnan(t)) f += fDbl("sicaklik", t) + ",";
  if (!isnan(h)) f += fDbl("nem", h) + ",";
  f += fInt("ts", tsMs());
  int code = fsRequest("POST", String("cihazlar/") + DEVICE_ID + "/gecmis", "",
                       String("{\"fields\":{") + f + "}}");
  Serial.printf("[FS] gecmis -> %d\n", code);
  return code == 200;
}

// ---------- Dashboard'dan "güncelle" komutu ----------
bool otaRequested() {
  String resp;
  int code = fsRequest("GET", String("cihazlar/") + DEVICE_ID, "&mask.fieldPaths=otaIste", "", &resp);
  if (code != 200) return false;
  int i = resp.indexOf("otaIste");
  if (i < 0) return false;
  int j = resp.indexOf("booleanValue", i);
  if (j < 0) return false;
  return resp.substring(j, min((int)resp.length(), j + 24)).indexOf("true") >= 0;
}

void clearOtaFlag() {
  fsRequest("PATCH", String("cihazlar/") + DEVICE_ID, "&updateMask.fieldPaths=otaIste",
            String("{\"fields\":{") + fBool("otaIste", false) + "}}");
}

// ---------- İnternet OTA ----------
// esp01.json örneği: {"version": 2, "url": "https://raw.githubusercontent.com/.../esp01.bin"}
void checkOta() {
  String json;
  {
    BearSSL::WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(OTA_JSON_URL) + "?t=" + millis())) return;  // ?t= CDN önbelleğini atlatır
    http.useHTTP10(true);
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) json = http.getString();
    http.end();
    if (code != 200) { Serial.printf("[OTA] json hata %d\n", code); return; }
  }

  int vi = json.indexOf("\"version\"");
  int ui = json.indexOf("\"url\"");
  if (vi < 0 || ui < 0) { Serial.println("[OTA] json bozuk"); return; }
  int remoteVer = json.substring(json.indexOf(':', vi) + 1).toInt();
  int q1 = json.indexOf('"', json.indexOf(':', ui) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) return;
  String binUrl = json.substring(q1 + 1, q2);

  Serial.printf("[OTA] mevcut=%d uzak=%d\n", FW_VERSION, remoteVer);
  if (remoteVer <= FW_VERSION) return;

  Serial.println("[OTA] yeni sürüm indiriliyor: " + binUrl);
  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = ESPhttpUpdate.update(client, binUrl);
  if (r == HTTP_UPDATE_FAILED)
    Serial.printf("[OTA] HATA (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
  else if (r == HTTP_UPDATE_NO_UPDATES)
    Serial.println("[OTA] güncelleme yok");
}

// ---------- WiFi / NTP ----------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(DEVICE_ID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] bağlanıyor");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());
  else Serial.println("\n[WiFi] bağlanamadı, loop içinde denemeye devam");
}

void syncTime() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");  // TR = UTC+3, yaz saati yok
  unsigned long t0 = millis();
  while (!timeValid() && millis() - t0 < 20000) { delay(250); }
  Serial.println(timeValid() ? "[NTP] saat alındı" : "[NTP] saat alınamadı");
}

#if USE_ARDUINO_OTA
void setupArduinoOta() {
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]()  { Serial.println("[ArduinoOTA] başladı"); });
  ArduinoOTA.onEnd([]()    { Serial.println("\n[ArduinoOTA] bitti"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ArduinoOTA] hata %u\n", e); });
  ArduinoOTA.begin();
}
#endif

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n\n== %s fw v%d ==\n", DEVICE_ID, FW_VERSION);
  dht.begin();

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
#if USE_ARDUINO_OTA
    setupArduinoOta();
#endif
    if (timeValid()) registerDevice();
  }
}

void loop() {
#if USE_ARDUINO_OTA
  ArduinoOTA.handle();
#endif

  // WiFi koptuysa: otomatik yeniden bağlanma devrede; çok uzun sürerse yeniden başlat
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiLostAt == 0) wifiLostAt = millis();
    if (millis() - wifiLostAt > 5UL * 60 * 1000) ESP.restart();
    delay(500);
    return;
  }
  if (wifiLostAt != 0) { wifiLostAt = 0; syncTime(); registerDevice(); }
  if (!timeValid()) { syncTime(); return; }

  unsigned long now = millis();

  if (firstRun || now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    float t, h;
    readSensor(t, h);
    Serial.printf("[DHT] t=%.1f h=%.1f\n", t, h);
    bool ok = sendLive(t, h);
    blink();

    if (firstRun || now - lastHist >= HIST_INTERVAL_MS) {
      lastHist = now;
      sendHistory(t, h);
    }
    failCount = ok ? 0 : failCount + 1;
    if (failCount >= 20) ESP.restart();   // 10 dk boyunca hiç yazılamadıysa yeniden başla
  }

  if (now - lastCmd >= CMD_INTERVAL_MS) {
    lastCmd = now;
    if (otaRequested()) {
      Serial.println("[CMD] dashboard güncelleme istedi");
      clearOtaFlag();
      checkOta();
    }
  }

  if (!firstRun && now - lastOta >= OTA_CHECK_MS) {
    lastOta = now;
    checkOta();
  }
  if (firstRun) { lastOta = now; lastCmd = now; }

  firstRun = false;
  delay(10);
}
