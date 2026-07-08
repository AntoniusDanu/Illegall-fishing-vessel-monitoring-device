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
// GANTI PAKE IP LAPTOP LU! (Misal: 192.168.1.15)
const char* serverName = "http://10.122.105.175:3000/api/sensor"; 

// =====================================================
// KONFIGURASI TELEGRAM BOT
// =====================================================
const char* botToken = "8940104771:AAFonWgOBldtY4B28VzGuGObhInMLSceVT0";
const char* chatID   = "1357432515";

unsigned long lastTelegramSent = 0;
const unsigned long TELEGRAM_COOLDOWN = 60000; 

// =====================================================
// KONFIGURASI LORA & PDR
// =====================================================
#define SS_PIN   5
#define RST_PIN  14
#define DIO0_PIN 26

volatile bool loraReady = false;
unsigned long packetCounter = 0;
// Asumsi TX ngirim data setiap 1000ms (1 Detik). Sesuaikan kalau beda!
const unsigned long TX_INTERVAL_MS = 3000; 

// =====================================================
// FUNGSI KIRIM ALERT TELEGRAM
// =====================================================
void sendTelegramAlert(String message) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); 
    
    HTTPClient http;
    String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage";
    
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    String jsonPayload = "{\"chat_id\":\"" + String(chatID) + "\", \"text\":\"" + message + "\"}";
    
    int httpCode = http.POST(jsonPayload);
    if (httpCode == 200) { // WAJIB 200 OK
      Serial.println("[TELEGRAM] 🚨 Alert Terkirim!");
    } else {
      Serial.println("[TELEGRAM] GAGAL KIRIM! Code: " + String(httpCode));
      Serial.println("[TELEGRAM] Response: " + http.getString());
    }
    http.end();
  }
}

// =====================================================
// FUNGSI PARSING & KIRIM DATA
// =====================================================
void processAndSendData(String incoming, int rssi, float snr, float pdr) {
  int latIdx = incoming.indexOf("LAT=");
  int lonIdx = incoming.indexOf(",LON=");
  int satIdx = incoming.indexOf(",SAT=");
  int splIdx = incoming.indexOf(",SPL=");
  int fftIdx = incoming.indexOf(",FFT=");
  int emaIdx = incoming.indexOf(",EMA=");

  // 1. CEK DATA KEPOTONG (Packet Loss)
  if (latIdx == -1 || lonIdx == -1 || satIdx == -1 || splIdx == -1 || fftIdx == -1 || emaIdx == -1) {
    Serial.println("[ERROR] Paket LoRa terpotong/cacat! Incoming: " + incoming);
    return; 
  }

  // 2. POTONG STRING
  String latStr = incoming.substring(latIdx + 4, lonIdx);
  String lonStr = incoming.substring(lonIdx + 5, satIdx);
  String satStr = incoming.substring(satIdx + 5, splIdx);
  String splStr = incoming.substring(splIdx + 5, fftIdx);
  String fftStr = incoming.substring(fftIdx + 5, emaIdx);
  String emaStr = incoming.substring(emaIdx + 5);

  // 3. SANITASI SUPER KETAT (PENGHILANG KARAKTER ALIEN)
  // Convert ke tipe data aslinya, lalu jadikan string lagi. 
  // Ini akan BUANG semua simbol aneh seperti  $#
  float latVal = latStr.toFloat(); latStr = String(latVal, 6); // 6 desimal GPS
  float lonVal = lonStr.toFloat(); lonStr = String(lonVal, 6);
  int   satVal = satStr.toInt();   satStr = String(satVal);
  float splVal = splStr.toFloat(); splStr = String(splVal, 1); // 1 desimal
  float fftVal = fftStr.toFloat(); fftStr = String(fftVal, 1);
  float emaVal = emaStr.toFloat(); emaStr = String(emaVal, 1);

  Serial.println("\n[DATA DITERIMA BERSIH]");
  Serial.print("GPS -> LAT: " + latStr + " | LON: " + lonStr + "\n");
  Serial.print("MIC -> SPL: " + splStr + " | FFT: " + fftStr + " | EMA: " + emaStr + "\n");
  Serial.print("LORA-> RSSI: " + String(rssi) + " | SNR: " + String(snr) + " | PDR: " + String(pdr) + "%\n");

  // Logika Telegram Alert
  if (emaVal > 80.0 && fftVal > 0.0) { 
    if (millis() - lastTelegramSent > TELEGRAM_COOLDOWN || lastTelegramSent == 0) {
      // PERHATIKAN: Pakai \\n untuk enter agar JSON tidak rusak!
      String alertMsg = "🚨 PERINGATAN: Deteksi Kapal Ilegal!\\n\\n";
      alertMsg += "📍 Lokasi:\\nhttps://maps.google.com/maps?q=" + latStr + "," + lonStr + "\\n\\n";
      alertMsg += "🔊 Intensitas Suara (EMA): " + emaStr + " dB";
      
      sendTelegramAlert(alertMsg);
      lastTelegramSent = millis();
    }
  }

  // Tembak ke Next.js Dashboard
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");

    // JSON sudah pasti bersih sekarang!
    String jsonPayload = "{";
    jsonPayload += "\"spl\":" + splStr + ",";
    jsonPayload += "\"fft\":" + fftStr + ",";
    jsonPayload += "\"ema\":" + emaStr + ",";
    jsonPayload += "\"lat\":" + latStr + ",";
    jsonPayload += "\"lon\":" + lonStr + ",";
    jsonPayload += "\"sat\":" + satStr + ",";
    jsonPayload += "\"rssi\":" + String(rssi) + ",";
    jsonPayload += "\"snr\":" + String(snr) + ",";
    jsonPayload += "\"pdr\":" + String(pdr); 
    jsonPayload += "}";

    int httpCode = http.POST(jsonPayload);
    
    if (httpCode == 200) {
      Serial.println("[HTTP] SUKSES 200! Data masuk Next.js!");
    } else {
      Serial.println("[HTTP] GAGAL! Code: " + String(httpCode));
      Serial.println("[HTTP] Payload yg dikirim: " + jsonPayload); 
    }
    http.end();
  }
}

// =====================================================
// TASK: LORA RX
// =====================================================
void TaskLoRaRX(void *pvParameters) {
  Serial.println("[LORA] Menunggu Data dari Pelampung TX...");
  for (;;) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      String incoming = "";
      while (LoRa.available()) {
        incoming += (char)LoRa.read();
      }
      
      int rssi = LoRa.packetRssi();
      float snr = LoRa.packetSnr();
      packetCounter++;

      // Kalkulasi PDR
      unsigned long expectedPackets = millis() / TX_INTERVAL_MS;
      if (expectedPackets == 0) expectedPackets = 1;
      float pdr = ((float)packetCounter / (float)expectedPackets) * 100.0;
      if (pdr > 100.0) pdr = 100.0; // Limit max 100%

      processAndSendData(incoming, rssi, snr, pdr);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool initLoRa() {
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW); delay(20);
  digitalWrite(RST_PIN, HIGH); delay(100);
  
  LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
  int retry = 0;
  while (!LoRa.begin(433E6)) {
    retry++; delay(1000);
    if (retry >= 10) return false;
  }
  
  // === SETTINGAN LORA DI SINI (SETELAH BEGIN) ===
  LoRa.setSyncWord(0xF3);           // Password jaringan (Harus sama di TX & RX)
  LoRa.setTxPower(20);              // Power maksimal 20dBm
  LoRa.setSpreadingFactor(8);       // Naikin SF biar tahan noise (Default 7)
  LoRa.setSignalBandwidth(125E3);   // Bandwidth standar
  LoRa.enableCrc();                 // Aktifin proteksi data cacat
  // ==============================================

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[SISTEM] ALAT PENERIMA STARTING...");
  
  if (initLoRa()) {
    xTaskCreatePinnedToCore(TaskLoRaRX, "TaskLoRaRX", 8192, NULL, 1, NULL, 1);
  } else {
    Serial.println("LORA GAGAL!");
    while(1);
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n[WIFI] Connected!");
}

void loop() {
  vTaskDelete(NULL);
}