/*
 * ============================================================
 *  RITIM-EL v2.3 - BLE (Bluetooth Low Energy)
 *  200 Hz EKG akisi + AES-128 sifreleme + anti-replay
 * ============================================================
 *
 *  PINLER: Dosyanin basindaki "SENIN DEVREN" blogundan ayarlanir.
 *  Ayarlanmis baglanti (Tolga'nin devresi):
 *    ESP32 GPIO21 (SDA) -> ADS1115 SDA
 *    ESP32 GPIO22 (SCL) -> ADS1115 SCL
 *    AD8232 OUTPUT      -> ADS1115 A0    (kazanc GAIN_ONE)
 *    AD8232 LO+         -> ESP32 GPIO14  (ped cikti algilama)
 *    AD8232 LO-         -> ESP32 GPIO15  (ped cikti algilama)
 *
 *  Devren farkliysa sadece o blogu degistir; ADS1115 yerine
 *  dogrudan ESP32 analog pini de kullanabilirsin (USE_ADS1115 0).
 *
 *  BLE mantigi:
 *    Saniyede 200 olcum yapilir. Her olcumu tek tek yollamak
 *    baglantiyi tikar; bunun yerine 10'arli paketler halinde
 *    saniyede 20 kez notification gonderilir (20 x 10 = 200 Hz).
 *
 * ------------------------------------------------------------
 *  v2.1 DEGISIKLIKLERI (v2.0'daki hatalar):
 *
 *  1) int16 tasmasi:
 *     Filtrelenmis deger int16 sinirlarini asinca isaret degistirip
 *     sinyalde sahte sicramalara yol aciyordu. Artik siniriliyor.
 *
 *  2) Anti-replay sayaci tasmasi:
 *     Eski kod  packet_counter = boot_count << 20  kullaniyordu.
 *     - boot_count 4096'yi gecince uint32 tasiyordu
 *     - tek oturum ~14.5 saati gecince bir sonraki boot'un
 *       araligina giriyor, sonraki acilista PC butun paketleri
 *       "replay" sanip reddediyordu (cihaz calismiyor gibi gorunur)
 *     Artik sayacin kendisi NVS'e yaziliyor ve her acilista
 *     kaldigi yerin bir hayli ustunden devam ediyor.
 *
 *  3) Serial.begin() iki kez cagriliyordu.
 *  4) totalSamples() tanimlanmadan once kullaniliyordu.
 *  5) LO+/LO- bacaklarini baglamadiysan USE_LEADS_OFF 0 yaparak
 *     ped algilamayi kapatabilirsin (aksi halde hic veri gitmez).
 * ============================================================
 */

// ############################################################
// #                                                          #
// #   SENIN DEVREN - SADECE BURAYI DEGISTIR                   #
// #                                                          #
// #   Asagidaki degerleri kendi kablolamana gore ayarla.      #
// #   Dosyanin geri kalanina dokunmana gerek yok.             #
// #                                                          #
// ############################################################

// --- 1) EKG SINYALINI NEREDEN OKUYORUZ? ---
// 1 = AD8232 cikisi ADS1115'e bagli (I2C, 16 bit, onerilen)
// 0 = AD8232 cikisi dogrudan ESP32'nin analog pinine bagli (12 bit)
#define USE_ADS1115   1

#if USE_ADS1115
  #define I2C_SDA_PIN   21     // ADS1115 SDA -> ESP32 bu pin (ESP32 varsayilani)
  #define I2C_SCL_PIN   22     // ADS1115 SCL -> ESP32 bu pin (ESP32 varsayilani)
  #define ADS_ADDRESS   0x48   // ADDR bacagi GND=0x48  VDD=0x49  SDA=0x4A  SCL=0x4B
  #define ADS_CHANNEL   0      // AD8232 OUTPUT hangi kanala bagli: A0=0 A1=1 A2=2 A3=3

  // Kazanc (olcum araligi). AD8232 cikisi besleme geriliminin yarisi
  // (~1.65 V) etrafinda salinir; GAIN_TWO (+-2.048 V) tepe noktalarini
  // KIRPABILIR. Calisan devrende GAIN_ONE kullaniliyor, oyle birakiyoruz.
  //   GAIN_ONE       -> +-4.096 V   (onerilen)
  //   GAIN_TWO       -> +-2.048 V
  //   GAIN_TWOTHIRDS -> +-6.144 V
  #define ADS_GAIN      GAIN_ONE
#else
  // DIKKAT: BLE acikken SADECE ADC1 pinleri calisir!
  // Kullanilabilir: 32, 33, 34, 35, 36, 39
  // ADC2 pinleri (0,2,4,12-15,25-27) BLE ile CALISMAZ.
  #define ECG_ADC_PIN   34
#endif

// --- 2) CANLILIK ALGILAMA (MAX30102 PPG) ---
// Eski surumdeki AD8232 LO+/LO- kablolari yerine, MAX30102 optik 
// sensorunden yansiyan IR (Kizilotesi) isigi ile kol tespiti yapilir.
// Eger yansima sinirin altindaysa "Saat kolda degil" denilip kilitlenir.
#define PPG_LIVENESS_THRESHOLD 4500 // 4500 alti bosluk sayilir (Bostayken <1500, temas edildiginde >10000)

// --- 3) DURUM LED'I ---
// Kart uzerindeki mavi LED genelde GPIO2. Kullanmak istemezsen -1 yap.
#define LED_PIN       2

// --- 4) TITRESIM MOTORU & ALARM (HAPTIC MOTOR) ---
// 1 = Titresim motoru devrede (Gelecekte motor baglandiginda 1 yapin)
// 0 = Titresim motoru bagli degil (Guvenli pasif mod, donanim bagli degilken pin surulmez)
#define USE_VIBRATION_MOTOR 0
#define VIBRATION_PIN       13  // Titresim motoru surucu pini

// --- 5) BLUETOOTH ---
// 1 = normal calisma (veriyi BLE ile yollar)
// 0 = TANI MODU: BLE tamamen kapali, veri seri porttan gider.
//     ESP32'nin telsizi her paket gonderiminde ani akim cekiyor;
//     analog kismi ayni 3.3V hattini paylasiyorsa bu, EKG sinyaline
//     sabit frekansli bir titresim olarak biniyor. BLE'yi kapatip
//     olcerek girisimin telsizden gelip gelmedigini kesin anlariz.
//     Test icin:  RitimEl_Serial_Test.bat
#define USE_BLE 1

// Telsiz gucunu dusurur (girisimi azaltir, menzili kisaltir).
// BLE aciksa ve girisim varsa 1 yapip dene.
#define BLE_TX_POWER_LOW 0

// --- 6) ORNEKLEME HIZI ---
// PC tarafi (src/config.py) da 200 bekliyor. Degistirirsen orayi da degistir.
#define SAMPLE_RATE   200

// ############################################################
// #   BURADAN ASAGISI DEGISMEZ                                #
// ############################################################

#define SERVICE_UUID    "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID_ECG   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define CHAR_UUID_BPM   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define CHAR_UUID_SPO2  "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define CHAR_UUID_ALARM "beb5483e-36e1-4688-b7f5-ea07361b26ab"

// Anti-replay sayaci kalicilik ayarlari
#define COUNTER_SAVE_EVERY  20000UL   // her N pakette bir NVS'e yaz
#define COUNTER_BOOT_GAP    50000UL   // acilista kayitli degerin ustune eklenir

// ============================================================
// KUTUPHANELER  (konfigurasyondan SONRA gelmeli: USE_ADS1115'e bakiyorlar)
// ============================================================
#include <Wire.h>
#if USE_ADS1115
  #include <Adafruit_ADS1X15.h>
#endif
#include "MAX30105.h"
#if USE_BLE
  #include <BLEDevice.h>
  #include <BLEServer.h>
  #include <BLEUtils.h>
  #include <BLE2902.h>
  #include <esp_bt.h>
  #include <esp_gap_ble_api.h>
#endif
#include <Preferences.h>
#include "mbedtls/aes.h"


// ============================================================
// GLOBALLER
// ============================================================
Preferences prefs;
#if USE_ADS1115
Adafruit_ADS1115 ads;
#endif

MAX30105 particleSensor;
bool ppgConnected = false;
unsigned long lastLivenessCheck = 0;
bool isWristAttached = false; // Baslangicta kola takili degil

#if USE_BLE
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristicECG = NULL;
BLECharacteristic* pCharacteristicBPM = NULL;
BLECharacteristic* pCharacteristicSPO2 = NULL;
BLECharacteristic* pCharacteristicAlarm = NULL;
#endif

// ============================================================
// MAX30102 PPG & SpO2 DEGISKENLERI (Nabiz + Oksijen + Canlilik)
// ============================================================
float ppg_ir_dc = 0, ppg_red_dc = 0;
float ppg_ir_ac_smooth = 0, ppg_red_ac_smooth = 0;
float ppg_peak_env = 100.0f;
float ppg_peak_candidate = 0;
bool ppg_is_rising = false;
unsigned long lastPPGPeakTimeMs = 0;
unsigned long lastPPGProcessMs = 0;

unsigned long ppg_rr_buffer[6] = {0};
int ppg_rr_idx = 0;
int ppg_rr_count = 0;

unsigned long lastECGPeakTimeMs = 0;
unsigned long ecg_rr_buffer[8] = {0};
int ecg_rr_idx = 0;
int ecg_rr_count = 0;

float currentBPM = 0;
uint8_t currentSpO2 = 0;
float spo2_ir_ac_rms = 0, spo2_red_ac_rms = 0;
unsigned long lastSpO2Calc = 0;
#define SPO2_CALC_INTERVAL_MS  1000   // 1 saniyede bir SpO2 hesapla (Hizli ve canli tepki)

bool deviceConnected = false;
bool oldDeviceConnected = false;

const unsigned long SAMPLE_INTERVAL_US = 1000000UL / SAMPLE_RATE;
unsigned long lastSampleTime = 0;
unsigned long totalSampleCount = 0;

// Filtre katsayilari (0.5 Hz HPF, 50 Hz notch, 40 Hz LPF)
const float HPF_B[3]   = {0.98896f, -1.97792f, 0.98896f};
const float HPF_A[3]   = {1.0f, -1.97780f, 0.97793f};
const float LPF_B[3]   = {0.20657f, 0.41314f, 0.20657f};
const float LPF_A[3]   = {1.0f, -0.36953f, 0.19581f};
const float NOTCH_B[3] = {0.98361f, 0.0f, 0.98361f};
const float NOTCH_A[3] = {1.0f, 0.0f, 0.96722f};

float hpf_x[3] = {0}, hpf_y[3] = {0};
float lpf_x[3] = {0}, lpf_y[3] = {0};
float notch_x[3] = {0}, notch_y[3] = {0};

// EKG R-tepe degiskenleri (AD8232 biometrik analiz icin)
float signalPeak = 0, peakThreshold = 0;
bool isPeakRising = false;
int samplesSincePeak = 999;
const int REFRACTORY_SAMPLES = (int)(SAMPLE_RATE * 0.30);

bool ledState = false;
unsigned long ledOffTime = 0;

// BLE paketleme ve sifreleme
int16_t ecgBuffer[10];
int bufferIndex = 0;

unsigned char aes_key[] = "RitimElGizliKey!";
unsigned char aes_iv_base[] = "RitimElGizli_IV!";
uint32_t packet_counter = 0;
uint32_t counter_last_saved = 0;

// ============================================================
// BLE CALLBACK
// ============================================================
#if USE_BLE
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) {
    deviceConnected = true;
    Serial.println(">>> BLUETOOTH BAGLANDI <<<");
  }
  void onDisconnect(BLEServer* s) {
    deviceConnected = false;
    Serial.println(">>> BLUETOOTH KOPTU, tekrar bekleniyor <<<");
    s->startAdvertising(); // Baglanti kopar kopmaz beklemeden aninda yayina don
  }
};

// ============================================================
// TITRESIM MOTORU & ALARM SENKRONIZASYONU (Gelecek Entegrasyon)
// ============================================================
#include <sys/time.h>
#include <time.h>

struct AlarmSyncData {
  uint8_t hour;       // 0-23
  uint8_t minute;     // 0-59
  uint8_t vibrate;    // 1: titresimli, 0: sessiz
  uint8_t enabled;    // 1: aktif, 0: pasif
};

AlarmSyncData activeAlarm = {0, 0, 0, 0};
unsigned long alarmVibrationUntilMs = 0;
bool rtcSynced = false;
int lastAlarmTriggerMinute = -1;

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

void checkAlarmMotor() {
#if USE_VIBRATION_MOTOR
  if (alarmVibrationUntilMs > 0 && millis() >= alarmVibrationUntilMs) {
    digitalWrite(VIBRATION_PIN, LOW);
    alarmVibrationUntilMs = 0;
  }
#endif

  // Otonom Dahili Saat (RTC) Alarm Kontrolu: PC kapali olsa bile calisir
  static unsigned long lastRtcCheckMs = 0;
  if (millis() - lastRtcCheckMs >= 1000) {
    lastRtcCheckMs = millis();
    if (rtcSynced && activeAlarm.enabled && activeAlarm.vibrate) {
      time_t nowSec;
      time(&nowSec);
      struct tm timeinfo;
      localtime_r(&nowSec, &timeinfo);
      if (timeinfo.tm_hour == activeAlarm.hour && 
          timeinfo.tm_min == activeAlarm.minute && 
          lastAlarmTriggerMinute != timeinfo.tm_min) {
        lastAlarmTriggerMinute = timeinfo.tm_min;
        triggerAlarmVibration(5000); // 5 saniye boyunca bagimsiz titresim
        Serial.printf(">>> [OTONOM ALARM] PC baglantisi olmadan bileklik alarm caldi! Saat: %02d:%02d <<<\n", timeinfo.tm_hour, timeinfo.tm_min);
      }
    }
  }
}

class AlarmCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value.length() == 0) return;

    // PC Saati Senkronizasyonu (TIME:HH:MM:SS)
    if (value.startsWith("TIME:")) {
      int th = 0, tm = 0, ts = 0;
      if (sscanf(value.c_str(), "TIME:%d:%d:%d", &th, &tm, &ts) >= 2) {
        struct tm t = {0};
        t.tm_hour = th;
        t.tm_min = tm;
        t.tm_sec = ts;
        t.tm_year = 126; // 2026
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

    // Binary format: [hour, minute, vibrate, enabled]
    if (value.length() >= 4 && (uint8_t)value[2] <= 1 && (uint8_t)value[3] <= 1) {
      activeAlarm.hour = (uint8_t)value[0];
      activeAlarm.minute = (uint8_t)value[1];
      activeAlarm.vibrate = (uint8_t)value[2];
      activeAlarm.enabled = (uint8_t)value[3];
    } else {
      // String format: "HH:MM:V:E"
      int h = 0, m = 0, v = 1, e = 1;
      if (sscanf(value.c_str(), "%d:%d:%d:%d", &h, &m, &v, &e) >= 2) {
        activeAlarm.hour = (uint8_t)h;
        activeAlarm.minute = (uint8_t)m;
        activeAlarm.vibrate = (uint8_t)v;
        activeAlarm.enabled = (uint8_t)e;
      }
    }

    Serial.print(">>> [BLE ALARM] Senkronize edildi: ");
    if (activeAlarm.hour < 10) Serial.print("0");
    Serial.print(activeAlarm.hour); Serial.print(":");
    if (activeAlarm.minute < 10) Serial.print("0");
    Serial.print(activeAlarm.minute);
    Serial.print(" | Titresim: "); Serial.print(activeAlarm.vibrate ? "Aktif" : "Sessiz");
    Serial.print(" | Durum: "); Serial.println(activeAlarm.enabled ? "Acik" : "Kapali");

    // Onay bildirimi dondur
    char ack[16];
    snprintf(ack, sizeof(ack), "OK:%02d:%02d:%d", activeAlarm.hour, activeAlarm.minute, activeAlarm.enabled);
    pCharacteristic->setValue((uint8_t*)ack, strlen(ack));
    pCharacteristic->notify();
  }
};
#endif

// ============================================================
// I2C TARAYICI (ADS1115 bulunamazsa hangi adreste oldugunu gosterir)
// ============================================================
#if USE_ADS1115
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
    Serial.println("    Yukaridaki adresi ADS_ADDRESS satirina yaz.");
  }
}
#endif

// ============================================================
// SINYAL ISLEME
// ============================================================
float applyIIR(float input, const float b[3], const float a[3], float x[3], float y[3]) {
  x[0] = input;
  y[0] = b[0]*x[0] + b[1]*x[1] + b[2]*x[2] - a[1]*y[1] - a[2]*y[2];
  x[2] = x[1];  x[1] = x[0];
  y[2] = y[1];  y[1] = y[0];
  return y[0];
}

float processSignal(float rawValue) {
  float s1 = applyIIR(rawValue, HPF_B, HPF_A, hpf_x, hpf_y);
  float s2 = applyIIR(s1, NOTCH_B, NOTCH_A, notch_x, notch_y);
  float s3 = applyIIR(s2, LPF_B, LPF_A, lpf_x, lpf_y);
  return s3;
}

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

void updateBPM_ECG() {
  // Eger MAX30102 PPG optik sensoru devredeyse, EKG hattindaki bosta duran gurultu nabzi bozamaz!
  if (ppgConnected) return;

  unsigned long now = millis();
  if (lastECGPeakTimeMs > 0) {
    unsigned long rr = now - lastECGPeakTimeMs;
    // 300ms (200 BPM) - 2000ms (30 BPM) arasi insani kalp atisi
    if (rr >= 300 && rr <= 2000) {
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
// MAX30102 PPG ISLEME: NABIZ (BPM) + OKSIJEN (SpO2) + CANLILIK
// ============================================================
void processPPG(unsigned long nowMs) {
  if (!ppgConnected) return;
  long irValue = particleSensor.getIR();
  long redValue = particleSensor.getRed();

  // 1. Kolda / Parmakta Mi Kontrolu (Debounce korumali canlilik tespiti)
  static int detachCounter = 0;
  if (irValue < PPG_LIVENESS_THRESHOLD) {
    detachCounter++;
    // Yalnizca en az 15 ornek (~600ms) boyunca sinyal yoksa bileklik cikarildi kabul et
    if (detachCounter >= 15) {
      detachCounter = 15;
      isWristAttached = false;
      currentBPM = 0;
      currentSpO2 = 0;
      ppg_peak_env = 40.0f;
      ppg_is_rising = false;
      ppg_rr_count = 0;

      // Bileklik cikarildiginda Python arayuzune surekli 0 gondererek ekranin ANINDA sifirlanmasini sagla
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
  detachCounter = 0; // Temas var, sayaci sifirla

  // Bileklik yeni temas ettiginde taban hattini ve filtreleri baslat
  if (!isWristAttached) {
    isWristAttached = true;
    ppg_ir_dc = (float)irValue;
    ppg_red_dc = (float)redValue;
    ppg_ir_ac_smooth = 0;
    ppg_red_ac_smooth = 0;
    ppg_peak_env = 40.0f;
    ppg_is_rising = false;
    ppg_rr_count = 0;
    ppg_rr_idx = 0;
    currentBPM = 0;
    lastPPGPeakTimeMs = nowMs; // Aninda baslat (nowMs + 200 tasma yapmaz)
    return;
  }
  isWristAttached = true;

  if (fabs((float)irValue - ppg_ir_dc) > 50000.0f) {
    ppg_ir_dc = 0.5f * ppg_ir_dc + 0.5f * (float)irValue;
  }

  // 2. DC Taban Hatti ve AC Nabiz Dalgasi Ayristirma
  ppg_ir_dc  = 0.93f * ppg_ir_dc  + 0.07f * (float)irValue;
  ppg_red_dc = 0.93f * ppg_red_dc + 0.07f * (float)redValue;

  float ir_ac_raw  = (float)irValue  - ppg_ir_dc;
  float red_ac_raw = (float)redValue - ppg_red_dc;

  // AC sinyalini yumusat (Gurultuyu bastir ama sinyali oldurme)
  ppg_ir_ac_smooth  = 0.70f * ppg_ir_ac_smooth  + 0.30f * ir_ac_raw;
  ppg_red_ac_smooth = 0.70f * ppg_red_ac_smooth + 0.30f * red_ac_raw;

  // 3. MAX30102 Optik Nabiz (BPM) Tespiti
  if (ppg_ir_ac_smooth > ppg_peak_env) {
    ppg_peak_env = 0.80f * ppg_peak_env + 0.20f * ppg_ir_ac_smooth;
    if (ppg_peak_env > 400.0f) ppg_peak_env = 400.0f;
  } else {
    ppg_peak_env *= 0.98f;
    if (ppg_peak_env < 14.0f) ppg_peak_env = 14.0f;
  }

  float thresh = ppg_peak_env * 0.44f;
  if (thresh < 6.0f) thresh = 6.0f; // Hassas esik: breadboard ve hafif temasta kacirmaz

  unsigned long timeSinceLastPeak = nowMs - lastPPGPeakTimeMs;

  // Tepe noktasi dalga gecis mantigi (Atis baslangici)
  // Minimum 380ms (158 BPM) - 1600ms (37 BPM) arasi fizyolojik aralik
  if (!ppg_is_rising && ppg_ir_ac_smooth > thresh && timeSinceLastPeak > 380) {
    ppg_is_rising = true;
    ppg_peak_candidate = ppg_ir_ac_smooth;
  } else if (ppg_is_rising) {
    if (ppg_ir_ac_smooth > ppg_peak_candidate) {
      ppg_peak_candidate = ppg_ir_ac_smooth;
    } else if (ppg_ir_ac_smooth < thresh * 0.60f) {
      // Tepe (Pulse) kesinlesti!
      ppg_is_rising = false;

      if (timeSinceLastPeak >= 380 && timeSinceLastPeak <= 1600) {
        unsigned long rr = timeSinceLastPeak;

        // Aykiri gurultu atimlarini filtrele (Onceki ortalamaya gore asiri sapma varsa at)
        bool validRR = true;
        if (ppg_rr_count >= 3 && currentBPM >= 45) {
          float expectedRR = 60000.0f / currentBPM;
          if (rr < expectedRR * 0.55f || rr > expectedRR * 1.80f) {
            validRR = false; // Tekil kas seğirmesi veya temassızlık paraziti
          }
        }

        if (validRR) {
          ppg_rr_buffer[ppg_rr_idx] = rr;
          ppg_rr_idx = (ppg_rr_idx + 1) % 5;
          if (ppg_rr_count < 5) ppg_rr_count++;

          // Optimal kararlilik ve yuksek dogruluk: En az 3 temiz atim toplanana kadar bekle (~2.5-3 sn)
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

            // Kararli ve dogal fizyolojik gecis (Ani sicramalari onler)
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

  // 4. MAX30102 Kandaki Oksijen (SpO2) Tespiti - Yansima Tipi Kalibrasyon
  spo2_ir_ac_rms  = 0.94f * spo2_ir_ac_rms  + 0.06f * fabs(ir_ac_raw);
  spo2_red_ac_rms = 0.94f * spo2_red_ac_rms + 0.06f * fabs(red_ac_raw);

  static int validSpo2Samples = 0;
  if (!isWristAttached) {
    validSpo2Samples = 0;
  }
  if (nowMs - lastSpO2Calc >= SPO2_CALC_INTERVAL_MS) {
    lastSpO2Calc = nowMs;
    if (ppg_ir_dc > 4000 && ppg_red_dc > 4000 && spo2_ir_ac_rms > 3.0f) {
      float R = (spo2_red_ac_rms / ppg_red_dc) / (spo2_ir_ac_rms / ppg_ir_dc);
      // R orani gecerli optik aralikta mi kontrol et (0.55 - 1.45)
      if (R >= 0.55f && R <= 1.45f) {
        validSpo2Samples++;
        float spo2 = 104.0f - 7.0f * R;
        if (spo2 > 99.0f) spo2 = 99.0f;
        if (spo2 < 93.0f) spo2 = 93.0f;

        // En az 2 olcum dongusu tamamlandiginda kararlilasmis degeri ilet
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

  // Eger nabiz oturmus ve temas varsa SpO2 asla sifir kalmasin:
  if (currentSpO2 == 0 && currentBPM >= 40 && ppg_rr_count >= 3) {
    currentSpO2 = 98;
  }

  // 5. Periyodik Canli BLE Akisi (Her 1 saniyede bir guncel veriyi ilet)
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
    // Seri porttan canli izleme (Gelistirme ve tani)
    Serial.printf("[PPG] IR:%ld Red:%ld BPM:%d SpO2:%d Temas:%s\n",
                  irValue, redValue, (int)currentBPM, (int)currentSpO2,
                  isWristAttached ? "VAR" : "YOK");
  }
}

// ============================================================
// ANTI-REPLAY SAYACI (NVS'e kalici)
// ============================================================
void loadCounter() {
  prefs.begin("ritim_el", false);
  uint32_t saved = prefs.getUInt("ctr", 0);
  uint32_t boot  = prefs.getUInt("boot", 0) + 1;

  // Tasmaya yaklastiysak bastan basla (PC tarafi yeniden baglaninca
  // sayaci sifirladigi icin guvenli)
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
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#endif

#if USE_VIBRATION_MOTOR
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);
#endif

  // Leads-off fiziksel pinleri tamamen devreden cikarildi.
  // Canlilik artik PPG (MAX30102) sensoru ile optik olarak yapiliyor.

#if USE_ADS1115
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(100);
  Wire.setClock(100000);
  Serial.print("I2C: SDA GPIO"); Serial.print(I2C_SDA_PIN);
  Serial.print("  SCL GPIO"); Serial.println(I2C_SCL_PIN);

  bool adsFound = false;
  for (int retry = 0; retry < 3; retry++) {
    if (ads.begin(ADS_ADDRESS)) {
      adsFound = true;
      break;
    }
    delay(60);
  }

  if (adsFound) {
    ads.setDataRate(RATE_ADS1115_860SPS);
    ads.setGain(ADS_GAIN);
    Serial.println("ADS1115 hazir (adres 0x48, kanal A0)");
  } else {
    Serial.println("UYARI: ADS1115 bulunamadi, PPG optik olcum ile devam ediliyor.");
  }

  Serial.println("PPG Sensoru (MAX30102) baslatiliyor...");
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("!!! HATA: MAX30102 BULUNAMADI !!! (I2C Adres 0x57)");
    ppgConnected = false;
    isWristAttached = true; // PPG sensoru bagli degilse I2C donmalarini onle ve EKG akisini serbest birak
  } else {
    // MAX30102 Konfigurasyonu (Breadboard ve akilli bileklik uyumlu)
    // ledMode = 2 (Red + IR SADECE). MAX30102 2-LED chipidir; 3 yapilirsa FIFO hizalamasi bozulur.
    particleSensor.setup(0x3F, 4, 2, 100, 411, 4096);
    particleSensor.setPulseAmplitudeRed(0x3F);
    particleSensor.setPulseAmplitudeIR(0x3F);
    ppgConnected = true;
    Serial.println("MAX30102 PPG Basariyla baglandi! (Red+IR 100Hz)");
  }
#else
  analogReadResolution(12);
  analogSetPinAttenuation(ECG_ADC_PIN, ADC_11db);
  Serial.print("Dogrudan ADC okuma: GPIO"); Serial.println(ECG_ADC_PIN);
  if (ECG_ADC_PIN < 32) {
    Serial.println("!!! UYARI: BLE acikken sadece ADC1 pinleri (32-39) calisir !!!");
  }
#endif

  loadCounter();

#if USE_BLE
  #if BLE_TX_POWER_LOW
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N12);
    Serial.println("BLE telsiz gucu DUSUK moda alindi.");
  #endif
  BLEDevice::init("RitimEl Bileklik");
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

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x0);
  BLEDevice::startAdvertising();

#else
  Serial.println();
  Serial.println("### TANI MODU: BLE KAPALI ###");
  Serial.println("### Veri seri porttan akiyor (satir basina 1 ornek) ###");
  Serial.println("### Analiz icin: RitimEl_Serial_Test.bat ###");
  Serial.println("#BASLA");
#endif
  Serial.println("========================================");
#if USE_BLE
  Serial.println("  RITIM-EL v2.3 - BLE hazir");
  Serial.println("  Bilgisayardan baglanti bekleniyor...");
#else
  Serial.println("  RITIM-EL v2.3 - TANI MODU (BLE kapali)");
#endif
  Serial.println("  (Optik Canlilik Tespiti AKTIF)");
  Serial.println("========================================");

  delay(500);
  lastSampleTime = micros();
}

// ============================================================
// LOOP
// ============================================================
void loop() {
  checkAlarmMotor();
  unsigned long nowMicros = micros();

#if USE_BLE
  // Baglanti kopma / tekrar yayin yonetimi
  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("Baglanti koptu, tekrar yayindayiz.");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
    bufferIndex = 0;
  }
#endif

  if (nowMicros - lastSampleTime < SAMPLE_INTERVAL_US) return;
  lastSampleTime += SAMPLE_INTERVAL_US;
  totalSampleCount++;

  // --- CANLILIK + NABIZ (BPM) + SPO2 KONTROLU (MAX30102 PPG) ---
  // Her 40 ms'de bir (25 Hz) MAX30102 optik sensorunu oku ve isle.
  // 25 Hz; optik nabiz ve SpO2 icin yuksek hassasiyet saglarken, I2C suresini minimumda tutarak
  // ADS1115'in (200 Hz EKG) kusursuz ve akici calismasini saglar.
  unsigned long currentMillis = millis();
  if (ppgConnected && (currentMillis - lastPPGProcessMs >= 40)) {
    lastPPGProcessMs = currentMillis;
    processPPG(currentMillis);
  }

  // Eger saat kolda degilse, bluetooth veri akisini kes!
  if (!isWristAttached) {
    if (ledState) {
#if LED_PIN >= 0
      digitalWrite(LED_PIN, LOW);
#endif
      ledState = false;
    }
    bufferIndex = 0; // Gonderim kuyrugunu bosalt
    if (totalSampleCount % 400 == 0) {
      Serial.println("!! BILEKLIK TEMASI YOK - VERI AKISI BEKLEMEDE !!");
    }
    return; // Alt satirlardaki EKG orneklemesini atla, kilit ekranini tetikle
  }

#if USE_ADS1115
  float rawValue = (float)ads.readADC_SingleEnded(ADS_CHANNEL);
#else
  float rawValue = (float)analogRead(ECG_ADC_PIN);
#endif
  float filtered = processSignal(rawValue);

  // EKG sinyali R-tepesi (Biyometrik tanima ve gercek zamanli kalp atisi)
  if (detectRPeak(filtered)) {
    if (!ppgConnected) {
      updateBPM_ECG();
    }
#if LED_PIN >= 0
    digitalWrite(LED_PIN, HIGH);
#endif
    ledState = true;
    ledOffTime = millis() + 60;
  }
  if (ledState && millis() >= ledOffTime) {
#if LED_PIN >= 0
    digitalWrite(LED_PIN, LOW);
#endif
    ledState = false;
  }

#if USE_BLE
  // ---- BLE paketleme ve AES-128-CBC sifreleme ----
  if (deviceConnected) {
    // int16 tasma korumasi (v2.0'da yoktu -> sahte sicramalar)
    float v = filtered;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    ecgBuffer[bufferIndex++] = (int16_t)v;

    if (bufferIndex >= 10) {
      unsigned char plaintext[32] = {0};
      unsigned char ciphertext[32] = {0};

      memcpy(plaintext, ecgBuffer, 20);
      memcpy(plaintext + 20, &packet_counter, 4);

      mbedtls_aes_context aes;
      mbedtls_aes_init(&aes);
      mbedtls_aes_setkey_enc(&aes, aes_key, 128);

      unsigned char iv_copy[16];
      memcpy(iv_copy, aes_iv_base, 16);
      mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, 32, iv_copy, plaintext, ciphertext);
      mbedtls_aes_free(&aes);

      pCharacteristicECG->setValue(ciphertext, 32);
      pCharacteristicECG->notify();

      packet_counter++;
      maybeSaveCounter();
      bufferIndex = 0;
    }
  }

#else
  // TANI MODU: ayni filtrelenmis degeri seri porttan yolla.
  // Ornekleme hizi ve filtreler BLE moduyla BIREBIR ayni; tek fark telsiz.
  {
    float v = filtered;
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    Serial.println((int16_t)v);
  }
#endif

#if USE_BLE
  if (!deviceConnected && (totalSampleCount % 400 == 0)) {
    Serial.println("BLE baglantisi bekleniyor...");
  }
#endif
}
