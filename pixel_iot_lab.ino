/*
  ============================================================================
  PIXEL IOT LAB  -  ESP-12E (ESP8266) + HC-SR04 + DHT11 + Touch + OLED + Buzzer
  ============================================================================
  Papan di Arduino IDE : "NodeMCU 1.0 (ESP-12E Module)" (atau Generic ESP8266)
  Library (Library Manager):
    - Adafruit SSD1306
    - Adafruit GFX Library
    - DHT sensor library (Adafruit)  + Adafruit Unified Sensor
    - ESP8266WiFi / ESP8266WebServer / ESP8266mDNS  (ikut paket board ESP8266)

  File dalam folder sketch ini:
    pixel_iot_lab.ino   <- file ini
    dashboard.h         <- halaman dashboard (disimpan di flash)

  CARA PAKAI
    1. Isi WIFI_SSID dan WIFI_PASS di bawah.
    2. Sesuaikan nomor pin di bagian PIN (semua masih placeholder).
    3. Upload, buka Serial Monitor 115200 untuk melihat alamat IP.
    4. Buka http://<IP-ESP>/  atau  http://pixel.local/  di browser.
       Jika WiFi gagal tersambung, ESP membuat WiFi sendiri bernama
       "PixelIoT-Lab" (password pixel1234), lalu buka http://192.168.4.1/

  CATATAN WIRING
    VCC / GND
      OLED, DHT11, Touch  -> 3V3 dan GND
      HC-SR04             -> 5V (pin VIN/VU) dan GND
      Buzzer              -> pin sinyal ke PIN_BUZZER, kaki lain ke GND
    PENTING: pin ECHO HC-SR04 mengeluarkan 5V, sedangkan pin ESP8266 hanya
    tahan 3.3V. Pasang pembagi tegangan di jalur ECHO:
         ECHO ---[ 1k ]---+---[ 2k ]--- GND
                          +--------------> PIN_ECHO (ESP)
    (atau pakai HC-SR04P / modul versi 3.3V, lalu tidak perlu pembagi)

  PERILAKU
    - Sentuh sensor   : layar OLED bereaksi (maskot senang + hati), buzzer 2x bip.
    - Objek dekat     : layar "DEKAT", maskot ketakutan, bip makin cepat saat makin dekat.
    - Suhu tinggi     : layar "PANAS", maskot kepanasan, bip panjang berulang.
    - Tahan sentuhan 1,2 detik : mute / unmute buzzer.
    - Prioritas buzzer & layar : SENTUH > JARAK > SUHU
  ============================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include "dashboard.h"

// ───────────────────────────── WIFI ─────────────────────────────
const char* WIFI_SSID = "ROBOTICTEAM";
const char* WIFI_PASS = "Robo1234";
const char* AP_SSID   = "PixelIoT-Lab";     // dipakai jika WiFi rumah gagal
const char* AP_PASS   = "pixel1234";        // minimal 8 karakter
const char* MDNS_NAME = "pixel";            // -> http://pixel.local/

// ───────────────────────────── PIN (PLACEHOLDER, ATUR SENDIRI) ─────────────────────────────
#ifndef D0              // jika memakai board "Generic ESP8266 Module"
  #define D0 16
  #define D1 5
  #define D2 4
  #define D3 0
  #define D4 2
  #define D5 14
  #define D6 12
  #define D7 13
  #define D8 15
#endif

#define PIN_OLED_SDA  D2    // GPIO4
#define PIN_OLED_SCL  D3    // GPIO5
#define PIN_TRIG      D6    // GPIO14  HC-SR04 TRIG
#define PIN_ECHO      D5    // GPIO12  HC-SR04 ECHO (lewat pembagi tegangan!)
#define PIN_DHT       D1    // GPIO13  DHT11 data
#define PIN_TOUCH     D7    // GPIO16  sensor touch (TTP223, output digital)
#define PIN_BUZZER    D8    // GPIO15  buzzer

// ───────────────────────────── OPSI ─────────────────────────────
#define OLED_ADDR          0x3C   // alamat I2C OLED (kadang 0x3D)
#define TOUCH_ACTIVE_HIGH  1      // 1 = sensor HIGH saat disentuh (TTP223 default)
#define BUZZER_PASSIVE     0      // 0 = buzzer aktif (cukup ON/OFF), 1 = pasif (pakai nada)
#define DHT_TYPE           DHT11

// ───────────────────────────── OBJEK ─────────────────────────────
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(PIN_DHT, DHT_TYPE);
ESP8266WebServer server(80);

// ───────────────────────────── STATE ─────────────────────────────
enum Reason { R_NONE, R_TOUCH, R_DIST, R_TEMP, R_TEST };
enum Mood   { M_IDLE, M_HAPPY, M_SCARED, M_HOT };

float temperature = NAN, humidity = NAN, distanceCm = -1;
bool  dhtOk = false;

bool touchNow = false, holdHandled = false;
uint32_t touchCount = 0;
unsigned long touchPressAt = 0, touchLastAt = 0, touchWindowUntil = 0;

int  cfgDist = 20;            // cm  : alarm jika jarak < nilai ini
int  cfgTemp = 32;            // C   : alarm jika suhu >= nilai ini
bool alarmDist = false, alarmTemp = false;

bool muted = false, buzzerOn = false, apMode = false;
unsigned long testUntil = 0, toastUntil = 0;
const char* toastText = "";

unsigned long lastDist = 0, lastDht = 0, lastDraw = 0, lastLog = 0;

// ───────────────────────────── BUZZER ─────────────────────────────
void buzzerSet(bool on, unsigned int freq) {
  static unsigned int lastFreq = 0;
  if (on == buzzerOn && (!on || freq == lastFreq)) return;   // tidak ada perubahan
  buzzerOn = on; lastFreq = freq;
#if BUZZER_PASSIVE
  if (on) tone(PIN_BUZZER, freq); else noTone(PIN_BUZZER);
#else
  (void)freq;
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#endif
}

Reason currentReason() {
  unsigned long now = millis();
  if (now < testUntil)        return R_TEST;
  if (now < touchWindowUntil) return R_TOUCH;
  if (alarmDist)              return R_DIST;
  if (alarmTemp)              return R_TEMP;
  return R_NONE;
}

const char* reasonName(Reason r) {
  switch (r) {
    case R_TOUCH: return "touch";
    case R_DIST:  return "dist";
    case R_TEMP:  return "temp";
    case R_TEST:  return "test";
    default:      return "none";
  }
}

void updateBuzzer() {
  unsigned long now = millis();
  Reason r = currentReason();
  bool on = false;
  unsigned int freq = 2000;

  if (!muted || r == R_TEST) {
    switch (r) {
      case R_TEST:
        on = true; freq = 1800; break;
      case R_TOUCH: {                       // dua bip pendek saat disentuh
        unsigned long dt = now - touchPressAt;
        on = (dt < 80) || (dt > 160 && dt < 240);
        freq = 2400; break;
      }
      case R_DIST: {                        // makin dekat, makin cepat
        static unsigned long lastBeep = 0;
        long period = constrain((long)(distanceCm * 15), 100L, 700L);
        if (now - lastBeep >= (unsigned long)period) lastBeep = now;
        on = (now - lastBeep) < 60;
        freq = 1200; break;
      }
      case R_TEMP:                          // bip panjang berulang
        on = (now % 1000) < 600;
        freq = 700; break;
      default: break;
    }
  }
  buzzerSet(on, freq);
}

// ───────────────────────────── SENSOR ─────────────────────────────
float readDistanceOnce() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);   // timeout 25 ms (~4 m)
  if (us == 0) return -1;
  float cm = us * 0.0343f / 2.0f;
  return (cm < 2.0f || cm > 400.0f) ? -1 : cm;
}

float readDistanceFiltered() {                           // median dari 3 pembacaan
  float a = readDistanceOnce(), b = readDistanceOnce(), c = readDistanceOnce();
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;                                              // -1 dianggap paling kecil
}

void showToast(const char* t, unsigned long ms = 1200) { toastText = t; toastUntil = millis() + ms; }

void readTouch() {
  static bool lastRaw = false, stable = false;
  static unsigned long changedAt = 0;
  unsigned long now = millis();
  bool raw = (digitalRead(PIN_TOUCH) == (TOUCH_ACTIVE_HIGH ? HIGH : LOW));
  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt > 30 && raw != stable) {
    stable = raw;
    if (stable) {                                        // baru disentuh
      touchCount++;
      touchPressAt = touchLastAt = now;
      touchWindowUntil = now + 1500;
      holdHandled = false;
      Serial.printf("[touch] #%lu\n", (unsigned long)touchCount);
    }
  }
  touchNow = stable;
  if (stable) {
    touchWindowUntil = max(touchWindowUntil, now + 600); // layar tetap "sentuh" selama ditahan
    if (!holdHandled && now - touchPressAt >= 1200) {    // tahan 1,2 dtk = toggle mute
      holdHandled = true;
      muted = !muted;
      showToast(muted ? "MUTE ON" : "MUTE OFF");
    }
  }
}

void updateAlarms() {
  // jarak: histeresis 2 cm supaya tidak berkedip di batas
  if (distanceCm > 0) {
    if (!alarmDist && distanceCm < cfgDist) alarmDist = true;
    else if (alarmDist && distanceCm >= cfgDist + 2) alarmDist = false;
  } else {
    alarmDist = false;
  }
  // suhu: histeresis 1 derajat
  if (dhtOk) {
    if (!alarmTemp && temperature >= cfgTemp) alarmTemp = true;
    else if (alarmTemp && temperature <= cfgTemp - 1) alarmTemp = false;
  } else {
    alarmTemp = false;
  }
}

const char* oledMode() {
  unsigned long now = millis();
  if (now < touchWindowUntil) return "touch";
  if (alarmDist) return "near";
  if (alarmTemp) return "hot";
  return "idle";
}

// ───────────────────────────── OLED (gambar) ─────────────────────────────
inline void fillR(int x, int y, int w, int h, bool on = true) { display.fillRect(x, y, w, h, on ? SSD1306_WHITE : SSD1306_BLACK); }
inline void outR(int x, int y, int w, int h)                  { display.drawRect(x, y, w, h, SSD1306_WHITE); }
inline void px(int x, int y, bool on = true)                  { display.drawPixel(x, y, on ? SSD1306_WHITE : SSD1306_BLACK); }

void txt(const String& s, int x, int y, uint8_t size = 1, bool inverted = false) {
  display.setTextSize(size);
  display.setTextColor(inverted ? SSD1306_BLACK : SSD1306_WHITE);
  display.setCursor(x, y);
  display.print(s);
}

void drawMascot(int x, int y, Mood m, uint32_t f) {
  fillR(x + 15, y - 5, 1, 5);                                 // antena
  fillR(x + 14, y - 8, 3, 3, m != M_IDLE || (f % 8) < 4);
  outR(x, y, 32, 26);                                         // badan (sudut dipotong)
  px(x, y, false); px(x + 31, y, false); px(x, y + 25, false); px(x + 31, y + 25, false);
  fillR(x - 2, y + 9, 2, 8); fillR(x + 32, y + 9, 2, 8);      // telinga
  int e[2] = { x + 7, x + 21 };

  if (m == M_IDLE) {
    bool blink = (f % 40) > 36;
    for (int i = 0; i < 2; i++) { if (blink) fillR(e[i], y + 12, 4, 2); else fillR(e[i], y + 7, 4, 7); }
    px(x + 10, y + 17); px(x + 11, y + 18); fillR(x + 12, y + 19, 8, 1); px(x + 20, y + 18); px(x + 21, y + 17);
  } else if (m == M_HAPPY) {
    for (int i = 0; i < 2; i++) {
      int ex = e[i] - 1;
      for (int k = 0; k < 2; k++) {
        px(ex, y + 9 + k); px(ex + 1, y + 8 + k); px(ex + 2, y + 7 + k);
        px(ex + 3, y + 7 + k); px(ex + 4, y + 8 + k); px(ex + 5, y + 9 + k);
      }
    }
    fillR(x + 10, y + 16, 12, 2); fillR(x + 11, y + 18, 10, 2); fillR(x + 13, y + 20, 6, 1);
  } else if (m == M_SCARED) {
    for (int i = 0; i < 2; i++) { outR(e[i] - 1, y + 6, 6, 8); fillR(e[i] + ((f % 6 < 3) ? 0 : 1), y + 8, 2, 3); }
    outR(x + 12, y + 16, 8, 7);
  } else {                                                    // M_HOT
    for (int i = 0; i < 2; i++) { fillR(e[i], y + 8, 5, 1); fillR(e[i], y + 10, 5, 3); }
    for (int i = 0; i < 12; i++) px(x + 10 + i, y + 19 + ((i >> 1) & 1));
    fillR(x + 27, y + 3 + ((f % 12) >> 2), 2, 3);             // keringat
  }
}

const char* HEART[6] = { ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..." };
void drawHeart(int x, int y, int sc) {
  for (int j = 0; j < 6; j++) for (int i = 0; i < 7; i++) if (HEART[j][i] == 'X') fillR(x + i * sc, y + j * sc, sc, sc);
}

void drawWifi(int x, int y) {
  if (apMode) { txt("AP", x + 2, y); return; }
  int r = WiFi.RSSI();
  int lvl = (WiFi.status() != WL_CONNECTED) ? 0 : (r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1);
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    if (i < lvl) fillR(x + i * 4, y + 8 - h, 3, h); else fillR(x + i * 4, y + 7, 3, 1);
  }
}

void drawTopBar(const char* title) {
  txt(title, 2, 2);
  if (muted) txt("MUTE", 80, 2);
  drawWifi(110, 1);
  fillR(0, 11, 128, 1);
}

String distStr()  { return distanceCm < 0 ? String("---") : String((int)distanceCm) + "cm"; }
String tempStr()  { return dhtOk ? String(temperature, 1) + "C" : String("ERR"); }
String humStr()   { return dhtOk ? String((int)(humidity + 0.5f)) + "%" : String("ERR"); }

void screenIdle(uint32_t f) {
  drawTopBar("PIXEL LAB");
  drawMascot(10, 24, M_IDLE, f);
  int p = (millis() / 2500) % 3;
  const char* lab[3] = { "TEMP", "HUMID", "JARAK" };
  String val = (p == 0) ? tempStr() : (p == 1) ? humStr() : distStr();
  txt(lab[p], 56, 14);
  txt(val, 56, 26, 2);
  for (int i = 0; i < 3; i++) { if (i == p) fillR(100 + i * 8, 56, 4, 4); else outR(100 + i * 8, 56, 4, 4); }
  if (((f >> 3) % 2) == 0) txt("TOUCH ME", 4, 56);
}

void screenNear(uint32_t f) {
  bool fl = ((f >> 2) % 2) == 0;
  if (fl) fillR(0, 0, 128, 11);
  txt("! DEKAT !", 37, 2, 1, fl);
  drawMascot(10 + (((f >> 1) % 2) ? 1 : -1), 24, M_SCARED, f);
  txt("JARAK", 56, 14);
  txt(distStr(), 56, 26, 2);
  outR(56, 46, 64, 8);
  float d = (distanceCm < 0) ? cfgDist : distanceCm;
  float ratio = constrain(((float)cfgDist - d) / (float)cfgDist, 0.0f, 1.0f);
  fillR(58, 48, (int)(ratio * 60), 4);
}

void screenHot(uint32_t f) {
  bool fl = ((f >> 2) % 2) == 0;
  if (fl) fillR(0, 0, 128, 11);
  txt("! PANAS !", 37, 2, 1, fl);
  drawMascot(10, 24, M_HOT, f);
  txt("SUHU", 56, 14);
  txt(tempStr(), 56, 26, 2);
  outR(56, 46, 64, 8);
  float ratio = constrain((temperature - (cfgTemp - 5)) / 10.0f, 0.0f, 1.0f);
  fillR(58, 48, (int)(ratio * 60), 4);
}

void screenTouch(uint32_t f) {
  drawTopBar("PIXEL LAB");
  drawMascot(10, 24, M_HAPPY, f);
  const int sp[4][2] = { {4, 22}, {50, 30}, {46, 18}, {2, 50} };      // kilauan berkedip
  for (int i = 0; i < 4; i++) if (((f + i * 3) % 8) < 4) { fillR(sp[i][0], sp[i][1] - 2, 1, 5); fillR(sp[i][0] - 2, sp[i][1], 5, 1); }
  txt("YAY!", 60, 14, 2);
  txt("TOUCH:" + String((unsigned long)touchCount), 60, 34);
  drawHeart(60, 46, 2);
}

void drawOled() {
  uint32_t f = millis() / 83;
  display.clearDisplay();
  const char* m = oledMode();
  if      (!strcmp(m, "touch")) screenTouch(f);
  else if (!strcmp(m, "near"))  screenNear(f);
  else if (!strcmp(m, "hot"))   screenHot(f);
  else                          screenIdle(f);

  if (millis() < toastUntil) {                                         // pop-up kecil (mute on/off)
    int w = strlen(toastText) * 6;
    fillR(18, 24, 92, 18, false); outR(18, 24, 92, 18);
    txt(toastText, 64 - w / 2, 29);
  }
  display.display();
}

void splash(const String& l1, const String& l2 = "") {
  display.clearDisplay();
  drawMascot(10, 22, M_IDLE, millis() / 83);
  txt("PIXEL LAB", 56, 16);
  txt(l1, 56, 32);
  txt(l2, 56, 44);
  display.display();
}

// ───────────────────────────── WEB SERVER ─────────────────────────────
void sendJson(const char* body, int code = 200) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void handleData() {
  unsigned long now = millis();
  char t[12], h[12], rssi[12];
  if (dhtOk) { snprintf(t, sizeof t, "%.1f", temperature); snprintf(h, sizeof h, "%.1f", humidity); }
  else       { strcpy(t, "null"); strcpy(h, "null"); }
  if (apMode) strcpy(rssi, "null"); else snprintf(rssi, sizeof rssi, "%d", WiFi.RSSI());

  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String ssid = apMode ? String(AP_SSID) : String(WIFI_SSID);
  long ago = touchLastAt == 0 ? -1 : (long)(now - touchLastAt);
  const char* toast = (now < toastUntil) ? toastText : "";

  char buf[720];
  snprintf(buf, sizeof buf,
    "{\"temperature\":%s,\"humidity\":%s,\"dhtOk\":%s,\"distance\":%.1f,"
    "\"touch\":%s,\"touchCount\":%lu,\"touchAgo\":%ld,"
    "\"buzzer\":%s,\"reason\":\"%s\",\"muted\":%s,"
    "\"oled\":\"%s\",\"oledPage\":%d,\"oledToast\":\"%s\","
    "\"cfg\":{\"dist\":%d,\"temp\":%d},\"alarm\":{\"dist\":%s,\"temp\":%s},"
    "\"sys\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%s,\"uptime\":%lu,\"heap\":%u}}",
    t, h, dhtOk ? "true" : "false", distanceCm,
    touchNow ? "true" : "false", (unsigned long)touchCount, ago,
    buzzerOn ? "true" : "false", reasonName(currentReason()), muted ? "true" : "false",
    oledMode(), (int)((now / 2500) % 3), toast,
    cfgDist, cfgTemp, alarmDist ? "true" : "false", alarmTemp ? "true" : "false",
    apMode ? "AP" : "STA", ssid.c_str(), ip.c_str(), rssi, now, (unsigned)ESP.getFreeHeap());
  sendJson(buf);
}

void handleMute() {
  if (server.hasArg("on")) muted = server.arg("on") == "1"; else muted = !muted;
  showToast(muted ? "MUTE ON" : "MUTE OFF");
  sendJson("{\"ok\":true}");
}

void handleConfig() {
  if (server.hasArg("dist")) cfgDist = constrain(server.arg("dist").toInt(), 2, 200);
  if (server.hasArg("temp")) cfgTemp = constrain(server.arg("temp").toInt(), 10, 60);
  Serial.printf("[cfg] dist=%d temp=%d\n", cfgDist, cfgTemp);
  sendJson("{\"ok\":true}");
}

void handleTest() { testUntil = millis() + 600; sendJson("{\"ok\":true}"); }

void setupServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleData);
  server.on("/api/mute", HTTP_ANY, handleMute);
  server.on("/api/config", HTTP_ANY, handleConfig);
  server.on("/api/test", HTTP_ANY, handleTest);
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.onNotFound([]() { sendJson("{\"error\":\"not found\"}", 404); });
  server.begin();
}

// ───────────────────────────── WIFI ─────────────────────────────
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Menyambung ke WiFi \"%s\"", WIFI_SSID);
  unsigned long t0 = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250); Serial.print('.');
    String d = ""; for (int i = 0; i <= dots % 4; i++) d += '.';
    splash("WiFi", d); dots++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    Serial.print("Tersambung. IP: "); Serial.println(WiFi.localIP());
  } else {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("WiFi gagal, mode AP aktif. IP: "); Serial.println(WiFi.softAPIP());
  }
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

// ───────────────────────────── SETUP / LOOP ─────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n== PIXEL IOT LAB ==");

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_TOUCH, INPUT);           // GPIO16 (D0) tidak punya pull-up internal
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED tidak ditemukan! Cek kabel SDA/SCL dan alamat I2C.");
  }
  display.setTextWrap(false);
  display.clearDisplay(); display.display();
  dht.begin();

  splash("Mulai...");
  connectWiFi();
  setupServer();

  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  splash(apMode ? "Mode AP" : "Siap!", ip.substring(0, 9));
  buzzerSet(true, 2000); delay(80); buzzerSet(false, 0);    // bip tanda siap
  delay(1800);
}

void loop() {
  server.handleClient();
  MDNS.update();
  unsigned long now = millis();

  readTouch();

  if (now - lastDist >= 120) {
    lastDist = now;
    distanceCm = readDistanceFiltered();
  }
  if (now - lastDht >= 2000) {                 // DHT11 minimal 1 detik antar pembacaan
    lastDht = now;
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { temperature = t; humidity = h; dhtOk = true; }
    else                        { dhtOk = false; }
  }

  updateAlarms();
  updateBuzzer();

  if (now - lastDraw >= 66) { lastDraw = now; drawOled(); }

  if (now - lastLog >= 2000) {
    lastLog = now;
    Serial.printf("T=%.1fC H=%.0f%% D=%.1fcm touch=%d alarm[d=%d t=%d] mute=%d heap=%u\n",
                  temperature, humidity, distanceCm, touchNow, alarmDist, alarmTemp, muted, (unsigned)ESP.getFreeHeap());
  }
}
