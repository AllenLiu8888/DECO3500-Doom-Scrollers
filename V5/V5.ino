#include <Arduino.h>
#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ==================== 参数设置 ====================
#define PIN_HX_DOUT 16
#define PIN_HX_SCK  4
#define PIN_SERVO   18
#define PIN_BUZZ    19
#define PIN_BTN     23
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define LCD_ADDR    0x27

#define BUZZ_TYPE 2          // 0=无源, 1=高电平有源, 2=低电平有源(蓝板)
#define EMA_ALPHA 0.2f
#define SERVO_MIN 0
#define SERVO_MAX 180
#define SERVO_STEP 8
#define SERVO_INTERVAL 5
#define LCD_COLS 16
#define LCD_ROWS 2

// 固定区间参数
float noPhoneWeight = 82000;     // 空板读数
float withPhoneWeight = 95000;   // 手机放上后读数
float margin = 500;              // 容差区间
float threshold = 0;             // 自动计算
bool phonePresent = false;

// 全局对象
HX711 scale;
Servo servo;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
bool lcdOK = false;

// 状态机
enum State { IDLE, FOCUS, ALERT };
State state = IDLE;

// 控制变量
float emaRaw = 0;
bool servoAttached = false;
int servoPos = SERVO_MIN;
int servoDir = +SERVO_STEP;
uint32_t lastServo = 0;

// ==================== 蜂鸣器函数 ====================
void buzzInit() {
#if (BUZZ_TYPE == 0)
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);
#elif (BUZZ_TYPE == 1)
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);
#else
  pinMode(PIN_BUZZ, INPUT); // 开漏初始高阻
#endif
}
void beepOn() {
#if (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, HIGH);
#elif (BUZZ_TYPE == 2)
  pinMode(PIN_BUZZ, OUTPUT);
  digitalWrite(PIN_BUZZ, LOW);
#endif
}
void beepOff() {
#if (BUZZ_TYPE == 1)
  digitalWrite(PIN_BUZZ, LOW);
#elif (BUZZ_TYPE == 2)
  pinMode(PIN_BUZZ, INPUT);
#endif
}

// ==================== 舵机 ====================
void servoEnsureAttached() { if (!servoAttached) { servo.attach(PIN_SERVO); servoAttached = true; } }
void servoDetach()         { if (servoAttached)  { servo.detach(); servoAttached = false; } }
void servoHome()           { if (servoAttached)  { servo.write(SERVO_MIN); } }

// ==================== LCD ====================
void setupLCD() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init(); lcd.backlight(); lcd.clear();
  lcdOK = true;
}

// ==================== HX711 ====================
float readSmooth() {
  if (scale.is_ready()) {
    long raw = scale.read();
    emaRaw = EMA_ALPHA * raw + (1 - EMA_ALPHA) * emaRaw;
  }
  return emaRaw;
}

// ==================== 状态切换 ====================
void enterState(State s) {
  state = s;
  if (lcdOK) lcd.clear();

  switch (s) {
    case IDLE:
      beepOff(); servoDetach();
      lcd.print("Pomodoro Ready");
      break;
    case FOCUS:
      beepOff(); servoDetach();
      lcd.print("Focus Mode");
      break;
    case ALERT:
      beepOn(); servoEnsureAttached();
      servoPos = SERVO_MIN; servoDir = +SERVO_STEP;
      lcd.print("Phone Removed!");
      break;
  }
}

// ==================== 主流程 ====================
void setup() {
  Serial.begin(115200);
  buzzInit();
  setupLCD();
  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  pinMode(PIN_BTN, INPUT_PULLUP);
  threshold = (noPhoneWeight + withPhoneWeight) / 2.0;
  emaRaw = noPhoneWeight;

  lcd.setCursor(0,1);
  lcd.print("Threshold:");
  lcd.print((int)threshold);
  delay(1500);
  enterState(IDLE);
}

void loop() {
  static bool running = true;
  float val = readSmooth();
  float diff = val - threshold;

  // 检测手机在否
  bool current = (val > (threshold - margin));
  if (current != phonePresent) {
    phonePresent = current;
    if (!phonePresent) enterState(ALERT);
    else enterState(FOCUS);
  }

  // 状态动作
  if (state == ALERT) {
    uint32_t now = millis();
    if (now - lastServo >= SERVO_INTERVAL) {
      lastServo = now;
      servoPos += servoDir;
      if (servoPos >= SERVO_MAX) { servoPos = SERVO_MAX; servoDir = -SERVO_STEP; }
      if (servoPos <= SERVO_MIN) { servoPos = SERVO_MIN; servoDir = +SERVO_STEP; }
      servo.write(servoPos);
    }
  }

  // LCD 显示
  static uint32_t lastUI = 0;
  if (millis() - lastUI > 200) {
    lastUI = millis();
    lcd.setCursor(0,1);
    lcd.print("Val:"); lcd.print((int)val);
    lcd.print("  ");
    lcd.print(phonePresent ? "ON " : "OFF");
  }

  delay(10);
}
 