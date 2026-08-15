// Room 1 counter: ESP32 + HC-SR04 (left side) + PIR (right side)
// Ultrasonic first -> entry to Room 1; PIR first -> exit from Room 1.
#include <WiFi.h>
#include <WebServer.h>

// Put your Wi-Fi name and password here before uploading.
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const int TRIG_PIN = 5;   // HC-SR04 TRIG (left side)
const int ECHO_PIN = 34;  // HC-SR04 ECHO: use a 5V-to-3.3V voltage divider
const int PIR_PIN = 21;   // PIR OUT (right side)
const int NEAR_DISTANCE_CM = 30;
const unsigned long SENSOR_INTERVAL_MS = 100;
const unsigned long SEQUENCE_WINDOW_MS = 3000;

WebServer server(80);
enum FirstSensor { NONE, LEFT_SENSOR, RIGHT_SENSOR };
FirstSensor firstSensor = NONE;
unsigned long firstSensorAt = 0;
unsigned long lastSensorRead = 0;
bool lastLeftActive = false;
bool lastRightActive = false;
int occupancy = 0;
String pendingEvent = "";

float getDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return duration == 0 ? -1 : duration * 0.0343 / 2;
}

void sendStatus() {
  String json = "{\"event\":\"" + pendingEvent + "\",\"occupancy\":" + String(occupancy) + "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
  pendingEvent = "";  // Send each event only once.
}

void handleFirstTrigger(FirstSensor sensor) {
  unsigned long now = millis();
  if (firstSensor == NONE || now - firstSensorAt > SEQUENCE_WINDOW_MS) {
    firstSensor = sensor;
    firstSensorAt = now;
    return;
  }
  if (sensor == firstSensor) return;

  if (firstSensor == LEFT_SENSOR && sensor == RIGHT_SENSOR) {
    occupancy++;
    pendingEvent = "entry";
    Serial.println("ENTRY: left to right");
  } else if (firstSensor == RIGHT_SENSOR && sensor == LEFT_SENSOR) {
    occupancy = max(0, occupancy - 1);
    pendingEvent = "exit";
    Serial.println("EXIT: right to left");
  }
  firstSensor = NONE;
}

void readSensors() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL_MS) return;
  lastSensorRead = millis();
  float distanceCm = getDistanceCm();
  bool leftActive = distanceCm > 0 && distanceCm <= NEAR_DISTANCE_CM;
  bool rightActive = digitalRead(PIR_PIN) == HIGH;

  if (leftActive && !lastLeftActive) handleFirstTrigger(LEFT_SENSOR);
  if (rightActive && !lastRightActive) handleFirstTrigger(RIGHT_SENSOR);
  lastLeftActive = leftActive;
  lastRightActive = rightActive;
  if (firstSensor != NONE && millis() - firstSensorAt > SEQUENCE_WINDOW_MS) firstSensor = NONE;
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("ESP32 URL: http://");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  connectToWiFi();
  server.on("/api/status", HTTP_GET, sendStatus);
  server.begin();
  Serial.println("Room 1 sensor server started");
}

void loop() {
  server.handleClient();
  readSensors();
}
