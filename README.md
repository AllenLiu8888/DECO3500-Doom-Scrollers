# 🍅 Dual ESP32 Social Pomodoro System

A dual-user **focus and accountability system** built with **ESP32**, **HX711**, **LCD**, **servo motor**, and **buzzer**.  
When either person picks up their phone during focus mode, **both devices trigger alarms** — reminding both partners to stay focused.

> "If one loses focus, both get punished."

---

## 🔧 Overview

This system helps two users stay focused together.  
Once both press the start button, a 25-minute focus timer begins simultaneously on both ESP32 devices.  
If either person lifts their phone, both devices will react:

- 🔊 Buzzers on both devices sound  
- ⚙️ Servo motor moves on the device of the person who lifted the phone  
- 💡 LCDs display clear messages:
  - **“Phone moved! Put phone back!”**
  - **“Partner moved! Put phone back!”**

After the phone is replaced, both systems return to the waiting state and can restart focus mode.

---

## ⚙️ Hardware Components

| Module | Function |
|--------|-----------|
| **ESP32 DevKit V1** | Main controller (logic + ESP-NOW communication) |
| **HX711 + Load Cell** | Detects if the phone is placed on the platform |
| **LCD1602 (I2C)** | Displays status and countdown |
| **Servo Motor** | Provides visual feedback when triggered |
| **Buzzer** | Audio alert (supports low-level trigger modules) |
| **Push Button** | Starts or stops the system |

---

## 🔌 Pin Connection (Default)

| Component | ESP32 Pin | Power |
|------------|------------|--------|
| HX711 DOUT | GPIO 16 | 5V |
| HX711 SCK | GPIO 4 | 5V |
| Servo | GPIO 18 | 5V |
| Buzzer | GPIO 19 | 5V |
| Button | GPIO 23 | 3.3V (with INPUT_PULLUP) |
| LCD SDA | GPIO 21 | 3.3V |
| LCD SCL | GPIO 22 | 3.3V |
| **All grounds must be connected together.** | | ✅ |

---

## 📦 Required Libraries

Install these libraries via **Arduino IDE → Tools → Manage Libraries...**

| Library | Description |
|----------|-------------|
| `HX711` | For reading the load cell data |
| `ESP32Servo` | For controlling the servo motor |
| `LiquidCrystal_I2C` | For the 1602 I2C LCD display |
| `WiFi.h` *(built-in)* | Required by ESP-NOW |
| `esp_now.h` *(built-in)* | For peer-to-peer communication |

---

## 🖥️ Arduino IDE Setup

Board: ESP32 Dev Module
Upload Speed: 921600
CPU Frequency: 240 MHz
Flash Frequency: 80 MHz
Partition Scheme: Default 4MB with spiffs
Baud Rate: 115200


---

## 🔗 Pairing Instructions

1. Flash the same code to both ESP32 devices.  
2. Open Serial Monitor (115200) to view each device’s MAC address: 
   1. My A MAC: 24:6F:28:AB:CD:EF
   2. My B MAC: 24:6F:28:AB:CD:EF
3. Copy the MAC of **Device A** into **Device B**’s `PEER_MAC[]` array, and vice versa.  
4. Re-upload the firmware to both devices.  
5. Press the button on both units to start synchronized focus mode.  
6. When one lifts the phone → both buzzers sound and screens update instantly.

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
|--------|--------------|
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
5. If one picks up their phone → both devices react.  
6. Replace the phone to reset and start again.

---

## 🧠 Technology

- **ESP-NOW** peer-to-peer communication (no Wi-Fi needed)  
- Real-time dual-device synchronization  
- Smooth LCD updates (non-blocking)  
- EMA filtering for stable HX711 readings  
- Modular logic for focus / alert / break cycles  

---

## 🧩 Future Improvements

- Multi-user group focus  
- Bluetooth app control  
- Online data logging via Wi-Fi  
- Adjustable focus time  
- Power management & battery mode  

---

## 📘 Documentation

For detailed wiring diagrams, timing charts, and source code explanation,  
please visit the **[Project Wiki](./wiki)**.

---

## 🪪 License

MIT License © 2025  
Created by **Allen + ChatGPT Engineering**

> "When focus becomes social, distraction turns into cooperation."

