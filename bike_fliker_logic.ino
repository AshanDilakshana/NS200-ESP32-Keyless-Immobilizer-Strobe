/*
 * ESP32 NS200 BS4 Smart Watch/Phone Keyless Ignition, Immobilizer & Strobe
 * ========================================================================
 * Target Motorcycle: Bajaj Pulsar NS 200 BS4
 * Target Board     : ESP32 Dev Module
 * 
 * RESOLVED LOGIC & SAFETY INTERLOCKS:
 * 1. Normal Key Mode Isolation:
 *    - When riding with Physical Key: Wi-Fi stays OFF, 10s Side-Stand Shutoff is DISABLED.
 * 2. Keyless Mode (Hidden Switch Trigger):
 *    - Enabled ONLY when Pin 27 detects current from Hidden Switch.
 *    - Turns Wi-Fi ON -> Connects Watch/Phone -> Latches Ignition Relay ON (Pin 14).
 *    - 10s Side Stand Auto-OFF applies ONLY during this Keyless Session!
 * 3. Samsung Watch Priority on Side Stand:
 *    - Stand DOWN >= 10s -> Checks ONLY Samsung Watch (0E:6B:D8:4C:2E:F2).
 *    - Watch connected -> Stays ON. Watch disconnects/turns OFF Wi-Fi -> Shuts down instantly!
 * 4. Pothole / Speed-Bump Protection:
 *    - 700ms debounce before Engine Cut-Off triggers in gear.
 * 5. Pass Switch Strobe & Immobilizer:
 *    - 3 Clicks : Unlock Immobilizer
 *    - 5 Clicks : Toggle 15 Hz Flicker / Steady Mode
 *    - 8 Clicks : Lock Immobilizer
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_wifi.h"

Preferences preferences;

// ==========================================
// 1. REGISTERED DEVICE MAC ADDRESSES
// ==========================================
const char* KEYLESS_WIFI_SSID = "NS200_SMART_KEY";
const char* KEYLESS_WIFI_PASS = "AshanNS200#Key"; // Minimum 8 characters
const bool  HIDE_WIFI_SSID    = false;

const uint8_t WATCH_MAC[6]   = {0x0E, 0x6B, 0xD8, 0x4C, 0x2E, 0xF2}; // Samsung Watch
const uint8_t PHONE_1_MAC[6] = {0x82, 0xCA, 0xBF, 0xDC, 0x14, 0xF1}; // Android Phone
const uint8_t PHONE_2_MAC[6] = {0x60, 0x57, 0xC8, 0x03, 0xBB, 0x3A}; // iPhone Real Hardware MAC

// ==========================================
// 2. PIN DEFINITIONS
// ==========================================
const int PIN_IGNITION_RELAY = 14; // Keyless Ignition Main Relay
const int PIN_ENGINE_CUTOFF  = 13; // Kill Switch / CDI Ground Relay
const int PIN_PASS_RELAY     = 12; // Pass Light Relay / MOSFET
const int PIN_STATUS_LED     = 2;  // Built-in LED

const int PIN_KEYLESS_TRIG   = 27; // Hidden Switch Trigger Sense (Active LOW via GND)
const int PIN_NEUTRAL_SW     = 18; // Neutral Gear Switch (Active LOW)
const int PIN_SIDESTAND_SW   = 19; // Side Stand Switch (Active LOW)
const int PIN_PASS_SW        = 4;  // Pass Switch (Active LOW)

// ==========================================
// 3. POLARITY & TIMING CONFIGURATION
// ==========================================
const int ACTIVE_KEYLESS_TRIG = LOW;
const int ACTIVE_NEUTRAL      = LOW;
const int ACTIVE_SIDESTAND    = LOW;
const int ACTIVE_PASS_SW      = LOW;

const int IGNITION_ON  = HIGH;
const int IGNITION_OFF = LOW;

const int CUTOFF_ACTIVE   = HIGH;
const int CUTOFF_INACTIVE = LOW;

const int PASS_LIGHT_ON  = HIGH; 
const int PASS_LIGHT_OFF = LOW;

const unsigned long DEBOUNCE_DELAY_MS          = 35;
const unsigned long SIDESTAND_CUTOFF_DELAY_MS  = 700;   // 700ms Pothole bounce grace
const unsigned long WIFI_SEARCH_TIMEOUT_MS     = 30000; // 30s connection window on boot
const unsigned long KEYLESS_STAND_OFF_DELAY_MS = 10000; // 10s on Side Stand
const unsigned long KEYLESS_BOOT_GRACE_MS      = 20000; // 20s initial boot grace if on stand

const unsigned long PASS_WINDOW_MS             = 4000;  // 4s multi-click window
const unsigned long MULTI_CLICK_SETTLE_MS      = 450;   // 450ms settle pause
const unsigned long IMMOBILIZER_LOCK_HOLD_MS   = 10000; // 10s Long Press on Pass Switch to LOCK Immobilizer

const int FLICKER_FREQ_HZ = 15;
const unsigned long FLICKER_INTERVAL_MS = 1000 / (FLICKER_FREQ_HZ * 2);

// ==========================================
// 4. OPERATING MODES & SYSTEM STATE
// ==========================================
enum PassOperatingMode {
  MODE_NORMAL = 0,
  MODE_FLICKER = 1
};

PassOperatingMode currentMode = MODE_NORMAL;
bool isImmobilized = false;

// Keyless Mode Session Control
bool isKeylessSessionActive       = false; // True ONLY if started via Hidden Switch
bool isKeylessWifiRunning         = false;
unsigned long wifiSearchStartTime = 0;
int lastConnectedStationCount     = 0;

bool keylessIgnitionLatched       = false;
bool keylessAutoOffTriggered      = false;
bool standLiftedSinceIgnition     = false;
unsigned long ignitionTurnedOnTime = 0;
bool standShutoffTimerActive      = false;
unsigned long standShutoffStartTime = 0;
bool stand10sHoldNoticePrinted    = false;

// Debounce & Multi-click tracking
int keylessTrigCurrentReading;
int keylessTrigDebouncedState  = !ACTIVE_KEYLESS_TRIG;
int lastKeylessTrigReading     = !ACTIVE_KEYLESS_TRIG;
unsigned long lastKeylessTrigDebounceTime = 0;

int passCurrentReading;
int passDebouncedState  = !ACTIVE_PASS_SW;
int lastPassReading     = !ACTIVE_PASS_SW;
unsigned long lastPassDebounceTime = 0;

int passPressCount = 0;
unsigned long passWindowStartTime = 0;
unsigned long lastPassPressTime   = 0;
unsigned long passHoldStartTime   = 0;
bool immobilizerLockTriggeredThisPress = false;

unsigned long lastFlickerToggleTime = 0;
bool flickerOutputState = false;

int neutralCurrentReading;
int neutralDebouncedState  = !ACTIVE_NEUTRAL;
int lastNeutralReading     = !ACTIVE_NEUTRAL;
unsigned long lastNeutralDebounceTime = 0;

int sideStandCurrentReading;
int sideStandDebouncedState  = !ACTIVE_SIDESTAND;
int lastSideStandReading     = !ACTIVE_SIDESTAND;
unsigned long lastSideStandDebounceTime = 0;

bool sideStandHazardTimerActive = false;
unsigned long sideStandHazardStartTime = 0;

// ==========================================
// 5. HARDWARE ABSTRACTION HELPERS
// ==========================================
bool isKeylessTriggerActive() { return (keylessTrigDebouncedState == ACTIVE_KEYLESS_TRIG); }
bool isNeutralEngaged()       { return (neutralDebouncedState == ACTIVE_NEUTRAL); }
bool isSideStandDown()        { return (sideStandDebouncedState == ACTIVE_SIDESTAND); }
bool isPassSwitchPressed()    { return (passDebouncedState == ACTIVE_PASS_SW); }

void printMacAddress(const uint8_t* mac) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print(buf);
}

// ==========================================
// 6. WI-FI PRESENCE & AUTHENTICATION
// ==========================================
void startKeylessWifi(unsigned long currentMillis) {
  if (!isKeylessWifiRunning) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(KEYLESS_WIFI_SSID, KEYLESS_WIFI_PASS, 1, HIDE_WIFI_SSID ? 1 : 0, 4);
    isKeylessWifiRunning      = true;
    wifiSearchStartTime       = currentMillis;
    lastConnectedStationCount = 0;
    Serial.println("\n[WIFI AP STARTED] Searching for Samsung Watch / Phones...");
  }
}

void stopKeylessWifi() {
  if (isKeylessWifiRunning) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    isKeylessWifiRunning      = false;
    lastConnectedStationCount = 0;
    Serial.println("[WIFI AP STOPPED]");
  }
}

bool isAnyAuthorizedDeviceConnected() {
  if (!isKeylessWifiRunning) return false;

  wifi_sta_list_t stationList;
  if (esp_wifi_ap_get_sta_list(&stationList) != ESP_OK) return false;

  if (stationList.num != lastConnectedStationCount) {
    lastConnectedStationCount = stationList.num;
    for (int i = 0; i < stationList.num; i++) {
      uint8_t* mac = stationList.sta[i].mac;
      Serial.print(">>> DEVICE CONNECTED! MAC: ");
      printMacAddress(mac);
      if (memcmp(mac, WATCH_MAC, 6) == 0) {
        Serial.println("  <-- [SAMSUNG WATCH MATCHED]");
      } else if (memcmp(mac, PHONE_1_MAC, 6) == 0 || memcmp(mac, PHONE_2_MAC, 6) == 0) {
        Serial.println("  <-- [PHONE MATCHED]");
      } else {
        Serial.println("  <-- [UNREGISTERED DEVICE]");
      }
    }
  }

  // Check if any of the 3 registered MACs are connected
  for (int i = 0; i < stationList.num; i++) {
    uint8_t* mac = stationList.sta[i].mac;
    if (memcmp(mac, WATCH_MAC, 6) == 0 ||
        memcmp(mac, PHONE_1_MAC, 6) == 0 ||
        memcmp(mac, PHONE_2_MAC, 6) == 0) {
      return true;
    }
  }
  return false;
}

bool isSamsungWatchConnected() {
  if (!isKeylessWifiRunning) return false;

  wifi_sta_list_t stationList;
  if (esp_wifi_ap_get_sta_list(&stationList) == ESP_OK) {
    for (int i = 0; i < stationList.num; i++) {
      if (memcmp(stationList.sta[i].mac, WATCH_MAC, 6) == 0) {
        return true; // Specifically Samsung Watch
      }
    }
  }
  return false;
}

// ==========================================
// 7. KEYLESS IGNITION LOGIC (NO CONFLICTS)
// ==========================================
void handleKeylessIgnition(unsigned long currentMillis) {
  // 1. Session Initiation: Detect Current on Pin 27 (Hidden Switch)
  if (!isKeylessSessionActive) {
    if (isKeylessTriggerActive() && !keylessAutoOffTriggered) {
      isKeylessSessionActive = true;
      startKeylessWifi(currentMillis);
    } else {
      // Normal Key Mode -> Keep Wi-Fi and Keyless Relay OFF
      stopKeylessWifi();
      digitalWrite(PIN_IGNITION_RELAY, IGNITION_OFF);
      return;
    }
  }

  // 2. Waiting for Initial Device Connection (Watch / Phone)
  // Even if Immobilizer is LOCKED, authorized device CAN turn Ignition ON to power up ESP32!
  // (However, Engine Cut-Off stays ACTIVE until unlocked via 3 Pass clicks).
  if (!keylessIgnitionLatched) {
    if (isAnyAuthorizedDeviceConnected()) {
      keylessIgnitionLatched    = true;
      ignitionTurnedOnTime      = currentMillis;
      standLiftedSinceIgnition  = !isSideStandDown();
      standShutoffTimerActive   = false;
      stand10sHoldNoticePrinted = false;

      digitalWrite(PIN_IGNITION_RELAY, IGNITION_ON);
      Serial.println("\n>>> KEYLESS IGNITION: ON & LATCHED! <<<");
      if (isImmobilized) {
        Serial.println(">>> [SECURITY WARNING] IMMOBILIZER IS LOCKED! Press Pass Switch 3x to Start Engine! <<<");
      }
    } 
    else if ((currentMillis - wifiSearchStartTime) >= WIFI_SEARCH_TIMEOUT_MS) {
      Serial.println("[KEYLESS TIMEOUT] No authorized device detected in 30s. Shutting down.");
      stopKeylessWifi();
      isKeylessSessionActive  = false;
      keylessAutoOffTriggered = true;
      digitalWrite(PIN_IGNITION_RELAY, IGNITION_OFF);
    }
    return;
  }

  // 3. Keyless Mode is Running -> Monitor Side Stand Auto-OFF
  if (!isSideStandDown()) {
    standLiftedSinceIgnition  = true;
    standShutoffTimerActive   = false;
    stand10sHoldNoticePrinted = false;
  } else {
    // Side Stand is DOWN
    bool canRun10sTimer = standLiftedSinceIgnition ||
                          isSamsungWatchConnected() ||
                          ((currentMillis - ignitionTurnedOnTime) >= KEYLESS_BOOT_GRACE_MS);

    if (canRun10sTimer) {
      if (!standShutoffTimerActive) {
        standShutoffTimerActive   = true;
        standShutoffStartTime     = currentMillis;
        stand10sHoldNoticePrinted = false;
        Serial.println("[KEYLESS] Side Stand DOWN -> 10s Countdown Started...");
      } 
      else if ((currentMillis - standShutoffStartTime) >= KEYLESS_STAND_OFF_DELAY_MS) {
        // 10s Elapsed -> Evaluate ONLY Samsung Watch
        if (isSamsungWatchConnected()) {
          if (!stand10sHoldNoticePrinted) {
            stand10sHoldNoticePrinted = true;
            Serial.println("[KEYLESS] 10s passed. Samsung Watch is CONNECTED -> Ignition stays ON.");
          }
        } else {
          // Watch is disconnected or Wi-Fi turned off -> IMMEDIATE SHUTDOWN!
          keylessAutoOffTriggered = true;
          isKeylessSessionActive  = false;
          keylessIgnitionLatched  = false;
          digitalWrite(PIN_ENGINE_CUTOFF, CUTOFF_ACTIVE);
          digitalWrite(PIN_IGNITION_RELAY, IGNITION_OFF);
          stopKeylessWifi();

          Serial.println("\n>>> STAND DOWN >= 10s & WATCH DISCONNECTED -> KEYLESS POWER OFF! <<<");
          return;
        }
      }
    }
  }

  digitalWrite(PIN_IGNITION_RELAY, IGNITION_ON);
}

// ==========================================
// 8. ENGINE CUT-OFF SAFETY (POTHOLE FILTER)
// ==========================================
bool shouldCutOffEngine(unsigned long currentMillis) {
  // 1. Immobilizer or Keyless Auto-OFF Priority
  if (isImmobilized || (isKeylessSessionActive == false && keylessAutoOffTriggered)) {
    sideStandHazardTimerActive = false;
    return true; // Engine MUST remain killed
  }

  // 2. Normal Side Stand + Gear Safety with 700ms Pothole Debounce
  bool inNeutral = isNeutralEngaged();
  bool standDown = isSideStandDown();

  if (standDown && !inNeutral) {
    if (!sideStandHazardTimerActive) {
      sideStandHazardTimerActive = true;
      sideStandHazardStartTime = currentMillis;
    }
    // Only kill engine if stand is down continuously for >= 700ms in gear
    if ((currentMillis - sideStandHazardStartTime) >= SIDESTAND_CUTOFF_DELAY_MS) {
      return true;
    }
    return false; // In 700ms bounce grace period
  } else {
    sideStandHazardTimerActive = false;
    return false;
  }
}

// ==========================================
// 9. PASS SWITCH MULTI-CLICK & LONG PRESS
// ==========================================
void lockImmobilizer() {
  if (!isImmobilized) {
    isImmobilized = true;
    preferences.begin("bike_strobe", false);
    preferences.putBool("locked", true);
    preferences.end();
    Serial.println("\n=================================================");
    Serial.println(">>> ANTI-THEFT IMMOBILIZER LOCKED (10s HOLD)! <<<");
    Serial.println("=================================================");
    passPressCount = 0;
  }
}

void executePassCommand(int clicks) {
  if (isImmobilized) {
    if (clicks == 3) {
      isImmobilized = false;
      preferences.begin("bike_strobe", false);
      preferences.putBool("locked", false);
      preferences.end();
      Serial.println("\n>>> IMMOBILIZER UNLOCKED (3 CLICKS)! <<<");
      passPressCount = 0;
    }
    return;
  }

  if (clicks == 5) {
    currentMode = (currentMode == MODE_NORMAL) ? MODE_FLICKER : MODE_NORMAL;
    preferences.begin("bike_strobe", false);
    preferences.putInt("mode", (int)currentMode);
    preferences.end();
    Serial.print("\n>>> PASS MODE TOGGLED: ");
    Serial.println(currentMode == MODE_FLICKER ? "15 Hz FLICKER" : "NORMAL STEADY");
    passPressCount = 0; // Click counter එක ක්ෂණිකව reset කරන්න
  }
}

void handlePassSwitchPattern(unsigned long currentMillis) {
  passCurrentReading = digitalRead(PIN_PASS_SW);

  if (passCurrentReading != lastPassReading) {
    lastPassDebounceTime = currentMillis;
  }

  if ((currentMillis - lastPassDebounceTime) > DEBOUNCE_DELAY_MS) {
    if (passCurrentReading != passDebouncedState) {
      passDebouncedState = passCurrentReading;

      if (passDebouncedState == ACTIVE_PASS_SW) {
        // Button Pressed Down -> Start 10s Timer
        passHoldStartTime = currentMillis;
        immobilizerLockTriggeredThisPress = false;

        if (passPressCount == 0) {
          passWindowStartTime = currentMillis;
          passPressCount = 1;
        } else {
          passPressCount++;
        }
        lastPassPressTime = currentMillis;
      } else {
        // Button Released
        immobilizerLockTriggeredThisPress = false;
      }
    }
  }
  lastPassReading = passCurrentReading;

  // 10-Second Long Press Detection -> LOCK IMMOBILIZER
  if (isPassSwitchPressed() && !isImmobilized && !immobilizerLockTriggeredThisPress) {
    if ((currentMillis - passHoldStartTime) >= IMMOBILIZER_LOCK_HOLD_MS) {
      immobilizerLockTriggeredThisPress = true;
      lockImmobilizer();
    }
  }

  // Settle timeout for Multi-Clicks (3 clicks or 5 clicks)
  if (passPressCount > 0 && !isPassSwitchPressed()) {
    if ((currentMillis - lastPassPressTime) >= MULTI_CLICK_SETTLE_MS) {
      if ((currentMillis - passWindowStartTime) <= PASS_WINDOW_MS) {
        if ((isImmobilized && passPressCount == 3) || (!isImmobilized && passPressCount == 5)) {
          executePassCommand(passPressCount);
          passPressCount = 0;
          return;
        }
      }
    }
  }

  if (passPressCount > 0 && ((currentMillis - passWindowStartTime) > PASS_WINDOW_MS)) {
    passPressCount = 0;
  }
}

// ==========================================
// 10. SENSOR DEBOUNCE ROUTINE
// ==========================================
void debounceSensors(unsigned long currentMillis) {
  // Hidden Trigger Sense
  keylessTrigCurrentReading = digitalRead(PIN_KEYLESS_TRIG);
  if (keylessTrigCurrentReading != lastKeylessTrigReading) {
    lastKeylessTrigDebounceTime = currentMillis;
  }
  if ((currentMillis - lastKeylessTrigDebounceTime) > DEBOUNCE_DELAY_MS) {
    if (keylessTrigCurrentReading == ACTIVE_KEYLESS_TRIG &&
        keylessTrigDebouncedState != ACTIVE_KEYLESS_TRIG &&
        keylessAutoOffTriggered) {
      keylessAutoOffTriggered = false; // Reset lock when trigger is pressed again
    }
    keylessTrigDebouncedState = keylessTrigCurrentReading;
  }
  lastKeylessTrigReading = keylessTrigCurrentReading;

  // Neutral Switch
  neutralCurrentReading = digitalRead(PIN_NEUTRAL_SW);
  if (neutralCurrentReading != lastNeutralReading) {
    lastNeutralDebounceTime = currentMillis;
  }
  if ((currentMillis - lastNeutralDebounceTime) > DEBOUNCE_DELAY_MS) {
    neutralDebouncedState = neutralCurrentReading;
  }
  lastNeutralReading = neutralCurrentReading;

  // Side Stand Switch
  sideStandCurrentReading = digitalRead(PIN_SIDESTAND_SW);
  if (sideStandCurrentReading != lastSideStandReading) {
    lastSideStandDebounceTime = currentMillis;
  }
  if ((currentMillis - lastSideStandDebounceTime) > DEBOUNCE_DELAY_MS) {
    sideStandDebouncedState = sideStandCurrentReading;
  }
  lastSideStandReading = sideStandCurrentReading;
}

// ==========================================
// 11. SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_OFF);

  preferences.begin("bike_strobe", false);
  int savedMode = preferences.getInt("mode", (int)MODE_NORMAL);
  currentMode = (savedMode == (int)MODE_FLICKER) ? MODE_FLICKER : MODE_NORMAL;
  isImmobilized = preferences.getBool("locked", false);
  preferences.end();

  pinMode(PIN_IGNITION_RELAY, OUTPUT);
  pinMode(PIN_ENGINE_CUTOFF, OUTPUT);
  pinMode(PIN_PASS_RELAY, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);

  digitalWrite(PIN_IGNITION_RELAY, IGNITION_OFF);
  digitalWrite(PIN_ENGINE_CUTOFF, isImmobilized ? CUTOFF_ACTIVE : CUTOFF_INACTIVE);
  digitalWrite(PIN_PASS_RELAY, PASS_LIGHT_OFF);
  digitalWrite(PIN_STATUS_LED, LOW);

  pinMode(PIN_KEYLESS_TRIG, (ACTIVE_KEYLESS_TRIG == HIGH) ? INPUT_PULLDOWN : INPUT_PULLUP);
  pinMode(PIN_NEUTRAL_SW, INPUT_PULLUP);
  pinMode(PIN_SIDESTAND_SW, INPUT_PULLUP);
  pinMode(PIN_PASS_SW, INPUT_PULLUP);

  keylessTrigDebouncedState = digitalRead(PIN_KEYLESS_TRIG);
  lastKeylessTrigReading    = keylessTrigDebouncedState;
  neutralDebouncedState     = digitalRead(PIN_NEUTRAL_SW);
  lastNeutralReading        = neutralDebouncedState;
  sideStandDebouncedState   = digitalRead(PIN_SIDESTAND_SW);
  lastSideStandReading      = sideStandDebouncedState;

  Serial.println("\n=======================================================");
  Serial.println("ESP32 NS200 BS4 Smart Watch Keyless & Strobe (No-Conflict)");
  Serial.println("=======================================================");
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Debounce safety sensors & Hidden switch
  debounceSensors(currentMillis);

  // 2. Track Pass switch multi-click
  handlePassSwitchPattern(currentMillis);

  // 3. Handle Keyless Ignition Logic (Active only when Hidden Switch triggered)
  handleKeylessIgnition(currentMillis);

  // 4. Engine Cut-Off Safety Relay
  if (shouldCutOffEngine(currentMillis)) {
    digitalWrite(PIN_ENGINE_CUTOFF, CUTOFF_ACTIVE);
  } else {
    digitalWrite(PIN_ENGINE_CUTOFF, CUTOFF_INACTIVE);
  }

  // 5. Pass Light / Strobe Control
  // Operates whenever Pass Switch is currently held (unless Immobilizer is locked)
  if (!isImmobilized && isPassSwitchPressed()) {
    if (currentMode == MODE_FLICKER) {
      if (currentMillis - lastFlickerToggleTime >= FLICKER_INTERVAL_MS) {
        lastFlickerToggleTime = currentMillis;
        flickerOutputState = !flickerOutputState;
      }
      digitalWrite(PIN_PASS_RELAY, flickerOutputState ? PASS_LIGHT_ON : PASS_LIGHT_OFF);
      digitalWrite(PIN_STATUS_LED, flickerOutputState ? HIGH : LOW);
    } else {
      digitalWrite(PIN_PASS_RELAY, PASS_LIGHT_ON);
      digitalWrite(PIN_STATUS_LED, HIGH);
    }
  } else {
    digitalWrite(PIN_PASS_RELAY, PASS_LIGHT_OFF);
    digitalWrite(PIN_STATUS_LED, LOW);
    flickerOutputState = false;
  }
}
