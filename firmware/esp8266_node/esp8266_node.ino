/*
 * IoT Dashboard düğümü — ESP8266 (NodeMCU / Wemos D1 mini / genel 4MB kart)
 *
 * GÜVENLİK MODELİ (repo public olduğu için):
 *   - Koda/binary'ye HİÇBİR şifre gömülü değil. WiFi şifresi ve cihaz şifresi cihazın flash'ında (EEPROM)
 *     durur; Seri Monitör'den bir kez girilir (aşağıya bak). Bu yüzden .bin dosyası public repoda durabilir.
 *   - Cihaz Firestore'a anonim değil, KENDİ Firebase Auth hesabıyla (id@cihaz.example.com) yazar.
 *     Firestore kuralları (firestore.rules) sadece dashboard sahibine ve ilgili cihaza izin verir.
 *   - Google sunucularına TLS sertifika doğrulamasıyla bağlanır (VERIFY_TLS).
 *
 * İLK KURULUM (cihaz başına bir kez, kabloyla):
 *   1. Firebase Console > Authentication > Users > "Add user":  e-posta = <id>@cihaz.example.com , şifre = sen seç
 *   2. Bu sketch'i yükle, Seri Monitörü aç: 115200 baud, satır sonu = "Newline"
 *   3. Ayar modunda şunları yaz (her satırdan sonra Enter):
 *        ssid=WIFI_ADIN
 *        pass=WIFI_SIFREN
 *        id=esp8266-1            (küçük harf/rakam/tire; Firebase'deki e-postanın @'ten önceki kısmı)
 *        dpass=1.adimda_sectigin_sifre
 *        save
 *   Sonra ayar değiştirmek için: açılışta 3 sn içinde Seri Monitöre herhangi bir şey yazıp Enter.
 *
 * Ne yapar: NTP (TR saati), DHT22 okuma, Firestore'a canlı veri + geçmiş, kablosuz güncelleme:
 *   1) ArduinoOTA (aynı WiFi'da, Arduino IDE'den ağ portu; şifre = dpass)
 *   2) İnternet OTA: GitHub'daki firmware/esp8266.json'a bakar, sürüm büyükse .bin'i indirir
 *      (+ dashboard'daki "Güncelle" butonu hemen tetikler)
 * NTP için ek kütüphane GEREKMEZ: configTime() ESP8266 çekirdeğinin içinde (SNTP).
 *
 * Gerekli kütüphaneler (Library Manager): "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
 * Kart: NodeMCU 1.0 / LOLIN(WEMOS) D1 mini ; Flash Size: 4MB (FS:2MB OTA:~1019KB) veya (FS:none ...)
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#include <DHT.h>
#include "google_root.h"

// ======================= AYARLAR (gizli olmayan) =======================
#define FW_VERSION       2               // Her yeni .bin için ARTIR; esp8266.json'daki "version" ile birlikte

#define FB_PROJECT       "dashboardveritabani"
#define FB_API_KEY       "AIzaSyAaSlbKLJxQIqGzaUXsIt8RwGM8XM9fg7w"   // Firebase web anahtarı gizli değildir; koruma kurallarda + Auth'ta
#define DEVICE_DOMAIN    "cihaz.example.com"

#define OTA_JSON_URL     "https://raw.githubusercontent.com/ahmet-kaan5565/dashboar/main/firmware/esp8266.json"
#define AUTO_OTA_CHECK   1               // 0 = sadece dashboard'daki Güncelle butonuyla
#define VERIFY_TLS       1               // Google'a sertifika doğrulamalı bağlan. Seri'de TLS hatası görürsen geçici 0 yap.

#define DHT_PIN          4               // NodeMCU/D1 mini: D2 = GPIO4
#define DHT_TYPE         DHT22           // DHT11 ise DHT11 yap

// Firestore ücretsiz kota: 20.000 yazma/gün. 30sn canlı = 2880/gün, 5dk geçmiş = 288/gün (cihaz başına)
const unsigned long SEND_INTERVAL_MS  = 30UL * 1000;
const unsigned long HIST_INTERVAL_MS  = 5UL * 60 * 1000;
const unsigned long CMD_INTERVAL_MS   = 60UL * 1000;        // dashboard'dan "güncelle" komutu kontrolü
const unsigned long OTA_CHECK_MS      = 30UL * 60 * 1000;   // otomatik sürüm kontrolü
const unsigned long TOKEN_LIFE_MS     = 50UL * 60 * 1000;   // Firebase ID token 60 dk geçerli
// ========================================================================

// ---------- Cihazın flash'ında saklanan ayarlar ----------
struct Cfg {
  uint32_t magic;
  char ssid[33];
  char pass[64];
  char id[24];
  char dpass[48];
};
const uint32_t CFG_MAGIC = 0x49304432;
Cfg cfg;

DHT dht(DHT_PIN, DHT_TYPE);
BearSSL::X509List gRoots;

String idToken;
unsigned long tokenAt = 0;
unsigned long lastSend = 0, lastHist = 0, lastCmd = 0, lastOta = 0, wifiLostAt = 0;
bool firstRun = true;
int failCount = 0;

// ---------- Ayarlar (EEPROM) ----------
bool loadCfg() {
  EEPROM.begin(sizeof(Cfg));
  EEPROM.get(0, cfg);
  cfg.ssid[sizeof cfg.ssid - 1] = 0;  cfg.pass[sizeof cfg.pass - 1] = 0;
  cfg.id[sizeof cfg.id - 1] = 0;      cfg.dpass[sizeof cfg.dpass - 1] = 0;
  return cfg.magic == CFG_MAGIC && cfg.ssid[0] && cfg.id[0] && cfg.dpass[0];
}

void saveCfg() {
  cfg.magic = CFG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// id ve dpass JSON/e-posta içine girer: sıkı doğrula
bool validId(const String& v) {
  if (v.length() < 3 || v.length() >= sizeof cfg.id) return false;
  for (unsigned i = 0; i < v.length(); i++) {
    char c = v[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}
bool validJsonSafe(const String& v, size_t maxLen) {
  if (v.length() < 6 || v.length() >= maxLen) return false;
  for (unsigned i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c == '"' || c == '\\' || c < 32) return false;
  }
  return true;
}

void handleCmd(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (line.startsWith("ssid=")) {
    String v = line.substring(5);
    if (v.length() == 0 || v.length() >= sizeof cfg.ssid) { Serial.println("HATA: ssid 1-32 karakter"); return; }
    strlcpy(cfg.ssid, v.c_str(), sizeof cfg.ssid); Serial.println("ok");
  } else if (line.startsWith("pass=")) {
    String v = line.substring(5);
    if (v.length() >= sizeof cfg.pass) { Serial.println("HATA: pass en fazla 63 karakter"); return; }
    strlcpy(cfg.pass, v.c_str(), sizeof cfg.pass); Serial.println("ok");
  } else if (line.startsWith("id=")) {
    String v = line.substring(3);
    if (!validId(v)) { Serial.println("HATA: id 3-23 karakter, sadece a-z 0-9 -"); return; }
    strlcpy(cfg.id, v.c_str(), sizeof cfg.id); Serial.println("ok");
  } else if (line.startsWith("dpass=")) {
    String v = line.substring(6);
    if (!validJsonSafe(v, sizeof cfg.dpass)) { Serial.println("HATA: dpass 6-47 karakter, \" ve \\ olmasın"); return; }
    strlcpy(cfg.dpass, v.c_str(), sizeof cfg.dpass); Serial.println("ok");
  } else if (line == "show") {
    Serial.printf("ssid=%s\npass=(%u karakter)\nid=%s\ndpass=(%u karakter)\n",
                  cfg.ssid, (unsigned)strlen(cfg.pass), cfg.id, (unsigned)strlen(cfg.dpass));
  } else if (line == "save") {
    if (!cfg.ssid[0] || !cfg.id[0] || !cfg.dpass[0]) { Serial.println("HATA: ssid, id, dpass dolu olmalı"); return; }
    saveCfg();
    Serial.println("Kaydedildi, yeniden başlıyor...");
    delay(300);
    ESP.restart();
  } else if (line == "exit") {
    ESP.restart();
  } else {
    Serial.println("Bilinmeyen komut. ssid= pass= id= dpass= show save exit");
  }
}

void configMode(bool firstTime) {
  if (firstTime) memset(&cfg, 0, sizeof cfg);
  Serial.println();
  Serial.println(F("=== AYAR MODU ==="));
  Serial.println(F("Komutlar: ssid=...  pass=...  id=...  dpass=...  show  save  exit"));
  String line;
  unsigned long lastHint = 0;
  for (;;) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { handleCmd(line); line = ""; }
      else if (line.length() < 120) line += c;
    }
    if (firstTime && millis() - lastHint > 10000) {
      lastHint = millis();
      Serial.println(F("(Ayar yok. ssid= pass= id= dpass= yazıp save de. Seri Monitör: Newline)"));
    }
    delay(5);
  }
}

bool askConfig(unsigned long ms) {
  Serial.printf("Ayar modu için %lu sn içinde bir şey yazıp Enter'a bas...\n", ms / 1000);
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (Serial.available()) { while (Serial.available()) Serial.read(); return true; }
    delay(20);
  }
  return false;
}

// ---------- Yardımcılar ----------
bool timeValid() { return time(nullptr) > 1700000000; }

String tsMs() { return String((unsigned long)time(nullptr)) + "000"; }

void blink() {
  digitalWrite(LED_BUILTIN, LOW);  delay(40);
  digitalWrite(LED_BUILTIN, HIGH);
}

String jsonStr(const String& j, const char* key) {
  String k = String("\"") + key + "\"";
  int i = j.indexOf(k);            if (i < 0) return "";
  i = j.indexOf(':', i + k.length()); if (i < 0) return "";
  int a = j.indexOf('"', i + 1);   if (a < 0) return "";
  int b = j.indexOf('"', a + 1);   if (b < 0) return "";
  return j.substring(a + 1, b);
}

// Google sunucuları için TLS ayarı
void tlsSetup(BearSSL::WiFiClientSecure& c) {
#if VERIFY_TLS
  c.setTrustAnchors(&gRoots);
  c.setX509Time(time(nullptr));
#else
  c.setInsecure();
#endif
}

// ---------- Firebase Auth (cihaz hesabı) ----------
bool signIn() {
  BearSSL::WiFiClientSecure client;
  tlsSetup(client);
  HTTPClient http;
  if (!http.begin(client, "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" FB_API_KEY)) return false;
  http.useHTTP10(true);
  http.setTimeout(12000);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"email\":\"") + cfg.id + "@" DEVICE_DOMAIN "\",\"password\":\"" + cfg.dpass + "\",\"returnSecureToken\":true}";
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  if (code <= 0) {
    char eb[96] = "";
    client.getLastSSLError(eb, sizeof eb);
    Serial.printf("[AUTH] bağlantı hatası %d (%s) ssl: %s\n", code, http.errorToString(code).c_str(), eb);
#if VERIFY_TLS
    Serial.println(F("[AUTH] TLS doğrulama hatası olabilir: saat doğru mu? Geçici olarak VERIFY_TLS 0 deneyebilirsin."));
#endif
  }
  http.end();
  if (code != 200) {
    if (code > 0) Serial.printf("[AUTH] HTTP %d: %s  (Firebase'de bu e-posta/şifreyle kullanıcı var mı?)\n", code, jsonStr(resp, "message").c_str());
    return false;
  }
  idToken = jsonStr(resp, "idToken");
  tokenAt = millis();
  Serial.printf("[AUTH] giriş ok (token %u karakter)\n", (unsigned)idToken.length());
  return idToken.length() > 100;
}

bool ensureToken() {
  if (idToken.length() > 0 && millis() - tokenAt < TOKEN_LIFE_MS) return true;
  idToken = "";
  return signIn();
}

// ---------- Firestore REST ----------
// Dönüş: HTTP kodu (<=0 = bağlantı hatası, -100 = giriş yapılamadı)
int fsRequest(const char* method, const String& path, const String& query,
              const String& body, String* out = nullptr) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!ensureToken()) return -100;
    BearSSL::WiFiClientSecure client;
    tlsSetup(client);
    HTTPClient http;
    String url = String("https://firestore.googleapis.com/v1/projects/") + FB_PROJECT +
                 "/databases/(default)/documents/" + path + (query.length() ? "?" + query : String());
    if (!http.begin(client, url)) return -1;
    http.useHTTP10(true);          // chunked cevap yok, parse kolay
    http.setTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + idToken);
    int code = http.sendRequest(method, body);
    if (out && code > 0) *out = http.getString();
    if (code <= 0) {
      char eb[96] = "";
      client.getLastSSLError(eb, sizeof eb);
      Serial.printf("[FS] bağlantı hatası %d ssl: %s\n", code, eb);
    }
    http.end();
    if (code == 401 && attempt == 0) { idToken = ""; continue; }   // token dolmuş: yeniden giriş yapıp bir kez daha dene
    if (code == 403) Serial.println(F("[FS] 403: Firestore kuralları bu hesabı reddetti (e-posta/id eşleşiyor mu?)"));
    return code;
  }
  return -1;
}

String fStr(const char* k, const String& v) { return String("\"") + k + "\":{\"stringValue\":\"" + v + "\"}"; }
String fInt(const char* k, const String& v) { return String("\"") + k + "\":{\"integerValue\":\"" + v + "\"}"; }
String fDbl(const char* k, float v)         { return String("\"") + k + "\":{\"doubleValue\":" + String(v, 1) + "}"; }
String fBool(const char* k, bool v)         { return String("\"") + k + "\":{\"booleanValue\":" + (v ? "true" : "false") + "}"; }

// ---------- Firestore: cihaz kaydı (parent doküman) ----------
// Alt koleksiyonu olan ama kendisi olmayan doküman "hayalet"tir ve collection listesinde görünmez.
// Dashboard cihazı bulsun diye parent dokümanı burada yazıyoruz. cfg/otaIste alanlarına dokunmuyoruz (updateMask).
bool registerDevice() {
  String body = String("{\"fields\":{") +
    fStr("tip", "esp8266") + "," +
    fInt("fw", String(FW_VERSION)) + "," +
    fStr("ip", WiFi.localIP().toString()) + "," +
    fInt("rssi", String(WiFi.RSSI())) + "," +
    fInt("boot", timeValid() ? tsMs() : String("0")) + "}}";
  String q = "updateMask.fieldPaths=tip&updateMask.fieldPaths=fw&updateMask.fieldPaths=ip"
             "&updateMask.fieldPaths=rssi&updateMask.fieldPaths=boot";
  int code = fsRequest("PATCH", String("cihazlar/") + cfg.id, q, body);
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

  int code = fsRequest("PATCH", String("cihazlar/") + cfg.id + "/canli/veri", "",
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
  int code = fsRequest("POST", String("cihazlar/") + cfg.id + "/gecmis", "",
                       String("{\"fields\":{") + f + "}}");
  Serial.printf("[FS] gecmis -> %d\n", code);
  return code == 200;
}

// ---------- Dashboard'dan "güncelle" komutu ----------
bool otaRequested() {
  String resp;
  int code = fsRequest("GET", String("cihazlar/") + cfg.id, "mask.fieldPaths=otaIste", "", &resp);
  if (code != 200) return false;
  int i = resp.indexOf("otaIste");
  if (i < 0) return false;
  int j = resp.indexOf("booleanValue", i);
  if (j < 0) return false;
  return resp.substring(j, min((int)resp.length(), j + 24)).indexOf("true") >= 0;
}

void clearOtaFlag() {
  fsRequest("PATCH", String("cihazlar/") + cfg.id, "updateMask.fieldPaths=otaIste",
            String("{\"fields\":{") + fBool("otaIste", false) + "}}");
}

// ---------- İnternet OTA ----------
// esp8266.json örneği: {"version": 3, "url": "https://raw.githubusercontent.com/.../esp8266.bin"}
// NOT: GitHub raw için TLS doğrulanmıyor (bkz. README "Bilinen riskler"). .bin'de gizli bilgi yok.
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
  if (!binUrl.startsWith("https://raw.githubusercontent.com/ahmet-kaan5565/dashboar/")) {   // başka yerden indirme
    Serial.println("[OTA] url izinli değil"); return;
  }

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
  WiFi.hostname(cfg.id);
  WiFi.begin(cfg.ssid, cfg.pass);
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

void setupArduinoOta() {
  ArduinoOTA.setHostname(cfg.id);
  ArduinoOTA.setPassword(cfg.dpass);
  ArduinoOTA.onStart([]()  { Serial.println("[ArduinoOTA] başladı"); });
  ArduinoOTA.onEnd([]()    { Serial.println("\n[ArduinoOTA] bitti"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ArduinoOTA] hata %u\n", e); });
  ArduinoOTA.begin();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n\n== IoT düğümü fw v%d ==\n", FW_VERSION);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  if (!loadCfg()) configMode(true);          // hiç ayar yoksa dönmez
  else if (askConfig(3000)) configMode(false);

  Serial.printf("[CFG] id=%s ssid=%s\n", cfg.id, cfg.ssid);
  gRoots.append(GTS_ROOT_R1);
  dht.begin();

  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
    setupArduinoOta();
    if (timeValid()) registerDevice();
  }
}

void loop() {
  ArduinoOTA.handle();

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

#if AUTO_OTA_CHECK
  if (!firstRun && now - lastOta >= OTA_CHECK_MS) {
    lastOta = now;
    checkOta();
  }
#endif
  if (firstRun) { lastOta = now; lastCmd = now; }

  firstRun = false;
  delay(10);
}
