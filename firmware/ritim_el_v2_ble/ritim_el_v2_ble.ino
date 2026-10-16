/*
 * ============================================================
 *  RİTİM-EL Bileklik Firmware'i — v2.8
 *  ESP32 + AD8232 (EKG) + MAX30102 (PPG) + BLE
 * ============================================================
 *
 *  Ne yapar?
 *    - AD8232'den gelen EKG sinyalini 200 Hz'de örnekler, cihaz üzerinde
 *      filtreler (0,5 Hz yüksek geçiren, 50 Hz şebeke notch, 40 Hz alçak geçiren).
 *    - MAX30102 ile bilekliğin takılı olup olmadığını (canlılık), nabzı ve
 *      SpO2'yi ölçer. Bileklik takılı değilse EKG gönderilmez.
 *    - EKG'yi 10'arlı paketler halinde saniyede 20 BLE bildirimiyle
 *      (20 x 10 = 200 örnek/sn) şifreli ve kimliği doğrulanmış olarak gönderir.
 *    - Pil seviyesini standart BLE Pil Servisi'yle (0x180F) yayınlar.
 *    - Bileklik koldan çıkınca derin uykuya girer; takılınca MAX30102'nin
 *      yakınlık kesmesiyle uyanır.
 *
 *  Güvenlik (özet):
 *    - Her bileklik USB kablosuyla PC'ye eşleştirilir; PC rastgele 128 bit bir
 *      anahtar üretip seri porttan gönderir, anahtar NVS'te saklanır.
 *      Kablosuz eşleştirme yoktur (fiziksel erişim gerekir).
 *    - Eşleştirilmiş paket (32 bayt):
 *        [0:4]   paket sayacı (açık)
 *        [4:24]  10 x int16 EKG, AES-128-CTR ile şifreli
 *                (anahtar akışı = AES(K_enc, nonce || sayaç || blok_no))
 *        [24:32] HMAC-SHA256(K_mac, "RE3" || nonce || paket[0:24]) ilk 8 bayt
 *      K_enc ve K_mac, cihaz anahtarından HMAC ile türetilir.
 *    - Oturum nonce'u: PC her bağlantıda yeni bir nonce yazar. Kaydedilip başka
 *      oturumda tekrar oynatılan paketler PC'de reddedilir (replay koruması).
 *    - Paket sayacı NVS'te tutulur; yeniden başlatmada geriye gitmez.
 *
 *  Seri port komutları (115200 baud, satır sonu \n):
 *    RITIMEL:INFO          -> sürüm ve eşleştirme durumu
 *    RITIMEL:PAIR:<32 hex> -> cihaz anahtarını yaz
 *    RITIMEL:UNPAIR        -> cihaz anahtarını sil
 *    RITIMEL:BAT           -> pil gerilimi / yüzdesi / USB durumu
 *    RITIMEL:SLEEP         -> derin uyku istatistikleri
 *
 *  Donanım ayarları aşağıdaki "DONANIM AYARLARI" bölümündedir.
 *  Sürüm geçmişi için depodaki CHANGELOG.md dosyasına bakın.
 * ============================================================
 */

// ############################################################
// #   DONANIM AYARLARI                                       #
// #   Kendi bağlantınıza göre yalnızca bu bölümü değiştirin. #
// ############################################################

// --- 1) EKG sinyali nereden okunuyor? ---
// 1 = AD8232 çıkışı ADS1115'e bağlı (I2C, 16 bit, önerilen)
// 0 = AD8232 çıkışı doğrudan ESP32'nin analog pinine bağlı (12 bit)
#define USE_ADS1115   1

// I2C hattı: MAX30102 her zaman, ADS1115 ise USE_ADS1115 1 iken bu hatta
#define I2C_SDA_PIN   21     // ESP32 varsayılan SDA
#define I2C_SCL_PIN   22     // ESP32 varsayılan SCL

#if USE_ADS1115
  #define ADS_ADDRESS   0x48   // ADDR bacağı: GND=0x48  VDD=0x49  SDA=0x4A  SCL=0x4B
  #define ADS_CHANNEL   0      // AD8232 çıkışının bağlı olduğu kanal (A0..A3 -> 0..3)

  // Ölçüm aralığı. AD8232 çıkışı besleme geriliminin yarısı (~1,5 V) etrafında
  // salınır; GAIN_TWO (+-2,048 V) tepe noktalarını kırpabilir.
  //   GAIN_ONE       -> +-4,096 V   (önerilen)
  //   GAIN_TWO       -> +-2,048 V
  //   GAIN_TWOTHIRDS -> +-6,144 V
  #define ADS_GAIN      GAIN_ONE
#else
  // BLE açıkken yalnızca ADC1 pinleri (32-39) kullanılabilir;
  // ADC2 pinleri (0, 2, 4, 12-15, 25-27) Bluetooth ile birlikte çalışmaz.
  #define ECG_ADC_PIN   34
#endif

// --- 2) Canlılık algılama (MAX30102) ---
// Bilekliğin takılı olduğu, deriden yansıyan kızılötesi (IR) ışığın seviyesinden
// anlaşılır. Ölçülen değerler: boşta < 1500, deriye temasta > 10000.
#define PPG_LIVENESS_THRESHOLD 4500
// 1 = MAX30102 bulunamazsa EKG gönderilmez (canlılık kontrolü atlanamaz, önerilen)
// 0 = MAX30102 olmadan da EKG gönderilir (yalnızca sensörsüz geliştirme için)
#define REQUIRE_PPG_LIVENESS 1

// --- 3) Durum LED'i (kullanılmıyorsa -1) ---
#define LED_PIN       2

// --- 4) Titreşim motoru (alarm) ---
// Motor takılı değilken 0 bırakın; pin hiç sürülmez.
#define USE_VIBRATION_MOTOR 0
#define VIBRATION_PIN       13

// --- 5) Bluetooth ---
// 1 = normal çalışma (veri BLE ile gönderilir)
// 0 = tanı modu: BLE tamamen kapalı, filtrelenmiş EKG seri porttan satır satır
//     yazılır. Telsizin EKG'ye parazit karıştırıp karıştırmadığını ayırt etmek için.
#define USE_BLE 1

// Eşleştirilmemiş bileklik EKG'yi tüm cihazlarda ortak olan geliştirme anahtarıyla
// şifreler; bu anahtar gizli sayılmamalıdır.
// 1 = eşleştirilmeden de EKG gönder (geliştirme)
// 0 = ürün derlemesi: USB ile eşleştirilmemiş bileklik hiç EKG göndermez
#ifndef ALLOW_UNPAIRED_ECG
#define ALLOW_UNPAIRED_ECG 1
#endif

// Telsiz gücünü düşürür (paraziti azaltır, menzili kısaltır)
#define BLE_TX_POWER_LOW 0

// --- 6) Örnekleme hızı (PC uygulaması da 200 Hz bekler) ---
#define SAMPLE_RATE   200

// --- 7) Güç yönetimi ve pil (şarjlı kart) ---
// 80 MHz, Bluetooth'un çalıştığı en düşük işlemci hızıdır.
#define CPU_FREQ_MHZ      80
// AD8232 SDN bacağı (LOW = kapalı). Kartta 100k pull-up vardır. Bağlı değilse -1.
#define AD8232_SDN_PIN    33
// AD8232 açıldıktan sonra çıkışın oturması için beklenen süre (EKG gönderilmez)
#define AD8232_SETTLE_MS  500
// Pil gerilimi: 100k/100k bölücü, pil = ölçüm x 2. Bağlı değilse -1.
#define VBAT_SENSE_PIN    36
#define VBAT_DIVIDER      2.0f
// USB algılama: 100k/180k bölücü, USB takılıyken HIGH. Bağlı değilse -1.
#define USB_DET_PIN       25
// Bağlantı yokken reklam aralığı (ms) ve açılışta/kopmada hızlı reklam süresi
#define ADV_SLOW_MIN_MS   900
#define ADV_SLOW_MAX_MS   1000
#define ADV_FAST_WINDOW_MS 30000UL

// --- 8) Derin uyku ---
// MAX30102 INT bacağı (aktif LOW, RTC GPIO olmalı). Bağlı değilse -1:
// uykudan yalnızca zamanlayıcıyla uyanılır (SLEEP_POLL_S gecikmeli).
#define MAX_INT_PIN       35
// Koldan çıkık ve BLE bağlı değilken bu süre sonunda derin uyku
#define DEEP_SLEEP_AFTER_MS           120000UL   // 2 dk
// BLE bağlıyken koldan çıkık kalma süresi sınırı (PC açık unutulursa). 0 = uyuma
#define DEEP_SLEEP_CONNECTED_AFTER_MS 600000UL   // 10 dk
// Yedek kontrol: kesme gelmese de bu aralıkla uyanıp MAX30102'ye bakılır (sn)
#define SLEEP_CHECK_S     60
// Yakınlık kesmesi çalışmıyorsa (ya da INT bağlı değilse) yoklama aralığı (sn)
#define SLEEP_POLL_S      15
// Yakınlık modu: pilot LED akımı (0,2 mA birim) ve eşik (ADC değerinin üst 8 biti,
// 1 = 1024 sayım). Takılı bileklik > 10000, boşta < 1500 sayım verir.
#define PROX_LED_PA       0x3F
#define PROX_THRESHOLD    2

// ############################################################
// #   BU SATIRDAN SONRASI DONANIMA BAĞLI DEĞİLDİR            #
// ############################################################

#define FW_VERSION  "2.8"

#define SERVICE_UUID    "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID_ECG   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_UUID_BPM   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define CHAR_UUID_SPO2  "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CHAR_UUID_ALARM "beb5483e-36e1-4688-b7f5-ea07361b26ab"

// Paket sayacının NVS'e yazılma sıklığı ve açılışta eklenen pay.
// SAVE_EVERY < BOOT_GAP olduğu sürece sayaç yeniden başlatmada asla geriye gitmez.
#define COUNTER_SAVE_EVERY  20000UL
#define COUNTER_BOOT_GAP    50000UL

// ============================================================
// KÜTÜPHANELER (yukarıdaki ayarlara göre seçilir)
// ============================================================
#include <Wire.h>
#if USE_ADS1115
  #include <Adafruit_ADS1X15.h>
#endif
#include "MAX30105.h"   // SparkFun MAX3010x kütüphanesi (MAX30102 ile uyumlu)
#if USE_BLE
  #include <BLEDevice.h>
  #include <BLEServer.h>
  #include <BLEUtils.h>
  #include <BLE2902.h>
  #include <esp_bt.h>
  #include <esp_gap_ble_api.h>
#endif
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <sys/time.h>
#include <time.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

// Geliştirme anahtarı depoda tutulmaz: secrets_example.h dosyasını secrets.h
// adıyla kopyalayıp değerleri doldurun (secrets.h .gitignore'dadır).
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "secrets.h bulunamadi: secrets_example.h dosyasini ayni klasore secrets.h adiyla kopyalayin."
#endif
static_assert(sizeof(DEV_AES_KEY) == 17, "DEV_AES_KEY tam 16 karakter olmali");
static_assert(sizeof(DEV_AES_IV) == 17, "DEV_AES_IV tam 16 karakter olmali");

// ============================================================
// GENEL DEĞİŞKENLER
// ============================================================
Preferences prefs;
#if USE_ADS1115
Adafruit_ADS1115 ads;
#endif

MAX30105 particleSensor;
bool ppgConnected = false;      // MAX30102 bulundu mu
bool isWristAttached = false;   // bileklik kolda mı (canlılık)

#if USE_BLE
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristicECG = NULL;
BLECharacteristic* pCharacteristicBPM = NULL;
BLECharacteristic* pCharacteristicSPO2 = NULL;
BLECharacteristic* pCharacteristicAlarm = NULL;
BLECharacteristic* pCharacteristicBatLevel = NULL;   // 0x2A19 Battery Level
BLECharacteristic* pCharacteristicBatState = NULL;   // 0x2A1A Battery Power State
#endif

// BLE görevi (callback) yazar, ana döngü okur
volatile bool deviceConnected = false;
bool oldDeviceConnected = false;

// --- PPG: nabız, SpO2 ve canlılık ---
float ppg_ir_dc = 0, ppg_red_dc = 0;                 // DC taban hattı
float ppg_ir_ac_smooth = 0, ppg_red_ac_smooth = 0;   // yumuşatılmış AC (nabız dalgası)
float ppg_peak_env = 100.0f;                         // tepe zarfı (uyarlamalı eşik için)
float ppg_peak_candidate = 0;
bool ppg_is_rising = false;
unsigned long lastPPGPeakTimeMs = 0;
unsigned long lastPPGProcessMs = 0;

unsigned long ppg_rr_buffer[5] = {0};   // son 5 atım aralığı (ms), medyan alınır
int ppg_rr_idx = 0;
int ppg_rr_count = 0;

// MAX30102 yoksa nabız EKG R tepelerinden hesaplanır
unsigned long lastECGPeakTimeMs = 0;
unsigned long ecg_rr_buffer[8] = {0};
int ecg_rr_idx = 0;
int ecg_rr_count = 0;

float currentBPM = 0;
uint8_t currentSpO2 = 0;                 // 0 = henüz ölçülmedi
float spo2_ir_ac_rms = 0, spo2_red_ac_rms = 0;
unsigned long lastSpO2Calc = 0;
int validSpo2Samples = 0;
#define SPO2_CALC_INTERVAL_MS  1000

// Örnekleme FreeRTOS tik saatine kilitlidir (Arduino-ESP32'de 1000 Hz)
#if (configTICK_RATE_HZ % SAMPLE_RATE) != 0
  #error "SAMPLE_RATE, FreeRTOS tik hizini (configTICK_RATE_HZ) tam bolmeli"
#endif
const TickType_t SAMPLE_TICKS = configTICK_RATE_HZ / SAMPLE_RATE;
TickType_t lastSampleTick = 0;
unsigned long totalSampleCount = 0;

// AD8232 güç durumu
bool ad8232On = false;
unsigned long ad8232OnSinceMs = 0;

// --- EKG filtreleri: 2. derece IIR, fs = 200 Hz ---
// scipy.signal.butter(2, 0.5, 'highpass', fs=200) ve butter(2, 40, 'lowpass', fs=200)
const float HPF_B[3]   = {0.98895425f, -1.97790850f, 0.98895425f};
const float HPF_A[3]   = {1.0f, -1.97778648f, 0.97803051f};
const float LPF_B[3]   = {0.20657208f, 0.41314417f, 0.20657208f};
const float LPF_A[3]   = {1.0f, -0.36952738f, 0.19581571f};
// 50 Hz şebeke notch'u. 50 Hz = fs/4 olduğundan cos(w0) = 0 ve orta katsayılar sıfırdır.
const float NOTCH_B[3] = {0.98361f, 0.0f, 0.98361f};
const float NOTCH_A[3] = {1.0f, 0.0f, 0.96722f};

float hpf_x[3] = {0}, hpf_y[3] = {0};
float lpf_x[3] = {0}, lpf_y[3] = {0};
float notch_x[3] = {0}, notch_y[3] = {0};

#if USE_BLE
// Bluetooth bağlantı olayı paraziti için uyarlamalı notch filtreleri.
// Telsiz her bağlantı aralığında açılır ve EKG'ye bu frekansta ve harmoniklerinde
// parazit karışır (bkz. computeBleNotchFreqs).
#define BLE_NOTCH_Q        30.0f
#define BLE_NOTCH_MAX      3
#define BLE_NOTCH_MIN_HZ   5.0f
#define BLE_NOTCH_MAX_HZ   45.0f
#define BLE_NOTCH_KMAX     6
volatile uint16_t bleConnIntervalUnits = 0;   // 1,25 ms birim; 0 = bağlantı yok (BLE görevi yazar)
uint16_t bleNotchUnits = 0;                    // notch'ların en son ayarlandığı aralık
int bleNotchCount = 0;
float bleNotchHz[BLE_NOTCH_MAX];
float bleNotchB[BLE_NOTCH_MAX][3], bleNotchA[BLE_NOTCH_MAX][3];
float bleNotchX[BLE_NOTCH_MAX][3], bleNotchY[BLE_NOTCH_MAX][3];
#endif

// EKG R tepesi algılama
float signalPeak = 0, peakThreshold = 0;
bool isPeakRising = false;
int samplesSincePeak = 999;
const int REFRACTORY_SAMPLES = (int)(SAMPLE_RATE * 0.30);   // 300 ms: iki atım arası en kısa süre

bool ledState = false;
unsigned long ledOffTime = 0;

// --- BLE paketleme ve şifreleme ---
int16_t ecgBuffer[10];
int bufferIndex = 0;

uint32_t packet_counter = 0;
uint32_t counter_last_saved = 0;

// Oturum nonce'u: PC her bağlantıda yazar. Eşleştirilmiş modda anahtar akışına ve
// HMAC'e, eşleştirilmemiş modda paketin 24-31. baytlarına girer.
#define NONCE_LEN 8
uint8_t session_nonce[NONCE_LEN] = {0};

// Cihaza özel anahtar (USB eşleştirmesiyle NVS'e yazılır)
#define DEVKEY_LEN  16
uint8_t devKey[DEVKEY_LEN] = {0};
bool devicePaired = false;
uint8_t kEnc[16] = {0};      // AES-128-CTR anahtarı
uint8_t kMac[32] = {0};      // HMAC-SHA256 anahtarı
uint8_t keyId[4] = {0};      // anahtar kimliği: eşleştirmeyi doğrulamak için, anahtarı açığa çıkarmaz

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, msg, msgLen, out);
}

// Tek cihaz anahtarından amaca göre ayrı anahtarlar türetilir (şifreleme / MAC / kimlik)
void deriveKeys() {
  uint8_t tmp[32];
  hmacSha256(devKey, DEVKEY_LEN, (const uint8_t*)"RITIMEL-ENC", 11, tmp);
  memcpy(kEnc, tmp, 16);
  hmacSha256(devKey, DEVKEY_LEN, (const uint8_t*)"RITIMEL-MAC", 11, kMac);
  hmacSha256(devKey, DEVKEY_LEN, (const uint8_t*)"RITIMEL-KID", 11, tmp);
  memcpy(keyId, tmp, 4);
  memset(tmp, 0, sizeof(tmp));
}

void clearKeys() {
  memset(devKey, 0, sizeof(devKey));
  memset(kEnc, 0, sizeof(kEnc));
  memset(kMac, 0, sizeof(kMac));
  memset(keyId, 0, sizeof(keyId));
  devicePaired = false;
}

void loadDeviceKey() {
  prefs.begin("ritim_el", false);
  if (prefs.isKey("devkey") && prefs.getBytesLength("devkey") == DEVKEY_LEN) {
    prefs.getBytes("devkey", devKey, DEVKEY_LEN);
    devicePaired = true;
    deriveKeys();
  } else {
    clearKeys();
  }
  prefs.end();
}

void printKeyId() {
  for (int i = 0; i < 4; i++) Serial.printf("%02X", keyId[i]);
}

// Tek onaltılık karakteri sayıya çevirir; geçersizse -1
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "a1B2..." biçimindeki hex metni bayt dizisine çevirir (uzunluk tam 2*n olmalı)
bool parseHex(const char* hx, size_t hexLen, uint8_t* out, size_t n) {
  if (hexLen != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int hi = hexNibble(hx[2 * i]), lo = hexNibble(hx[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// ============================================================
// PİL, USB VE AD8232 GÜCÜ
// ============================================================
#define BATTERY_READ_MS   10000UL   // pil gerilimi yavaş değişir: 10 sn'de bir
#define USB_CHECK_MS      500UL

uint16_t batteryMv = 0;        // 0 = ölçülemiyor (bölücü bağlı değil)
int batteryPct = -1;           // -1 = bilinmiyor
bool usbPresent = false;
float batteryMvAvg = 0;
unsigned long lastBatteryReadMs = 0;
unsigned long lastUsbCheckMs = 0;
int lastBatLevelSent = -1, lastBatStateSent = -1;

// 1S Li-Po, hafif yük (~30 mA) altında gerilim -> yaklaşık doluluk (doğrusal ara değer)
const uint16_t BAT_MV[]  = {3300, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
const uint8_t  BAT_PCT[] = {   0,    4,   12,   25,   40,   55,   64,   78,   90,  100};

int batteryPercentFromMv(uint16_t mv) {
  const int n = sizeof(BAT_MV) / sizeof(BAT_MV[0]);
  if (mv <= BAT_MV[0]) return 0;
  for (int i = 1; i < n; i++) {
    if (mv < BAT_MV[i])
      return BAT_PCT[i - 1] + (int)((long)(BAT_PCT[i] - BAT_PCT[i - 1]) * (mv - BAT_MV[i - 1]) /
                                    (BAT_MV[i] - BAT_MV[i - 1]));
  }
  return 100;
}

uint16_t readBatteryMv() {
#if VBAT_SENSE_PIN >= 0
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(VBAT_SENSE_PIN);
  float mv = (sum / 16.0f) * VBAT_DIVIDER;
  // Bölücü yoksa pin boşta kalır; Li-Po aralığı dışındaki değer = ölçülemiyor
  if (mv < 2500.0f || mv > 4600.0f) return 0;
  return (uint16_t)(mv + 0.5f);
#else
  return 0;
#endif
}

// Battery Power State (0x2A1A): her alan 2 bit, 0 = bilinmiyor, 2 = hayır, 3 = evet
// bit 0-1 pil takılı | 2-3 deşarj oluyor | 4-5 şarj oluyor | 6-7 seviye (2 = iyi, 3 = kritik)
uint8_t batteryPowerState() {
  uint8_t present = batteryMv ? 3 : 0;
#if USB_DET_PIN >= 0
  uint8_t discharging = usbPresent ? 2 : (batteryMv ? 3 : 0);
  uint8_t charging = usbPresent ? 3 : 2;   // USB takılı = şarj oluyor (ya da dolu, şarj entegresi tutar)
#else
  uint8_t discharging = 0, charging = 0;
#endif
  uint8_t level = batteryMv ? (batteryPct <= 10 ? 3 : 2) : 0;
  return present | (discharging << 2) | (charging << 4) | (level << 6);
}

// Değer değiştiyse seri porta yazar ve BLE ile bildirir
void publishBattery() {
  uint8_t level = batteryPct < 0 ? 0 : (uint8_t)batteryPct;
  uint8_t state = batteryPowerState();
  if (level != lastBatLevelSent || state != lastBatStateSent) {
    if (batteryMv) Serial.printf("[PIL] %.2f V  %%%d  USB: %s\n", batteryMv / 1000.0f, batteryPct,
                                 usbPresent ? "takili (sarj)" : "yok");
    else Serial.printf("[PIL] olculemiyor  USB: %s\n", usbPresent ? "takili" : "yok");
  }
#if USE_BLE
  if (pCharacteristicBatLevel != NULL && level != lastBatLevelSent) {
    pCharacteristicBatLevel->setValue(&level, 1);
    if (deviceConnected) pCharacteristicBatLevel->notify();
  }
  if (pCharacteristicBatState != NULL && state != lastBatStateSent) {
    pCharacteristicBatState->setValue(&state, 1);
    if (deviceConnected) pCharacteristicBatState->notify();
  }
#endif
  lastBatLevelSent = level;
  lastBatStateSent = state;
}

void updateBattery(unsigned long nowMs, bool force) {
  bool usbChanged = false;
#if USB_DET_PIN >= 0
  if (force || nowMs - lastUsbCheckMs >= USB_CHECK_MS) {
    lastUsbCheckMs = nowMs;
    bool u = digitalRead(USB_DET_PIN) == HIGH;
    usbChanged = (u != usbPresent);
    usbPresent = u;
  }
#endif
  if (!force && !usbChanged && nowMs - lastBatteryReadMs < BATTERY_READ_MS) return;
  lastBatteryReadMs = nowMs;
  uint16_t mv = readBatteryMv();
  // USB takılıp çıkınca gerilim şarj gerilimine sıçrar: ortalama baştan başlatılır
  if (mv == 0) batteryMvAvg = 0;
  else if (batteryMvAvg == 0 || usbChanged) batteryMvAvg = mv;
  else batteryMvAvg = 0.7f * batteryMvAvg + 0.3f * mv;
  batteryMv = (uint16_t)(batteryMvAvg + 0.5f);
  batteryPct = batteryMv ? batteryPercentFromMv(batteryMv) : -1;
  publishBattery();
}

// AD8232'yi aç/kapat. SDN LOW = kapalı.
// AD8232 3,0 V ile beslenir ve hiçbir bacağına +VS + 0,3 V'tan yüksek gerilim
// uygulanmamalıdır. ESP32'nin 3,3 V'luk HIGH çıkışı bu sınırda olduğundan SDN HIGH
// sürülmez: açmak için pin girişe alınır, kart üzerindeki 100k pull-up 3,0 V'a çeker.
void setAD8232(bool on) {
  if (on == ad8232On) return;
  ad8232On = on;
#if AD8232_SDN_PIN >= 0
  if (on) {
    pinMode(AD8232_SDN_PIN, INPUT);
  } else {
    pinMode(AD8232_SDN_PIN, OUTPUT);
    digitalWrite(AD8232_SDN_PIN, LOW);
  }
#endif
  if (on) ad8232OnSinceMs = millis();
  Serial.println(on ? "[GUC] AD8232 acildi" : "[GUC] AD8232 kapatildi");
}

// AD8232 açık ve çıkışı oturdu mu?
bool ecgReady(unsigned long nowMs) {
  if (!ad8232On) return false;
#if AD8232_SDN_PIN >= 0
  return nowMs - ad8232OnSinceMs >= AD8232_SETTLE_MS;
#else
  return true;
#endif
}

// ============================================================
// DERİN UYKU DURUMU
// RTC belleği derin uykuda korunur, güç kesilince sıfırlanır.
// ============================================================
#define SLEEP_MAGIC       0x52454C38UL   // "REL8": RTC belleğindeki kayıt geçerli
#define PROX_MISS_LIMIT   2              // zamanlayıcı takılı bileği bu kadar yakalarsa kesmeye güvenme
#define FALSE_WAKE_LIMIT  3              // art arda bu kadar yersiz kesmede bir tur yalnızca zamanlayıcı

// PROX:    MAX30102 yakınlık kesmesi + yedek zamanlayıcı
// POLL:    kesme güvenilmez; MAX30102 kapalı, SLEEP_POLL_S'de bir yoklama
// BACKOFF: art arda yersiz kesme; bir tur yalnızca zamanlayıcı (uyan-uyu döngüsünü önler)
enum SleepMode : uint8_t { SLEEP_PROX = 0, SLEEP_POLL = 1, SLEEP_BACKOFF = 2 };

struct SleepState {
  uint32_t magic;
  uint32_t sleeps;       // derin uykuya giriş sayısı
  uint32_t proxWakes;    // kesmeyle uyanıldı, bileklik takılıydı
  uint32_t falseWakes;   // kesme geldi ama bileklik takılı değildi
  uint32_t missWakes;    // kesme gelmedi, zamanlayıcı takılı buldu
  uint8_t  proxMisses;   // art arda kaçırılan takma
  uint8_t  falseRow;     // art arda yersiz kesme
  uint8_t  lastMode;     // son uykunun modu (SleepMode)
};
RTC_DATA_ATTR SleepState sleepRtc = {0, 0, 0, 0, 0, 0, 0, 0};

// Sıradaki uykunun modu (durumu değiştirmez)
uint8_t chooseSleepMode() {
#if MAX_INT_PIN < 0
  return SLEEP_POLL;
#else
  if (sleepRtc.proxMisses >= PROX_MISS_LIMIT) return SLEEP_POLL;
  if (sleepRtc.falseRow >= FALSE_WAKE_LIMIT) return SLEEP_BACKOFF;
  return SLEEP_PROX;
#endif
}

const char* sleepModeName(uint8_t m) {
  return m == SLEEP_POLL ? "poll" : (m == SLEEP_BACKOFF ? "backoff" : "prox");
}

// ============================================================
// SERİ PORT KOMUTLARI (eşleştirme yalnızca USB kablosuyla yapılır)
// ============================================================
void processSerialCommand(const char* cmd) {
  if (strncmp(cmd, "RITIMEL:PAIR:", 13) == 0) {
    const char* hx = cmd + 13;
    size_t hexLen = strlen(hx);
    uint8_t k[DEVKEY_LEN];
    if (hexLen != 2 * DEVKEY_LEN) {
      Serial.println("RITIMEL:PAIR:ERR:LEN");
      return;
    }
    if (!parseHex(hx, hexLen, k, DEVKEY_LEN)) {
      Serial.println("RITIMEL:PAIR:ERR:HEX");
      return;
    }
    prefs.begin("ritim_el", false);
    size_t written = prefs.putBytes("devkey", k, DEVKEY_LEN);
    prefs.end();
    if (written != DEVKEY_LEN) {
      memset(k, 0, sizeof(k));
      Serial.println("RITIMEL:PAIR:ERR:NVS");
      return;
    }
    memcpy(devKey, k, DEVKEY_LEN);
    memset(k, 0, sizeof(k));
    devicePaired = true;
    deriveKeys();
    Serial.print("RITIMEL:PAIR:OK:");
    printKeyId();
    Serial.println();
  } else if (strcmp(cmd, "RITIMEL:UNPAIR") == 0) {
    prefs.begin("ritim_el", false);
    prefs.remove("devkey");
    prefs.end();
    clearKeys();
    Serial.println("RITIMEL:UNPAIR:OK");
  } else if (strcmp(cmd, "RITIMEL:INFO") == 0) {
    Serial.print("RITIMEL:INFO:fw=" FW_VERSION ":paired=");
    Serial.print(devicePaired ? 1 : 0);
    if (devicePaired) {
      Serial.print(":kid=");
      printKeyId();
    }
    Serial.println();
  } else if (strcmp(cmd, "RITIMEL:BAT") == 0) {
    updateBattery(millis(), true);
    Serial.printf("RITIMEL:BAT:mv=%u:pct=%d:usb=%d\n", (unsigned)batteryMv, batteryPct, usbPresent ? 1 : 0);
  } else if (strcmp(cmd, "RITIMEL:SLEEP") == 0) {
    Serial.printf("RITIMEL:SLEEP:n=%lu:prox=%lu:false=%lu:miss=%lu:mode=%s\n",
                  (unsigned long)sleepRtc.sleeps, (unsigned long)sleepRtc.proxWakes,
                  (unsigned long)sleepRtc.falseWakes, (unsigned long)sleepRtc.missWakes,
                  sleepModeName(chooseSleepMode()));
  }
}

char serialLine[96];
size_t serialLen = 0;
bool serialOverflow = false;

// Seri porttaki baytları bloklamadan satır satır toplar
void handleSerialCommands() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (serialLen > 0 && !serialOverflow) {
        serialLine[serialLen] = 0;
        processSerialCommand(serialLine);
      }
      serialLen = 0;
      serialOverflow = false;
    } else if (serialLen < sizeof(serialLine) - 1) {
      serialLine[serialLen++] = (char)c;
    } else {
      serialOverflow = true;   // aşırı uzun satır komut olarak işlenmez
    }
  }
}

// ============================================================
// EKG PAKETLERİ
// ============================================================
// Eşleştirilmiş paket: [sayaç 4][AES-128-CTR(EKG) 20][HMAC-SHA256 ilk 8 bayt]
void buildAuthenticatedPacket(const int16_t* samples, uint32_t ctr, uint8_t out[32]) {
  // CTR anahtar akışı: her blok AES(K_enc, nonce || sayaç || blok_no).
  // Nonce oturuma, sayaç pakete özel olduğundan aynı anahtar akışı iki kez kullanılmaz.
  uint8_t ks[32], blk[16];
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, kEnc, 128);
  for (uint32_t j = 0; j < 2; j++) {
    memcpy(blk, session_nonce, NONCE_LEN);
    memcpy(blk + 8, &ctr, 4);
    memcpy(blk + 12, &j, 4);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, blk, ks + 16 * j);
  }
  mbedtls_aes_free(&aes);

  memcpy(out, &ctr, 4);
  const uint8_t* pt = (const uint8_t*)samples;
  for (int i = 0; i < 20; i++) out[4 + i] = pt[i] ^ ks[i];

  // Etiket nonce'u da kapsar: başka oturumdan kopyalanan paket doğrulanamaz
  uint8_t macIn[3 + NONCE_LEN + 24], tag[32];
  memcpy(macIn, "RE3", 3);
  memcpy(macIn + 3, session_nonce, NONCE_LEN);
  memcpy(macIn + 3 + NONCE_LEN, out, 24);
  hmacSha256(kMac, sizeof(kMac), macIn, sizeof(macIn), tag);
  memcpy(out + 24, tag, 8);
}

// Eşleştirilmemiş geliştirme paketi: [EKG 20][sayaç 4][nonce 8], ortak anahtarla AES-128-CBC.
// Anahtar ve IV tüm cihazlarda aynı olduğundan gizlilik sağlamaz; bu yüzden PC uygulaması
// eşleştirilmemiş bilekliği kilit ekranında kabul etmez ve ürün derlemesinde
// ALLOW_UNPAIRED_ECG 0 yapılır.
void buildDevPacket(const int16_t* samples, uint32_t ctr, uint8_t out[32]) {
  static const unsigned char devKeyShared[] = DEV_AES_KEY;
  static const unsigned char devIvShared[] = DEV_AES_IV;
  unsigned char plaintext[32] = {0};
  memcpy(plaintext, samples, 20);
  memcpy(plaintext + 20, &ctr, 4);
  memcpy(plaintext + 24, session_nonce, NONCE_LEN);

  unsigned char iv[16];
  memcpy(iv, devIvShared, 16);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, devKeyShared, 128);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, 32, iv, plaintext, out);
  mbedtls_aes_free(&aes);
}

// ============================================================
// BLUETOOTH (BLE)
// ============================================================
#if USE_BLE
// Reklam aralığı. Hızlı: kütüphane varsayılanı 20-40 ms. Yavaş: ~1 sn (pil için).
#define ADV_UNITS(ms)       ((uint16_t)((ms) * 8UL / 5UL))   // 0,625 ms birim
#define ADV_FAST_MIN_UNITS  0x20
#define ADV_FAST_MAX_UNITS  0x40
bool advSlow = false;
unsigned long advFastUntilMs = 0;
bool readvertisePending = false;
unsigned long disconnectedAtMs = 0;

void setAdvInterval(bool slow) {
  BLEAdvertising* a = BLEDevice::getAdvertising();
  a->setMinInterval(slow ? ADV_UNITS(ADV_SLOW_MIN_MS) : ADV_FAST_MIN_UNITS);
  a->setMaxInterval(slow ? ADV_UNITS(ADV_SLOW_MAX_MS) : ADV_FAST_MAX_UNITS);
}

// Reklamı yeni aralıkla yeniden başlatır (ana döngüden, bağlantı yokken)
void restartAdvertising(bool slow) {
  BLEAdvertising* a = BLEDevice::getAdvertising();
  a->stop();
  setAdvInterval(slow);
  advSlow = slow;
  if (!deviceConnected) a->start();
  Serial.println(slow ? "[BLE] Reklam yavas (~1 sn): pil tasarrufu"
                      : "[BLE] Reklam hizli (PC hemen bulsun)");
}

// Açılış, bağlantı kopması ya da bileklik takılması: PC'nin hemen bulması için bir süre hızlı reklam
void advertiseFast(unsigned long nowMs) {
  advFastUntilMs = nowMs + ADV_FAST_WINDOW_MS;
  if (!deviceConnected && advSlow) restartAdvertising(false);
}

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) {
    memset(session_nonce, 0, sizeof(session_nonce));   // yeni oturum: PC yeni nonce yazacak
    deviceConnected = true;
    Serial.println(">>> BLUETOOTH BAGLANDI <<<");
  }
  // Kütüphane iki onConnect'i de çağırır; bu sürüm bağlantı aralığını da verir
  void onConnect(BLEServer* s, esp_ble_gatts_cb_param_t* param) {
    if (param) bleConnIntervalUnits = param->connect.conn_params.interval;
  }
  void onDisconnect(BLEServer* s) {
    deviceConnected = false;
    bleConnIntervalUnits = 0;
    memset(session_nonce, 0, sizeof(session_nonce));
    Serial.println(">>> BLUETOOTH KOPTU, tekrar bekleniyor <<<");
    setAdvInterval(false);   // kopmadan sonra hızlı reklamla başla
    s->startAdvertising();   // beklemeden yeniden yayına çık
  }
};

// PC bağlantı aralığını sonradan değiştirirse (Windows ilk keşifte kısa aralık,
// sonra 60 ms kullanır) GAP olayı gelir; notch filtreleri buna göre güncellenir.
static void bleGapHandler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  if (event == ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT && param &&
      param->update_conn_params.status == ESP_BT_STATUS_SUCCESS) {
    bleConnIntervalUnits = param->update_conn_params.conn_int;
  }
}

// ============================================================
// ALARM VE TİTREŞİM MOTORU
// PC saati ve alarmı BLE ile gönderir; alarm, PC kapalı olsa bile bileklikte çalar.
// ============================================================
#define ALARM_REARM_MS  120000UL   // aynı alarm 2 dk içinde ikinci kez çalmaz

struct AlarmSyncData {
  uint8_t hour;       // 0-23
  uint8_t minute;     // 0-59
  uint8_t vibrate;    // 1: titreşimli, 0: sessiz
  uint8_t enabled;    // 1: açık, 0: kapalı
};

AlarmSyncData activeAlarm = {0, 0, 0, 0};
unsigned long alarmVibrationUntilMs = 0;
bool rtcSynced = false;
// Son çalma anı millis() ile tutulur: PC saati yeniden eşitlediğinde (tarih sıfırlanır)
// etkilenmez, alarm ertesi gün de çalar.
bool alarmHasFired = false;
unsigned long lastAlarmFireMs = 0;

// Titreşimli alarm kuruluyken derin uykuya girilmez (saat ve alarm uykuda kaybolur)
bool alarmNeedsAwake() {
#if USE_VIBRATION_MOTOR
  return rtcSynced && activeAlarm.enabled && activeAlarm.vibrate;
#else
  return false;
#endif
}

void triggerAlarmVibration(unsigned long durationMs = 3000) {
#if USE_VIBRATION_MOTOR
  digitalWrite(VIBRATION_PIN, HIGH);
  alarmVibrationUntilMs = millis() + durationMs;
#endif
  Serial.println(">>> [ALARM] Titresim uyarisi calisti <<<");
}

void stopAlarmVibration() {
#if USE_VIBRATION_MOTOR
  digitalWrite(VIBRATION_PIN, LOW);
  alarmVibrationUntilMs = 0;
#endif
  Serial.println(">>> [ALARM] Titresim durduruldu <<<");
}

// Ana döngüden çağrılır: titreşimi süresi dolunca durdurur, saniyede bir alarmı kontrol eder
void checkAlarmMotor() {
#if USE_VIBRATION_MOTOR
  if (alarmVibrationUntilMs > 0 && (long)(millis() - alarmVibrationUntilMs) >= 0) {
    digitalWrite(VIBRATION_PIN, LOW);
    alarmVibrationUntilMs = 0;
  }
#endif

  static unsigned long lastRtcCheckMs = 0;
  if (millis() - lastRtcCheckMs < 1000) return;
  lastRtcCheckMs = millis();
  if (!rtcSynced || !activeAlarm.enabled || !activeAlarm.vibrate) return;

  time_t nowSec;
  time(&nowSec);
  struct tm timeinfo;
  localtime_r(&nowSec, &timeinfo);
  if (timeinfo.tm_hour == activeAlarm.hour && timeinfo.tm_min == activeAlarm.minute &&
      (!alarmHasFired || millis() - lastAlarmFireMs >= ALARM_REARM_MS)) {
    alarmHasFired = true;
    lastAlarmFireMs = millis();
    triggerAlarmVibration(5000);
    Serial.printf(">>> [OTONOM ALARM] PC baglantisi olmadan bileklik alarm caldi! Saat: %02d:%02d <<<\n",
                  timeinfo.tm_hour, timeinfo.tm_min);
  }
}

// Alarm karakteristiğine yazılan komutlar:
//   NONCE:<16 hex>  oturum nonce'u (her bağlantıda)
//   TIME:HH:MM:SS   bileklik saatini eşitle
//   RING / TEST     hemen titret,  STOP  titreşimi durdur
//   HH:MM[:V[:E]]   alarm kur (V: titreşim 0/1, E: açık 0/1; varsayılan 1)
class AlarmCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();   // ESP32 core 2.x (std::string) ve 3.x (String) uyumlu
    if (value.length() == 0) return;

    if (value.startsWith("NONCE:")) {
      uint8_t tmp[NONCE_LEN];
      if (parseHex(value.c_str() + 6, value.length() - 6, tmp, NONCE_LEN)) {
        memcpy(session_nonce, tmp, NONCE_LEN);
        Serial.println(">>> [GUVENLIK] Oturum nonce'u alindi <<<");
      }
      return;
    }

    if (value.startsWith("TIME:")) {
      int th = 0, tm = 0, ts = 0;
      if (sscanf(value.c_str(), "TIME:%d:%d:%d", &th, &tm, &ts) >= 2 &&
          th >= 0 && th < 24 && tm >= 0 && tm < 60 && ts >= 0 && ts < 60) {
        // Yalnızca günün saati kullanılır; tarih sabit ve önemsizdir.
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_hour = th;
        t.tm_min = tm;
        t.tm_sec = ts;
        t.tm_year = 126;   // 2026-01-01
        t.tm_mon = 0;
        t.tm_mday = 1;
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        rtcSynced = true;
        Serial.printf(">>> [RTC] Bileklik dahili saati esitlendi: %02d:%02d:%02d <<<\n", th, tm, ts);
      }
      return;
    }

    if (value == "RING" || value == "TEST") {
      triggerAlarmVibration(3000);
      return;
    }
    if (value == "STOP") {
      stopAlarmVibration();
      return;
    }

    int h = 0, m = 0, v = 1, e = 1;
    if (sscanf(value.c_str(), "%d:%d:%d:%d", &h, &m, &v, &e) < 2 ||
        h < 0 || h > 23 || m < 0 || m > 59 || (v != 0 && v != 1) || (e != 0 && e != 1)) {
      Serial.println(">>> [BLE ALARM] Gecersiz komut yok sayildi <<<");
      return;
    }
    if (h != activeAlarm.hour || m != activeAlarm.minute) alarmHasFired = false;   // yeni saat: kurulabilir
    activeAlarm.hour = (uint8_t)h;
    activeAlarm.minute = (uint8_t)m;
    activeAlarm.vibrate = (uint8_t)v;
    activeAlarm.enabled = (uint8_t)e;

    Serial.printf(">>> [BLE ALARM] Senkronize edildi: %02d:%02d | Titresim: %s | Durum: %s\n",
                  activeAlarm.hour, activeAlarm.minute, activeAlarm.vibrate ? "Aktif" : "Sessiz",
                  activeAlarm.enabled ? "Acik" : "Kapali");

    // PC'ye onay
    char ack[16];
    snprintf(ack, sizeof(ack), "OK:%02d:%02d:%d", activeAlarm.hour, activeAlarm.minute, activeAlarm.enabled);
    pCharacteristic->setValue((uint8_t*)ack, strlen(ack));
    pCharacteristic->notify();
  }
};
#endif  // USE_BLE

// ============================================================
// I2C TARAYICI (sensör bulunamazsa hattaki adresleri listeler)
// ============================================================
void scanI2C() {
  Serial.println("    I2C hatti taraniyor...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.print("      + Cihaz bulundu: 0x");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("      - Hicbir I2C cihazi yok. SDA/SCL pinlerini ve");
    Serial.println("        3.3V/GND baglantisini kontrol et.");
  } else {
    Serial.println("    Yukaridaki adresi ADS_ADDRESS satirina yaz (MAX30102 = 0x57).");
  }
}

// ============================================================
// EKG SİNYAL İŞLEME
// ============================================================
// 2. derece IIR filtre (Direct Form I)
float applyIIR(float input, const float b[3], const float a[3], float x[3], float y[3]) {
  x[0] = input;
  y[0] = b[0]*x[0] + b[1]*x[1] + b[2]*x[2] - a[1]*y[1] - a[2]*y[2];
  x[2] = x[1];  x[1] = x[0];
  y[2] = y[1];  y[1] = y[0];
  return y[0];
}

// Filtreler ilk örnekte (açılış ya da bileklik yeniden takıldığında) ham değerle
// başlatılır. Aksi halde ~13000'lik DC seviyesi yüksek geçiren filtrede 1-2 sn süren
// büyük bir geçici sıçrama yaratır ve ilk R tepeleri kaybolur.
bool filtersPrimed = false;

#if USE_BLE
// Bağlantı aralığından EKG bandına düşen parazit frekanslarını hesaplar.
// Telsiz 1/aralık frekansında açılır; 200 Hz örneklemede harmonikler katlanarak
// (aliasing) 5-45 Hz bandına düşebilir. Aynı hesap PC tarafında da yapılır.
int computeBleNotchFreqs(uint16_t units, float out[BLE_NOTCH_MAX]) {
  if (units < 6 || units > 3200) return 0;   // BLE standardındaki geçerli aralık
  const float fs = (float)SAMPLE_RATE;
  float f0 = 800.0f / units;                 // 1 / (units x 1,25 ms)
  int n = 0;
  for (int k = 1; k <= BLE_NOTCH_KMAX && n < BLE_NOTCH_MAX; k++) {
    float f = fmodf(k * f0, fs);
    if (f > fs / 2) f = fs - f;              // Nyquist'in üstü katlanır
    if (f < BLE_NOTCH_MIN_HZ || f > BLE_NOTCH_MAX_HZ) continue;
    bool dup = false;
    for (int j = 0; j < n; j++) if (fabsf(out[j] - f) < 0.5f) dup = true;
    if (!dup) out[n++] = f;
  }
  return n;
}

// 2. derece notch tasarımı (scipy.signal.iirnotch ile aynı katsayılar)
void designNotch(float f, float q, float b[3], float a[3]) {
  float w0 = 2.0f * (float)M_PI * f / (float)SAMPLE_RATE;
  float beta = tanf(w0 / q / 2.0f);
  float g = 1.0f / (1.0f + beta);
  b[0] = g; b[1] = -2.0f * g * cosf(w0); b[2] = g;
  a[0] = 1.0f; a[1] = -2.0f * g * cosf(w0); a[2] = 2.0f * g - 1.0f;
}

// Bağlantı aralığı değiştiyse notch'ları yeniden ayarla (ana döngüde, örnekler arasında)
void updateBleNotches() {
  uint16_t u = bleConnIntervalUnits;
  if (u == bleNotchUnits) return;
  bleNotchUnits = u;
  bleNotchCount = computeBleNotchFreqs(u, bleNotchHz);
  for (int i = 0; i < bleNotchCount; i++) {
    designNotch(bleNotchHz[i], BLE_NOTCH_Q, bleNotchB[i], bleNotchA[i]);
    for (int j = 0; j < 3; j++) { bleNotchX[i][j] = 0; bleNotchY[i][j] = 0; }
  }
  if (u) {
    Serial.printf("[BLE] Baglanti araligi %.1f ms -> parazit filtresi:", u * 1.25f);
    if (!bleNotchCount) Serial.print(" gerekmiyor (EKG bandi disinda)");
    for (int i = 0; i < bleNotchCount; i++) Serial.printf(" %.2f", bleNotchHz[i]);
    Serial.println(bleNotchCount ? " Hz" : "");
  }
}
#endif

// Ham örnek -> yüksek geçiren -> 50 Hz notch -> alçak geçiren -> (BLE notch'ları)
float processSignal(float rawValue) {
  if (!filtersPrimed) {
    for (int i = 0; i < 3; i++) {
      hpf_x[i] = rawValue; hpf_y[i] = 0;
      notch_x[i] = 0; notch_y[i] = 0;
      lpf_x[i] = 0; lpf_y[i] = 0;
    }
#if USE_BLE
    for (int k = 0; k < BLE_NOTCH_MAX; k++)
      for (int i = 0; i < 3; i++) { bleNotchX[k][i] = 0; bleNotchY[k][i] = 0; }
#endif
    filtersPrimed = true;
  }
  float s1 = applyIIR(rawValue, HPF_B, HPF_A, hpf_x, hpf_y);
  float s2 = applyIIR(s1, NOTCH_B, NOTCH_A, notch_x, notch_y);
  float s3 = applyIIR(s2, LPF_B, LPF_A, lpf_x, lpf_y);
#if USE_BLE
  for (int k = 0; k < bleNotchCount; k++)
    s3 = applyIIR(s3, bleNotchB[k], bleNotchA[k], bleNotchX[k], bleNotchY[k]);
#endif
  return s3;
}

// Uyarlamalı eşikli R tepesi algılama: tepe zarfının %45'i aşılıp tekrar
// altına inildiğinde bir atım sayılır; 300 ms içinde ikinci atım kabul edilmez.
bool detectRPeak(float filteredValue) {
  samplesSincePeak++;
  float absVal = fabs(filteredValue);

  if (absVal > signalPeak) signalPeak = 0.875f * signalPeak + 0.125f * absVal;
  else                     signalPeak *= 0.998f;

  peakThreshold = signalPeak * 0.45f;
  if (samplesSincePeak < REFRACTORY_SAMPLES) return false;

  if (absVal > peakThreshold && !isPeakRising) isPeakRising = true;
  if (isPeakRising && absVal < peakThreshold * 0.6f) {
    isPeakRising = false;
    samplesSincePeak = 0;
    return true;
  }
  return false;
}

// MAX30102 yoksa nabız EKG'den hesaplanır (son 8 R-R aralığının ortalaması)
void updateBPM_ECG() {
  unsigned long now = millis();
  if (lastECGPeakTimeMs > 0) {
    unsigned long rr = now - lastECGPeakTimeMs;
    if (rr >= 300 && rr <= 2000) {   // 200 - 30 BPM
      ecg_rr_buffer[ecg_rr_idx] = rr;
      ecg_rr_idx = (ecg_rr_idx + 1) % 8;
      if (ecg_rr_count < 8) ecg_rr_count++;

      unsigned long sum = 0;
      for (int i = 0; i < ecg_rr_count; i++) sum += ecg_rr_buffer[i];
      float calculatedBPM = 60000.0f / ((float)sum / (float)ecg_rr_count);

      if (currentBPM < 35.0f || currentBPM > 220.0f) {
        currentBPM = calculatedBPM;
      } else {
        currentBPM = 0.70f * currentBPM + 0.30f * calculatedBPM;
      }

#if USE_BLE
      if (deviceConnected && pCharacteristicBPM != NULL) {
        uint8_t bpmInt = (uint8_t)round(currentBPM);
        pCharacteristicBPM->setValue(&bpmInt, 1);
        pCharacteristicBPM->notify();
      }
#endif
    }
  }
  lastECGPeakTimeMs = now;
}

// ============================================================
// MAX30102: CANLILIK + NABIZ + SpO2
// ============================================================
void processPPG(unsigned long nowMs, long irValue, long redValue) {

  // 1. Bileklik takılı mı? Kısa kesintiler yok sayılır: en az 15 örnek (~600 ms)
  //    boyunca IR eşiğin altında kalırsa bileklik çıkarılmış kabul edilir.
  static int detachCounter = 0;
  if (irValue < PPG_LIVENESS_THRESHOLD) {
    detachCounter++;
    if (detachCounter >= 15) {
      detachCounter = 15;
      isWristAttached = false;
      currentBPM = 0;
      currentSpO2 = 0;
      ppg_peak_env = 40.0f;
      ppg_is_rising = false;
      ppg_rr_count = 0;

      // PC ekranı hemen sıfırlansın diye nabız ve SpO2 için 0 bildirilir
      static unsigned long lastZeroNotifyMs = 0;
      if (nowMs - lastZeroNotifyMs >= 400) {
        lastZeroNotifyMs = nowMs;
#if USE_BLE
        if (deviceConnected) {
          uint8_t zero = 0;
          if (pCharacteristicBPM != NULL) {
            pCharacteristicBPM->setValue(&zero, 1);
            pCharacteristicBPM->notify();
          }
          if (pCharacteristicSPO2 != NULL) {
            pCharacteristicSPO2->setValue(&zero, 1);
            pCharacteristicSPO2->notify();
          }
        }
#endif
      }
    }
    return;
  }
  detachCounter = 0;

  // Bileklik yeni takıldı: taban hattını ve ölçüm durumunu sıfırdan başlat
  if (!isWristAttached) {
    isWristAttached = true;
    validSpo2Samples = 0;
    ppg_ir_dc = (float)irValue;
    ppg_red_dc = (float)redValue;
    ppg_ir_ac_smooth = 0;
    ppg_red_ac_smooth = 0;
    ppg_peak_env = 40.0f;
    ppg_is_rising = false;
    ppg_rr_count = 0;
    ppg_rr_idx = 0;
    currentBPM = 0;
    lastPPGPeakTimeMs = nowMs;
    return;
  }

  // Ani büyük seviye değişiminde (bileklik kaydı) taban hattı hızla yakalanır
  if (fabs((float)irValue - ppg_ir_dc) > 50000.0f) {
    ppg_ir_dc = 0.5f * ppg_ir_dc + 0.5f * (float)irValue;
  }

  // 2. DC taban hattı ve AC nabız dalgası
  ppg_ir_dc  = 0.93f * ppg_ir_dc  + 0.07f * (float)irValue;
  ppg_red_dc = 0.93f * ppg_red_dc + 0.07f * (float)redValue;

  float ir_ac_raw  = (float)irValue  - ppg_ir_dc;
  float red_ac_raw = (float)redValue - ppg_red_dc;

  // AC sinyali hafifçe yumuşatılır (gürültü bastırılır, nabız dalgası korunur)
  ppg_ir_ac_smooth  = 0.70f * ppg_ir_ac_smooth  + 0.30f * ir_ac_raw;
  ppg_red_ac_smooth = 0.70f * ppg_red_ac_smooth + 0.30f * red_ac_raw;

  // 3. Nabız: uyarlamalı eşikli tepe algılama
  if (ppg_ir_ac_smooth > ppg_peak_env) {
    ppg_peak_env = 0.80f * ppg_peak_env + 0.20f * ppg_ir_ac_smooth;
    if (ppg_peak_env > 400.0f) ppg_peak_env = 400.0f;
  } else {
    ppg_peak_env *= 0.98f;
    if (ppg_peak_env < 14.0f) ppg_peak_env = 14.0f;
  }

  float thresh = ppg_peak_env * 0.44f;
  if (thresh < 6.0f) thresh = 6.0f;   // zayıf temasta da atımlar kaçırılmasın

  unsigned long timeSinceLastPeak = nowMs - lastPPGPeakTimeMs;

  // Eşik aşılınca tepe adayı izlenir, sinyal eşiğin %60'ının altına inince atım kesinleşir.
  // Fizyolojik aralık: 380 ms (158 BPM) - 1600 ms (37 BPM)
  if (!ppg_is_rising && ppg_ir_ac_smooth > thresh && timeSinceLastPeak > 380) {
    ppg_is_rising = true;
    ppg_peak_candidate = ppg_ir_ac_smooth;
  } else if (ppg_is_rising) {
    if (ppg_ir_ac_smooth > ppg_peak_candidate) {
      ppg_peak_candidate = ppg_ir_ac_smooth;
    } else if (ppg_ir_ac_smooth < thresh * 0.60f) {
      ppg_is_rising = false;

      if (timeSinceLastPeak >= 380 && timeSinceLastPeak <= 1600) {
        unsigned long rr = timeSinceLastPeak;

        // Mevcut nabızdan çok sapan aralık (hareket, temas kaybı) atılır
        bool validRR = true;
        if (ppg_rr_count >= 3 && currentBPM >= 45) {
          float expectedRR = 60000.0f / currentBPM;
          if (rr < expectedRR * 0.55f || rr > expectedRR * 1.80f) {
            validRR = false;
          }
        }

        if (validRR) {
          ppg_rr_buffer[ppg_rr_idx] = rr;
          ppg_rr_idx = (ppg_rr_idx + 1) % 5;
          if (ppg_rr_count < 5) ppg_rr_count++;

          // En az 3 temiz atım (~2,5-3 sn) toplanınca son aralıkların medyanı kullanılır
          if (ppg_rr_count >= 3) {
            unsigned long sortedRR[5];
            for (int i = 0; i < ppg_rr_count; i++) sortedRR[i] = ppg_rr_buffer[i];
            for (int i = 0; i < ppg_rr_count - 1; i++) {
              for (int j = i + 1; j < ppg_rr_count; j++) {
                if (sortedRR[j] < sortedRR[i]) {
                  unsigned long t = sortedRR[i];
                  sortedRR[i] = sortedRR[j];
                  sortedRR[j] = t;
                }
              }
            }
            unsigned long medRR = sortedRR[ppg_rr_count / 2];
            float calculatedBPM = 60000.0f / (float)medRR;

            // Ani sıçramaları önlemek için yumuşak geçiş
            if (currentBPM < 35.0f || currentBPM > 180.0f) {
              currentBPM = calculatedBPM;
            } else {
              currentBPM = 0.70f * currentBPM + 0.30f * calculatedBPM;
            }

#if USE_BLE
            if (deviceConnected && pCharacteristicBPM != NULL) {
              uint8_t bpmInt = (uint8_t)round(currentBPM);
              pCharacteristicBPM->setValue(&bpmInt, 1);
              pCharacteristicBPM->notify();
            }
#endif

#if LED_PIN >= 0
            digitalWrite(LED_PIN, HIGH);
#endif
            ledState = true;
            ledOffTime = nowMs + 60;
          }
        }
      }
      lastPPGPeakTimeMs = nowMs;
    }
  }

  // 4. SpO2: kırmızı ve IR ışığın AC/DC oranlarından R = (AC_red/DC_red) / (AC_ir/DC_ir)
  spo2_ir_ac_rms  = 0.94f * spo2_ir_ac_rms  + 0.06f * fabs(ir_ac_raw);
  spo2_red_ac_rms = 0.94f * spo2_red_ac_rms + 0.06f * fabs(red_ac_raw);

  if (nowMs - lastSpO2Calc >= SPO2_CALC_INTERVAL_MS) {
    lastSpO2Calc = nowMs;
    if (ppg_ir_dc > 4000 && ppg_red_dc > 4000 && spo2_ir_ac_rms > 3.0f) {
      float R = (spo2_red_ac_rms / ppg_red_dc) / (spo2_ir_ac_rms / ppg_ir_dc);
      if (R >= 0.40f && R <= 2.00f) {
        validSpo2Samples++;
        // NOT: Doğrusal yaklaşım referans oksimetreyle kalibre edilmemiştir; değerler
        // yalnızca gösterge amaçlıdır, tıbbi amaçla kullanılamaz.
        float spo2 = 104.0f - 7.0f * R;
        if (spo2 > 100.0f) spo2 = 100.0f;
        if (spo2 < 70.0f) spo2 = 70.0f;

        // İlk iki ölçüm döngüsünden sonra gönderilir; o zamana kadar 0 (ölçülüyor) kalır
        if (validSpo2Samples >= 2) {
          if (currentSpO2 == 0) {
            currentSpO2 = (uint8_t)round(spo2);
          } else {
            currentSpO2 = (uint8_t)round(0.75f * (float)currentSpO2 + 0.25f * spo2);
          }

#if USE_BLE
          if (deviceConnected && pCharacteristicSPO2 != NULL) {
            pCharacteristicSPO2->setValue(&currentSpO2, 1);
            pCharacteristicSPO2->notify();
          }
#endif
        }
      }
    }
  }

  // 5. Saniyede bir güncel nabız/SpO2 bildirimi ve seri port izleme satırı
  static unsigned long lastStreamBLEMs = 0;
  if (nowMs - lastStreamBLEMs >= 1000) {
    lastStreamBLEMs = nowMs;
#if USE_BLE
    if (deviceConnected && isWristAttached) {
      if (currentBPM >= 40 && pCharacteristicBPM != NULL) {
        uint8_t bpmInt = (uint8_t)round(currentBPM);
        pCharacteristicBPM->setValue(&bpmInt, 1);
        pCharacteristicBPM->notify();
      }
      if (currentSpO2 >= 70 && pCharacteristicSPO2 != NULL) {
        pCharacteristicSPO2->setValue(&currentSpO2, 1);
        pCharacteristicSPO2->notify();
      }
    }
#endif
    Serial.printf("[PPG] IR:%ld Red:%ld BPM:%d SpO2:%d Temas:%s\n",
                  irValue, redValue, (int)currentBPM, (int)currentSpO2,
                  isWristAttached ? "VAR" : "YOK");
  }
}

// MAX30102 FIFO'sundaki hazır örnekleri bloklamadan okur. (Kütüphanenin getIR()/getRed()
// fonksiyonları yeni örnek gelene kadar beklediği için 200 Hz EKG örneklemesini bozar.)
void pollPPG(unsigned long nowMs) {
  if (!ppgConnected) return;
  particleSensor.check();
  while (particleSensor.available()) {
    long irValue  = (long)particleSensor.getFIFOIR();
    long redValue = (long)particleSensor.getFIFORed();
    particleSensor.nextSample();
    processPPG(nowMs, irValue, redValue);
  }
}

// ============================================================
// ANTI-REPLAY PAKET SAYACI (NVS'te kalıcı)
// ============================================================
// NVS'e her pakette yazmak flash'ı yıpratır; bu yüzden COUNTER_SAVE_EVERY pakette bir
// yazılır ve açılışta kayıtlı değerin COUNTER_BOOT_GAP üstünden devam edilir.
void loadCounter() {
  prefs.begin("ritim_el", false);
  uint32_t saved = prefs.getUInt("ctr", 0);
  uint32_t boot  = prefs.getUInt("boot", 0) + 1;

  // Taşmaya yaklaşıldıysa baştan başla (PC yeni bağlantıda sayacı sıfırdan kabul eder)
  if (saved > (0xFFFFFFFFUL - COUNTER_BOOT_GAP * 4)) saved = 0;

  packet_counter = saved + COUNTER_BOOT_GAP;
  counter_last_saved = packet_counter;

  prefs.putUInt("ctr", packet_counter);
  prefs.putUInt("boot", boot);
  prefs.end();

  Serial.print("Boot #"); Serial.print(boot);
  Serial.print("  sayac baslangici: "); Serial.println(packet_counter);
}

void maybeSaveCounter() {
  if (packet_counter - counter_last_saved < COUNTER_SAVE_EVERY) return;
  counter_last_saved = packet_counter;
  prefs.begin("ritim_el", false);
  prefs.putUInt("ctr", packet_counter);
  prefs.end();
}

// ============================================================
// DERİN UYKU
// ============================================================
bool wokeFromSleep = false;
bool serialReady = false;      // hızlı uyanış kontrolünde Serial başlatılmaz

// MAX30102'yi yakınlık moduna al: 50 Hz'de yalnızca kısa darbeli pilot LED yanar.
// Deri algılanınca INT LOW olur (ESP32 uyanır) ve sensör kendiliğinden ölçüme geçer.
// Not: Yakınlık modu MAX30101/30105 veri sayfalarında yer alır; MAX30102'de de
// çalıştığı bildirilmektedir. Çalışmazsa yedek zamanlayıcı devreye girer (bkz. SleepMode).
void maxArmProximity() {
  particleSensor.setup(0x3F, 1, 2, 50, 69, 4096);   // ölçümle aynı ADC aralığı: eşik aynı anlamda
  particleSensor.setPulseAmplitudeProximity(PROX_LED_PA);
  particleSensor.setProximityThreshold(PROX_THRESHOLD);
  particleSensor.enablePROXINT();
  particleSensor.getINT1();                          // bekleyen kesmeleri temizle: INT bırakılır
  particleSensor.getINT2();
  particleSensor.setLEDMode(0x03);                   // MODE yeniden yazılınca yakınlık modu başlar
}

// Uyanışta bileklik gerçekten takılı mı? BLE başlatılmadan, normal LED gücüyle birkaç
// IR örneği alınır (100 Hz'de ~30 ms). 3 örnekten 2'si canlılık eşiğini geçmeli.
bool quickWristCheck() {
  particleSensor.setup(0x3F, 1, 2, 100, 411, 4096);
  int seen = 0, high = 0;
  unsigned long t0 = millis();
  while (seen < 3 && millis() - t0 < 200) {
    particleSensor.check();
    while (particleSensor.available() && seen < 3) {
      if ((long)particleSensor.getFIFOIR() >= PPG_LIVENESS_THRESHOLD) high++;
      seen++;
      particleSensor.nextSample();
    }
    if (seen < 3) delay(5);
  }
  return high >= 2;
}

// Derin uykuya gir (geri dönmez). Uyanma kaynakları: MAX30102 INT (ext0), USB (ext1), zamanlayıcı.
void enterDeepSleep() {
  uint8_t mode = chooseSleepMode();
  if (mode == SLEEP_BACKOFF) sleepRtc.falseRow = 0;   // bu turdan sonra kesme yeniden denenir
#if LED_PIN >= 0
  digitalWrite(LED_PIN, LOW);
#endif
#if USE_VIBRATION_MOTOR
  digitalWrite(VIBRATION_PIN, LOW);
#endif
#if AD8232_SDN_PIN >= 0
  // SDN'de pull-up olduğundan uykuda bırakılan pin AD8232'yi açar (~170 uA): LOW'da tutulur
  pinMode(AD8232_SDN_PIN, OUTPUT);
  digitalWrite(AD8232_SDN_PIN, LOW);
  ad8232On = false;
  gpio_hold_en((gpio_num_t)AD8232_SDN_PIN);
  gpio_deep_sleep_hold_en();
#endif
#if MAX_INT_PIN >= 0
  if (mode == SLEEP_PROX) {
    maxArmProximity();
    if (digitalRead(MAX_INT_PIN) == LOW) mode = SLEEP_BACKOFF;   // INT hâlâ LOW: hemen uyanırdık
  }
#endif
  if (mode != SLEEP_PROX) {
    particleSensor.shutDown();                       // ~1 uA; uyanınca hızlı kontrol yeniden kurar
    particleSensor.getINT1();
    particleSensor.getINT2();
  }
  esp_sleep_enable_timer_wakeup((uint64_t)(mode == SLEEP_POLL ? SLEEP_POLL_S : SLEEP_CHECK_S) * 1000000ULL);
#if MAX_INT_PIN >= 0
  if (mode == SLEEP_PROX) esp_sleep_enable_ext0_wakeup((gpio_num_t)MAX_INT_PIN, 0);
#endif
#if USB_DET_PIN >= 0
  // USB takılınca uyan. Şu an zaten HIGH ise kaynak eklenmez (hemen uyanırdık).
  if (digitalRead(USB_DET_PIN) == LOW)
    esp_sleep_enable_ext1_wakeup(1ULL << USB_DET_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
#endif
  sleepRtc.lastMode = mode;
  sleepRtc.sleeps++;
  if (serialReady) {
    Serial.printf("[GUC] Derin uyku (%s). Bileklik takilinca uyanir.\n", sleepModeName(mode));
    Serial.flush();
  }
  esp_deep_sleep_start();
}

// setup()'ın başında çağrılır. Derin uykudan uyanıldıysa bileklik takılı mı bakılır;
// takılı değilse (ve USB yoksa) BLE hiç başlatılmadan tekrar uyunur.
// Hızlı kontrolde NVS'e yazılmaz; anti-replay sayacı yalnızca tam açılışta ilerler.
void handleWakeFromDeepSleep() {
#if AD8232_SDN_PIN >= 0
  pinMode(AD8232_SDN_PIN, OUTPUT);
  digitalWrite(AD8232_SDN_PIN, LOW);
  gpio_hold_dis((gpio_num_t)AD8232_SDN_PIN);         // uykuda tutulan LOW serbest bırakılır
#endif
  if (sleepRtc.magic != SLEEP_MAGIC) {               // güç yeni geldi: RTC belleği geçersiz
    memset(&sleepRtc, 0, sizeof(sleepRtc));
    sleepRtc.magic = SLEEP_MAGIC;
  }
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause != ESP_SLEEP_WAKEUP_EXT0 && cause != ESP_SLEEP_WAKEUP_EXT1 && cause != ESP_SLEEP_WAKEUP_TIMER)
    return;                                          // normal açılış (güç, reset, yükleme)
  wokeFromSleep = true;
  bool usb = false;
#if USB_DET_PIN >= 0
  pinMode(USB_DET_PIN, INPUT);
  usb = digitalRead(USB_DET_PIN) == HIGH;
#endif
  if (usb) return;                                   // şarja takıldı: tam açılış (USB varken uyunmaz)

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) return;   // sensör yok: tam açılış, hata görünsün
  bool worn = quickWristCheck();
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    if (worn) { sleepRtc.proxWakes++; sleepRtc.proxMisses = 0; sleepRtc.falseRow = 0; }
    else      { sleepRtc.falseWakes++; sleepRtc.falseRow++; }
  } else if (worn && sleepRtc.lastMode == SLEEP_PROX) {
    sleepRtc.missWakes++;                            // kesme gelmedi ama bileklik takılıydı
    sleepRtc.proxMisses++;
  }
  if (!worn) enterDeepSleep();                       // yersiz uyanma: tekrar uyu
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  handleWakeFromDeepSleep();          // yersiz uyanışta burada tekrar uyunur
  Serial.begin(115200);
  serialReady = true;
  delay(200);
  Serial.printf("Islemci: %u MHz\n", (unsigned)getCpuFrequencyMhz());
  if (wokeFromSleep) Serial.println("[GUC] Derin uykudan uyanildi: bileklik takili (ya da USB)");

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#endif

#if USE_VIBRATION_MOTOR
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);
#endif

  // AD8232, BLE bağlantısı kurulup bileklik takılana kadar kapalı
#if AD8232_SDN_PIN >= 0
  pinMode(AD8232_SDN_PIN, OUTPUT);
  digitalWrite(AD8232_SDN_PIN, LOW);
#endif
#if USB_DET_PIN >= 0
  pinMode(USB_DET_PIN, INPUT);        // seviyeyi dış bölücü belirler (iç direnç yok)
#endif
#if VBAT_SENSE_PIN >= 0
  analogSetPinAttenuation(VBAT_SENSE_PIN, ADC_11db);   // pil/2 <= 2,1 V
#endif

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);
  Wire.setClock(100000);
  Serial.print("I2C: SDA GPIO"); Serial.print(I2C_SDA_PIN);
  Serial.print("  SCL GPIO"); Serial.println(I2C_SCL_PIN);

#if USE_ADS1115
  bool adsFound = false;
  for (int retry = 0; retry < 3; retry++) {
    if (ads.begin(ADS_ADDRESS)) {
      adsFound = true;
      break;
    }
    delay(60);
  }

  if (adsFound) {
    ads.setDataRate(RATE_ADS1115_860SPS);   // 860 SPS: tek dönüşüm ~1,2 ms, 5 ms'lik örnek süresine sığar
    ads.setGain(ADS_GAIN);
    Serial.printf("ADS1115 hazir (adres 0x%02X, kanal A%d)\n", ADS_ADDRESS, ADS_CHANNEL);
  } else {
    Serial.println("UYARI: ADS1115 bulunamadi! EKG verisi gecersiz olacak.");
    scanI2C();
  }
#else
  analogReadResolution(12);
  analogSetPinAttenuation(ECG_ADC_PIN, ADC_11db);
  Serial.print("Dogrudan ADC okuma: GPIO"); Serial.println(ECG_ADC_PIN);
  if (ECG_ADC_PIN < 32) {
    Serial.println("!!! UYARI: BLE acikken sadece ADC1 pinleri (32-39) calisir !!!");
  }
#endif

  // MAX30102 her iki EKG okuma modunda da canlılık için gereklidir
  Serial.println("PPG Sensoru (MAX30102) baslatiliyor...");
  bool maxFound = false;
  for (int retry = 0; retry < 3 && !maxFound; retry++) {
    maxFound = particleSensor.begin(Wire, I2C_SPEED_FAST);
    if (!maxFound) delay(60);
  }
  if (!maxFound) {
    Serial.println("!!! HATA: MAX30102 BULUNAMADI !!! (I2C Adres 0x57)");
    scanI2C();
    ppgConnected = false;
#if REQUIRE_PPG_LIVENESS
    // Canlılık doğrulanamıyorsa EKG gönderilmez
    isWristAttached = false;
    Serial.println("!!! Canlilik sensoru olmadan EKG GONDERILMEYECEK (REQUIRE_PPG_LIVENESS 1) !!!");
#else
    isWristAttached = true;   // geliştirme modu: PPG olmadan EKG akışına izin ver
#endif
  } else {
    // ledMode 2 = yalnızca kırmızı + IR (MAX30102 iki LED'lidir; 3 yapılırsa FIFO hizası bozulur).
    // sampleRate 100 + sampleAverage 4 -> FIFO'ya saniyede 25 örnek; pollPPG() 40 ms'de bir okur.
    particleSensor.setup(0x3F, 4, 2, 100, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x3F);
    particleSensor.setPulseAmplitudeIR(0x3F);
    particleSensor.getINT1();   // açılış (PWR_RDY) kesmesini temizle: INT bırakılsın
    particleSensor.getINT2();
    ppgConnected = true;
    Serial.println("MAX30102 PPG Basariyla baglandi! (Red+IR 100Hz, 4'lu ortalama)");
  }

  loadCounter();
  loadDeviceKey();
  Serial.print("Eslestirme: ");
  if (devicePaired) { Serial.print("VAR (anahtar kimligi "); printKeyId(); Serial.println(")"); }
  else Serial.println("YOK - bilekligi USB ile PC uygulamasindan eslestirin");

#if USE_BLE
  #if BLE_TX_POWER_LOW
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N12);
    Serial.println("BLE telsiz gucu DUSUK moda alindi.");
  #endif
  BLEDevice::init("RitimEl Bileklik");
  BLEDevice::setCustomGapHandler(bleGapHandler);   // bağlantı aralığı güncellemeleri
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristicECG = pService->createCharacteristic(
      CHAR_UUID_ECG, BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristicECG->addDescriptor(new BLE2902());

  pCharacteristicBPM = pService->createCharacteristic(
      CHAR_UUID_BPM,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pCharacteristicBPM->addDescriptor(new BLE2902());

  pCharacteristicSPO2 = pService->createCharacteristic(
      CHAR_UUID_SPO2,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  pCharacteristicSPO2->addDescriptor(new BLE2902());

  pCharacteristicAlarm = pService->createCharacteristic(
      CHAR_UUID_ALARM,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristicAlarm->setCallbacks(new AlarmCallback());
  pCharacteristicAlarm->addDescriptor(new BLE2902());

  pService->start();

  // Standart BLE Pil Servisi. Ana servisten sonra eklenir ki ana servisin
  // tutamaçları (handle) önceki sürümlerle aynı kalsın.
  BLEService* pBatService = pServer->createService(BLEUUID((uint16_t)0x180F));
  pCharacteristicBatLevel = pBatService->createCharacteristic(
      BLEUUID((uint16_t)0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristicBatLevel->addDescriptor(new BLE2902());
  pCharacteristicBatState = pBatService->createCharacteristic(
      BLEUUID((uint16_t)0x2A1A),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristicBatState->addDescriptor(new BLE2902());
  pBatService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x0);
  setAdvInterval(false);
  BLEDevice::startAdvertising();
  advFastUntilMs = millis() + ADV_FAST_WINDOW_MS;   // açılışta 30 sn hızlı, sonra yavaş reklam

#else
  Serial.println();
  Serial.println("### TANI MODU: BLE KAPALI ###");
  Serial.println("### Veri seri porttan akiyor (satir basina 1 ornek) ###");
  Serial.println("### Arduino Seri Cizici ile izlenebilir ###");
  Serial.println("#BASLA");
#endif
  Serial.println("========================================");
#if USE_BLE
  Serial.println("  RITIM-EL v" FW_VERSION " - BLE hazir");
  Serial.println("  Bilgisayardan baglanti bekleniyor...");
#else
  Serial.println("  RITIM-EL v" FW_VERSION " - TANI MODU (BLE kapali)");
#endif
  Serial.println("  (Optik Canlilik Tespiti AKTIF)");
  Serial.println("========================================");

  updateBattery(millis(), true);
  delay(500);
  lastSampleTick = xTaskGetTickCount();
}

// ============================================================
// EKG PAKETİ GÖNDERİMİ
// ============================================================
#if USE_BLE
void sendEcgPacket() {
  uint8_t packet[32] = {0};
  if (devicePaired) buildAuthenticatedPacket(ecgBuffer, packet_counter, packet);
  else              buildDevPacket(ecgBuffer, packet_counter, packet);
  pCharacteristicECG->setValue(packet, 32);
  pCharacteristicECG->notify();
  packet_counter++;
  maybeSaveCounter();
}
#endif

// ============================================================
// LOOP — her tur bir EKG örneği (200 Hz)
// ============================================================
void loop() {
  // Sıradaki örnek anına (5 ms) kadar işlemci FreeRTOS'ta uyur. Bekleme tik saatine
  // kilitli olduğundan 200 Hz kaymaz; bir tur uzun sürerse (NVS yazımı vb.) beklemeden yetişir.
  xTaskDelayUntil(&lastSampleTick, SAMPLE_TICKS);
  totalSampleCount++;

  handleSerialCommands();   // USB eşleştirme komutları (bloklamaz)
  unsigned long currentMillis = millis();
  updateBattery(currentMillis, false);
#if USE_BLE
  checkAlarmMotor();

  // Bağlantı durumu bu tur boyunca tek bir değerden okunur (BLE görevi değiştirebilir)
  bool connected = deviceConnected;
  if (!connected && oldDeviceConnected) {
    oldDeviceConnected = false;
    advSlow = false;
    advFastUntilMs = currentMillis + ADV_FAST_WINDOW_MS;
    readvertisePending = true;            // reklam 500 ms sonra döngüden yeniden başlatılır
    disconnectedAtMs = currentMillis;
  }
  if (connected && !oldDeviceConnected) {
    oldDeviceConnected = true;
    bufferIndex = 0;
    readvertisePending = false;
    advSlow = false;
    setAdvInterval(false);                // bir sonraki kopmada hızlı reklamla başla
  }
  if (!connected) {
    if (readvertisePending && currentMillis - disconnectedAtMs >= 500) {
      readvertisePending = false;
      restartAdvertising(false);
      Serial.println("Baglanti koptu, tekrar yayindayiz.");
    } else if (!advSlow && (long)(currentMillis - advFastUntilMs) >= 0) {
      restartAdvertising(true);           // uzun süredir bağlantı yok: yavaş reklam
    }
  }
  updateBleNotches();
#endif

  // MAX30102 40 ms'de bir (25 Hz) okunur: nabız/SpO2 için yeterli, I2C hattını
  // 200 Hz EKG okumasına bırakır.
  if (ppgConnected && (currentMillis - lastPPGProcessMs >= 40)) {
    lastPPGProcessMs = currentMillis;
    pollPPG(currentMillis);
  }

#if USE_BLE
  static bool wasWristAttached = false;
  if (isWristAttached && !wasWristAttached) advertiseFast(currentMillis);   // takıldı: PC hemen bulsun
  wasWristAttached = isWristAttached;

  // Koldan çıkık kaldıysa derin uyku. Takılıyken, USB varken, sensör yokken ya da
  // titreşimli alarm kuruluyken uyunmaz.
  static unsigned long lastBusyMs = 0;
  if (isWristAttached || usbPresent || !ppgConnected || alarmNeedsAwake()) {
    lastBusyMs = currentMillis;
  } else {
    unsigned long offMs = currentMillis - lastBusyMs;
    if ((!connected && offMs >= DEEP_SLEEP_AFTER_MS) ||
        (connected && DEEP_SLEEP_CONNECTED_AFTER_MS > 0 && offMs >= DEEP_SLEEP_CONNECTED_AFTER_MS))
      enterDeepSleep();   // geri dönmez
  }
#endif

  // AD8232 yalnızca EKG gönderilecekken açık
#if USE_BLE
  setAD8232(isWristAttached && connected);
#else
  setAD8232(isWristAttached);
#endif

  if (ledState && (long)(currentMillis - ledOffTime) >= 0) {
#if LED_PIN >= 0
    digitalWrite(LED_PIN, LOW);
#endif
    ledState = false;
  }

  // Bileklik kolda değilse EKG akışı durur
  if (!isWristAttached) {
    if (ledState) {
#if LED_PIN >= 0
      digitalWrite(LED_PIN, LOW);
#endif
      ledState = false;
    }
    bufferIndex = 0;          // yarım paketi at
    filtersPrimed = false;    // tekrar takılınca filtreler yeni seviyeden başlasın
    if (totalSampleCount % 400 == 0) {
      Serial.println("!! BILEKLIK TEMASI YOK - VERI AKISI BEKLEMEDE !!");
    }
    return;
  }

  // BLE bağlantısı yok (AD8232 kapalı) ya da AD8232 yeni açıldı, çıkışı oturuyor
  if (!ecgReady(currentMillis)) {
    bufferIndex = 0;
    filtersPrimed = false;
#if USE_BLE
    if (!connected && (totalSampleCount % 400 == 0)) {
      Serial.println("BLE baglantisi bekleniyor...");
    }
#endif
    return;
  }

#if USE_ADS1115
  float rawValue = (float)ads.readADC_SingleEnded(ADS_CHANNEL);
#else
  float rawValue = (float)analogRead(ECG_ADC_PIN);
#endif
  float filtered = processSignal(rawValue);

  if (detectRPeak(filtered)) {
    if (!ppgConnected) updateBPM_ECG();
#if LED_PIN >= 0
    digitalWrite(LED_PIN, HIGH);
#endif
    ledState = true;
    ledOffTime = millis() + 60;
  }

  // int16 sınırına kırpılır (taşma işaret değiştirip sahte sıçrama yaratır)
  float v = filtered;
  if (v >  32767.0f) v =  32767.0f;
  if (v < -32768.0f) v = -32768.0f;

#if USE_BLE
  if (deviceConnected) {
    ecgBuffer[bufferIndex++] = (int16_t)v;
    if (bufferIndex >= 10) {
      // Ürün derlemesinde eşleştirilmemiş bileklik EKG yaymaz
      if (devicePaired || ALLOW_UNPAIRED_ECG) sendEcgPacket();
      bufferIndex = 0;
    }
  }
#else
  // Tanı modu: aynı filtrelenmiş değer seri porttan; tek fark telsizin kapalı olması
  Serial.println((int16_t)v);
#endif
}
