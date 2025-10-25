/* =============================================================================
 *  Dual ESP-NOW Pomodoro System (Auto-Resume) - Device A
 *
 *  Module Outline (numbered):
 *  1. Headers and macros (incl. ESP-NOW/IDF)
 *  2. Top-level configuration (device name, peer MAC, HX711 thresholds, durations, pins, channel)
 *  3. Global objects and runtime state (LCD/HX711/Servo/flags/timers)
 *  4. LCD/log helpers (lcdWrite16, lcdWriteMMSS, logErr/logInfo)
 *  5. Buzzer driver (BUZZ_TYPE 0/1/2; default 1 active-high)
 *  6. Servo driver (attach/detach/home/non-blocking sweep helper)
 *  7. HX711 sampling and presence decision (EMA smoothing)
 *  8. Button handling (debounce, min toggle gap, enter/exit running)
 *  9. ESP-NOW protocol (message struct, send, recv callback, READY-ACK)
 *  10. State entry helper (enterState)
 *  11. setup() init (explicit Wi-Fi start, read real MAC, set channel, register peer)
 *  12. loop() main (READY tick, peer-timeout log, business state machine)
 *  ========================================================================== */

 // ────────────────────────────────────────────────────────────────────────────
// 1. Headers and macros
 // ────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_err.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <ESP32Servo.h>

#define IS_A_SIDE 1  // Device A: sends START when both sides are READY

// ────────────────────────────────────────────────────────────────────────────
// 2. Top-level configuration
 // ────────────────────────────────────────────────────────────────────────────
const char* MY_NAME = "A";  // For logging: identify local device role

// Peer (B) STA MAC — paste B's MAC printed by TestingBeforeStart here
// uint8_t PEER_MAC[6] = {0x68, 0xFE, 0x71, 0x88, 0x2B, 0x64};
uint8_t PEER_MAC[6] = {0x68, 0xFE, 0x71, 0x88, 0x2B, 0x40};


// Buzzer type: 0=passive PWM, 1=active-high (default), 2=active-low (blue board)
#define BUZZ_TYPE 1

// Servo parameters (tunable):
// - SERVO_MIN_DEG / SERVO_MAX_DEG: motion range in degrees. Typical 0–180; we use 0–120 to avoid stalling.
// - SERVO_STEP_DEG: angle step per update. Larger = faster sweep but choppier.
// - SERVO_STEP_INTERVAL_MS: delay (ms) between steps. Larger = slower movement and longer pause.
//   Tuning hint: increase STEP to speed up; increase INTERVAL to slow down. Combine both for feel.
#define SERVO_MIN_DEG            0
#define SERVO_MAX_DEG            120
#define SERVO_STEP_DEG           8
#define SERVO_STEP_INTERVAL_MS   4

// HX711 thresholds — paste the three recommended values from TestingBeforeStart here
float NO_PHONE_VAL    = 82000;
float WITH_PHONE_VAL  = 95000;
float DETECT_MARGIN   = 700;
#define EMA_ALPHA 0.20f

// Pomodoro durations in milliseconds (user editable)
uint32_t FOCUS_MS = 25UL * 1000UL;
uint32_t BREAK_MS =  10UL * 1000UL;

// I2C / LCD
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define I2C_CLOCK_HZ 50000
#define LCD_ADDR 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// Pins
#define PIN_BTN     23
#define PIN_BUZZ    19
#define PIN_SERVO   18
#define PIN_HX_DOUT 16
#define PIN_HX_SCK   4

// ESP-NOW
#define ESPNOW_CHANNEL   1     // ESP-NOW channel; must match on both devices (default 1)
#define PEER_TIMEOUT_MS  5000  // Peer-timeout log interval (ms)

// ────────────────────────────────────────────────────────────────────────────
// 3. Global objects and runtime state
 // ────────────────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
HX711 scale;
Servo  servo;

bool lcdOK = false;
bool servoAttached = false;

enum LocState { IDLE, WAIT_PHONE, FOCUS, ALERT, BREAK_TIME };
LocState state = IDLE;

bool running      = false;   // Whether in READY (paired) workflow
bool ready_local  = false;   // Whether local button pressed
bool ready_peer   = false;   // Whether peer READY received
bool readyAckSent = false;   // Whether ACK already sent in response to peer READY

float emaVal = 0;
float THRESH = 0;            // Dynamic threshold used by phonePresent()

uint32_t stateStartMs    = 0;
uint32_t lastUiMs        = 0;
uint32_t lastServoStepMs = 0;
uint32_t lastPeerHeard   = 0;
uint32_t lastReadyTxMs   = 0;  // READY periodic tick timestamp

// Button debounce
bool     btnLast = true;
uint32_t btnLastMs = 0;
uint32_t lastToggleMs = 0;
const uint16_t BTN_DEBOUNCE_MS = 50;
const uint16_t BTN_MIN_TOGGLE_GAP_MS = 300;

// ────────────────────────────────────────────────────────────────────────────
// 4. LCD / logging utils
 // ────────────────────────────────────────────────────────────────────────────
void logErr(const char* msg){ Serial.printf("[ERR] %s\n", msg); }
void logInfo(const char* msg){ Serial.printf("[INFO] %s\n", msg); }

void lcdWrite16(uint8_t row, const char* s){
  if (!lcdOK) return;
  lcd.setCursor(0,row);
  for(int i=0;i<16;i++){
    char c = s[i];
    if(c=='\0' || c<32 || c>126) c=' ';
    lcd.print(c);
  }
}
void lcdWriteMMSS(uint8_t row, uint8_t col, uint32_t remain_ms){
  if (!lcdOK) return;
  uint32_t sec = remain_ms/1000;
  uint8_t mm = sec/60, ss = sec%60;
  char buf[6]; sprintf(buf,"%02u:%02u",mm,ss);
  lcd.setCursor(col,row); lcd.print(buf);
}

// ────────────────────────────────────────────────────────────────────────────
// 5. Buzzer driver (switch BUZZ_TYPE=0/1/2 to match hardware; default 1)
 // ────────────────────────────────────────────────────────────────────────────
#if (BUZZ_TYPE==0)
  #include "esp32-hal-ledc.h"
  #define BUZZ_LEDC_CH 0
  #define BUZZ_TONE_HZ 2000
#endif
void buzzInit(){
#if (BUZZ_TYPE==0)
  ledcSetup(BUZZ_LEDC_CH, BUZZ_TONE_HZ, 8);
  ledcAttachPin(PIN_BUZZ, BUZZ_LEDC_CH);
  ledcWrite(BUZZ_LEDC_CH, 0);
#elif (BUZZ_TYPE==1)
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT); // Active-low buzzer: INPUT=off, OUTPUT+LOW=on
#endif
}
void beepOn(){
#if (BUZZ_TYPE==0)
  ledcWriteTone(BUZZ_LEDC_CH, BUZZ_TONE_HZ);
#elif (BUZZ_TYPE==1)
  digitalWrite(PIN_BUZZ, HIGH);
#else
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);
#endif
}
void beepOff(){
#if (BUZZ_TYPE==0)
  ledcWrite(BUZZ_LEDC_CH, 0);
#elif (BUZZ_TYPE==1)
  digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT);
#endif
}

// ────────────────────────────────────────────────────────────────────────────
// 6. Servo driver
 // ────────────────────────────────────────────────────────────────────────────
void servoEnsureAttached(){ if(!servoAttached){ servo.attach(PIN_SERVO); servoAttached=true; } }
void servoDetachSafe(){ if(servoAttached){ servo.detach(); servoAttached=false; } }
void servoHome(){ if(servoAttached){ servo.write(SERVO_MIN_DEG); } }

// ────────────────────────────────────────────────────────────────────────────
// 7. HX711 sampling and decision
 // ────────────────────────────────────────────────────────────────────────────
float readSmooth(){
  static float ema=0;
  if (scale.is_ready()){
    long raw = scale.read();
    if (ema==0) ema = raw;
    ema = EMA_ALPHA*raw + (1.0f-EMA_ALPHA)*ema;
  }
  emaVal = ema; return emaVal;
}
inline bool phonePresent(float v){ return v > (THRESH - DETECT_MARGIN); }

// ────────────────────────────────────────────────────────────────────────────
// 8. Button handling
 // ────────────────────────────────────────────────────────────────────────────
bool buttonPressedEdgeStable(){
  bool r = digitalRead(PIN_BTN);
  uint32_t now = millis();
  if (r != btnLast && (now - btnLastMs) > BTN_DEBOUNCE_MS){
    btnLast = r; btnLastMs = now; return (r == LOW);
  }
  return false;
}
void handleButton(){
  if (!buttonPressedEdgeStable()) return;
  uint32_t now = millis();
  if (now - lastToggleMs < BTN_MIN_TOGGLE_GAP_MS) return;
  lastToggleMs = now;

  bool prev = running;
  running = !running;

  if (running && !prev){
    ready_local = true; ready_peer = false; readyAckSent = false;
    state = WAIT_PHONE; stateStartMs = millis();
    lcdWrite16(0,"Waiting partner");
    lcdWrite16(1,"Press button...");
  }else if (!running && prev){
    ready_local = false; ready_peer = false; readyAckSent = false;
    beepOff(); servoDetachSafe();
    state = IDLE;
    lcdWrite16(0,"Pomodoro Ready ");
    lcdWrite16(1,"Press button > ");
  }
}

// ────────────────────────────────────────────────────────────────────────────
// 9. ESP-NOW protocol (READY-ACK + error logging)
 // ────────────────────────────────────────────────────────────────────────────
constexpr uint8_t MSG_READY      = 1;
constexpr uint8_t MSG_CANCEL     = 2;
constexpr uint8_t MSG_START      = 3;
constexpr uint8_t MSG_ALERT_SELF = 4;
constexpr uint8_t MSG_CLEARED    = 5;

struct __attribute__((packed)) Msg {
  uint8_t  type;
  uint32_t ts;
  uint32_t arg;
  char     from[4];
};

esp_now_peer_info_t peer{};

void sendMsg(uint8_t t, uint32_t arg=0){
  Msg m; m.type=t; m.ts=millis(); m.arg=arg;
  strncpy(m.from, MY_NAME, 3); m.from[3]='\0';
  esp_err_t r = esp_now_send(PEER_MAC, (uint8_t*)&m, sizeof(Msg));
  if (r != ESP_OK) Serial.printf("[ERR] sendMsg type=%u esp_err=0x%X\n", t, (unsigned)r);
}
void onRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len != sizeof(Msg)) return;
  Msg m; memcpy(&m, data, sizeof(Msg));
  lastPeerHeard = millis();

  switch(m.type){
    case MSG_READY: {
      bool wasReadyPeer = ready_peer;
      ready_peer = true;
      Serial.println("[NET] Peer READY");

      // First time seeing peer READY while running && ready_local=true → send ACK (once)
      if (running && ready_local && !wasReadyPeer && !readyAckSent){
        sendMsg(MSG_READY);
        readyAckSent = true;
        Serial.println("[NET] READY-ACK sent");
      }

    #if IS_A_SIDE
      // Device A: both READY → send START once and enter FOCUS
      if (ready_local && ready_peer){
        Serial.println("[NET] Both READY -> START");
        sendMsg(MSG_START, FOCUS_MS);
        THRESH = (NO_PHONE_VAL + WITH_PHONE_VAL)/2.0f;
        state = FOCUS; stateStartMs = millis(); lastUiMs=0;
        lcdWrite16(0,"Focus mode     "); lcdWrite16(1,"Time left 25:00");
      }
    #else
      // Device B waits for A's START
      if (ready_local && ready_peer){
        lcdWrite16(0,"Waiting partner");
        lcdWrite16(1,"Focus starting ");
      }
    #endif
      break;
    }

    case MSG_CANCEL:
      ready_peer = false;
      beepOff(); servoDetachSafe();
      state = IDLE;
      lcdWrite16(0,"Partner cancel "); lcdWrite16(1,"Press button > ");
      Serial.println("[NET] Peer CANCEL");
      break;

    case MSG_START:
      ready_local = true; ready_peer = true;
      THRESH = (NO_PHONE_VAL + WITH_PHONE_VAL)/2.0f;
      state = FOCUS; stateStartMs = millis(); lastUiMs = 0;
      lcdWrite16(0,"Focus mode     "); lcdWrite16(1,"Time left 25:00");
      Serial.println("[NET] START received -> FOCUS");
      break;

    case MSG_ALERT_SELF:
      // Peer lifted phone: beep + show prompt + return to WAIT_PHONE
      beepOn(); servoDetachSafe();
      lcdWrite16(0,"Partner moved! "); lcdWrite16(1,"Put phone back!");
      state = WAIT_PHONE; stateStartMs = millis();
      Serial.println("[NET] Peer ALERT");
      break;

    case MSG_CLEARED: {
      // Peer replaced phone: if local present → resume FOCUS and send START again (idempotent)
      beepOff(); servoDetachSafe();
      bool presentNow = phonePresent(readSmooth());
      if (presentNow) {
        ready_local = true; ready_peer = true;
        state = FOCUS; stateStartMs = millis(); lastUiMs = 0;
        lcdWrite16(0,"Focus mode     "); lcdWrite16(1,"Time left 25:00");
        Serial.println("[NET] Peer CLEARED -> resume FOCUS & SEND START");
        sendMsg(MSG_START, FOCUS_MS);
      } else {
        lcdWrite16(0,"Partner ready  "); lcdWrite16(1,"Place phone    ");
        Serial.println("[NET] Peer CLEARED (waiting local)");
      }
      break;
    }

    default: break;
  }
}
void onSend(const wifi_tx_info_t*, esp_now_send_status_t s){
  if (s != ESP_NOW_SEND_SUCCESS) Serial.printf("[NET] TX failed (status=%d)\n", (int)s);
}

// ────────────────────────────────────────────────────────────────────────────
// 10. State entry helper
 // ────────────────────────────────────────────────────────────────────────────
void enterState(LocState s){
  state = s; stateStartMs = millis(); lastUiMs = 0;
  switch(s){
    case IDLE:
      lcdWrite16(0,"Pomodoro Ready "); lcdWrite16(1,"Press button > "); break;
    case WAIT_PHONE:
      lcdWrite16(0,"Place your phone"); lcdWrite16(1,"to start focus "); break;
    case FOCUS:
      lcdWrite16(0,"Focus mode     "); lcdWrite16(1,"Time left 25:00"); break;
    case ALERT:
      servoEnsureAttached(); beepOn();
      lcdWrite16(0,"Phone moved!   "); lcdWrite16(1,"Put phone back!");
      sendMsg(MSG_ALERT_SELF);
      break;
    case BREAK_TIME:
      lcdWrite16(0,"Break time     "); lcdWrite16(1,"Relax   05:00  "); break;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// 11. setup() (explicit Wi-Fi start, read real MAC, set channel)
 // ────────────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);

  // I2C/LCD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.beginTransmission(LCD_ADDR);
  if (Wire.endTransmission()==0){ lcd.init(); lcd.backlight(); lcdOK = true; }
  else logErr("LCD not found at 0x27 (headless)");
  if (lcdOK){ lcd.clear(); lcdWrite16(0,"Pomodoro Ready "); lcdWrite16(1,"Press button > "); }

  // Peripherals
  pinMode(PIN_BTN, INPUT_PULLUP);
  buzzInit(); servoDetachSafe();
  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  THRESH = (NO_PHONE_VAL + WITH_PHONE_VAL)/2.0f;

  // Wi-Fi / ESP-NOW: explicit start + read real MAC + set channel + register peer
  WiFi.mode(WIFI_STA);
  ESP_ERROR_CHECK( esp_wifi_start() );  // explicitly start Wi-Fi driver
  uint8_t sta_mac[6]={0};
  ESP_ERROR_CHECK( esp_read_mac(sta_mac, ESP_MAC_WIFI_STA) );
  Serial.printf("[NET] My %s STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                MY_NAME, sta_mac[0],sta_mac[1],sta_mac[2],sta_mac[3],sta_mac[4],sta_mac[5]);

  ESP_ERROR_CHECK( esp_wifi_set_promiscuous(true) );
  ESP_ERROR_CHECK( esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) );
  ESP_ERROR_CHECK( esp_wifi_set_promiscuous(false) );

  ESP_ERROR_CHECK( esp_now_init() );
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, PEER_MAC, 6);
  peer.channel = ESPNOW_CHANNEL; peer.encrypt = false;
  ESP_ERROR_CHECK( esp_now_add_peer(&peer) );
  bool peer_ok = esp_now_is_peer_exist(PEER_MAC);
  Serial.printf("[NET] Peer %02X:%02X:%02X:%02X:%02X:%02X %s\n",
                PEER_MAC[0],PEER_MAC[1],PEER_MAC[2],PEER_MAC[3],PEER_MAC[4],PEER_MAC[5],
                peer_ok ? "added" : "NOT ADDED!");
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSend);

  state = IDLE;
  logInfo("Setup done.");
}

// ────────────────────────────────────────────────────────────────────────────
// 12. loop() (READY periodic broadcast)
 // ────────────────────────────────────────────────────────────────────────────
void loop(){
  handleButton();
  uint32_t now = millis();

  // READY periodic send: when running and peer not yet READY → send every second
  if (running && !ready_peer){
    if (now - lastReadyTxMs >= 1000){
      lastReadyTxMs = now;
      sendMsg(MSG_READY);
      Serial.println("[NET] READY tick");
    }
  }

  // Peer timeout logging
  if (lastPeerHeard && (now - lastPeerHeard > PEER_TIMEOUT_MS)){
    Serial.println("[NET] Peer timeout");
    lastPeerHeard = now;
  }

  if (!running) return;

  float val = readSmooth();
  bool present = phonePresent(val);

  switch(state){
    case WAIT_PHONE:
      if (present && ready_peer){
        lcdWrite16(0,"Waiting partner");
        lcdWrite16(1,"Focus starting ");
      }
      break;

    case FOCUS:
      if (!present){ enterState(ALERT); break; }
      if (now - stateStartMs >= FOCUS_MS){ enterState(BREAK_TIME); break; }
      if (now - lastUiMs >= 250){
        lastUiMs = now;
        lcdWrite16(0,"Focus mode     "); lcdWrite16(1,"Time left 00:00");
        lcdWriteMMSS(1,10, FOCUS_MS - (now - stateStartMs));
      }
      break;

    case ALERT:
      if (servoAttached && (now - lastServoStepMs >= SERVO_STEP_INTERVAL_MS)){
        lastServoStepMs = now;
        static int pos = SERVO_MIN_DEG, dir = +SERVO_STEP_DEG;
        pos += dir;
        if (pos>=SERVO_MAX_DEG){ pos=SERVO_MAX_DEG; dir=-SERVO_STEP_DEG; }
        if (pos<=SERVO_MIN_DEG){ pos=SERVO_MIN_DEG; dir=+SERVO_STEP_DEG; }
        servo.write(pos);
      }
      if (present){
        beepOff(); servoHome();
        sendMsg(MSG_CLEARED);
        state = WAIT_PHONE; stateStartMs = now;
        lcdWrite16(0,"Place your phone"); lcdWrite16(1,"to start focus ");
      }
      break;

    case BREAK_TIME:
      if (now - lastUiMs >= 250){
        lastUiMs = now;
        lcdWrite16(0,"Break time     "); lcdWrite16(1,"Relax   00:00  ");
        if (now - stateStartMs < BREAK_MS)
          lcdWriteMMSS(1,8, BREAK_MS - (now - stateStartMs));
      }
      if (now - stateStartMs >= BREAK_MS){
        state = WAIT_PHONE; stateStartMs = now;
        lcdWrite16(0,"Place your phone"); lcdWrite16(1,"to start focus ");
      }
      break;

    default: break;
  }
}