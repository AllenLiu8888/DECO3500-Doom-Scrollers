# 🍅 Dual ESP32 Social Pomodoro System

A dual-user focus and accountability system built with ESP32, HX711, LCD, a 20 kg servo motor, and an active‑high buzzer.
When either person picks up their phone during focus mode, both devices trigger alarms — reminding both partners to stay focused.

> "If one loses focus, both get punished."

---

## 🔧 Overview

This system helps two users stay focused together.
Once both press the start button, a 25‑minute focus timer starts on both ESP32 devices at the same time.
If either person lifts their phone, both devices will react:

- Buzzers on both devices sound
- The servo motor moves on the device of the person who lifted the phone
- LCDs display clear messages:
  - "Phone moved! Put phone back!"
  - "Partner moved! Put phone back!"

After the phone is replaced, both systems return to the waiting state and can restart focus mode.

---

## ⚙️ Hardware Components

| Module | Function |
|--------|----------|
| ESP32 DevKit V1 | Main controller (logic + ESP‑NOW communication) |
| HX711 + Load Cell | Detects whether the phone is on the platform |
| LCD1602 (I2C) | Displays status and countdown |
| Servo Motor (20 kg) | Motion feedback; signal from ESP32, external power recommended |
| Buzzer (active‑high, default) | Audio alert; high‑level triggered; external power recommended |
| 4×AA Battery Pack (~6 V) | Powers servo and buzzer; ESP32 powered separately |
| Push Button | Starts or stops the system |

---

## 🔌 Pin Connection (Default)

| Component | ESP32 Pin | Power |
|-----------|-----------|-------|
| HX711 DOUT | GPIO 16 | 5 V |
| HX711 SCK | GPIO 4 | 5 V |
| Servo | GPIO 18 | External (battery pack) |
| Buzzer | GPIO 19 | External (battery pack) |
| Button | GPIO 23 | 3.3 V (INPUT_PULLUP) |
| LCD SDA | GPIO 21 | 3.3 V |
| LCD SCL | GPIO 22 | 3.3 V |
| All grounds tied together | — | ✅ |

---

## 📦 Required Libraries

Install via Arduino IDE → Tools → Manage Libraries...

| Library | Description |
|---------|-------------|
| HX711 | Read load‑cell data |
| ESP32Servo | Control the servo motor |
| LiquidCrystal_I2C | 1602 I2C LCD display |
| WiFi.h (built‑in) | Required by ESP‑NOW |
| esp_now.h (built‑in) | Peer‑to‑peer communication |

---

## 🖥️ Arduino IDE Setup

Board: ESP32 Dev Module
Upload Speed: 921600
CPU Frequency: 240 MHz
Flash Frequency: 80 MHz
Partition Scheme: Default 4MB with spiffs
Baud Rate: 115200

---

## 🔋 External Power & Wiring (Strongly Recommended)

- Use a 4×AA battery pack (~6 V) to power the servo and buzzer only; power the ESP32 separately (USB or a 5 V module).
- Wiring notes:
  - Servo V+ and buzzer V+ connect to the battery pack positive; their GND connect to the battery pack negative.
  - Tie the battery GND to ESP32 GND (mandatory, common ground).
  - Servo signal remains on ESP32 GPIO 18; buzzer signal on GPIO 19 (as in code).
- Why: In testing, ESP32 alone could not reliably power the whole system; the device might fail to boot or keep rebooting.
- Tip: For high current loads, add a 220–470 µF electrolytic capacitor across the battery pack to reduce supply ripple.

---

## 🔗 First‑Time Calibration & Pairing (Required)

Follow these steps to read each device’s MAC address and calibrate HX711 thresholds.

1. Flash `TestingBeforeStart/TestingBeforeStart.ino` to both devices.
1. Open the Serial Monitor (115200) and follow the LCD prompts to take two samples:
   - Empty platform → record NO_PHONE_VAL
   - Phone on platform → record WITH_PHONE_VAL
   - The sketch prints a recommended DETECT_MARGIN
   - It also prints the device’s STA MAC address (for ESP‑NOW pairing)
1. Record each device’s values and MAC:
   - Device A: NO_PHONE_VAL_A, WITH_PHONE_VAL_A, DETECT_MARGIN_A, MAC_A
   - Device B: NO_PHONE_VAL_B, WITH_PHONE_VAL_B, DETECT_MARGIN_B, MAC_B
1. Edit the runtime firmware:
   - Open `Device_A/Device_A.ino` and update the top configuration:
     - Keep `#define IS_A_SIDE 1` (A sends START once both are READY)
     - Set `MY_NAME = "A"`
     - Set `PEER_MAC` to B’s MAC_B
     - Replace NO_PHONE_VAL/WITH_PHONE_VAL/DETECT_MARGIN with A’s measured values
   - Open `Device_B/Device_B.ino` and update the top configuration:
     - Do not define `IS_A_SIDE` (B waits for A’s START)
     - Set `MY_NAME = "B"`
     - Set `PEER_MAC` to A’s MAC_A
     - Replace NO_PHONE_VAL/WITH_PHONE_VAL/DETECT_MARGIN with B’s measured values
1. Upload A/B firmware to their respective devices.
1. Power on, press the buttons on both devices to enter READY:
   - A will automatically send START when both are READY, starting the synchronized timer.
   - If either phone is lifted, both devices buzz and show messages; restoring the phone resumes focus mode.

### Tips

- ESP‑NOW channel is fixed to 1 in code; both devices must match (already the default).

### One‑Screenshot Quick Start

![Calibration output example](./Static/TestingBeforeStart.png)

- Copy the highlighted "Device MAC (STA)" into the other device’s `PEER_MAC`.
- Copy the three lines under "=== Recommended Config ===" into this device’s code at `// HX711 thresholds — paste the three recommended values from TestingBeforeStart here`, replacing `NO_PHONE_VAL`, `WITH_PHONE_VAL`, and `DETECT_MARGIN`.

### A/B Differences

Both firmwares share the same logic; the only differences are in the top configuration:

- Role and name: A defines `IS_A_SIDE` and sets `MY_NAME="A"`; B does not define `IS_A_SIDE` and sets `MY_NAME="B"`
- Peer MAC: A uses `PEER_MAC = MAC_B`; B uses `PEER_MAC = MAC_A`
- HX711 thresholds: each device fills in its own NO_PHONE_VAL, WITH_PHONE_VAL, and DETECT_MARGIN

---

## 🧩 Operation Flow

[IDLE] → Press button → [WAIT_PHONE]
↓
Phone placed on load cell → [FOCUS 25:00]
↓
If phone removed → [ALERT on both devices]
↓
Phone replaced → [WAIT_PHONE]
↓
After cycle → [BREAK 5:00] → repeat

---

## 📄 Display Messages

| State | LCD Message |
|-------|-------------|
| IDLE | Pomodoro Ready / Press button > |
| WAIT_PHONE | Place your phone / to start focus |
| FOCUS | Focus mode / Time left 25:00 |
| ALERT (self) | Phone moved! / Put phone back! |
| ALERT (partner) | Partner moved! / Put phone back! |
| BREAK | Break time / Relax 05:00 |

---

## 🚀 How to Use

1. Power on both devices.
2. Press the button on each ESP32 to enter ready mode.
3. Place your phone on the load cell.
4. Focus together for 25 minutes.
5. If one picks up their phone, both devices react.
6. Put the phone back to resume and start again.

---

## 🧠 Technology

- ESP‑NOW peer‑to‑peer communication (no Wi‑Fi needed)
- Real‑time dual‑device synchronization
- Smooth, non‑blocking LCD updates
- EMA filtering for stable HX711 readings
- Modular logic for focus / alert / break cycles

---

## 🧩 Future Improvements

- Multi‑user group focus
- Bluetooth app control
- Online data logging via Wi‑Fi
- Adjustable focus time
- Power management & battery mode

---

## 🛠️ Troubleshooting

- Won’t boot or random reboots: power the servo and buzzer from the AA pack and ensure common ground.
- Buzzer silent: check wiring; module should be active‑high by default.
- Servo jitter/weak torque: ensure external power, proper wiring gauge, solid ground; add 220–470 µF capacitor.
- LCD blank: confirm I2C address 0x27 and pins SDA=21, SCL=22.
- ESP‑NOW pairing fails: ensure both use the same channel (default 1) and peer MACs are correct (A uses B’s MAC, B uses A’s MAC).
- False triggers/no trigger: re‑run `TestingBeforeStart` and replace the three recommended values in the HX711 section of each device’s code.

---

## 📘 Documentation

For wiring diagrams, timing charts, and detailed source notes,
please visit the Project Wiki (./wiki).

---

## 🪪 License

MIT License © 2025
Created by Allen + ChatGPT Engineering
> "When focus becomes social, distraction turns into cooperation."

