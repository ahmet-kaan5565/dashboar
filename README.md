# dashboar — ESP8266 / ESP-01 IoT dashboard

Cihazlar → Firestore (REST, Firebase Auth ile) → `index.html` (GitHub Pages). Cihazlar kablosuz güncellenebilir.

> **Bu repo public.** Burada gizli hiçbir şey yok ve olmamalı. Koruma kodun gizliliğinden değil,
> **Firebase Auth + Firestore kurallarından** gelir. Aşağıdaki "Güvenlik kurulumu" yapılmadan sistem KORUNMASIZDIR.

## Yapı
- `index.html` — dashboard (e-posta/şifre ile giriş, Firestore realtime)
- `firestore.rules` — Firestore güvenlik kuralları (Console'a yapıştırılır)
- `firmware/esp8266_node/` — ESP8266 (NodeMCU / D1 mini)
- `firmware/esp01_node/` — ESP-01S (1MB)
- `firmware/esp8266.json`, `firmware/esp01.json` — OTA sürüm bildirimi

## 1) Güvenlik kurulumu (bir kez, Firebase Console)
1. **Authentication → Sign-in method → Email/Password**'ü aç.
2. **Authentication → Users → Add user** ile hesapları aç:
   - Sen: kendi e-postan + güçlü bir şifre (dashboard girişi)
   - Her cihaz için: `esp8266-1@cihaz.example.com`, `esp01-1@cihaz.example.com` (+ ayrı, uzun şifreler)
3. **Firestore → Rules**: `firestore.rules` içeriğini yapıştır, **Publish**.
4. (Önerilir) Authentication ayarlarında kendi kendine kayıt (sign-up) kapatma seçeneği varsa kapat. Kurallar zaten
   sadece yukarıdaki e-postalara izin verir; bu ek bir katman.
5. Eski açık kurallar ve eski `iot2025` şifresi geçersiz sayılmalı: kuralları yayınladığında artık kimse yazamaz/okuyamaz.

## 2) Cihaz kurulumu (cihaz başına bir kez, kabloyla)
1. Arduino IDE: "esp8266" kart paketi + kütüphaneler *DHT sensor library*, *Adafruit Unified Sensor*. (NTP için ek kütüphane yok.)
2. İlgili `.ino`'yu yükle (kart/flash ayarları dosyanın en üstünde).
3. Seri Monitör (115200, Newline) → ayar modu:
```
ssid=WIFI_ADIN
pass=WIFI_SIFREN
id=esp8266-1
dpass=2.adimda_cihaz_icin_sectigin_sifre
save
```
Şifreler **cihazın flash'ında** durur, koda/`.bin`'e girmez. Yeniden ayar: açılışta 3 sn içinde Seri'ye bir şey yazıp Enter.

## 3) Kablosuz güncelleme
**İnternetten (iki cihaz):**
1. Kodda değişiklik yap, `FW_VERSION`'ı artır.
2. Arduino IDE → *Sketch → Export Compiled Binary* → `.bin`'i `firmware/esp8266.bin` / `firmware/esp01.bin` olarak koy
   (`.bin` içinde şifre YOK, public durabilir).
3. İlgili json'daki `"version"`'ı `FW_VERSION` ile aynı yapıp pushla.
4. Cihaz 30 dk içinde bulur; ya da dashboard'da **⟳ Güncelle** (~1 dk).

**Aynı WiFi'dayken (sadece ESP8266):** Arduino IDE → Port → ağ portu, şifre = cihaz şifresi (`dpass`).

## Firestore yapısı
```
cihazlar/{id}              -> fw, ip, rssi, boot, tip, cfg{...}, otaIste
cihazlar/{id}/canli/veri   -> sicaklik, nem, saat, tarih, durum, ts
cihazlar/{id}/gecmis/{*}   -> sicaklik, nem, ts
```

## Bilinen riskler / sınırlar
- **Firebase web API anahtarı kodda görünür** — bu normal (gizli değil); korumayı Auth + kurallar sağlar.
- Firestore ve giriş istekleri sertifika doğrulamalı (GTS Root R1 gömülü). Seri'de TLS hatası görürsen saat/NTP'yi kontrol et,
  `VERIFY_TLS`'i geçici 0 yapıp dene. Kök sertifika 2036'da bitiyor.
- **İnternet OTA'da GitHub için TLS doğrulanmıyor**: ağında aktif saldırgan varsa sahte firmware verebilir. Güvendiğin bir ağda kullan;
  istersen `AUTO_OTA_CHECK 0` yapıp sadece dashboard butonuyla güncelle. İmzalı firmware ileride eklenebilir.
- Repoya yazma yetkisi olan biri `.bin`'i değiştirirse cihazlar onu yükler: GitHub hesabına 2FA aç.
- Bir cihaz çalınırsa: Firebase Console'da o cihaz hesabını sil/şifresini değiştir; diğer cihazlar etkilenmez.
- ESP-01'in flash'ı 512KB ise OTA çalışmaz.
