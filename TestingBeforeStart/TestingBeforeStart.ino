#include <Arduino.h>
#include <WiFi.h>
#include <HX711.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "esp_wifi.h"
// Calibration helper for this project:
// 1) Prints a stable STA MAC (paste into the other device's PEER_MAC)
// 2) Samples HX711 in two states (empty / with phone) using EMA smoothing
// 3) Prints recommended DETECT_MARGIN to use with NO_PHONE_VAL / WITH_PHONE_VAL
// 4) Shows a 12‑char MAC (no colons) on the LCD for quick reference
#include "esp_mac.h"   // Provides ESP_MAC_WIFI_STA and esp_read_mac

/* ===== Pins ===== */
#define PIN_HX_DOUT 16
#define PIN_HX_SCK  4
#define PIN_BTN     23
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define LCD_ADDR    0x27
#define LCD_COLS    16
#define LCD_ROWS    2

#define SAMPLE_TIME_MS 2000
#define EMA_ALPHA 0.20f
#define I2C_CLOCK_HZ 50000
#define BTN_DEBOUNCE_MS 35

HX711 scale;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

bool btnLast = true;
uint32_t btnLastMs = 0;

float emaVal = 0;
long NO_PHONE_VAL = 0;
long WITH_PHONE_VAL = 0;

/* ---------- LCD helpers ---------- */
void lcdWrite16(uint8_t row, const char* s) {
  lcd.setCursor(0, row);
  for (int i = 0; i < 16; ++i) {
    char c = s[i];
    if (c == '\0') c = ' ';
    if (c < 32 || c > 126) c = ' ';
    lcd.print(c);
  }
}

/* ---------- Button ---------- */
bool buttonPressed() {
  bool r = digitalRead(PIN_BTN);
  uint32_t now = millis();
  if (r != btnLast && (now - btnLastMs) > BTN_DEBOUNCE_MS) {
    btnLast = r; btnLastMs = now;
    return (r == LOW);
  }
  return false;
}

/* ---------- HX711 ---------- */
float readSmooth() {
  if (scale.is_ready()) {
    long raw = scale.read();
    emaVal = EMA_ALPHA * raw + (1 - EMA_ALPHA) * emaVal;
  }
  return emaVal;
}

long sampleAverage(uint32_t dur_ms, const char* msg) {
  lcd.clear();
  lcdWrite16(0, msg);
  lcdWrite16(1, "Sampling...    ");

  uint32_t t0 = millis();
  long sum = 0; long n = 0;
  while (millis() - t0 < dur_ms) {
    readSmooth();
    sum += (long)emaVal;
    n++;
    delay(5);
  }
  return (n > 0) ? (sum / n) : (long)emaVal;
}

/* ---------- MAC helpers ---------- */
static bool isAllZero(const uint8_t mac[6]) {
  for (int i = 0; i < 6; ++i) if (mac[i] != 0) return false;
  return true;
}

static void macToString(const uint8_t mac[6], char out17[18], bool withColons=true) {
  if (withColons) {
    sprintf(out17, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    sprintf(out17, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
}

/** Get a stable STA MAC (compatible with Arduino-ESP32 v3.x / IDF v5)
 *  1) Read hardware MAC from eFuse via esp_read_mac
 *  2) Read Wi‑Fi driver MAC via WiFi/esp_wifi APIs
 *  3) If driver MAC is all zeros: esp_wifi_start() and read again; if still zeros, set driver MAC from eFuse
 */
String getStableStaMac(char lcdShort[13]) {
  // 1) eFuse
  uint8_t efuseMac[6] = {0};
  esp_err_t er = esp_read_mac(efuseMac, ESP_MAC_WIFI_STA);
  if (er != ESP_OK) {
    esp_wifi_get_mac(WIFI_IF_STA, efuseMac);
  }
  char efuseStr[18]; macToString(efuseMac, efuseStr, true);

  // 2) Driver
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  uint8_t wifiMac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, wifiMac);
  bool wifiZero = isAllZero(wifiMac);

  if (wifiZero) {
    esp_wifi_start();
    delay(150);
    esp_wifi_get_mac(WIFI_IF_STA, wifiMac);
    wifiZero = isAllZero(wifiMac);

    if (wifiZero) {
      // 3) Still zeros: set driver MAC from eFuse, then read back
      esp_wifi_set_mac(WIFI_IF_STA, efuseMac);
      delay(50);
      esp_wifi_get_mac(WIFI_IF_STA, wifiMac);
    }
  }

  char wifiStr[18]; macToString(wifiMac, wifiStr, true);
  char shortNoColon[18]; macToString(wifiMac, shortNoColon, false);
  strncpy(lcdShort, shortNoColon, 12); lcdShort[12] = '\0';

  Serial.println("=== MAC Check ===");
  Serial.printf("eFuse STA MAC : %s\n", efuseStr);
  Serial.printf("WiFi  STA MAC : %s\n", wifiStr);
  if (strcmp(efuseStr, wifiStr) != 0) {
    Serial.println("Note: WiFi MAC differs from eFuse (overridden or interim).");
  }
  Serial.println("=================");

  return String(wifiStr);
}

/* ---------- Setup ---------- */
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  lcd.init(); lcd.backlight(); lcd.clear();

  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);

  // Obtain stable MAC and show 12‑char (no colons) shorthand on LCD
  char lcdMac12[13];
  String macFull = getStableStaMac(lcdMac12);

  Serial.println("\n=== ESP32 HX711 Calibration (Simplified) ===");
  Serial.printf("Device MAC (STA): %s\n", macFull.c_str());
  Serial.println("Follow LCD steps. Press the button when ready.");

  lcdWrite16(0, "Your MAC (STA) ");
  lcdWrite16(1, lcdMac12);
  delay(3000);

  lcd.clear();
  lcdWrite16(0, "Press to start ");
}

/* ---------- Main Loop ---------- */
enum Step { S_WAIT, S_EMPTY, S_PHONE, S_DONE };
Step step = S_WAIT;

void loop() {
  if (!buttonPressed()) return;

  switch (step) {
    case S_WAIT:
      lcd.clear();
      lcdWrite16(0, "Step1: NoPhone ");
      lcdWrite16(1, "Keep empty...  ");
      delay(1200);
      NO_PHONE_VAL = sampleAverage(SAMPLE_TIME_MS, "Sampling empty ");
      Serial.printf("\nNO_PHONE_VAL = %ld\n", NO_PHONE_VAL);
      lcd.clear();
      lcdWrite16(0, "Done empty     ");
      lcdWrite16(1, "Press for phone");
      step = S_EMPTY;
      break;

    case S_EMPTY:
      lcd.clear();
      lcdWrite16(0, "Step2: Phone   ");
      lcdWrite16(1, "Place phone... ");
      delay(1200);
      WITH_PHONE_VAL = sampleAverage(SAMPLE_TIME_MS, "Sampling phone ");
      Serial.printf("WITH_PHONE_VAL = %ld\n", WITH_PHONE_VAL);
      lcd.clear();
      lcdWrite16(0, "Done phone     ");
      lcdWrite16(1, "Generating conf");
      delay(800);
      step = S_PHONE;
      break;

    case S_PHONE: {
      long gap = WITH_PHONE_VAL - NO_PHONE_VAL;
      long sug = gap / 6;
      if (sug < 300)  sug = 300;
      if (sug > 1200) sug = 1200;
      Serial.println("\n=== Recommended Config ===");
      Serial.printf("float NO_PHONE_VAL   = %ld;\n", NO_PHONE_VAL);
      Serial.printf("float WITH_PHONE_VAL = %ld;\n", WITH_PHONE_VAL);
      Serial.printf("float DETECT_MARGIN  = %ld;\n", sug);
      Serial.println("===========================");
      lcd.clear();
      lcdWrite16(0, "Config printed ");
      lcdWrite16(1, "Check Serial   ");
      step = S_DONE;
      break;
    }

    case S_DONE:
      lcdWrite16(0, "Press to repeat");
      lcdWrite16(1, "if needed      ");
      step = S_WAIT;
      delay(800);
      break;
  }
}
