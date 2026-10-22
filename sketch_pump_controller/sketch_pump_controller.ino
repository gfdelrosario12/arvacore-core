#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ============================================================
// ARVACORE - ESP32 CENTRAL PUMP & ACTUATION CONTROLLER
// Alternate Wetting and Drying (AWD) Monitoring Subsystem
//
// Hardware Assignments:
//   RELAY_PUMP_PIN      -> GPIO 26 (Main Irrigation Pump Relay)
//   RELAY_VALVE_PIN     -> GPIO 27 (Distribution Solenoid Valve Relay)
//   FLOW_SENSOR_PIN     -> GPIO 4  (YF-S201 Hall-Effect Flow Sensor Pulse In)
//   STATUS_LED_PIN      -> GPIO 2  (Built-in LED / Operational Heartbeat)
//
// Relays are ACTIVE LOW (LOW = ON / Energized, HIGH = OFF)
// ============================================================

// -------------------- CONTROLLER CONFIGURATION ---------------
const char* CONTROLLER_ID = "CTRL-FIELD-01";

// Backend API endpoint for command polling and actuation telemetry uplink
const char* BACKEND_BASE = "https://api-aquaflow.gladwindr.me/api/v1";
const char* GATEWAY_KEY = "ed630e681ef735f36954469b48f832f210286246ca95fff62e62086cab74e8a9";

// Wi-Fi Credentials
const char* WIFI_SSID = "ARVACORE_GATEWAY";
const char* WIFI_PASS = "arvacore2026";

// -------------------- PIN ASSIGNMENTS ------------------------
const uint8_t RELAY_PUMP_PIN  = 26;
const uint8_t RELAY_VALVE_PIN = 27;
const uint8_t FLOW_SENSOR_PIN = 4;
const uint8_t STATUS_LED_PIN  = 2;

// Active LOW relay definitions
const uint8_t RELAY_ON  = LOW;
const uint8_t RELAY_OFF = HIGH;

// -------------------- SAFETY & FLOW PARAMETERS ---------------
const unsigned long POLL_INTERVAL_MS = 3000UL;        // Poll backend commands every 3s
const unsigned long TELEMETRY_INTERVAL_MS = 5000UL;   // Telemetry uplink every 5s
const unsigned long MAX_PUMP_RUNTIME_MS = 45UL * 60UL * 1000UL; // 45m max single pulse safety lockout
const float MIN_VERIFIED_FLOW_LPM = 5.0f;             // Verification threshold for active flow

// Flow sensor calibration: YF-S201 produces ~7.5 pulses per second per 1 L/min
const float FLOW_CALIBRATION_FACTOR = 7.5f;

// -------------------- STATE VARIABLES -----------------------
volatile unsigned long flowPulseCount = 0;
unsigned long lastPulseCalcTime = 0;
float currentFlowRateLpm = 0.0f;
float totalLitersDelivered = 0.0f;

bool isPumpActive = false;
bool isValveOpen = false;
unsigned long pumpStartTime = 0;
unsigned long lastPollTime = 0;
unsigned long lastTelemetryTime = 0;
String activeCorrelationId = "";
String lastExecutedCorrelationId = "";

// ------------------------------------------------------------
// Flow Sensor ISR
// ------------------------------------------------------------
void IRAM_ATTR flowPulseCounter() {
  flowPulseCount++;
}

// ------------------------------------------------------------
// Calculate Flow Rate
// ------------------------------------------------------------
void updateFlowRate() {
  unsigned long now = millis();
  unsigned long elapsed = now - lastPulseCalcTime;
  if (elapsed >= 1000) {
    noInterrupts();
    unsigned long pulses = flowPulseCount;
    flowPulseCount = 0;
    interrupts();

    float elapsedSec = elapsed / 1000.0f;
    currentFlowRateLpm = (pulses / elapsedSec) / FLOW_CALIBRATION_FACTOR;
    totalLitersDelivered += (currentFlowRateLpm / 60.0f) * elapsedSec;
    lastPulseCalcTime = now;
  }
}

// Forward declaration
void sendCommandAck(const String &correlationId, const String &status, const String &action, const String &reason);

// ------------------------------------------------------------
// Relay Actuation Control
// ------------------------------------------------------------
void setPumpState(bool activate, const String &correlationId = "", const String &actionName = "IRRIGATION") {
  if (activate) {
    digitalWrite(RELAY_VALVE_PIN, RELAY_ON); // Open valve first
    delay(200);
    digitalWrite(RELAY_PUMP_PIN, RELAY_ON);  // Engage pump
    digitalWrite(STATUS_LED_PIN, HIGH);
    isPumpActive = true;
    isValveOpen = true;
    pumpStartTime = millis();
    activeCorrelationId = correlationId;
    Serial.printf("[ARVACORE] Signal received & executed: PUMP ON (Valve OPEN) | CorrId: %s\n", correlationId.c_str());
    if (correlationId.length() > 0) {
      sendCommandAck(correlationId, "EXECUTED", actionName, "Pump and valve engaged successfully");
    }
  } else {
    digitalWrite(RELAY_PUMP_PIN, RELAY_OFF); // Cut pump first
    delay(200);
    digitalWrite(RELAY_VALVE_PIN, RELAY_OFF);// Close valve
    digitalWrite(STATUS_LED_PIN, LOW);
    isPumpActive = false;
    isValveOpen = false;
    pumpStartTime = 0;
    Serial.printf("[ARVACORE] Signal executed: PUMP OFF (Valve CLOSED) | CorrId: %s\n", correlationId.c_str());
    if (correlationId.length() > 0) {
      sendCommandAck(correlationId, "EXECUTED", actionName, "Pump and valve disengaged successfully");
    }
    activeCorrelationId = "";
  }
}

// ------------------------------------------------------------
// Wi-Fi Connection
// ------------------------------------------------------------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 15) {
    delay(500);
    Serial.print(F("."));
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected! IP: %s | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println(F("\n[WIFI] Connection timeout. Retrying on next loop."));
  }
}

// ------------------------------------------------------------
// Send Command Acknowledgment to Backend
// ------------------------------------------------------------
void sendCommandAck(const String &correlationId, const String &status, const String &action, const String &reason) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(BACKEND_BASE) + "/irrigation/ack/" + correlationId;
  if (!http.begin(client, url)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ArvaCore-Gateway-Key", GATEWAY_KEY);

  String json = "{";
  json += "\"controllerId\":\"" + String(CONTROLLER_ID) + "\",";
  json += "\"status\":\"" + status + "\",";
  json += "\"action\":\"" + action + "\",";
  json += "\"reason\":\"" + reason + "\",";
  json += "\"currentFlowRateLpm\":" + String(currentFlowRateLpm, 2) + ",";
  json += "\"totalLiters\":" + String(totalLitersDelivered, 2) + ",";
  json += "\"pumpActive\":" + String(isPumpActive ? "true" : "false") + ",";
  json += "\"valveOpen\":" + String(isValveOpen ? "true" : "false");
  json += "}";

  int httpCode = http.POST(json);
  if (httpCode > 0) {
    Serial.printf("[ARVACORE-ACK] Sent status %s for CorrId %s | Code: %d\n", status.c_str(), correlationId.c_str(), httpCode);
  } else {
    Serial.printf("[ARVACORE-ACK] Failed sending ACK: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ------------------------------------------------------------
// Uplink Actuator Telemetry to Backend
// ------------------------------------------------------------
void uplinkActuatorTelemetry() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(BACKEND_BASE) + "/telemetry/uplink/" + String(CONTROLLER_ID);
  if (!http.begin(client, url)) return;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-ArvaCore-Gateway-Key", GATEWAY_KEY);

  String json = "{";
  json += "\"nodeId\":\"" + String(CONTROLLER_ID) + "\",";
  json += "\"pumpStatus\":\"" + String(isPumpActive ? "pumping" : "off") + "\",";
  json += "\"valveStatus\":\"" + String(isValveOpen ? "open" : "closed") + "\",";
  json += "\"flowRateLitersPerMin\":" + String(currentFlowRateLpm, 2) + ",";
  json += "\"totalLiters\":" + String(totalLitersDelivered, 2) + ",";
  json += "\"measurements\":{";
  json += "\"water_flow\":" + String(currentFlowRateLpm, 2);
  json += "}";
  json += "}";

  int httpCode = http.POST(json);
  if (httpCode > 0) {
    Serial.printf("[ACTUATOR-UPLINK] POST HTTP %d | Flow: %.1f L/min | Pump: %s\n",
                  httpCode, currentFlowRateLpm, isPumpActive ? "ON" : "OFF");
  } else {
    Serial.printf("[ACTUATOR-UPLINK] Failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Helper to extract JSON string property
String extractJsonField(const String &json, const String &fieldName) {
  int keyIndex = json.indexOf("\"" + fieldName + "\":");
  if (keyIndex < 0) return "";
  int valStart = keyIndex + fieldName.length() + 3;
  while (valStart < json.length() && (json[valStart] == ' ' || json[valStart] == '\"')) valStart++;
  int valEnd = valStart;
  while (valEnd < json.length() && json[valEnd] != '\"' && json[valEnd] != ',' && json[valEnd] != '}') valEnd++;
  return json.substring(valStart, valEnd);
}

// ------------------------------------------------------------
// Poll Backend for Irrigation Commands
// ------------------------------------------------------------
void pollIrrigationCommands() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(BACKEND_BASE) + "/irrigation/pending-command/" + String(CONTROLLER_ID);
  if (!http.begin(client, url)) return;

  http.addHeader("Accept", "application/json");
  http.addHeader("X-ArvaCore-Gateway-Key", GATEWAY_KEY);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    String correlationId = extractJsonField(payload, "correlationId");
    String command = extractJsonField(payload, "command");

    if (correlationId.length() > 0 && command.length() > 0) {
      if (correlationId == lastExecutedCorrelationId) {
        // Prevent duplicate execution
        http.end();
        return;
      }

      Serial.printf("[ARVACORE] New command received: %s | CorrId: %s\n", command.c_str(), correlationId.c_str());
      lastExecutedCorrelationId = correlationId;

      // Send initial RECEIVED acknowledgment
      sendCommandAck(correlationId, "RECEIVED", command, "Command received by controller");

      if (command.equalsIgnoreCase("STOP_IRRIGATION") || command.equalsIgnoreCase("SAFE_STOP") ||
          command.equalsIgnoreCase("EMERGENCY_STOP") || command.equalsIgnoreCase("MANUAL_STOP")) {
        setPumpState(false, correlationId, command);
      } else if (command.equalsIgnoreCase("START_IRRIGATION") || command.equalsIgnoreCase("MANUAL_START") ||
                 command.equalsIgnoreCase("REFLOOD")) {
        setPumpState(true, correlationId, command);
      } else if (command.equalsIgnoreCase("STATUS_REQUEST")) {
        sendCommandAck(correlationId, "EXECUTED", command, "Current state reported");
      } else {
        sendCommandAck(correlationId, "REJECTED", command, "Unknown command type");
      }
    }
  }
  http.end();
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configure Relays & LEDs
  pinMode(RELAY_PUMP_PIN, OUTPUT);
  pinMode(RELAY_VALVE_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  // Default to SAFE OFF state immediately upon power on
  digitalWrite(RELAY_PUMP_PIN, RELAY_OFF);
  digitalWrite(RELAY_VALVE_PIN, RELAY_OFF);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Configure Flow Sensor pulse interrupt
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseCounter, FALLING);

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" ARVACORE - ESP32 CENTRAL PUMP CONTROLLER"));
  Serial.println(F(" Relays + YF-S201 Flow Sensor Subsystem"));
  Serial.println(F("========================================"));
  Serial.printf("Controller ID: %s\n", CONTROLLER_ID);
  Serial.printf("Backend Host: %s\n", BACKEND_BASE);
  Serial.printf("Relay Pump GPIO: %d | Relay Valve GPIO: %d\n", RELAY_PUMP_PIN, RELAY_VALVE_PIN);
  Serial.printf("Flow Sensor GPIO: %d (Pulse Interrupt)\n", FLOW_SENSOR_PIN);
  Serial.println(F("Initial State: ALL RELAYS SAFE OFF"));
  Serial.println(F("========================================"));

  connectWiFi();
}

// ------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // 1. Calculate current flow rate every second
  updateFlowRate();

  // 2. Hardware Safety Interlock: Timeout guard for pump
  if (isPumpActive && (now - pumpStartTime >= MAX_PUMP_RUNTIME_MS)) {
    Serial.println(F("[SAFETY INTERLOCK] Max single pulse runtime exceeded. Emergency shutoff."));
    setPumpState(false, activeCorrelationId, "SAFETY_TIMEOUT");
  }

  // 3. Dry-Run Protection: If pump is ON for >15s but flow rate < 5 L/min
  if (isPumpActive && (now - pumpStartTime > 15000UL) && (currentFlowRateLpm < MIN_VERIFIED_FLOW_LPM)) {
    Serial.printf("[FLOW VERIFICATION FAULT] Pump ON but flow rate only %.1f L/min (< %.1f L/min). Halting pump.\n",
                  currentFlowRateLpm, MIN_VERIFIED_FLOW_LPM);
    setPumpState(false, activeCorrelationId, "DRY_RUN_PROTECTION");
  }

  // 4. Poll Backend for Commands
  if (now - lastPollTime >= POLL_INTERVAL_MS) {
    lastPollTime = now;
    pollIrrigationCommands();
  }

  // 5. Uplink Telemetry and Flow Status
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;
    uplinkActuatorTelemetry();
  }
}
