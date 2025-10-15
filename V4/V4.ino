#include <Arduino.h>
#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ========= 顶部配置区（你只用改这里） ========= */
// 蜂鸣器类型：0=无源(需PWM)；1=有源-高电平触发；2=有源-低电平触发（蓝色板）
#define BUZZ_TYPE 2

// 舵机报警扫动参数
#define SERVO_MIN_DEG         0      // 最小角度
#define SERVO_MAX_DEG         180    // 最大角度
#define SERVO_STEP_DEG        4      // 每步移动角度（越大越快）
#define SERVO_STEP_INTERVAL_MS 8     // 每步间隔毫秒（越小越快）

// 手机放回后需要稳定多久才退出报警
#define PHONE_BACK_HOLD_MS    600

// Pomodoro 时长（毫秒）
uint32_t FOCUS_MS = 25UL * 60UL * 1000UL;
uint32_t BREAK_MS =  5UL * 60UL * 1000UL;
/* ============================================ */

// 引脚
#define PIN_HX_DOUT   16
#define PIN_HX_SCK    4
#define PIN_SERVO     18
#define PIN_BUZZ      19
#define PIN_BTN       23

// I2C LCD1602
#define I2C_SDA_PIN   21
#define I2C_SCL_PIN   22
#define LCD_ADDR      0x27
#define LCD_COLS      16
#define LCD_ROWS      2

// HX711 平滑与阈值
const uint16_t BASELINE_TIME_MS = 2500;
const float    EMA_ALPHA        = 0.2f;
const long     REMOVE_TRIG      = 3000;  // <= -3000 认为拿起
const long     REMOVE_RELEASE   = 1200;

enum SysState { IDLE, WAIT_PHONE, FOCUS, BREAK, ALERT };
enum Mode     { M_LCD=1, M_HX711=2, M_BUZZ=3, M_SERVO=4, M_BUTTON=5, M_FULL=6 };

Mode     currentMode = M_LCD;
SysState state       = IDLE;

HX711 scale;
Servo  servo;
bool   servoAttached = false;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
bool   lcdOK = false;

long   baseline = 0;
float  emaRaw   = 0.0f;
bool   phone_on = false;
bool   running  = false;
uint32_t stateStart = 0;

// 按钮消抖
bool     btnLast   = true;
uint32_t btnLastMs = 0;
const    uint16_t BTN_DEBOUNCE_MS = 35;

// 舵机报警状态
int       alertPos = SERVO_MIN_DEG;
int       alertDir = +SERVO_STEP_DEG;
uint32_t  alertLastMs = 0;
uint32_t  phoneBackStableMs = 0;

/* -------- 蜂鸣器：按类型编译 -------- */
#if (BUZZ_TYPE == 0)        // 无源（PWM）
  #include "esp32-hal-ledc.h"
  #define BUZZ_LEDC_CH     0
  #define BUZZ_TONE_HZ     2000
#elif (BUZZ_TYPE == 1)      // 有源-高电平触发
  // 无需额外包含
#elif (BUZZ_TYPE == 2)      // 有源-低电平触发（蓝色板）
  // 无需额外包含
#else
  #error "BUZZ_TYPE must be 0,1,or 2"
#endif

// void buzzInit() {
// #if (BUZZ_TYPE == 0) // 无源
//   ledcSetup(BUZZ_LEDC_CH, BUZZ_TONE_HZ, 8);
//   ledcAttachPin(PIN_BUZZ, BUZZ_LEDC_CH);
//   ledcWrite(BUZZ_LEDC_CH, 0);
// #else                 // 有源
//   pinMode(PIN_BUZZ, OUTPUT);
//   // 初始静音
//   #if (BUZZ_TYPE == 1) // 高电平触发
//     digitalWrite(PIN_BUZZ, LOW);
//   #else                // 低电平触发
//     digitalWrite(PIN_BUZZ, HIGH);
//   #endif
// #endif
// }

// void beepOn() {
// #if (BUZZ_TYPE == 0)        // 无源：输出音调
//   ledcWriteTone(BUZZ_LEDC_CH, BUZZ_TONE_HZ);
// #elif (BUZZ_TYPE == 1)      // 有源-高电平触发：拉高响
//   digitalWrite(PIN_BUZZ, HIGH);
// #else                       // 有源-低电平触发：拉低响（蓝色板）
//   digitalWrite(PIN_BUZZ, LOW);
// #endif
// }
// void beepOff() {
// #if (BUZZ_TYPE == 0)
//   ledcWrite(BUZZ_LEDC_CH, 0);
// #elif (BUZZ_TYPE == 1)
//   digitalWrite(PIN_BUZZ, LOW);
// #else
//   digitalWrite(PIN_BUZZ, HIGH);
// #endif
// }
// 低电平触发板（蓝色 MH-FMD 等）——开漏式控制
void buzzInit() {
  // 初始为“关”：高阻输入，交给板载5V上拉
  pinMode(PIN_BUZZ, INPUT);
}

void beepOn() {
  // 触发“响”：输出低电平
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);
}

void beepOff() {
  // 关闭：改回输入高阻（不要输出HIGH）
  pinMode(PIN_BUZZ, INPUT);
  // 不需要再写电平
}

/* -------- 舵机 -------- */
void servoEnsureAttached() { if (!servoAttached) { servo.attach(PIN_SERVO); servoAttached = true; } }
void servoDetach()         { if (servoAttached)  { servo.detach(); servoAttached = false; } }
void servoHome()           { if (servoAttached)  { servo.write(SERVO_MIN_DEG); } }

/* -------- LCD / 菜单 -------- */
void lcdMsg(int r,int c,const String& s){ if(lcdOK){ lcd.setCursor(c,r); lcd.print(s); } }
void printMenu() {
  Serial.println("\n=== Select Test Mode (type number then Enter) ===");
  Serial.println("1) LCD Test");
  Serial.println("2) HX711 Test");
  Serial.println("3) Buzzer Test");
  Serial.println("4) Servo Test");
  Serial.println("5) Button Test");
  Serial.println("6) Full System (Pomodoro)");
}
void setupLCD() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);
  delay(120);
  lcdOK = true;
  lcd.init(); lcd.backlight(); lcd.clear();
}

/* -------- Setup -------- */
void setup() {
  Serial.begin(115200);
  delay(200);

  buzzInit();
  beepOff();
  pinMode(PIN_BTN, INPUT_PULLUP);

  setupLCD();
  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  servoDetach();

  if (lcdOK) { lcdMsg(0,0,"Mode: LCD Test"); lcdMsg(1,0,"Type 1..6"); }
  printMenu();
}

/* -------- 工具函数 -------- */
void msToMMSS(uint32_t ms, char* out) {
  uint32_t sec = ms / 1000;
  uint16_t mm = sec / 60;
  uint16_t ss = sec % 60;
  sprintf(out, "%02u:%02u", mm, ss);
}
long readRawSmooth() {
  long raw = scale.is_ready() ? scale.read() : (long)emaRaw;
  emaRaw = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * emaRaw;
  return (long)emaRaw;
}
void sampleBaselineWithPhoneOn() {
  uint32_t t0 = millis(); long sum=0; int cnt=0;
  while (millis()-t0 < BASELINE_TIME_MS) { sum += readRawSmooth(); cnt++; delay(20); }
  baseline = (cnt>0) ? (sum/cnt) : 0; emaRaw = baseline;
}
void updatePhonePresence(long delta) {
  if (!phone_on && delta >= 1500)                phone_on = true;
  else if (phone_on && delta <= -REMOVE_RELEASE) phone_on = false;
}
bool phoneRemovedInstant(long delta) { return (delta <= -REMOVE_TRIG); }

void enterState(SysState s) {
  state = s; stateStart = millis();
  if (lcdOK) lcd.clear();
  switch (s) {
    case IDLE:       beepOff(); servoDetach(); lcdMsg(0,0,"Pomodoro Ready"); lcdMsg(1,0,"Press button"); break;
    case WAIT_PHONE: beepOff(); servoDetach(); lcdMsg(0,0,"Place phone");   lcdMsg(1,0,"Hold steady");  break;
    case FOCUS:      beepOff(); servoDetach(); lcdMsg(0,0,"Focus");         break;
    case BREAK:      beepOff(); servoDetach(); lcdMsg(0,0,"Break Time");    break;
    case ALERT:      /* 文案在 ALERT 动态显示 */                           break;
  }
}
void handleButton() {
  bool reading = digitalRead(PIN_BTN); uint32_t now = millis();
  if (reading != btnLast && (now - btnLastMs) > BTN_DEBOUNCE_MS) {
    btnLast = reading; btnLastMs = now;
    if (reading == LOW) {
      if (!running) { running = true;  enterState(WAIT_PHONE); }
      else          { running = false; enterState(IDLE);       }
    }
  }
}
void showCountdown(const char* label, uint32_t total_ms) {
  char buf[8]; uint32_t elapsed = millis() - stateStart;
  uint32_t remain  = (elapsed >= total_ms) ? 0 : (total_ms - elapsed);
  msToMMSS(remain, buf);
  if (lcdOK) { lcd.setCursor(0,0); lcd.print(label); lcd.setCursor(LCD_COLS-5,0); lcd.print(buf); }
}

/* -------- 测试模式 -------- */
void modeLCD() {
  static uint32_t t0=0; static bool on=true;
  if (millis()-t0 > 1000) { t0 = millis(); on=!on; if (lcdOK) { if (on) lcd.backlight(); else lcd.noBacklight(); } }
  if (lcdOK) { lcd.setCursor(0,0); lcd.print("LCD OK "); lcd.setCursor(0,1); lcd.print("Blinking BG   "); }
}
void modeHX711() {
  long raw = readRawSmooth();
  if (lcdOK) { lcd.setCursor(0,0); lcd.print("HX711 RAW:     "); lcd.setCursor(0,1); lcd.print((long)emaRaw); lcd.print("        "); }
}
void modeBuzzer() {
  static uint32_t t0=0; static bool on=false;
  if (millis()-t0 > 400) { t0=millis(); on=!on; if(on)beepOn(); else beepOff(); }
  if (lcdOK) { lcd.setCursor(0,0); lcd.print("Buzzer Test    "); lcd.setCursor(0,1); lcd.print(on?"BEEPING        ":"OFF            "); }
}
void modeServo() {
  servoEnsureAttached();
  // 用你设定的参数做一次来回扫（阻塞，仅测试）
  for (int p = SERVO_MIN_DEG; p <= SERVO_MAX_DEG; p += SERVO_STEP_DEG) { servo.write(p); delay(SERVO_STEP_INTERVAL_MS); }
  for (int p = SERVO_MAX_DEG; p >= SERVO_MIN_DEG; p -= SERVO_STEP_DEG) { servo.write(p); delay(SERVO_STEP_INTERVAL_MS); }
  servoDetach();
  if (lcdOK) { lcd.setCursor(0,0); lcd.print("Servo Test     "); lcd.setCursor(0,1); lcd.print("Sweep done     "); }
}

/* -------- 全系统（非阻塞 ALERT） -------- */
void modeFullSystem() {
  handleButton();
  if (!running) { if (lcdOK){ lcd.setCursor(0,0); lcd.print("Press button..."); lcd.setCursor(0,1); lcd.print("                "); } return; }

  long raw   = readRawSmooth();
  long delta = (long)(emaRaw - baseline);
  updatePhonePresence(delta);

  switch (state) {
    case WAIT_PHONE: {
      if (lcdOK){ lcd.setCursor(0,1); lcd.print(phone_on ? "Phone: ON       " : "Phone: OFF      "); }
      if (phone_on) { sampleBaselineWithPhoneOn(); enterState(FOCUS); }
    } break;

    case FOCUS: {
      showCountdown("Focus", FOCUS_MS);
      if (lcdOK){ lcd.setCursor(0,1); lcd.print("Phone: "); lcd.print(phone_on ? "ON " : "OFF"); }
      if (phoneRemovedInstant(delta)) {
        alertPos = SERVO_MIN_DEG; alertDir = +SERVO_STEP_DEG; alertLastMs = millis(); phoneBackStableMs = 0;
        enterState(ALERT);
      }
      else if ((millis() - stateStart) >= FOCUS_MS) enterState(BREAK);
    } break;

    case ALERT: {
      if (lcdOK) { lcd.setCursor(0,0); lcd.print("Phone removed!  "); }
      beepOn();
      servoEnsureAttached();
      uint32_t now = millis();
      if (now - alertLastMs >= SERVO_STEP_INTERVAL_MS) {
        alertLastMs = now;
        alertPos += alertDir;
        if (alertPos >= SERVO_MAX_DEG) { alertPos = SERVO_MAX_DEG; alertDir = -SERVO_STEP_DEG; }
        if (alertPos <= SERVO_MIN_DEG) { alertPos = SERVO_MIN_DEG; alertDir = +SERVO_STEP_DEG; }
        servo.write(alertPos);
      }
      long d_raw   = readRawSmooth();
      long d_delta = (long)(emaRaw - baseline);
      updatePhonePresence(d_delta);
      if (phone_on) {
        if (phoneBackStableMs == 0) phoneBackStableMs = now;
        if (now - phoneBackStableMs >= PHONE_BACK_HOLD_MS) {
          beepOff(); servoHome(); sampleBaselineWithPhoneOn(); enterState(WAIT_PHONE); phoneBackStableMs = 0;
        }
      } else {
        phoneBackStableMs = 0;
      }
    } break;

    case BREAK: {
      showCountdown("Break", BREAK_MS);
      if (lcdOK){ lcd.setCursor(0,1); lcd.print("Relax :)        "); }
      if ((millis() - stateStart) >= BREAK_MS) { if (lcdOK){ lcd.clear(); lcdMsg(0,0,"Put phone back"); lcdMsg(1,0,"to start focus"); } enterState(WAIT_PHONE); }
    } break;

    case IDLE:
    default: break;
  }
}

/* -------- 主循环 -------- */
void loop() {
  if (Serial.available()) {
    int ch = Serial.read();
    if (ch>='1' && ch<='6') {
      currentMode = (Mode)(ch - '0');
      if (lcdOK) { lcd.clear(); }
      Serial.print("Switched to mode: "); Serial.println((int)currentMode);
      if (currentMode == M_FULL) { running=false; enterState(IDLE); }
    }
  }
  switch (currentMode) {
    case M_LCD:    modeLCD();        break;
    case M_HX711:  modeHX711();      break;
    case M_BUZZ:   modeBuzzer();     break;
    case M_SERVO:  modeServo();      break;
    case M_FULL:   modeFullSystem(); break;
    default:       modeLCD();        break;
  }
  delay(10);
}
