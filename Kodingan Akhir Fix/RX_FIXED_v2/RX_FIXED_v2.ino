#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// =====================================================
// KONFIGURASI WIFI & SERVER DASHBOARD (LOKAL)
// =====================================================
const char* ssid = "POCO X7";
const char* password = "rrayrann";
const char* serverName = "http://10.158.61.175:3000/api/sensor";

// =====================================================
// KONFIGURASI TELEGRAM BOT
// =====================================================
const char* botToken = "8940104771:AAFonWgOBldtY4B28VzGuGObhInMLSceVT0";
const char* chatID   = "1357432515";

unsigned long lastTelegramSent = 0;
const unsigned long TELEGRAM_COOLDOWN = 60000;

// =====================================================
// KONFIGURASI LORA
// =====================================================
#define SS_PIN   5
#define RST_PIN  14
#define DIO0_PIN 26

// ======================================================
// ARSITEKTUR BARU (INI KUNCI PERBAIKANNYA):
//
// 1. TaskLoRaRX (Core 1, prio 2): CUMA baca radio, secepat
//    mungkin, lalu lempar paket ke queue. Tidak pernah
//    nyentuh WiFi/HTTP. Radio jadi hampir selalu listening.
//
// 2. TaskNetwork (Core 0, prio 1): ambil paket dari queue,
//    parse, kirim HTTP dashboard + Telegram. Boleh lambat
//    (TLS 2-5 detik) tanpa bikin paket LoRa hilang.
//
// 3. Pembacaan FIFO pakai retry: LoRa.available() membaca
//    register SPI setiap iterasi, dan satu glitch pembacaan
//    bisa bikin dia "bohong" balikin 0 padahal data masih
//    ada. Retry beberapa kali sebelum nyerah.
//
// 4. PDR dihitung dari SEQUENCE NUMBER yang dikirim TX
//    (SEQ=...), bukan dari millis(). Ini satu-satunya cara
//    PDR yang valid buat data skripsi.
// ======================================================

typedef struct {
  char payload[160];
  int  rssi;
  float snr;
} LoRaPacket_t;

QueueHandle_t packetQueue;

// Statistik PDR berbasis sequence number
unsigned long firstSeq = 0;
unsigned long lastSeq  = 0;
unsigned long receivedCount = 0;
unsigned long corruptCount  = 0;
bool seqInit = false;

// =====================================================
// FUNGSI KIRIM ALERT TELEGRAM (dipanggil dari TaskNetwork)
// =====================================================
void sendTelegramAlert(const char* message) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  char url[160];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", botToken);

  http.begin(client, url);
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  http.addHeader("Content-Type", "application/json");

  char jsonPayload[512];
  snprintf(jsonPayload, sizeof(jsonPayload),
           "{\"chat_id\":\"%s\",\"text\":\"%s\"}", chatID, message);

  int httpCode = http.POST(jsonPayload);
  if (httpCode == 200) {
    Serial.println("[TELEGRAM] Alert terkirim!");
  } else {
    Serial.printf("[TELEGRAM] GAGAL! Code: %d\n", httpCode);
  }
  http.end();
}

// =====================================================
// TASK: NETWORK (Core 0) - parsing + HTTP + Telegram
// =====================================================
void TaskNetwork(void *pvParameters) {
  LoRaPacket_t pkt;

  for (;;) {
    // Nunggu paket dari TaskLoRaRX (blocking, hemat CPU)
    if (xQueueReceive(packetQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

    // ---- PARSE PAKAI sscanf (lebih ketat dari indexOf) ----
    unsigned long seq;
    float lat, lon, spl, fft, ema;
    int sat;
    int n = sscanf(pkt.payload,
                   "SEQ=%lu,LAT=%f,LON=%f,SAT=%d,SPL=%f,FFT=%f,EMA=%f",
                   &seq, &lat, &lon, &sat, &spl, &fft, &ema);

    if (n != 7) {
      corruptCount++;
      Serial.printf("[ERROR] Paket cacat (kebaca %d/7 field): %s\n", n, pkt.payload);
      continue;
    }

    // ---- PDR BERBASIS SEQUENCE NUMBER ----
    if (!seqInit) { firstSeq = seq; seqInit = true; }
    if (seq < firstSeq) firstSeq = seq;   // jaga-jaga TX di-reset
    lastSeq = seq;
    receivedCount++;

    unsigned long expected = lastSeq - firstSeq + 1;
    float pdr = (expected > 0) ? (100.0f * receivedCount / expected) : 0.0f;
    if (pdr > 100.0f) pdr = 100.0f;

    Serial.println("\n[DATA DITERIMA BERSIH]");
    Serial.printf("SEQ : %lu | Diterima: %lu/%lu | Cacat: %lu\n",
                  seq, receivedCount, expected, corruptCount);
    Serial.printf("GPS -> LAT: %.6f | LON: %.6f | SAT: %d\n", lat, lon, sat);
    Serial.printf("MIC -> SPL: %.1f | FFT: %.1f | EMA: %.1f\n", spl, fft, ema);
    Serial.printf("LORA-> RSSI: %d | SNR: %.2f | PDR: %.2f%%\n", pkt.rssi, pkt.snr, pdr);

    // ---- TELEGRAM ALERT ----
    if (ema > 80.0f && fft > 0.0f) {
      if (millis() - lastTelegramSent > TELEGRAM_COOLDOWN || lastTelegramSent == 0) {
        char alertMsg[300];
        snprintf(alertMsg, sizeof(alertMsg),
                 "PERINGATAN: Deteksi Kapal Ilegal!\\n\\n"
                 "Lokasi:\\nhttps://maps.google.com/maps?q=%.6f,%.6f\\n\\n"
                 "Intensitas Suara (EMA): %.1f dB",
                 lat, lon, ema);
        sendTelegramAlert(alertMsg);
        lastTelegramSent = millis();
      }
    }

    // ---- KIRIM KE DASHBOARD NEXT.JS ----
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverName);
      http.setConnectTimeout(2000);
      http.setTimeout(3000);
      http.addHeader("Content-Type", "application/json");

      char json[300];
      snprintf(json, sizeof(json),
               "{\"spl\":%.1f,\"fft\":%.1f,\"ema\":%.1f,"
               "\"lat\":%.6f,\"lon\":%.6f,\"sat\":%d,"
               "\"rssi\":%d,\"snr\":%.2f,\"pdr\":%.2f,\"seq\":%lu}",
               spl, fft, ema, lat, lon, sat, pkt.rssi, pkt.snr, pdr, seq);

      int httpCode = http.POST(json);
      if (httpCode == 200) {
        Serial.println("[HTTP] SUKSES 200! Data masuk Next.js!");
      } else {
        Serial.printf("[HTTP] GAGAL! Code: %d\n", httpCode);
        Serial.printf("[HTTP] Payload: %s\n", json);
      }
      http.end();
    } else {
      Serial.println("[WIFI] Putus! Mencoba reconnect...");
      WiFi.reconnect();
    }
  }
}

// =====================================================
// TASK: LORA RX (Core 1) - CUMA baca radio, super ringan
// =====================================================
void TaskLoRaRX(void *pvParameters) {
  Serial.println("[LORA] Menunggu data dari pelampung TX...");

  for (;;) {
    int packetSize = LoRa.parsePacket();
    if (packetSize > 0) {
      LoRaPacket_t pkt;
      int idx = 0;
      int retries = 0;

      // ==================================================
      // Baca FIFO dengan RETRY. available() baca register
      // lewat SPI tiap kali; satu glitch bisa bikin dia
      // balikin 0 di tengah jalan. Jangan langsung nyerah.
      // ==================================================
      while (idx < packetSize && idx < (int)sizeof(pkt.payload) - 1) {
        if (LoRa.available()) {
          pkt.payload[idx++] = (char)LoRa.read();
          retries = 0;
        } else {
          if (++retries > 20) break;   // beneran habis / hardware error
          delayMicroseconds(100);
        }
      }
      pkt.payload[idx] = '\0';
      pkt.rssi = LoRa.packetRssi();
      pkt.snr  = LoRa.packetSnr();

      if (idx != packetSize) {
        corruptCount++;
        Serial.printf("[WARNING] Kebaca %d dari %d byte walau sudah retry. "
                      "Cek wiring SPI / power! Paket dibuang.\n", idx, packetSize);
      } else {
        // Lempar ke TaskNetwork, JANGAN proses di sini
        if (xQueueSend(packetQueue, &pkt, 0) != pdTRUE) {
          Serial.println("[WARNING] Queue penuh, paket di-drop.");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool initLoRa() {
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW); delay(20);
  digitalWrite(RST_PIN, HIGH); delay(100);

  LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);

  // ==================================================
  // PENTING: turunin clock SPI dari default 8 MHz ke
  // 1 MHz. Di breadboard/jumper, 8 MHz rawan bit error
  // -> inilah sumber "karakter alien" dan available()
  // yang bohong. 1 MHz tetap jauh lebih cepat dari
  // kebutuhan (paket cuma ~70 byte tiap 3 detik).
  // ==================================================
  LoRa.setSPIFrequency(1E6);

  int retry = 0;
  while (!LoRa.begin(433E6)) {
    retry++; delay(1000);
    if (retry >= 10) return false;
  }

  LoRa.setSyncWord(0xF3);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(8);
  LoRa.setSignalBandwidth(125E3);
  LoRa.enableCrc();

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[SISTEM] ALAT PENERIMA STARTING (v2)...");

  packetQueue = xQueueCreate(8, sizeof(LoRaPacket_t));
  if (packetQueue == NULL) {
    Serial.println("[SISTEM] GAGAL bikin queue!");
    while (1);
  }

  if (!initLoRa()) {
    Serial.println("LORA GAGAL!");
    while (1);
  }

  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected!");
  Serial.print("[WIFI] IP ESP32: "); Serial.println(WiFi.localIP());

  // RX di Core 1 prio 2 (paling penting, harus responsif)
  xTaskCreatePinnedToCore(TaskLoRaRX,  "TaskLoRaRX",  4096,  NULL, 2, NULL, 1);
  // Network di Core 0 prio 1, stack gede buat TLS Telegram
  xTaskCreatePinnedToCore(TaskNetwork, "TaskNetwork", 12288, NULL, 1, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);
}
