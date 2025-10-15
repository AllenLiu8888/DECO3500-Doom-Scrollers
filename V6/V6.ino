#include <Arduino.h>
#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ============= 顶部可调参数 ============= */

// 蜂鸣器类型：0=无源(PWM, 需要额外驱动)  1=有源-高电平触发(黑色三针)  2=有源-低电平触发(蓝色板，推荐)
#define BUZZ_TYPE 2

// 舵机扫动
#define SERVO_MIN_DEG            0
#define SERVO_MAX_DEG            180
#define SERVO_STEP_DEG           8     // 越大越快
#define SERVO_STEP_INTERVAL_MS   5     // 越小越快

// 固定区间检测（用你自己的两组实际读数替换）
float NO_PHONE_VAL    = 82000;   // 空板平均值
float WITH_PHONE_VAL  = 95000;   // 放上手机平均值
float DETECT_MARGIN   = 700;     // 容差（300~1200 视抖动调节）

// 番茄钟时间（毫秒）
uint32_t FOCUS_MS = 25UL * 60UL * 1000UL;  // 25 分钟
uint32_t BREAK_MS = 5UL  * 60UL * 1000UL;  // 5 分钟

// HX711 平滑
#define EMA_ALPHA 0.20f          // 0.10~0.30 越小越稳，越大越灵

// LCD I2C 频率
#define I2C_CLOCK_HZ 50000       // 50kHz 更抗干扰

/* ============= 硬件引脚 ============= */
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

/* ============= 全局对象 ============= */
HX711 scale;
Servo  servo;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

/* ============= 运行变量 ============= */
enum State { IDLE, WAIT_PHONE, FOCUS, ALERT, BREAK_TIME };
State state = IDLE;

bool   running = false;
float  emaVal  = 0.0f;
float  THRESH  = 0;              // (NO_PHONE_VAL + WITH_PHONE_VAL)/2

// 按钮消抖
bool     btnLast   = true;
uint32_t btnLastMs = 0;
const    uint16_t BTN_DEBOUNCE_MS = 35;

// 舵机
bool     servoAttached = false;
int      alertPos = SERVO_MIN_DEG;
int      alertDir = +SERVO_STEP_DEG;
uint32_t lastServoStepMs = 0;

// 计时
uint32_t stateStartMs = 0;
uint32_t lastUiMs     = 0;

/* ============= 实用函数：固定 16 列 ASCII 写入 ============= */
void lcdWrite16(uint8_t row, const char* text) {
  lcd.setCursor(0, row);
  for (int i = 0; i < 16; ++i) {
    char c = text[i];
    if (c == '\0') { lcd.print(' '); continue; }
    if (c < 32 || c > 126) c = ' ';  // 非 ASCII 可见字符替换为空格
    lcd.print(c);
  }
}

void lcdWriteMMSS(uint8_t row, uint8_t col, uint32_t remain_ms) {
  uint32_t sec = remain_ms / 1000;
  uint8_t mm = sec / 60, ss = sec % 60;
  lcd.setCursor(col, row);
  char buf[6]; // "mm:ss"
  sprintf(buf, "%02u:%02u", mm, ss);
  lcd.print(buf);
}

/* ============= 蜂鸣器 ============= */
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
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW); // 关闭
#else
  pinMode(PIN_BUZZ, INPUT);    // 开漏：高阻=关闭
#endif
}
void beepOn() {
#if (BUZZ_TYPE == 0)
  ledcWriteTone(BUZZ_LEDC_CH, BUZZ_TONE_HZ);
#elif (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, HIGH);
#else
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW); // 低电平触发
#endif
}
void beepOff() {
#if (BUZZ_TYPE == 0)
  ledcWrite(BUZZ_LEDC_CH, 0);
#elif (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT);    // 释放为输入高阻
#endif
}

/* ============= 舵机 ============= */
void servoEnsureAttached() { if (!servoAttached) { servo.attach(PIN_SERVO); servoAttached = true; } }
void servoDetachSafe()     { if (servoAttached)  { servo.detach(); servoAttached = false; } }
void servoHome()           { if (servoAttached)  { servo.write(SERVO_MIN_DEG); } }

/* ============= LCD ============= */
void setupLCD() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // 启动页
  lcdWrite16(0, "Pomodoro Ready ");
  lcdWrite16(1, "Press button > ");
}

/* ============= HX711 ============= */
float readSmooth() {
  if (scale.is_ready()) {
    long raw = scale.read();
    emaVal = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * emaVal;
  }
  return emaVal;
}

bool phoneIsPresent(float value) {
  // 阈值 + 容差
  return value > (THRESH - DETECT_MARGIN);
}

/* ============= 按钮 ============= */
void handleButton() {
  bool reading = digitalRead(PIN_BTN);
  uint32_t now = millis();
  if (reading != btnLast && (now - btnLastMs) > BTN_DEBOUNCE_MS) {
    btnLast = reading; btnLastMs = now;
    if (reading == LOW) {
      running = !running;
      stateStartMs = millis();
      if (running) {
        state = WAIT_PHONE;
        lcdWrite16(0, "Place your phone");
        lcdWrite16(1, "to start focus ");
      } else {
        state = IDLE;
        beepOff(); servoDetachSafe();
        lcdWrite16(0, "Pomodoro Ready ");
        lcdWrite16(1, "Press button > ");
      }
    }
  }
}

/* ============= 状态切换 ============= */
void enterState(State newState) {
  state = newState;
  stateStartMs = millis();
  lastUiMs = 0; // 让 UI 立即刷新
  lcd.clear();

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
      // 行2: "Time left mm:ss"
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

/* ============= SETUP ============= */
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);

  buzzInit();
  setupLCD();

  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  emaVal = NO_PHONE_VAL;           // 启动平滑初值
  THRESH = (NO_PHONE_VAL + WITH_PHONE_VAL) / 2.0f;

  state = IDLE;
}

/* ============= LOOP ============= */
void loop() {
  handleButton();
  if (!running) return;

  float val = readSmooth();
  bool  present = phoneIsPresent(val);
  uint32_t now = millis();

  switch (state) {
    case WAIT_PHONE: {
      if (present) enterState(FOCUS);
      // UI 提示常驻即可
    } break;

    case FOCUS: {
      // 倒计时
      uint32_t elapsed = now - stateStartMs;
      if (present == false) { enterState(ALERT); break; }
      if (elapsed >= FOCUS_MS) { enterState(BREAK_TIME); break; }

      // UI 限频刷新
      if (now - lastUiMs >= 250) {
        lastUiMs = now;
        lcdWrite16(0, "Focus mode     ");
        // 行2第10列开始写 mm:ss，前缀 "Time left "
        lcdWrite16(1, "Time left 00:00");
        uint32_t remain = FOCUS_MS - elapsed;
        lcdWriteMMSS(1, 10, remain); // 行1(第二行)列10开始写 mm:ss
      }
    } break;

    case ALERT: {
      // 舵机非阻塞扫动
      if (now - lastServoStepMs >= SERVO_STEP_INTERVAL_MS) {
        lastServoStepMs = now;
        alertPos += alertDir;
        if (alertPos >= SERVO_MAX_DEG) { alertPos = SERVO_MAX_DEG; alertDir = -SERVO_STEP_DEG; }
        if (alertPos <= SERVO_MIN_DEG) { alertPos = SERVO_MIN_DEG; alertDir = +SERVO_STEP_DEG; }
        servo.write(alertPos);
      }
      // 放回手机 → 恢复 FOCUS（倒计时继续）
      if (present) enterState(FOCUS);
      // LCD 两行提示已在 enterState( ALERT ) 时写好
    } break;

    case BREAK_TIME: {
      uint32_t elapsed = now - stateStartMs;
      if (elapsed >= BREAK_MS) { enterState(WAIT_PHONE); break; }

      if (now - lastUiMs >= 250) {
        lastUiMs = now;
        lcdWrite16(0, "Break time     ");
        lcdWrite16(1, "Relax   00:00  ");
        uint32_t remain = BREAK_MS - elapsed;
        lcdWriteMMSS(1, 8, remain); // 行2第9列开始写
      }
      // 休息模式不检测手机，直到时间到
    } break;

    case IDLE:
    default: break;
  }
}
