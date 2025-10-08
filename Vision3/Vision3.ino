#include <HX711.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define PIN_HX_DOUT 16
#define PIN_HX_SCK  4
#define PIN_SERVO   18
#define PIN_BUZZ    19
#define PIN_BTN     23
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define LCD_ADDR    0x27

HX711 scale;
Servo servo;
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

enum SysState { IDLE, WAIT_PHONE, FOCUS, BREAK, ALERT };
SysState state = IDLE;

bool running = false;
bool phone_on = false;
uint32_t stateStart = 0;

// 固定阈值判断
const long PHONE_ON_THRESHOLD  = 11000;
const long PHONE_OFF_THRESHOLD = 10000;

uint32_t FOCUS_MS = 2UL * 60UL * 1000UL;
uint32_t BREAK_MS = 1UL  * 60UL * 1000UL;

void beepOn()  { digitalWrite(PIN_BUZZ, HIGH); }
void beepOff() { digitalWrite(PIN_BUZZ, LOW); }

void servoFullSweep() {
  servo.attach(PIN_SERVO);
  for (int pos = 0; pos <= 180; pos += 2) {
    servo.write(pos);
    delay(5);
  }
  for (int pos = 180; pos >= 0; pos -= 2) {
    servo.write(pos);
    delay(5);
  }
  servo.detach();
}

void lcdMsg(int r, int c, const String &s) {
  lcd.setCursor(c, r);
  lcd.print(s);
}

void enterState(SysState s) {
  state = s;
  stateStart = millis();
  lcd.clear();
  switch (s) {
    case IDLE:
      beepOff();
      lcdMsg(0, 0, "Pomodoro Ready");
      lcdMsg(1, 0, "Press button");
      break;
    case WAIT_PHONE:
      lcdMsg(0, 0, "Place phone...");
      break;
    case FOCUS:
      lcdMsg(0, 0, "Focus started");
      lcdMsg(1, 0, "Do not touch!");
      break;
    case BREAK:
      lcdMsg(0, 0, "Break time");
      lcdMsg(1, 0, "Relax :)");
      break;
    case ALERT:
      lcdMsg(0, 0, "Phone removed!");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZ, OUTPUT);
  beepOff();
  pinMode(PIN_BTN, INPUT_PULLUP);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(50000);
  lcd.init();
  lcd.backlight();

  scale.begin(PIN_HX_DOUT, PIN_HX_SCK);
  enterState(IDLE);
}

void loop() {
  bool btnPressed = (digitalRead(PIN_BTN) == LOW);
  static bool lastBtn = HIGH;

  if (btnPressed && lastBtn == HIGH) {
    delay(20);
    if (digitalRead(PIN_BTN) == LOW) {
      running = !running;
      if (running) enterState(WAIT_PHONE);
      else enterState(IDLE);
    }
  }
  lastBtn = btnPressed;

  if (!running) return;
  if (!scale.is_ready()) return;

  long raw = scale.read();
  Serial.println(raw);

  switch (state) {
    case WAIT_PHONE:
      if (raw > PHONE_ON_THRESHOLD) {
        phone_on = true;
        enterState(FOCUS);
      } else {
        lcdMsg(1, 0, "Waiting...");
      }
      break;

    case FOCUS:
      if (raw < PHONE_OFF_THRESHOLD) {
        enterState(ALERT);
      } else if (millis() - stateStart > FOCUS_MS) {
        enterState(BREAK);
      }
      break;

    case ALERT:
      beepOn();
      servoFullSweep();
      delay(3000);
      beepOff();
      enterState(WAIT_PHONE);
      break;

    case BREAK:
      if (millis() - stateStart > BREAK_MS) {
        lcdMsg(0, 0, "Break over");
        lcdMsg(1, 0, "Put phone back");
        enterState(WAIT_PHONE);
      }
      break;

    default:
      break;
  }

  delay(50);
}
