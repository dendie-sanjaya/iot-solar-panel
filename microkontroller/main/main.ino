#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESPAsyncWebServer.h> 
#include <EEPROM.h>
#include <ArduinoJson.h> // <<< BARU: Library untuk JSON

// --- Definisi Pin Hardware ---
const int SENSOR_CAHAYA_PIN = 13; // D7 (GPIO 13)
const int RELAY_PIN = 4;   // D2 (GPIO 4)
const int RESET_BUTTON_PIN = 16; // D0 (GPIO 16)

// --- Konstanta Konfigurasi EEPROM ---
#define EEPROM_SIZE 512 
#define EEPROM_WIFI_SSID_ADDR 0
#define EEPROM_WIFI_PASS_ADDR 32
#define EEPROM_MQTT_SERVER_ADDR 64
#define EEPROM_CONFIGURED_FLAG_ADDR 96 
const int MAX_STRING_LENGTH = 31; 
const size_t JSON_DOC_CAPACITY = JSON_OBJECT_SIZE(2) + 50; // Kapasitas untuk JSON

// --- Variabel Konfigurasi ---
char stored_ssid[MAX_STRING_LENGTH + 1] = "";
char stored_pass[MAX_STRING_LENGTH + 1] = "";
char stored_mqtt_server[MAX_STRING_LENGTH + 1] = "";
bool is_configured = false;
bool is_lamp_on = false;
bool ap_mode_active = false; 

// --- Variabel untuk Optimasi Log & Debug Periodik ---
unsigned long last_debug_time = 0; // Variabel untuk menyimpan waktu terakhir debug
const long debug_interval = 5000; // 5000 milidetik = 5 detik (Interval debug)

// --- Konstanta MQTT ---
const char* INFO_TOPIC = "solar/lampu/status"; 
const char* CLIENT_ID = "ESP8266_Solar_Lamp"; 

// --- Objek Library ---
WiFiClient espClient;
PubSubClient client(espClient);
AsyncWebServer server(80);

// --- Konstanta Mode AP ---
const char* AP_SSID = "Konfigurasi_Solar_Lamp";
const char* AP_PASS = "12345678";

// --- PROTOTIPE FUNGSI WEB HANDLER ---
void handleRoot(AsyncWebServerRequest *request);
void handleConfig(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request); 
// -----------------------------------------------------------

// Fungsi untuk membuat dan mengirim payload JSON
void publishStatus(bool status) {
    DynamicJsonDocument doc(JSON_DOC_CAPACITY);
    
    doc["device_id"] = CLIENT_ID;
    doc["status_lamp"] = status ? "ON" : "OFF"; 
    
    char jsonBuffer[64]; 
    serializeJson(doc, jsonBuffer);
    
    if (client.connected()) {
        client.publish(INFO_TOPIC, jsonBuffer);
        Serial.print("INFO: MQTT Published JSON: "); 
        Serial.println(jsonBuffer);
    }
}


// --- Fungsi Baca/Tulis EEPROM ---
void writeStringToEEPROM(int addr, const char* data) {
 EEPROM.write(addr + MAX_STRING_LENGTH, strlen(data)); 
 for (int i = 0; i < MAX_STRING_LENGTH; ++i) {
  if (i < strlen(data)) {
   EEPROM.write(addr + i, data[i]);
  } else {
   EEPROM.write(addr + i, 0);
  }
 }
 EEPROM.commit();
}
void readStringFromEEPROM(int addr, char* buffer) {
 int len = EEPROM.read(addr + MAX_STRING_LENGTH); 
 len = min(len, MAX_STRING_LENGTH);
 for (int i = 0; i < len; ++i) {
  buffer[i] = EEPROM.read(addr + i);
 }
 buffer[len] = '\0'; 
}
void loadConfiguration() {
  Serial.println("DEBUG: Memuat Konfigurasi EEPROM...");
 EEPROM.begin(EEPROM_SIZE);
 if (EEPROM.read(EEPROM_CONFIGURED_FLAG_ADDR) == 1) {
  is_configured = true;
  readStringFromEEPROM(EEPROM_WIFI_SSID_ADDR, stored_ssid);
  readStringFromEEPROM(EEPROM_WIFI_PASS_ADDR, stored_pass);
  readStringFromEEPROM(EEPROM_MQTT_SERVER_ADDR, stored_mqtt_server);
    Serial.print("DEBUG: Konfigurasi Ditemukan. SSID: "); Serial.println(stored_ssid);
 } else {
  is_configured = false;
    Serial.println("DEBUG: Konfigurasi TIDAK Ditemukan.");
 }
 EEPROM.end();
}
void saveConfiguration(const String& ssid, const String& pass, const String& mqtt_server) {
  Serial.println("DEBUG: Menyimpan Konfigurasi ke EEPROM...");
 EEPROM.begin(EEPROM_SIZE);
 writeStringToEEPROM(EEPROM_WIFI_SSID_ADDR, ssid.c_str());
 writeStringToEEPROM(EEPROM_WIFI_PASS_ADDR, pass.c_str());
 writeStringToEEPROM(EEPROM_MQTT_SERVER_ADDR, mqtt_server.c_str());
 EEPROM.write(EEPROM_CONFIGURED_FLAG_ADDR, 1); 
 EEPROM.commit();
 EEPROM.end();
 strncpy(stored_ssid, ssid.c_str(), MAX_STRING_LENGTH);
 strncpy(stored_pass, pass.c_str(), MAX_STRING_LENGTH);
 strncpy(stored_mqtt_server, mqtt_server.c_str(), MAX_STRING_LENGTH);
 is_configured = true;
  Serial.println("DEBUG: Konfigurasi TERSIMPAN. Melakukan restart...");
}

// --- Fungsi WiFi/MQTT --- 
void reconnect() {
 while (!client.connected()) {
    Serial.println("DEBUG: Mencoba koneksi ulang MQTT...");
  if (client.connect(CLIENT_ID)) {
      Serial.println("DEBUG: MQTT Berhasil Terkoneksi.");
         publishStatus(is_lamp_on); // <<< PUBLISH JSON
  } else {
      Serial.print("ERROR: MQTT Gagal, rc="); Serial.print(client.state());
      Serial.println(". Menunggu 5 detik...");
   delay(5000);
  }
 }
}
void setupWiFi() {
  Serial.print("DEBUG: Mencoba koneksi STA ke SSID: "); Serial.println(stored_ssid);
 WiFi.mode(WIFI_STA);
 WiFi.begin(stored_ssid, stored_pass);
 int timeout = 0;
 while (WiFi.status() != WL_CONNECTED && timeout < 60) { 
  delay(500);
    Serial.print(".");
  timeout++;
 }
  Serial.println();
 if (WiFi.status() == WL_CONNECTED) {
    Serial.print("INFO: WiFi Terhubung. IP: "); Serial.println(WiFi.localIP());
  client.setServer(stored_mqtt_server, 1883);
 } else {
    Serial.println("ERROR: WiFi GAGAL Terhubung setelah timeout.");
  is_configured = false; 
 }
}

// --- Implementasi Fungsi Mode AP/Konfigurasi ---
void handleRoot(AsyncWebServerRequest *request) {
  Serial.println("DEBUG: Menerima Request / (Root) - Menampilkan halaman Konfigurasi.");
 String html = "<html><head><title>Konfigurasi Lampu Solar</title><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
 html += "<h2>Konfigurasi Sistem Lampu Solar</h2>";
 html += "<form method='get' action='/config'>";
 html += "SSID WiFi: <input type='text' name='ssid' required><br><br>";
 html += "Password WiFi: <input type='password' name='pass'><br><br>";
 html += "MQTT Broker IP/Domain: <input type='text' name='mqtt' required><br><br>";
 html += "<input type='submit' value='Simpan Konfigurasi'>";
 html += "</form></body></html>";
 request->send(200, "text/html", html);
}
void handleConfig(AsyncWebServerRequest *request) {
  Serial.println("DEBUG: Menerima Request /config. Menyimpan data...");
 String ssid = request->arg("ssid");
 String pass = request->arg("pass");
 String mqtt_server = request->arg("mqtt");
 
 saveConfiguration(ssid, pass, mqtt_server); 
 
 String html = "<html><body><h1>Konfigurasi Berhasil Disimpan!</h1><p>ESP8266 me-restart...</p></body></html>";
 request->send(200, "text/html", html);
 
 delay(3000);
 ESP.restart(); 
}
void handleNotFound(AsyncWebServerRequest *request) {
  Serial.println("DEBUG: Menerima Request Tidak Dikenal. Mengirim pesan 404.");
  request->send(404, "text/plain", "404: Not Found. Akses konfigurasi melalui IP Access Point.");
}

void startAPMode() {
  Serial.println("DEBUG: [AP] Memulai startAPMode()...");
  ap_mode_active = true; 
  
  // Tahap 1: Inisialisasi WiFi AP
  Serial.println("DEBUG: [AP] Mengatur Mode WiFi ke WIFI_AP...");
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(AP_SSID, AP_PASS);

  if (ap_ok) {
    Serial.println("INFO: [AP] Sinyal AP BERHASIL diaktifkan.");
    Serial.print("INFO: [AP] SSID: "); Serial.println(AP_SSID);
    
    // --- LOG IP PENTING ---
    String ap_ip = WiFi.softAPIP().toString();
    Serial.print(">>> ALAMAT IP KONFIGURASI: http://"); 
    Serial.println(ap_ip);
    // -----------------------
    
  } else {
    Serial.println("FATAL: [AP] GAGAL mengaktifkan Sinyal AP.");
    return; 
  }
  
  // Tahap 2: Inisialisasi Web Server Handlers (Asynchronous)
  Serial.println("DEBUG: [AP] Mendaftarkan Web Handlers (Async)...");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.onNotFound(handleNotFound);

  // Tahap 3: Memulai Web Server
  server.begin();
  Serial.println("INFO: [AP] Web Server BERHASIL dimulai.");
}

// --- Fungsi Kontrol Lampu --- 
// Variabel global yang diperlukan (asumsikan sudah didefinisikan):
// unsigned long last_debug_time = 0;
// const long debug_interval = 5000; 
// bool is_lamp_on = false;

void controlLamp() {
int light_status = digitalRead(SENSOR_CAHAYA_PIN);

  // Logika Sensor: LOW = Gelap (Sudah dikoreksi)
bool its_dark = (light_status == LOW);
bool should_be_on = its_dark;
 
  // 1. 🔍 Tampilkan Debug Log Periodik (Setiap 5 detik)
  // Ini membantu Anda mengamati perubahan sensor saat memutar potensiometer.
  if (millis() - last_debug_time >= debug_interval) {
    Serial.print("DEBUG PERIODIK: Sensor (D7/GPIO 13) MENTAH: "); 
    Serial.print(light_status == HIGH ? "HIGH" : "LOW"); 
    last_debug_time = millis();
  }

// 2. ⚡ Cek Transisi Status (Jika ada perubahan status ON/OFF)
if (should_be_on && !is_lamp_on) { 
    // Transisi ke ON: Gelap terdeteksi, Lampu OFF
 Serial.println("==================================================");
 Serial.println("EVENT: Transisi ke GELAP!");
 Serial.println("COMMAND: RELAY ON. Sinyal dikirim: LOW (Active LOW).");
 
 digitalWrite(RELAY_PIN, HIGH); 
 is_lamp_on = true;
 publishStatus(false); 
 
 Serial.println("==================================================");
} else if (!should_be_on && is_lamp_on) { 
    // Transisi ke OFF: Terang terdeteksi, Lampu ON
 Serial.println("==================================================");
 Serial.println("EVENT: Transisi ke TERANG!");
 Serial.println("COMMAND: RELAY OFF. Sinyal dikirim: HIGH (Active LOW).");
 
 digitalWrite(RELAY_PIN, LOW); 
 is_lamp_on = false;
 publishStatus(true); 
 
 Serial.println("==================================================");
}
}

// --- Setup dan Loop Utama ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Sistem Solar Lamp Dimulai ---");
 delay(100);

  // OPTIMASI STABILITAS
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  Serial.println("DEBUG: WiFi Sleep Mode Dimatikan.");

 // Inisialisasi Pin Input/Output
  Serial.println("DEBUG: Inisialisasi Pin I/O.");
 pinMode(RELAY_PIN, OUTPUT);
 digitalWrite(RELAY_PIN, LOW); 
 pinMode(SENSOR_CAHAYA_PIN, INPUT); 
 pinMode(RESET_BUTTON_PIN, INPUT_PULLUP); 

  // Membersihkan stack WiFi
  Serial.println("DEBUG: Membersihkan stack WiFi...");
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

 // Cek Tombol Reset Manual
 if (digitalRead(RESET_BUTTON_PIN) == LOW) { 
    Serial.println("!!! Reset Manual Dideteksi. Menghapus Konfigurasi EEPROM !!!");
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.write(EEPROM_CONFIGURED_FLAG_ADDR, 0); 
  EEPROM.commit();
  EEPROM.end();
    Serial.println("!!! Konfigurasi Dihapus. Tekan Reset untuk Memulai Mode AP !!!");
  while (true) { 
   delay(100); 
   digitalWrite(RELAY_PIN, !digitalRead(RELAY_PIN)); 
  }
 }

 loadConfiguration();

  // Logika Mode Operasi
  if (!is_configured || stored_ssid[0] == '\0' || stored_mqtt_server[0] == '\0') {
    Serial.println("INFO: Memulai Mode Access Point (Config Mode).");
  startAPMode(); 
 } else {
    Serial.println("INFO: Memulai Mode Klien (Normal Operation).");
  setupWiFi(); 
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("ERROR: Koneksi WiFi GAGAL. Beralih ke AP untuk rekonfigurasi.");
   is_configured = false; 
      ap_mode_active = true; 
   startAPMode();
  }
 }
  Serial.println("--- Setup Selesai ---");
}

void loop() {
 if (is_configured && WiFi.status() == WL_CONNECTED) {
  // --- Mode Operasi Normal (STA/MQTT) ---
  if (!client.connected()) {
   reconnect();
  }
  client.loop(); 
  controlLamp();
  delay(500); 
 } else {
  // --- Mode Konfigurasi (AP) ---
  if (ap_mode_active) {
      yield(); 
  } else {
      // Logika untuk mencoba koneksi ulang di mode STA
   setupWiFi();
   if (WiFi.status() != WL_CONNECTED) {
    delay(5000); 
    ESP.restart(); 
   }
  }
 }
}