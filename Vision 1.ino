#include <HX711.h>
#include <ESP32Servo.h>

#define HX_DOUT   16
#define HX_SCK    27
#define SERVO_PIN 18

const long RISE_UP   = 3500;   // delta >= -> PHONE ON
const long RISE_DOWN = 1500;   // delta <= -> PHONE OFF
const uint16_t BASELINE_TIME_MS = 3000;
const float    EMA_ALPHA        = 0.2f;

HX711 scale;
Servo servo;
long  baseline = 0;
float emaRaw   = 0.0f;
bool  phone_on = false;

void servoSlap() {
  static bool dir = false;
  servo.write(dir ? 100 : 60);
  dir = !dir;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  scale.begin(HX_DOUT, HX_SCK);
  servo.attach(SERVO_PIN);
  servo.write(0);

  long sum = 0; int cnt = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < BASELINE_TIME_MS) {
    if (scale.is_ready()) { sum += scale.read(); cnt++; }
    delay(20);
  }
  baseline = (cnt > 0) ? (sum / cnt) : 0;
  emaRaw   = baseline;
}

void loop() {
  long raw = scale.is_ready() ? scale.read() : (long)emaRaw;
  emaRaw = EMA_ALPHA * raw + (1.0f - EMA_ALPHA) * emaRaw;
  long delta = (long)(emaRaw - baseline);

  // state machine: ON when delta high, OFF when delta low
  if (!phone_on && delta >= RISE_UP)  phone_on = true;    // placed
  else if (phone_on && delta <= RISE_DOWN) phone_on = false; // removed

  // action: ONLY spin when phone is removed
  if (!phone_on)  servoSlap();
  else            servo.write(0);

  Serial.print("RAW: ");   Serial.print((long)emaRaw);
  Serial.print("  DELTA: "); Serial.print(delta);
  Serial.print("  PHONE: "); Serial.println(phone_on ? 1 : 0);

  delay(80);
}
