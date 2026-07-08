#include <driver/i2s.h>
#include "arduinoFFT.h" 
#include <TinyGPS++.h>
#include <SPI.h>
#include <LoRa.h>

// =====================================================
// KONFIGURASI GPS (Serial 2)
// =====================================================
#define RXD2 17
#define TXD2 16
#define GPS_BAUD 9600

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

// =====================================================
// KONFIGURASI LORA
// =====================================================
#define SS_PIN   5
#define RST_PIN  14
#define DIO0_PIN 26

// =====================================================
// KONFIGURASI MIC INMP441 (I2S)
// =====================================================
#define I2S_WS 15  
#define I2S_SD 32  
#define I2S_SCK 21  // <--- UDAH DIGANTI KE 21 BIAR NGGAK 0.00!
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 8000  
#define SAMPLES 512       

double vReal[SAMPLES];
double vImag[SAMPLES];
arduinoFFT FFT = arduinoFFT(vReal, vImag, SAMPLES, SAMPLE_RATE); 

// =====================================================
// VARIABEL GLOBAL
// =====================================================
volatile bool gpsReady  = false;
volatile bool loraReady = false;
volatile bool gpsFix    = false;

float latitude  = 0.0;
float longitude = 0.0;
int satellites = 0;

volatile float sharedSPL = 0.0;
volatile float sharedFFT = 0.0;
volatile float sharedEMA = 0.0;

float EMA_a = 0.15; 
float EMA_SPL = 0;
bool isCalibrated = false;
unsigned long calibrationStartTime = 0;
double sumCalibrationSPL = 0;
int calibrationCount = 0;
float THRESHOLD_IDLE = 0;
const unsigned long CALIBRATION_TIME = 15000; 

// =====================================================
// TASK 1: MIC (Core 0)
// =====================================================
void TaskMic(void *pvParameters) {
  for (;;) {
    size_t bytesIn = 0;
    int32_t samples[SAMPLES];
    esp_err_t result = i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesIn, portMAX_DELAY);

    if (result == ESP_OK && bytesIn > 0) {
      int samplesRead = bytesIn / sizeof(int32_t);
      double sumSamples = 0;
      for (int i = 0; i < samplesRead; i++) sumSamples += (samples[i] >> 14);
      double dc_offset = sumSamples / samplesRead;

      double sumSquares = 0;
      for (int i = 0; i < samplesRead; i++) {
        double clean_sample = (samples[i] >> 14) - dc_offset;
        vReal[i] = clean_sample;
        vImag[i] = 0.0;
        sumSquares += clean_sample * clean_sample;
      }
      float RMS_Value = sqrt(sumSquares / samplesRead);
      
      float SPL_dB = 0;
      if (RMS_Value > 0) SPL_dB = 20.0 * log10(RMS_Value) + 40.0;

      // KALIBRASI (Ditampilin per detik)
      if (!isCalibrated) {
        if (calibrationStartTime == 0) calibrationStartTime = millis();
        
        sumCalibrationSPL += SPL_dB;
        calibrationCount++;
        
        // Print status kalibrasi tiap 1 detik biar rapi
        static unsigned long lastPrint = 0;
        if (millis() - lastPrint > 1000) {
           Serial.print("[MIC] Mempelajari ambient noise... SPL: "); 
           Serial.println(SPL_dB);
           lastPrint = millis();
        }
        
        if (millis() - calibrationStartTime > CALIBRATION_TIME) {
          THRESHOLD_IDLE = (sumCalibrationSPL / calibrationCount) + 5.0;
          isCalibrated = true;
          Serial.println("\n=====================================");
          Serial.println("[MIC] KALIBRASI SELESAI!");
          Serial.println("Threshold diset ke: " + String(THRESHOLD_IDLE) + " dB");
          Serial.println("ALAT SIAP TEMPUR!");
          Serial.println("=====================================\n");
        }
        vTaskDelay(pdMS_TO_TICKS(10)); 
        continue;
      }

      // MODE DETEKSI NORMAL
      float band_power = 0;
      if (SPL_dB > THRESHOLD_IDLE) {
        FFT.Windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD); 
        FFT.Compute(FFT_FORWARD);                        
        FFT.ComplexToMagnitude();
        for (int i = 0; i < (SAMPLES / 2); i++) {
          float frekuensi = (i * 1.0 * SAMPLE_RATE) / SAMPLES;
          if (frekuensi >= 50.0 && frekuensi <= 2000.0) band_power += vReal[i];
        }
        band_power = band_power / (SAMPLES / 2);
        if (band_power > 0) band_power = 10.0 * log10(band_power + 1);
      }

      if (EMA_SPL == 0) EMA_SPL = SPL_dB;
      else EMA_SPL = (EMA_a * SPL_dB) + ((1 - EMA_a) * EMA_SPL);

      sharedSPL = SPL_dB;
      sharedFFT = band_power;
      sharedEMA = EMA_SPL;

      vTaskDelay(pdMS_TO_TICKS(500)); 
    }
  }
}

// =====================================================
// TASK 2: GPS (Core 1)
// =====================================================
void TaskGPS(void *pvParameters) {
  for (;;) {
    while (gpsSerial.available()) {
      gps.encode(gpsSerial.read());
    }
    
    if (gps.location.isValid()) {
      latitude = gps.location.lat();
      longitude = gps.location.lng();
      satellites = gps.satellites.value();
      gpsFix = true;
    } else {
      gpsFix = false;
      // Notifikasi kalau GPS nyangkut (muncul tiap 5 detik)
      static unsigned long lastGPSPrint = 0;
      if (millis() - lastGPSPrint > 5000 && isCalibrated) {
         Serial.println("[GPS] Mencari satelit... (Di dalam ruangan mungkin susah dapet)");
         lastGPSPrint = millis();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// =====================================================
// TASK 3: LORA TX (Core 1)
// =====================================================
void TaskLoRa(void *pvParameters) {
  for (;;) {
    // SYARAT MUTLAK: Kalibrasi Kelar + LoRa Ready + GPS Dapet Sinyal
    if (isCalibrated && loraReady && gpsFix) { 
      
      String payload = "LAT=" + String(latitude, 6) +
                       ",LON=" + String(longitude, 6) +
                       ",SAT=" + String(satellites) +
                       ",SPL=" + String(sharedSPL, 1) +
                       ",FFT=" + String(sharedFFT, 1) +
                       ",EMA=" + String(sharedEMA, 1);

      LoRa.beginPacket();
      delay(10);
      LoRa.print(payload);
      int result = LoRa.endPacket();

      Serial.println("========== LORA TX ==========");
      Serial.println(payload);
      Serial.println("=============================\n");

      vTaskDelay(pdMS_TO_TICKS(3000));
      
    } else if (isCalibrated && loraReady && !gpsFix) {
      // Notif kalau Mic udah siap tapi masih nunggu GPS
      static unsigned long lastWaitPrint = 0;
      if (millis() - lastWaitPrint > 3000) {
         Serial.println("[LORA] Menunggu GPS 'Lock' satelit sebelum mulai ngirim data...");
         lastWaitPrint = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

// =====================================================
// SETUP UTAMA (STEP BY STEP)
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=================================");
  Serial.println("  SYSTEM CHECK: BUOY TX MODULE   ");
  Serial.println("=================================");

  // ---------------------------------------------------
  // STEP 1: CEK GPS
  // ---------------------------------------------------
  Serial.print("1. Inisialisasi GPS (Serial 2)... ");
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, RXD2, TXD2);
  gpsReady = true;
  Serial.println("OK!");
  delay(500);

  // ---------------------------------------------------
  // STEP 2: CEK LORA
  // ---------------------------------------------------
  Serial.print("2. Inisialisasi LoRa (SPI)... ");
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW); delay(20);
  digitalWrite(RST_PIN, HIGH); delay(100);
  LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
  
  int retry = 0;
  while (!LoRa.begin(433E6)) {
    retry++; delay(500);
    if (retry >= 10) break;
  }
  
  if (retry < 10) {
    LoRa.setSyncWord(0xF3);
    // --- SETTINGAN BARU (WAJIB SAMA KAYAK RX) ---
    LoRa.setTxPower(20);              // Power mentok 20dBm biar jangkauan jauh
    LoRa.setSpreadingFactor(8);       // Tahan noise/ombak (RX juga harus 8!)
    LoRa.setSignalBandwidth(125E3);   // Bandwidth standar
    LoRa.enableCrc();                 // Aktifin proteksi anti-data cacat
    // --------------------------------------------
    loraReady = true;
    Serial.println("OK!");
  } else {
    Serial.println("GAGAL! (Cek Kabel SPI)");
  }
  delay(500);

  // ---------------------------------------------------
  // STEP 3: CEK MIC (I2S)
  // ---------------------------------------------------
  Serial.print("3. Inisialisasi Mic (I2S)... ");
  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = SAMPLES,
    .use_apll = false
  };
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_start(I2S_PORT);
  Serial.println("OK!");
  delay(1000);

  // ---------------------------------------------------
  // STEP 4: START SYSTEM (RTOS)
  // ---------------------------------------------------
  Serial.println("\n[SISTEM] SEMUA MODUL HARDWARE READY.");
  Serial.println("[SISTEM] Memulai Multi-Tasking RTOS...");
  Serial.println("=================================\n");

  xTaskCreatePinnedToCore(TaskMic, "TaskMic", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(TaskGPS, "TaskGPS", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskLoRa, "TaskLoRa", 4096, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}