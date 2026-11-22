/*
  SmartFill 2.0 - ESP32 DevKit V1
  Hardware mapping follows the current firmware pin map in this file.
  The supplied connection diagram is a visual reference and some of its
  GPIO routes do not match this sketch; use docs/connection.html.

  GPIO map:
    HC-SR04 TRIG  5       HC-SR04 ECHO 18 (through voltage divider)
    OLED SDA     21       OLED SCL     22 (SSD1306 I2C, address 0x3C)
    Buzzer       23       (2N2222 transistor driver; GPIO HIGH = buzzer ON)
    Green LED    15       Blue LED     16       Red LED 17
    OK button    32       RESET button 33 (buttons to GND, INPUT_PULLUP)
    Relay IN     19       (RELAY_ACTIVE_LOW is false by default)

  Required libraries:
    - Adafruit GFX Library
    - Adafruit SSD1306
    - ArduinoJson 7.x

  Copy secrets.h.example to secrets.h beside this sketch and fill in
  the Wi-Fi and registered HydroLevel device credentials.

  Safety:
    - Pump is commanded OFF during boot.
    - Pump stops for invalid sensor, missing calibration, >=50%,
      or 15 minutes of continuous runtime.
    - AUTO thresholds are evaluated locally and work without Wi-Fi.
    - MANUAL ON expires if the device cannot contact the server.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include "secrets.h"

// -------------------- Pin definitions --------------------
static const uint8_t PIN_TRIG       = 5;
static const uint8_t PIN_ECHO       = 18;
static const uint8_t PIN_OLED_SDA   = 21;
static const uint8_t PIN_OLED_SCL   = 22;
static const uint8_t PIN_BUZZER     = 23;
static const uint8_t PIN_LED_GREEN  = 15;
static const uint8_t PIN_LED_BLUE   = 16;
static const uint8_t PIN_LED_RED    = 17;
static const uint8_t PIN_BUTTON_OK  = 32;
static const uint8_t PIN_BUTTON_RST = 33;
static const uint8_t PIN_RELAY      = 19;

// Most common 1-channel 5V relay boards are active LOW.
// If your relay energizes at boot or works in reverse, verify its input logic.
static const bool RELAY_ACTIVE_LOW = false;

// -------------------- Hardware / operating settings --------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

static const float MIN_DISTANCE_CM = 2.0f;
static const float MAX_DISTANCE_CM = 400.0f;
// Distance from the ultrasonic sensor face to the maximum safe water surface.
// Measure this physically. Keep it above the HC-SR04 blind zone.
static const float SENSOR_TO_RIM_OFFSET_CM = 6.0f;

// Automatic pump control thresholds (independent of LED/buzzer thresholds).
static const float PUMP_START_PERCENT = 10.0f;
static const float PUMP_STOP_PERCENT  = 50.0f;
static const unsigned long MAX_PUMP_RUNTIME_MS = 15UL * 60UL * 1000UL;
static const unsigned long MANUAL_COMMAND_TIMEOUT_MS = 30000UL;
static const unsigned long MEASURE_INTERVAL_MS = 1000UL;
static const unsigned long UPLOAD_INTERVAL_MS = 30000UL;
static const unsigned long COMMAND_POLL_INTERVAL_MS = 5000UL;
static const uint8_t SENSOR_SAMPLES = 5;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences preferences;
WiFiClientSecure tls;
HTTPClient http;

// -------------------- State --------------------
float emptyDistanceCm = 0.0f;
float lastDistanceCm = -1.0f;
float waterHeightCm = 0.0f;
float freeSpaceCm = 0.0f;
float waterLevelPercent = 0.0f;
bool calibrated = false;
bool sensorValid = false;
bool oledReady = false;

bool pumpOn = false;
String controlMode = "AUTO";  // AUTO or MANUAL
String commandPump = "OFF";   // ON or OFF
String commandUpdatedAt = "";
unsigned long lastSuccessfulCommandMs = 0;
unsigned long pumpStartedMs = 0;
unsigned long lastMeasureMs = 0;
unsigned long lastUploadMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastBuzzerToggleMs = 0;
bool buzzerState = false;
bool resetLongHandled = false;

// Button debounce state
bool lastOkReading = HIGH;
bool stableOkState = HIGH;
unsigned long okDebounceMs = 0;
bool lastResetReading = HIGH;
bool stableResetState = HIGH;
unsigned long resetDebounceMs = 0;
unsigned long resetPressedAtMs = 0;

String activeContainerId = "";
unsigned long intervalMs = UPLOAD_INTERVAL_MS;
int threshold25 = 25;
int threshold50 = 50;
int threshold75 = 75;
int threshold90 = 90;

// -------------------- Small helpers --------------------
void relayWrite(bool on) {
  const bool outputHigh = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_RELAY, outputHigh ? HIGH : LOW);
}

void pumpSet(bool on) {
  if (on == pumpOn) return;
  pumpOn = on;
  relayWrite(pumpOn);
  if (pumpOn) {
    pumpStartedMs = millis();
    Serial.println(F("[PUMP] ON"));
  } else {
    Serial.println(F("[PUMP] OFF"));
  }
}

unsigned long currentPumpRuntimeSeconds() {
  if (!pumpOn) return 0;
  return (millis() - pumpStartedMs) / 1000UL;
}

void setBuzzer(bool on) {
  buzzerState = on;
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
}

void allIndicatorsOff() {
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_BLUE, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  setBuzzer(false);
}

// -------------------- Sensor --------------------
float readDistanceOnce() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  const unsigned long duration = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (duration == 0) return -1.0f;
  return (duration * 0.0343f) / 2.0f;
}

float readDistanceCm() {
  float values[SENSOR_SAMPLES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < SENSOR_SAMPLES; i++) {
    const float d = readDistanceOnce();
    if (d >= MIN_DISTANCE_CM && d <= MAX_DISTANCE_CM) values[count++] = d;
    delay(35);
  }
  if (count == 0) return -1.0f;

  // Sort the valid values and return their median.
  for (uint8_t i = 1; i < count; i++) {
    const float key = values[i];
    int j = (int)i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  return values[count / 2];
}

void measureLevel() {
  lastDistanceCm = readDistanceCm();
  sensorValid = (lastDistanceCm >= MIN_DISTANCE_CM && lastDistanceCm <= MAX_DISTANCE_CM);

  if (!sensorValid) {
    waterHeightCm = 0;
    freeSpaceCm = 0;
    waterLevelPercent = 0;
    return;
  }

  freeSpaceCm = max(0.0f, lastDistanceCm - SENSOR_TO_RIM_OFFSET_CM);

  if (!calibrated || emptyDistanceCm <= SENSOR_TO_RIM_OFFSET_CM) {
    waterHeightCm = 0;
    waterLevelPercent = 0;
    return;
  }

  // Empty reference is the distance from sensor to the bottom/water surface
  // when the container is empty. The usable height ends at the rim offset.
  waterHeightCm = max(0.0f, emptyDistanceCm - lastDistanceCm);
  const float usableHeight = emptyDistanceCm - SENSOR_TO_RIM_OFFSET_CM;
  waterLevelPercent = (waterHeightCm / usableHeight) * 100.0f;
  waterLevelPercent = constrain(waterLevelPercent, 0.0f, 100.0f);

  // Reject readings that are clearly beyond the calibrated empty reference.
  if (lastDistanceCm > emptyDistanceCm + 5.0f) sensorValid = false;
}

// -------------------- Calibration / buttons --------------------
void showMessage(const String &line1, const String &line2 = "") {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 8);
  display.println(line1);
  display.setCursor(0, 25);
  display.println(line2);
  display.display();
}

bool waitForButtonPress(uint8_t pin, unsigned long timeoutMs) {
  const unsigned long started = millis();
  while (millis() - started < timeoutMs) {
    if (digitalRead(pin) == LOW) {
      delay(35);
      if (digitalRead(pin) == LOW) {
        while (digitalRead(pin) == LOW) delay(10);
        return true;
      }
    }
    delay(10);
  }
  return false;
}

void calibrateEmptyContainer() {
  pumpSet(false);
  showMessage("EMPTY CONTAINER", "Press OK to save");
  Serial.println(F("[CAL] Empty the container, position the sensor, then press OK again to save."));
  // Consume the initial press that opened calibration; wait for a separate press.
  while (digitalRead(PIN_BUTTON_OK) == LOW) delay(10);
  if (!waitForButtonPress(PIN_BUTTON_OK, 30000UL)) {
    showMessage("Calibration timed", "out - cancelled");
    delay(1000);
    return;
  }

  delay(500);
  const float measured = readDistanceCm();
  if (measured <= SENSOR_TO_RIM_OFFSET_CM + 2.0f || measured < 0) {
    showMessage("Calibration failed", "Check sensor / offset");
    Serial.println(F("[CAL] Invalid empty distance."));
    delay(1500);
    return;
  }

  emptyDistanceCm = measured;
  calibrated = true;
  preferences.putFloat("empty_cm", emptyDistanceCm);
  preferences.putBool("calibrated", true);
  measureLevel();
  showMessage("Calibration saved", String(emptyDistanceCm, 1) + " cm empty ref");
  Serial.printf("[CAL] Empty reference: %.1f cm\n", emptyDistanceCm);
  delay(1200);
}

void clearCalibration() {
  pumpSet(false);
  calibrated = false;
  emptyDistanceCm = 0;
  preferences.remove("empty_cm");
  preferences.putBool("calibrated", false);
  showMessage("Calibration reset", "Press OK to calibrate");
  Serial.println(F("[CAL] Saved calibration cleared."));
  delay(1200);
}

void updateButtons() {
  const unsigned long now = millis();
  const bool okReading = digitalRead(PIN_BUTTON_OK);
  if (okReading != lastOkReading) okDebounceMs = now;
  if ((now - okDebounceMs) > 35 && okReading != stableOkState) {
    stableOkState = okReading;
    if (stableOkState == LOW) calibrateEmptyContainer();
  }
  lastOkReading = okReading;

  const bool resetReading = digitalRead(PIN_BUTTON_RST);
  if (resetReading != lastResetReading) resetDebounceMs = now;
  if ((now - resetDebounceMs) > 35 && resetReading != stableResetState) {
    stableResetState = resetReading;
    if (stableResetState == LOW) {
      resetPressedAtMs = now;
      resetLongHandled = false;
    } else {
      if (!resetLongHandled) {
        // Short RESET press cancels a manual pump request locally.
        if (controlMode == "MANUAL") {
          commandPump = "OFF";
          pumpSet(false);
          showMessage("Manual pump", "Stopped by RESET");
          delay(500);
        }
      }
    }
  }
  lastResetReading = resetReading;

  if (stableResetState == LOW && !resetLongHandled &&
      now - resetPressedAtMs >= 2500UL) {
    resetLongHandled = true;
    clearCalibration();
  }
}

// -------------------- LEDs / buzzer --------------------
void updateIndicators() {
  digitalWrite(PIN_LED_GREEN, sensorValid && calibrated && waterLevelPercent >= threshold25 ? HIGH : LOW);
  digitalWrite(PIN_LED_BLUE,  sensorValid && calibrated && waterLevelPercent >= threshold50 ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,   sensorValid && calibrated && waterLevelPercent >= threshold75 ? HIGH : LOW);

  if (sensorValid && calibrated && waterLevelPercent >= threshold90) {
    if (millis() - lastBuzzerToggleMs >= 250UL) {
      lastBuzzerToggleMs = millis();
      setBuzzer(!buzzerState);
    }
  } else {
    setBuzzer(false);
  }
}

// -------------------- Pump safety / control --------------------
bool applyLocalSafety() {
  if (!calibrated || !sensorValid) {
    pumpSet(false);
    return false;
  }
  if (waterLevelPercent >= PUMP_STOP_PERCENT) {
    pumpSet(false);
    Serial.println(F("[SAFETY] Pump stopped at 50% water level."));
    return false;
  }
  if (pumpOn && millis() - pumpStartedMs >= MAX_PUMP_RUNTIME_MS) {
    pumpSet(false);
    Serial.println(F("[SAFETY] Maximum pump runtime reached."));
    return false;
  }
  return true;
}

void updatePumpControl() {
  if (!applyLocalSafety()) return;

  bool requestedOn = false;
  if (controlMode == "AUTO") {
    // Hysteresis: start at/below 10%; stop at 50%.
    requestedOn = pumpOn ? (waterLevelPercent < PUMP_STOP_PERCENT)
                         : (waterLevelPercent <= PUMP_START_PERCENT);
  } else if (controlMode == "MANUAL") {
    if (commandPump == "ON" &&
        millis() - lastSuccessfulCommandMs < MANUAL_COMMAND_TIMEOUT_MS) {
      requestedOn = true;
    } else {
      requestedOn = false;
      if (commandPump == "ON") Serial.println(F("[SAFETY] Manual command expired."));
    }
  }
  pumpSet(requestedOn);
}

// -------------------- OLED --------------------
void updateOLED() {
  if (!oledReady || millis() - lastDisplayMs < 300UL) return;
  lastDisplayMs = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("SMARTFILL 2.0"));
  display.setCursor(94, 0);
  display.print(WiFi.status() == WL_CONNECTED ? F("WiFi") : F("OFF"));
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 15);
  if (!calibrated) {
    display.println(F("Not calibrated"));
    display.setCursor(0, 29);
    display.println(F("Press OK to calibrate"));
  } else if (!sensorValid) {
    display.println(F("Sensor error"));
    display.setCursor(0, 29);
    display.println(F("Pump safety OFF"));
  } else {
    display.print(F("Level: "));
    display.print(waterLevelPercent, 1);
    display.println(F("%"));
    display.setCursor(0, 29);
    display.print(F("Height: "));
    display.print(waterHeightCm, 1);
    display.println(F(" cm"));
    display.setCursor(0, 41);
    display.print(F("Free: "));
    display.print(freeSpaceCm, 1);
    display.println(F(" cm"));
  }

  display.setCursor(0, 54);
  display.print(controlMode);
  display.print(F(" | Pump: "));
  display.print(pumpOn ? F("ON") : F("OFF"));
  display.display();
}

// -------------------- Wi-Fi / API --------------------
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connecting"));
  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000UL) {
    delay(300);
    Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? F(" connected") : F(" failed"));
  if (WiFi.status() == WL_CONNECTED) Serial.println(WiFi.localIP());
}

void applyServerConfig(JsonDocument &response) {
  if (!response["empty_distance_cm"].isNull()) {
    const float serverEmpty = response["empty_distance_cm"].as<float>();
    if (serverEmpty > SENSOR_TO_RIM_OFFSET_CM + 2.0f && serverEmpty <= MAX_DISTANCE_CM) {
      emptyDistanceCm = serverEmpty;
      calibrated = true;
      preferences.putFloat("empty_cm", emptyDistanceCm);
      preferences.putBool("calibrated", true);
    }
  }
  if (!response["interval_seconds"].isNull()) {
    const unsigned long seconds = response["interval_seconds"].as<unsigned long>();
    intervalMs = constrain(seconds, 5UL, 3600UL) * 1000UL;
  }
  if (!response["thresholds"]["led25"].isNull()) threshold25 = response["thresholds"]["led25"].as<int>();
  if (!response["thresholds"]["led50"].isNull()) threshold50 = response["thresholds"]["led50"].as<int>();
  if (!response["thresholds"]["led75"].isNull()) threshold75 = response["thresholds"]["led75"].as<int>();
  if (!response["thresholds"]["buzzer"].isNull()) threshold90 = response["thresholds"]["buzzer"].as<int>();
  if (!response["container_id"].isNull()) activeContainerId = response["container_id"].as<String>();
}

void applyCommand(const String &mode, const String &pump, const String &updatedAt) {
  const String newMode = mode;
  const String newPump = pump;
  if ((newMode == "AUTO" || newMode == "MANUAL") &&
      (newPump == "ON" || newPump == "OFF")) {
    controlMode = newMode;
    commandPump = newPump;
    commandUpdatedAt = updatedAt;
    lastSuccessfulCommandMs = millis();
    Serial.printf("[CMD] %s / %s\n", controlMode.c_str(), commandPump.c_str());
  }
}

bool uploadReading() {
  if (WiFi.status() != WL_CONNECTED) return false;

  JsonDocument doc;
  doc["device_code"] = DEVICE_CODE;
  if (activeContainerId.length()) doc["container_id"] = activeContainerId;
  doc["distance_cm"] = sensorValid ? lastDistanceCm : -1;
  doc["water_height_cm"] = waterHeightCm;
  doc["water_level_percent"] = waterLevelPercent;
  doc["free_space_cm"] = freeSpaceCm;
  doc["sensor_valid"] = sensorValid && calibrated;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["pump_status"] = pumpOn;
  doc["control_mode"] = controlMode;
  doc["pump_runtime_seconds"] = currentPumpRuntimeSeconds();

  String body;
  serializeJson(doc, body);
  const String url = String(SERVER_URL) + "/api/readings";
  http.begin(tls, url);
  http.setTimeout(12000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  const int status = http.POST(body);
  bool success = (status == 200);

  if (status == 200) {
    JsonDocument response;
    const DeserializationError err = deserializeJson(response, http.getStream());
    if (!err) {
      if (!response["error"].isNull()) success = false;
      applyServerConfig(response);
      if (!response["command"].isNull()) {
        applyCommand(response["command"]["mode"] | String("AUTO"),
                     response["command"]["pump"] | String("OFF"),
                     response["command"]["updated_at"] | String(""));
      }
    } else {
      success = false;
      Serial.printf("[JSON] Reading response parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[HTTP] POST /api/readings: %d\n", status);
  }
  http.end();
  return success;
}

void pollWebsiteCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  const String url = String(SERVER_URL) + "/api/device-command?device_code=" + DEVICE_CODE;
  http.begin(tls, url);
  http.setTimeout(10000);
  http.addHeader("X-API-Key", DEVICE_API_KEY);
  if (commandUpdatedAt.length()) http.addHeader("X-Command-Updated-At", commandUpdatedAt);

  const int status = http.GET();
  if (status == 200) {
    JsonDocument response;
    const DeserializationError err = deserializeJson(response, http.getStream());
    if (!err) {
      applyCommand(response["mode"] | String("AUTO"),
                   response["pump"] | String("OFF"),
                   response["updated_at"] | String(""));
    } else {
      Serial.printf("[JSON] Command response parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[HTTP] GET /api/device-command: %d\n", status);
  }
  http.end();
}

// -------------------- Setup / main loop --------------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BUTTON_OK, INPUT_PULLUP);
  pinMode(PIN_BUTTON_RST, INPUT_PULLUP);

  // Set OFF level before switching the relay GPIO to output mode.
  digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_RELAY, OUTPUT);
  pumpOn = false;
  allIndicatorsOff();

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledReady) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 15);
    display.println(F("SMARTFILL 2.0"));
    display.println(F("Starting..."));
    display.display();
  } else {
    Serial.println(F("[OLED] SSD1306 not found. Check SDA/SCL and 0x3C address."));
  }

  preferences.begin("smartfill", false);
  calibrated = preferences.getBool("calibrated", false);
  emptyDistanceCm = preferences.getFloat("empty_cm", 0.0f);
  if (emptyDistanceCm <= SENSOR_TO_RIM_OFFSET_CM + 2.0f || emptyDistanceCm > MAX_DISTANCE_CM) {
    calibrated = false;
    emptyDistanceCm = 0;
  }

  // HTTPS certificate validation is not configured in this starter firmware.
  // Use a proper root CA certificate before deploying outside a prototype.
  tls.setInsecure();

  measureLevel();
  ensureWiFi();
  showMessage("SmartFill ready", calibrated ? "Calibration loaded" : "Press OK to calibrate");
  Serial.println(F("[BOOT] Pump forced OFF. SmartFill 2.0 ready."));
}

void loop() {
  const unsigned long now = millis();

  updateButtons();

  if (now - lastMeasureMs >= MEASURE_INTERVAL_MS) {
    lastMeasureMs = now;
    measureLevel();
    updateIndicators();
  }

  // Local control and safety are evaluated even without an internet connection.
  updatePumpControl();
  updateOLED();

  if (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL_MS) {
    lastCommandPollMs = now;
    if (WiFi.status() != WL_CONNECTED) ensureWiFi();
    if (WiFi.status() == WL_CONNECTED) pollWebsiteCommand();
  }

  if (now - lastUploadMs >= intervalMs) {
    lastUploadMs = now;
    if (WiFi.status() != WL_CONNECTED) ensureWiFi();
    if (WiFi.status() == WL_CONNECTED) uploadReading();
  }

  // Apply a newly received website command immediately, subject to local safety.
  updatePumpControl();
  delay(20);
}
