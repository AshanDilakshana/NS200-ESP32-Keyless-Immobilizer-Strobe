# ESP32 Motorcycle Keyless Ignition, Anti-Theft Immobilizer & Tactical Pass Strobe

An advanced, reliable motorcycle security and smart automation system built for the **Bajaj Pulsar NS 200 BS4** powered by an **ESP32 Dev Module**.

---

## 🌟 Key Features

### 1. 🔑 Wireless Smart Keyless Ignition
- **Hidden Switch Wakeup**: Momentary press on a hidden switch triggers the ESP32 via a PC817 optocoupler.
- **Wi-Fi SoftAP Presence Detection**: Turns on a dedicated secured Wi-Fi Access Point (`NS200_SMART_KEY`) searching for registered MAC addresses (Samsung Galaxy Watch, Android Phone, iPhone).
- **Instant Ignition Latch**: When an authorized device connects, the main ignition relay (GPIO 14) latches ON and stays ON.
- **Normal Physical Key Mode Isolation**: When riding using the physical key, Wi-Fi stays OFF to minimize RF emissions and prevent parasitic power draw.

### 2. ⌚ Smart Watch Keep-Alive & Auto-Shutoff
- **10-Second Stand Auto-Shutoff**: When parked and the side stand is put down, a 10-second timer begins.
- **Watch Priority**:
  - If the **Samsung Galaxy Watch** is connected and within range, the bike stays ON.
  - As soon as the rider turns off Wi-Fi on the watch or walks away, the ignition turns OFF automatically.
  - If started only with a smartphone, the ignition automatically shuts off after 10 seconds on the side stand.

### 3. 🛡️ Anti-Theft Immobilizer (Lock & Unlock)
- **10-Second Long Press Lock**: Press and hold the stock Pass switch for 10 seconds to lock the immobilizer. The engine cut-off relay permanently grounds the kill circuit / CDI.
- **Persistent Memory**: Lock state is safely retained across power cycles in ESP32 Non-Volatile Flash (`Preferences / NVS`).
- **3-Click Unlock**: Unlock the engine by tapping the Pass switch 3 times within 4 seconds after ignition is turned on.
- **Secure Meter Wakeup**: Even while immobilized, authorized keyless devices can wake up the speedometer and electrical systems, but engine cranking remains locked until the 3-click code is entered.

### 4. ⚡ 15 Hz Tactical Pass Strobe & Steady High Beam
- **Mode Toggle**: Tap the Pass switch 5 times quickly (< 4 seconds) to toggle between:
  - **Steady Pass Mode**: Standard high-beam pass light while held.
  - **15 Hz Tactical Flicker Strobe**: Extremely disorienting tactical strobe (~33ms ON / 33ms OFF) for maximum daytime visibility and emergency signaling.
- **Safety Interlock**: Operates strictly while the pass switch is pressed; turns OFF immediately when released.

### 5. 🏍️ Road Safety & Pothole Bounce Protection
- **700ms Bounce Filter**: Prevents accidental engine stall caused by the side-stand sensor bouncing on harsh potholes or speed-bumps while riding in gear.
- **Side Stand Engine Cut-Off**: Automatically cuts off the engine if the side stand is down while in gear.

---

## 📌 GPIO Pin Configuration

| GPIO Pin | Function | Direction | Polarity / Configuration |
| :--- | :--- | :--- | :--- |
| **GPIO 14** | Main Ignition Relay | Output | **Active HIGH** (HIGH = Ignition ON, LOW = OFF) |
| **GPIO 13** | Engine Cut-Off (Kill Switch / CDI Ground) | Output | **Active HIGH** (HIGH = Engine KILLED, LOW = Run) |
| **GPIO 12** | Pass Light Relay / MOSFET | Output | **Active HIGH** (HIGH = Pass Light ON) |
| **GPIO 2**  | On-board Built-in Blue LED | Output | System status indicator |
| **GPIO 27** | Hidden Trigger Switch Input | Input | **Active LOW** (`INPUT_PULLUP`, Triggered to GND via PC817) |
| **GPIO 18** | Neutral Gear Switch Input | Input | **Active LOW** (`INPUT_PULLUP`, LOW in Neutral) |
| **GPIO 19** | Side Stand Switch Input | Input | **Active LOW** (`INPUT_PULLUP`, LOW when Stand Down) |
| **GPIO 4**  | Pass Switch Input | Input | **Active LOW** (`INPUT_PULLUP`, LOW when Pressed) |

---

## 🔌 Hardware Schematics & Wiring

### PC817 Optocoupler Trigger Circuit (Active LOW GND Trigger)

Using the PC817 optocoupler to sink GPIO 27 to GND ensures 100% electrical isolation between the motorcycle 12V electrical system and the ESP32 3.3V logic:

```text
  12V Motorcycle Side                     ESP32 Side
  -------------------                     ----------
  +12V Hidden Switch ----[ 1kΩ Resistor ]---- Pin 1 (Anode)      Pin 4 (Collector) ---- ESP32 GPIO 27
                                                                                       (Internal Pull-up)
  12V Chassis Ground ------------------------ Pin 2 (Cathode)    Pin 3 (Emitter)   ---- ESP32 GND
```

- When the hidden button is pressed, the internal phototransistor conducts, pulling **GPIO 27** down to **0V (GND)**.
- When released, the ESP32 internal pullup holds the pin solidly at **3.3V**.

---

## 🕹️ Pass Switch Controls

| Command | Action |
| :--- | :--- |
| **3 Clicks** (< 4s) | **Unlock Anti-Theft Immobilizer** (Allows engine to start) |
| **5 Clicks** (< 4s) | **Toggle Pass Light Mode** (Steady Normal Mode ⟷ 15 Hz Tactical Flicker Strobe) |
| **10s Long Press** | **Lock Anti-Theft Immobilizer** (Cuts engine spark; saved to NVS Flash) |
| **10 Clicks** (< 4s) | **Auto-Pair**: Automatically saves currently connected Samsung Watch MAC address into NVS |

---

## 📱 Registered Devices

The system authenticates registered devices by comparing MAC addresses against authorized entries:

```cpp
const char* KEYLESS_WIFI_SSID = "NS200_SMART_KEY";
const char* KEYLESS_WIFI_PASS = "AshanNS200#Key"; // Minimum 8 characters

const uint8_t WATCH_MAC[6]   = {0x0E, 0x6B, 0xD8, 0x4C, 0x2E, 0xF2}; // Samsung Watch
const uint8_t PHONE_1_MAC[6] = {0x82, 0xCA, 0xBF, 0xDC, 0x14, 0xF1}; // Android Phone
const uint8_t PHONE_2_MAC[6] = {0x60, 0x57, 0xC8, 0x03, 0xBB, 0x3A}; // iPhone
```

---

## 🚀 Installation & Setup

1. **Prerequisites**:
   - [Arduino IDE](https://www.arduino.cc/en/software) with the **ESP32 Board Package** installed.
   - Board Selection: **ESP32 Dev Module** or equivalent ESP32 board.
   - Flash Frequency: 80MHz, Partition Scheme: Default 4MB with SPIFFS.

2. **Upload**:
   - Connect the ESP32 via micro-USB.
   - Open `bike_fliker_logic.ino`.
   - Select your COM/Serial port and press **Upload**.
   - Open Serial Monitor at **115200 baud** for live debugging logs.

---

## 📄 License

This project is licensed under the MIT License - feel free to use, modify, and distribute for personal or educational motorcycle automation projects.
