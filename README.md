# dashboar — ESP8266 / ESP-01 IoT dashboard

Cihazlar → Firestore (REST) → `index.html` (GitHub Pages). Cihazlar kablosuz güncellenebilir.

## Yapı
- `index.html` — dashboard (Firestore realtime)
- `firmware/esp8266_node/` — ESP8266 (NodeMCU / D1 mini) kodu
- `firmware/esp01_node/` — ESP-01S (1MB) kodu
- `firmware/esp8266.json`, `firmware/esp01.json` — OTA sürüm bildirimi (cihazlar bunlara bakar)

## Kurulum (her cihaz için)
1. Klasördeki `secrets.example.h` dosyasını `secrets.h` olarak kopyala, WiFi + OTA şifresini yaz (git'e girmez).
2. Arduino IDE: "esp8266" kart paketi + kütüphaneler: *DHT sensor library*, *Adafruit Unified Sensor*.
3. Kart ayarları: ilgili `.ino` dosyasının en üstündeki yoruma bak (ESP-01 için Flash Size = `1MB (FS:none OTA:~502KB)`).
4. İlk yükleme kabloyla. Sonrası kablosuz.

## Kablosuz güncelleme
**İnternet üzerinden (her iki cihaz):**
1. Kodda değişiklik yap, `FW_VERSION`'ı artır.
2. Arduino IDE → *Sketch → Export Compiled Binary* → çıkan `.bin`'i `firmware/esp8266.bin` (veya `esp01.bin`) olarak repoya koy.
3. İlgili json'daki `"version"` değerini `FW_VERSION` ile aynı yap, pushla.
4. Cihaz 30 dk içinde kendisi bulur; ya da dashboard'da kartın altındaki **⟳ Güncelle**'ye bas (~1 dk).

**Aynı WiFi'dayken (sadece ESP8266):** Arduino IDE → Port → ağ portu (`esp8266-1`), şifre = `OTA_PASS`.

## Firestore yapısı
```
cihazlar/{id}              -> fw, ip, rssi, boot, tip, cfg{...}, otaIste
cihazlar/{id}/canli/veri   -> sicaklik, nem, saat, tarih, durum, ts
cihazlar/{id}/gecmis/{*}   -> sicaklik, nem, ts
```
Firestore kuralları: cihazlar anonim yazdığı için `cihazlar/**` için `allow read, write: if true` gerekir.
**Bu, API anahtarını bilen herkesin yazabilmesi demek** — aşağıdaki güvenlik notlarına bak.

## Bilinen riskler
- Dashboard şifresi (`iot2025`) HTML'de açık, sadece görsel bir kapı. Gerçek koruma için Firebase Auth gerekir.
- Cihazlar TLS sertifikasını doğrulamıyor (`setInsecure`). Ağdaki biri OTA sırasında sahte firmware verebilir.
- GitHub raw önbelleği ~5 dk; güncelleme hemen görünmeyebilir.
