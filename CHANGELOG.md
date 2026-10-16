# Sürüm Geçmişi

BLE servis/karakteristik UUID'leri ve EKG paket biçimi v2.5'ten bu yana değişmedi; yeni sürümler eski PC uygulamasıyla uyumludur.

## v2.8 — Derin uyku
- Bileklik 2 dk koldan çıkık kalırsa ve BLE bağlı değilse (bağlıyken 10 dk) ESP32 derin uykuya girer; USB takılıyken uyumaz.
- Uykuda MAX30102 yakınlık modunda bekler; deri algılanınca INT bacağı ESP32'yi uyandırır (ext0). USB takılınca da uyanır (ext1).
- Uyanışta BLE başlatılmadan kısa bir canlılık kontrolü yapılır; yersiz uyanmada tekrar uyunur.
- Yedek zamanlayıcı: yakınlık kesmesi takmayı kaçırırsa firmware bunu fark eder ve yoklama moduna geçer; art arda yersiz kesmede bir tur yalnızca zamanlayıcı kullanılır.
- Uykuda AD8232 SDN bacağı GPIO hold ile LOW tutulur.
- AD8232 SDN artık HIGH sürülmüyor (AD8232 3,0 V beslemeli, bacak gerilimi sınırı); açmak için pin bırakılır, kart üzerindeki pull-up çeker.
- `RITIMEL:SLEEP` seri komutu: uyku istatistikleri.
- Geliştirme anahtarı kaynak koddan çıkarıldı (`secrets.h`); `ALLOW_UNPAIRED_ECG 0` ile eşleştirilmemiş bileklik hiç EKG göndermez.
- Düzeltme: PC saati yeniden eşitlediğinde bir sonraki günün alarmı atlanabiliyordu; alarm artık `millis()` tabanlı tekrar korumasıyla her gün çalar.
- Alarm ve saat komutlarına aralık doğrulaması eklendi; kullanılmayan ikili alarm biçimi kaldırıldı.

## v2.7 — Güç tasarrufu ve pil servisi
- İşlemci 240 yerine 80 MHz'de çalışır.
- `loop()` boşta dönmek yerine bir sonraki örnek anına kadar FreeRTOS'ta uyur (`xTaskDelayUntil`); örnekleme tik saatine kilitlidir.
- BLE bağlı değilken ya da bileklik koldan çıkınca AD8232 kapatılır; açıldıktan sonra çıkışın oturması beklenir.
- Bağlantı yokken reklam aralığı ~1 sn; açılışta, kopmada ve bileklik takılınca 30 sn hızlı reklam.
- Standart BLE Pil Servisi (`0x180F`: `0x2A19` seviye, `0x2A1A` güç durumu) ve `RITIMEL:BAT` seri komutu.

## v2.6 — Bluetooth parazit filtresi
- Telsizin her bağlantı olayında EKG'ye karıştırdığı parazit (60 ms aralıkta 16,67 Hz ve harmonikleri) ölçüldü. Bu parazit her kullanıcıda aynı olduğu için farklı kişilerin EKG'lerini birbirine benzetir.
- Firmware bağlantı aralığını öğrenir ve EKG bandına (5–45 Hz) düşen harmoniklere dar notch filtreler (Q = 30) uygular.

## v2.5 — Cihaza özel anahtar
- Önceki sürümlerde tüm bilekliklerde aynı anahtar vardı. Artık her bileklik USB ile eşleştirilir ve kendine özel 128 bit anahtar alır.
- Eşleştirilmiş paket: AES-128-CTR ile şifreli EKG + HMAC-SHA256 etiketi.
- Seri komutlar: `RITIMEL:INFO`, `RITIMEL:PAIR:<hex>`, `RITIMEL:UNPAIR`.

## v2.4 — Kararlılık
- MAX30102 bloklamadan okunuyor: kütüphanenin `getIR()`/`getRed()` çağrıları yeni örnek beklediği için 200 Hz EKG örneklemesi ~12 Hz'e düşüyordu.
- 0,5 Hz yüksek geçiren filtre katsayısı düzeltildi.
- Oturum nonce'u ile replay koruması.
- SpO2 yalnızca geçerli bir ölçüm varsa gönderiliyor (öncesinde 0 = ölçülüyor); değer aralığı 70–100.
- MAX30102 bulunamazsa EKG akışı başlatılmıyor (canlılık kontrolü atlanamaz).

## v2.3 — Optik canlılık
- AD8232 LO+/LO- elektrot kopma pinleri yerine canlılık MAX30102 kızılötesi yansımasıyla algılanıyor.

## v2.1 — Hata düzeltmeleri
- Filtre çıkışı int16 sınırına kırpılıyor (taşma sahte sıçramalar yaratıyordu).
- Anti-replay sayacı NVS'te kalıcı; eski `boot_count << 20` yöntemi uzun oturumlarda taşıyordu.
