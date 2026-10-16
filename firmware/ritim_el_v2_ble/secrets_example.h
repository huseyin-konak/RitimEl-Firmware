// Ritim-El — yerel gizli ayarlar için ŞABLON
//
// Kullanım: bu dosyayı aynı klasöre "secrets.h" adıyla kopyalayın ve değerleri
// kendi PC uygulamanızdaki geliştirme anahtarıyla değiştirin.
// secrets.h .gitignore'dadır, depoya gönderilmez.
//
// Bu anahtar yalnızca EŞLEŞTİRİLMEMİŞ geliştirme modunda kullanılır
// (ALLOW_UNPAIRED_ECG 1). Eşleştirilmiş bileklikler USB ile PC'den aldıkları
// cihaza özel anahtarı kullanır; ürün derlemesinde ALLOW_UNPAIRED_ECG 0 yapılır
// ve bu anahtar hiç kullanılmaz.
//
// Her iki değer de tam 16 karakter olmalıdır (AES-128 anahtarı ve IV).
#pragma once

#define DEV_AES_KEY "CHANGE_ME_KEY_16"
#define DEV_AES_IV  "CHANGE_ME_IV__16"
