/*
  IoT-Based Glucose Drip Monitoring System
  Alert & Notification Module
  ------------------------------------------
  Board: ESP32 (WiFi-enabled)

  Function:
   - Reads drip flow rate from an IR drop-counter sensor
   - Reads drip chamber / bag level from an ultrasonic sensor
   - Detects three fault conditions:
       1. Flow rate anomaly (too fast / too slow / stopped)
       2. Backflow risk (sudden negative pressure / no drops + low level)
       3. Overflow risk (bag/chamber level above safe threshold)
   - Triggers local alarm (buzzer + LED) instantly
   - Sends a push notification to medical staff via a messaging
     webhook (Telegram Bot API used here as a free, reliable example;
     swap the sendTelegramAlert() body for any other notification
     service - email, SMS gateway, hospital app API, etc.)

  Wiring (adjust pins to your board):
   - IR drop sensor OUT      -> GPIO 4   (digital, interrupt)
   - Ultrasonic TRIG         -> GPIO 5
   - Ultrasonic ECHO         -> GPIO 18
   - Buzzer                  -> GPIO 19
   - Status LED (Green)      -> GPIO 21
   - Alarm LED (Red)         -> GPIO 22
*/

#include <WiFi.h>
#include <HTTPClient.h>

// ---------- USER CONFIG ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Telegram bot credentials (create a bot via @BotFather, get chat ID via @myidbot)
const char* TELEGRAM_BOT_TOKEN = "YOUR_BOT_TOKEN";
const char* TELEGRAM_CHAT_ID   = "YOUR_CHAT_ID";

// ---------- PIN CONFIG ----------
const int DROP_SENSOR_PIN = 4;
const int TRIG_PIN        = 5;
const int ECHO_PIN        = 18;
const int BUZZER_PIN      = 19;
const int LED_OK_PIN      = 21;
const int LED_ALARM_PIN   = 22;

// ---------- THRESHOLDS (tune for your hardware/IV set) ----------
const float NORMAL_MIN_DROPS_PER_MIN = 10.0;   // below this = too slow
const float NORMAL_MAX_DROPS_PER_MIN = 60.0;   // above this = too fast
const unsigned long NO_DROP_TIMEOUT_MS = 15000; // no drop in 15s = stopped/blocked
const float OVERFLOW_LEVEL_CM   = 3.0;  // distance from sensor to liquid; SMALLER distance = HIGHER level => overflow
const float EMPTY_BAG_LEVEL_CM  = 20.0; // larger distance => bag near empty (backflow risk)

// ---------- STATE ----------
volatile unsigned long dropCount = 0;
volatile unsigned long lastDropTimeMs = 0;
unsigned long windowStartMs = 0;
float dropsPerMinute = 0;

enum AlertState { NORMAL, FLOW_TOO_SLOW, FLOW_TOO_FAST, FLOW_STOPPED, OVERFLOW_RISK, BACKFLOW_RISK };
AlertState currentState = NORMAL;
AlertState lastNotifiedState = NORMAL;
unsigned long lastNotifyMs = 0;
const unsigned long NOTIFY_COOLDOWN_MS = 60000; // don't spam staff more than once/min per condition

// ---------- ISR ----------
void IRAM_ATTR onDropDetected() {
  dropCount++;
  lastDropTimeMs = millis();
}

void setup() {
  Serial.begin(115200);

  pinMode(DROP_SENSOR_PIN, INPUT_PULLUP);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_OK_PIN, OUTPUT);
  pinMode(LED_ALARM_PIN, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(DROP_SENSOR_PIN), onDropDetected, FALLING);

  connectWiFi();

  windowStartMs = millis();
  lastDropTimeMs = millis();
  digitalWrite(LED_OK_PIN, HIGH);
}

void loop() {
  unsigned long now = millis();

  // Compute drop rate every 5 seconds for responsiveness
  if (now - windowStartMs >= 5000) {
    float minutesElapsed = (now - windowStartMs) / 60000.0;
    dropsPerMinute = dropCount / minutesElapsed;
    dropCount = 0;
    windowStartMs = now;
  }

  float liquidLevelCm = readUltrasonicDistanceCm();

  AlertState newState = evaluateState(now, liquidLevelCm);

  if (newState != currentState) {
    currentState = newState;
    Serial.print("State changed to: ");
    Serial.println(stateToText(currentState));
  }

  handleLocalAlarm(currentState);
  maybeNotifyStaff(currentState, now);

  delay(200); // small loop delay; drop counting handled by interrupt regardless
}

AlertState evaluateState(unsigned long now, float liquidLevelCm) {
  // Overflow check takes priority (immediate physical risk)
  if (liquidLevelCm <= OVERFLOW_LEVEL_CM) {
    return OVERFLOW_RISK;
  }

  // No drop for too long -> stopped flow, check backflow risk too
  if (now - lastDropTimeMs > NO_DROP_TIMEOUT_MS) {
    if (liquidLevelCm >= EMPTY_BAG_LEVEL_CM) {
      return BACKFLOW_RISK; // bag empty + no flow => risk of air/backflow into line
    }
    return FLOW_STOPPED;
  }

  if (dropsPerMinute > 0) {
    if (dropsPerMinute < NORMAL_MIN_DROPS_PER_MIN) return FLOW_TOO_SLOW;
    if (dropsPerMinute > NORMAL_MAX_DROPS_PER_MIN) return FLOW_TOO_FAST;
  }

  return NORMAL;
}

void handleLocalAlarm(AlertState state) {
  if (state == NORMAL) {
    digitalWrite(LED_OK_PIN, HIGH);
    digitalWrite(LED_ALARM_PIN, LOW);
    noTone(BUZZER_PIN);
  } else {
    digitalWrite(LED_OK_PIN, LOW);
    digitalWrite(LED_ALARM_PIN, HIGH);
    // Different beep patterns could be added per state; simple continuous tone here
    tone(BUZZER_PIN, 2000);
  }
}

void maybeNotifyStaff(AlertState state, unsigned long now) {
  if (state == NORMAL) {
    lastNotifiedState = NORMAL;
    return;
  }

  bool stateChanged = (state != lastNotifiedState);
  bool cooldownPassed = (now - lastNotifyMs) > NOTIFY_COOLDOWN_MS;

  if (stateChanged || cooldownPassed) {
    String message = buildAlertMessage(state);
    sendTelegramAlert(message);
    lastNotifiedState = state;
    lastNotifyMs = now;
  }
}

String buildAlertMessage(AlertState state) {
  String base = "GLUCOSE DRIP ALERT\n";
  base += "Status: " + stateToText(state) + "\n";
  base += "Flow rate: " + String(dropsPerMinute, 1) + " drops/min\n";
  base += "Action required: Please check patient line immediately.";
  return base;
}

String stateToText(AlertState state) {
  switch (state) {
    case NORMAL: return "Normal";
    case FLOW_TOO_SLOW: return "Flow rate too slow";
    case FLOW_TOO_FAST: return "Flow rate too fast";
    case FLOW_STOPPED: return "Flow stopped / line blocked";
    case OVERFLOW_RISK: return "Overflow risk - chamber level high";
    case BACKFLOW_RISK: return "Backflow risk - bag empty, no flow";
    default: return "Unknown";
  }
}

float readUltrasonicDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long durationUs = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout
  if (durationUs == 0) return 999.0; // no echo, treat as out of range
  return durationUs * 0.0343 / 2.0;  // speed of sound conversion to cm
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection failed - alerts will be local only (buzzer/LED).");
  }
}

void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) +
               "/sendMessage?chat_id=" + String(TELEGRAM_CHAT_ID) +
               "&text=" + urlEncode(message);

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.println("Alert sent to staff. HTTP code: " + String(httpCode));
  } else {
    Serial.println("Failed to send alert: " + http.errorToString(httpCode));
  }
  http.end();
}

String urlEncode(const String &str) {
  String encoded = "";
  char c;
  char buf[4];
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else if (c == '\n') {
      encoded += "%0A";
    } else {
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}
