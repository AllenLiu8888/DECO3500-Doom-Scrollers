#include <Arduino.h>
#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ======== 可调参数 ======== */
// 蜂鸣器类型：0=无源(PWM-需驱动) 1=有源-高电平触发 2=有源-低电平触发(蓝色板)
#define BUZZ_TYPE 2

// 舵机报警扫动
#define SERVO_MIN_DEG            0
#define SERVO_MAX_DEG            90
#define SERVO_STEP_DEG           10     // 大=快
#define SERVO_STEP_INTERVAL_MS   2     // 小=快

// 固定区间检测（将下面两个值换成你设备的实际均值）
float NO_PHONE_VAL    = 82000;   // 空板均值
float WITH_PHONE_VAL  = 95000;   // 放手机均值
float DETECT_MARGIN   = 700;     // 容差（300~1200 视抖动调）

// 番茄钟时长（毫秒）
uint32_t FOCUS_MS = 1UL * 60UL * 1000UL;  // 25:00
uint32_t BREAK_MS = 1UL  * 60UL * 1000UL;  // 05:00

// HX711 平滑
#define EMA_ALPHA 0.20f

// I2C 频率
#define I2C_CLOCK_HZ 50000

/* ======== 引脚 ======== */
#define PIN_HX_DOUT 16
#define PIN_HX_SCK  4
#define PIN_SERVO   18
#define PIN_BUZZ    19
#define PIN_BTN     23
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define LCD_ADDR    0x27
#define LCD_COLS    16
#define LCD_ROWS    2

/* ======== 对象/变量 ======== */
HX711 scale;
Servo  servo;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

enum State { IDLE, WAIT_PHONE, FOCUS, ALERT, BREAK_TIME };
State state = IDLE;

bool     running = false;
float    emaVal  = 0.0f;
float    THRESH  = 0;

bool     btnLast = true;
uint32_t btnLastMs = 0;
const uint16_t BTN_DEBOUNCE_MS = 35;

bool     servoAttached = false;
int      alertPos = SERVO_MIN_DEG;
int      alertDir = +SERVO_STEP_DEG;
uint32_t lastServoStepMs = 0;

uint32_t stateStartMs = 0;
uint32_t lastUiMs     = 0;

/* ======== LCD工具：固定16列ASCII ======== */
void lcdWrite16(uint8_t row, const char* text) {
  lcd.setCursor(0, row);
  for (int i = 0; i < 16; ++i) {
    char c = text[i];
    if (c == '\0') c = ' ';
    if (c < 32 || c > 126) c = ' ';
    lcd.print(c);
  }
}
void lcdWriteMMSS(uint8_t row, uint8_t col, uint32_t remain_ms) {
  uint32_t sec = remain_ms / 1000;
  uint8_t mm = sec / 60, ss = sec % 60;
  lcd.setCursor(col, row);
  char buf[6]; sprintf(buf, "%02u:%02u", mm, ss);
  lcd.print(buf);
}

/* ======== 蜂鸣器 ======== */
#if (BUZZ_TYPE == 0)
  #include "esp32-hal-ledc.h"
  #define BUZZ_LEDC_CH 0
  #define BUZZ_TONE_HZ 2000
#endif
void buzzInit() {
#if (BUZZ_TYPE == 0)
  ledcSetup(BUZZ_LEDC_CH, BUZZ_TONE_HZ, 8);
  ledcAttachPin(PIN_BUZZ, BUZZ_LEDC_CH);
  ledcWrite(BUZZ_LEDC_CH, 0);
#elif (BUZZ_TYPE == 1)
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT); // 开漏：高阻=关
#endif
}
void beepOn() {
#if (BUZZ_TYPE == 0)
  ledcWriteTone(BUZZ_LEDC_CH, BUZZ_TONE_HZ);
#elif (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, HIGH);
#else
  pinMode(PIN_BUZZ, OUTPUT); digitalWrite(PIN_BUZZ, LOW);
#endif
}
void beepOff() {
#if (BUZZ_TYPE == 0)
  ledcWrite(BUZZ_LEDC_CH, 0);
#elif (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT);
#endif
}

/* ======== 舵机 ======== */
void servoEnsureAttached() { if (!servoAttached) { servo.attach(PIN_SERVO); servoAttached = true; } }
void servoDetachSafe()     { if (servoAttached)  { servo.detach(); servoAttached = false; } }
void servoHome()           { if (servoAttached)  { servo.write(SERVO_MIN_DEG); } }

/* ======== LCD/I2C ======== */
void setupLCD() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  lcd.init(); lcd.backlight(); lcd.clear();
  lcdWrite16(0, "Pomodoro Ready ");
  lcdWrite16(1, "Press button > ");
}

/* ======== HX711 ======== */
float readSmooth() {
  static float ema = 0;
  if (scale.is_ready()) {
    long raw = scale.read();
    ema = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * ema;
  }
  emaVal = ema;
  return emaVal;
}
inline bool phonePresent(float value) {
  return value > (THRESH - DETECT_MARGIN);
}

/* ======== 按钮 ======== */
void handleButton() {
  bool reading = digitalRead(PIN_BTN);
  uint32_t now = millis();
  if (reading != btnLast && (now - btnLastMs) > BTN_DEBOUNCE_MS) {
    btnLast = reading; btnLastMs = now;
    if (reading == LOW) {
      running = !running;
      if (running) {
        stateStartMs = millis();
        state = WAIT_PHONE;
        lcdWrite16(0, "Place your phone");
        lcdWrite16(1, "to start focus ");
      } else {
        state = IDLE; beepOff(); servoDetachSafe();
        lcdWrite16(0, "Pomodoro Ready ");
        lcdWrite16(1, "Press button > ");
      }
    }
  }
}

/* ======== 状态切换 ======== */
void enterState(State s) {
  state = s; stateStartMs = millis(); lastUiMs = 0; lcd.clear();
  switch (state) {
    case IDLE:
      beepOff(); servoDetachSafe();
      lcdWrite16(0, "Pomodoro Ready ");
      lcdWrite16(1, "Press button > ");
      break;
    case WAIT_PHONE:
      beepOff(); servoDetachSafe();
      lcdWrite16(0, "Place your phone");
      lcdWrite16(1, "to start focus ");
      break;
    case FOCUS:
      beepOff(); servoDetachSafe();
      lcdWrite16(0, "Focus mode     ");
      lcdWrite16(1, "Time left 00:00");
      break;
    case ALERT:
      servoEnsureAttached(); beepOn();
      alertPos = SERVO_MIN_DEG; alertDir = +SERVO_STEP_DEG;
      lcdWrite16(0, "Phone moved!   ");
      lcdWrite16(1, "Put phone back!");
      break;
    case BREAK_TIME:
      beepOff(); servoDetachSafe();
      lcdWrite16(0, "Break time     ");
      lcdWrite16(1, "Relax   00:00  ");
      break;
  }
}

/* ======== SETUP ======== */
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  buzzInit(); setupLCD();
  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  emaVal = NO_PHONE_VAL;
  THRESH = (NO_PHONE_VAL + WITH_PHONE_VAL) / 2.0f;
  state = IDLE;
}

/* ======== LOOP ======== */
void loop() {
  handleButton();
  if (!running) return;

  float val = readSmooth();
  bool present = phonePresent(val);
  uint32_t now = millis();

  switch (state) {
    case WAIT_PHONE:
      if (present) enterState(FOCUS);
      break;

    case FOCUS: {
      if (!present) { enterState(ALERT); break; }
      uint32_t elapsed = now - stateStartMs;
      if (elapsed >= FOCUS_MS) { enterState(BREAK_TIME); break; }
      if (now - lastUiMs >= 250) {
        lastUiMs = now;
        lcdWrite16(0, "Focus mode     ");
        lcdWrite16(1, "Time left 00:00");
        lcdWriteMMSS(1, 10, FOCUS_MS - elapsed); // 行2列10写mm:ss
      }
    } break;

    case ALERT:
      if (now - lastServoStepMs >= SERVO_STEP_INTERVAL_MS) {
        lastServoStepMs = now;
        alertPos += alertDir;
        if (alertPos >= SERVO_MAX_DEG) { alertPos = SERVO_MAX_DEG; alertDir = -SERVO_STEP_DEG; }
        if (alertPos <= SERVO_MIN_DEG) { alertPos = SERVO_MIN_DEG; alertDir = +SERVO_STEP_DEG; }
        servo.write(alertPos);
      }
      if (present) enterState(FOCUS); // 恢复专注，剩余时间继续
      break;

    case BREAK_TIME: {
      uint32_t elapsed = now - stateStartMs;
      if (elapsed >= BREAK_MS) { enterState(WAIT_PHONE); break; }
      if (now - lastUiMs >= 250) {
        lastUiMs = now;
        lcdWrite16(0, "Break time     ");
        lcdWrite16(1, "Relax   00:00  ");
        lcdWriteMMSS(1, 8, BREAK_MS - elapsed); // 行2列8写mm:ss
      }
    } break;

    case IDLE: default: break;
  }
}
