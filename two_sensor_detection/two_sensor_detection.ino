// Room 1 counter: ESP32 + HC-SR04 (left side) + PIR (right side)
// Ultrasonic first -> entry to Room 1; PIR first -> exit from Room 1.
// Plus: OV7670 (non-FIFO) camera, streamed over WebSocket at all times,
// running on core 0 so it never blocks the sensor/counter logic on core 1.
//
// Camera driver: https://github.com/kobatan/OV7670-ESP32
// Install it (Sketch > Include Library > Add .ZIP Library, or drop the
// OV7670-ESP32 folder into Documents/Arduino/libraries) before compiling.
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <SHA1Builder.h>
#include "base64.h"
#include <OV7670.h>

// Put your Wi-Fi name and password here before uploading.
const char* WIFI_SSID = "adui";
const char* WIFI_PASSWORD = "75cfd9d2";

// Reachable at http://esp32-room1.local/api/status once mDNS is up.
// Camera stream page is served separately at http://esp32-room1.local:81/
const char* MDNS_HOSTNAME = "esp32-room1";
const uint16_t CAM_WS_PORT = 81;

const int TRIG_PIN = 5;   // HC-SR04 TRIG (left side)
const int ECHO_PIN = 34;  // HC-SR04 ECHO: use a 5V-to-3.3V voltage divider
const int PIR_PIN = 21;   // PIR OUT (right side)
const int NEAR_DISTANCE_CM = 30;

// ---- OV7670 pin map (avoids GPIO 5/21/34, already used above) ----
// RESET -> tie straight to 3.3V (not a GPIO). PWDN -> tie straight to GND.
// HREF (HS) -> leave unconnected (this driver doesn't use it).
const camera_config_t CAM_CONF = {
  .D0 = 36,   // VP
  .D1 = 39,   // VN
  .D2 = 35,
  .D3 = 32,
  .D4 = 33,
  .D5 = 25,
  .D6 = 26,
  .D7 = 27,
  .XCLK = 14,
  .PCLK = 19,
  .VSYNC = 13,
  .xclk_freq_hz = 10000000,
  .ledc_timer = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0
};
const int CAM_SDA_PIN = 22;  // SCCB SDA (SIOD)
const int CAM_SCL_PIN = 23;  // SCCB SCL (SIOC)
#define CAM_RES QCIF
const uint16_t CAM_WIDTH = 176;
const uint16_t CAM_HEIGHT = 144;
const uint16_t CAM_DIV = 1;  // lines sent per WebSocket message
const unsigned long SENSOR_INTERVAL_MS = 100;
const unsigned long SEQUENCE_WINDOW_MS = 3000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// Set to false once wiring is verified, to keep Serial output quiet.
const bool DEBUG_SENSORS = true;
const unsigned long DEBUG_PRINT_INTERVAL_MS = 500;

// Set to true to skip Wi-Fi/web server entirely and just test the sensors
// over Serial. Flip back to false when you're ready to hook up the network.
const bool SENSOR_TEST_ONLY = false;

// Ring buffer of recent events so a frontend that polls slower than
// people walk through still sees every entry/exit via ?since=<id>.
const int EVENT_HISTORY_SIZE = 20;

WebServer server(80);
enum FirstSensor { NONE, LEFT_SENSOR, RIGHT_SENSOR };
FirstSensor firstSensor = NONE;
unsigned long firstSensorAt = 0;
unsigned long lastSensorRead = 0;
bool lastLeftActive = false;
bool lastRightActive = false;
int occupancy = 0;

struct Event {
  unsigned long id;
  String type;
  int occupancy;
};
Event eventHistory[EVENT_HISTORY_SIZE];
unsigned long nextEventId = 1;

float getDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  return duration == 0 ? -1 : duration * 0.0343 / 2;
}

void pushEvent(const String& type) {
  Event e{nextEventId++, type, occupancy};
  eventHistory[e.id % EVENT_HISTORY_SIZE] = e;
}

void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

void handleOptions() {
  sendCors();
  server.send(204);
}

void handleRoot() {
  sendCors();
  String html = "<h1>ESP32 Room 1 Sensor</h1><p>Occupancy: " + String(occupancy) +
                "</p><p>API: <a href=\"/api/status\">/api/status</a></p>";
  server.send(200, "text/html", html);
}

// Returns current occupancy plus every event newer than ?since=<id>
// (defaults to only the latest event) so a client can never miss one
// as long as it polls at least once per EVENT_HISTORY_SIZE events.
void sendStatus() {
  unsigned long since = nextEventId > 1 ? nextEventId - 2 : 0;
  if (server.hasArg("since")) since = server.arg("since").toInt();

  String events = "[";
  unsigned long oldestKept = nextEventId > EVENT_HISTORY_SIZE ? nextEventId - EVENT_HISTORY_SIZE : 1;
  unsigned long from = since + 1 < oldestKept ? oldestKept : since + 1;
  bool first = true;
  for (unsigned long id = from; id < nextEventId; id++) {
    Event& e = eventHistory[id % EVENT_HISTORY_SIZE];
    if (e.id != id) continue;  // slot was overwritten, shouldn't happen given oldestKept
    if (!first) events += ",";
    events += "{\"id\":" + String(e.id) + ",\"event\":\"" + e.type + "\",\"occupancy\":" + String(e.occupancy) + "}";
    first = false;
  }
  events += "]";

  String json = "{\"occupancy\":" + String(occupancy) + ",\"lastEventId\":" + String(nextEventId - 1) +
                ",\"events\":" + events + "}";
  sendCors();
  server.send(200, "application/json", json);
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
    pushEvent("entry");
    Serial.println("ENTRY: left to right");
  } else if (firstSensor == RIGHT_SENSOR && sensor == LEFT_SENSOR) {
    occupancy = max(0, occupancy - 1);
    pushEvent("exit");
    Serial.println("EXIT: right to left");
  }
  firstSensor = NONE;
}

void printDebugStatus(float distanceCm, bool leftActive, bool rightActive) {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint < DEBUG_PRINT_INTERVAL_MS) return;
  lastPrint = millis();

  Serial.print("[DEBUG] HC-SR04: ");
  if (distanceCm < 0) Serial.print("no echo (out of range/disconnected)");
  else Serial.print(String(distanceCm, 1) + " cm");
  Serial.print(leftActive ? " (NEAR)" : " (far)");

  Serial.print(" | PIR: ");
  Serial.print(rightActive ? "MOTION" : "idle");

  Serial.print(" | occupancy=" + String(occupancy));
  Serial.println();
}

void readSensors() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL_MS) return;
  lastSensorRead = millis();
  float distanceCm = getDistanceCm();
  bool leftActive = distanceCm > 0 && distanceCm <= NEAR_DISTANCE_CM;
  bool rightActive = digitalRead(PIR_PIN) == HIGH;

  if (DEBUG_SENSORS) printDebugStatus(distanceCm, leftActive, rightActive);

  if (leftActive && !lastLeftActive) {
    if (DEBUG_SENSORS) Serial.println("[DEBUG] LEFT (HC-SR04) triggered");
    handleFirstTrigger(LEFT_SENSOR);
  }
  if (rightActive && !lastRightActive) {
    if (DEBUG_SENSORS) Serial.println("[DEBUG] RIGHT (PIR) triggered");
    handleFirstTrigger(RIGHT_SENSOR);
  }
  if (!leftActive && lastLeftActive && DEBUG_SENSORS) Serial.println("[DEBUG] LEFT (HC-SR04) cleared");
  if (!rightActive && lastRightActive && DEBUG_SENSORS) Serial.println("[DEBUG] RIGHT (PIR) cleared");

  lastLeftActive = leftActive;
  lastRightActive = rightActive;
  if (firstSensor != NONE && millis() - firstSensorAt > SEQUENCE_WINDOW_MS) firstSensor = NONE;
}

void wifiScanDebug() {
  Serial.println("[DEBUG] Scanning for Wi-Fi networks...");
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("[DEBUG] No networks found at all (check antenna/power).");
    return;
  }
  bool foundTarget = false;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    Serial.printf("[DEBUG]  %2d: %-32s RSSI=%d  %s\n", i, ssid.c_str(), WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "encrypted");
    if (ssid == WIFI_SSID) foundTarget = true;
  }
  if (!foundTarget) {
    Serial.println("[DEBUG] Target SSID was NOT seen in the scan above.");
    Serial.println("[DEBUG] Common cause: the AP is 5GHz-only, or the SSID string doesn't match exactly.");
  }
  WiFi.scanDelete();
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    // 2=AUTH_EXPIRE 15=4WAY_HANDSHAKE_TIMEOUT(usually wrong password)
    // 201=NO_AP_FOUND 202=AUTH_FAIL 203=ASSOC_FAIL 8=ASSOC_LEAVE
    Serial.printf("[DEBUG] WiFi disconnect reason = %d\n", info.wifi_sta_disconnected.reason);
  }
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWifiEvent);
  wifiScanDebug();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long startAttempt = millis();
  wl_status_t lastStatus = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED) {
    wl_status_t st = WiFi.status();
    if (st != lastStatus) {
      // 1=NO_SSID_AVAIL 3=CONNECTED 4=CONNECT_FAILED(usually wrong password)
      // 5=CONNECTION_LOST 6=DISCONNECTED 7=NO_SHIELD
      Serial.printf("\n[DEBUG] WiFi.status() = %d\n", (int)st);
      lastStatus = st;
    }
    if (millis() - startAttempt > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWi-Fi connect timed out, retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      startAttempt = millis();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("ESP32 URL: http://");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.print("Also reachable at: http://");
    Serial.print(MDNS_HOSTNAME);
    Serial.println(".local");
  } else {
    Serial.println("mDNS setup failed");
  }
  return true;
}

// ==================== OV7670 camera (WebSocket stream) ====================
// Adapted from kobatan/OV7670-ESP32's OV7670_Web example: same WebSocket
// framing/handshake, but on its own port (CAM_WS_PORT) and its own FreeRTOS
// task pinned to core 0, so it runs continuously without blocking the
// sensor/counter logic in loop() on core 1.

OV7670 cam;
WiFiServer camServer(CAM_WS_PORT);
WiFiClient camWsClient;
bool camWsOn = false;

const char* camHtmlHead =
  "HTTP/1.1 200 OK\r\n"
  "Content-type:text/html\r\n"
  "Connection:close\r\n"
  "\r\n"
  "<!DOCTYPE html>\n<html><head><meta charset='UTF-8'>\n"
  "<meta name='viewport' content='width=device-width'>\n"
  "<title>OV7670 Live</title></head><body>\n";

const char* camHtmlBody =
  "<div id='msg' style='font-size:20px;color:#c00;'>Connecting...</div>\n"
  "<span id='msgIn'></span>\n"
  "<script>\n"
  "var wsUri = 'ws://";

const char* camHtmlScript =
  "var socket=null,ctx,width,height,imageData,pixels,fps=0,msg,msgIn;\n"
  "window.onload=function(){\n"
  "  msg=document.getElementById('msg'); msgIn=document.getElementById('msgIn');\n"
  "  var c=document.getElementById('cam'); ctx=c.getContext('2d');\n"
  "  width=c.width; height=c.height;\n"
  "  imageData=ctx.createImageData(width,1); pixels=imageData.data;\n"
  "  setTimeout(wsConnect,1000);\n"
  "}\n"
  "function wsConnect(){\n"
  "  socket=new WebSocket(wsUri); socket.binaryType='arraybuffer';\n"
  "  socket.onopen=function(){msg.innerHTML='CONNECTED';};\n"
  "  socket.onclose=function(){msg.innerHTML='disconnected, retrying'; setTimeout(wsConnect,1000);};\n"
  "  socket.onerror=function(e){msg.innerHTML=e.data;};\n"
  "  socket.onmessage=function(evt){ if(evt.data instanceof ArrayBuffer) drawLine(evt.data); };\n"
  "  setTimeout(fpsShow,1000);\n"
  "}\n"
  "function fpsShow(){ msgIn.innerHTML=fps+'fps'; fps=0; setTimeout(fpsShow,1000); }\n"
  "function drawLine(data){\n"
  "  var buf=new Uint16Array(data); var lineNo=buf[0];\n"
  "  for(var y=0;y<(buf.length-1)/width;y+=1){\n"
  "    var base=0;\n"
  "    for(var x=0;x<width;x+=1){\n"
  "      var c=1+x+y*width;\n"
  "      pixels[base]=(buf[c]&0xf800)>>8|(buf[c]&0xe000)>>13;\n"
  "      pixels[base+1]=(buf[c]&0x07e0)>>3|(buf[c]&0x0600)>>9;\n"
  "      pixels[base+2]=(buf[c]&0x001f)<<3|(buf[c]&0x001c)>>2;\n"
  "      pixels[base+3]=255; base+=4;\n"
  "    }\n"
  "    ctx.putImageData(imageData,0,lineNo+y);\n"
  "  }\n"
  "  if(lineNo+y==height) fps+=1;\n"
  "}\n"
  "</script></body></html>\n";

void camPrintHTML(WiFiClient& client) {
  client.print(camHtmlHead);
  client.print(F("<canvas id='cam' width='"));
  client.print(CAM_WIDTH);
  client.print(F("' height='"));
  client.print(CAM_HEIGHT);
  client.println(F("'></canvas>\n"));
  client.print(camHtmlBody);
  client.print(WiFi.localIP());
  client.print(":");
  client.print(CAM_WS_PORT);
  client.println(F("/';"));
  client.println(camHtmlScript);
}

String camHashKey(String key) {
  String str = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  SHA1Builder sha1;
  sha1.begin();
  sha1.add((const uint8_t*)str.c_str(), str.length());
  sha1.calculate();
  uint8_t hash[20];
  sha1.getBytes(hash);
  return base64::encode(hash, 20);
}

void camWsHandshake(WiFiClient& client) {
  String req;
  String hashReqKey;
  do {
    req = client.readStringUntil('\n');
    if (req.indexOf("Sec-WebSocket-Key") >= 0) {
      hashReqKey = req.substring(req.indexOf(':') + 2, req.indexOf('\r'));
    }
  } while (req.indexOf("\r") != 0);

  String resp = "HTTP/1.1 101 Switching Protocols\r\n";
  resp += "Upgrade: websocket\r\n";
  resp += "Connection: Upgrade\r\n";
  resp += "Sec-WebSocket-Accept: " + camHashKey(hashReqKey) + "\r\n\r\n";
  client.print(resp);
  camWsClient = client;
}

void camHandleHttp() {
  WiFiClient client = camServer.available();
  if (!client) return;

  while (client.connected()) {
    if (!client.available()) break;
    String req = client.readStringUntil('\n');

    if (req.indexOf("GET / HTTP") != -1) {
      while (req.indexOf("\r") != 0) {
        req = client.readStringUntil('\n');
        if (req.indexOf("websocket") != -1) {
          camWsHandshake(client);
          camWsOn = true;
          return;
        }
      }
      delay(10);
      camPrintHTML(client);
    } else {
      while (client.available()) client.read();
    }
    if (!camWsOn) {
      delay(1);
      client.stop();
      delay(1);
    }
  }
}

#define CAM_OP_BIN 0x82
uint8_t* camWsBuf = nullptr;
uint16_t camDataSize = 0;

bool camSetImgHeader(uint16_t w, uint16_t h) {
  uint16_t lineSize = w * 2;
  camDataSize = 2 + lineSize * h;
  camWsBuf = (uint8_t*)malloc(camDataSize + 4);
  if (camWsBuf == nullptr) {
    Serial.println("[CAM] buffer alloc failed");
    return false;
  }
  camWsBuf[0] = CAM_OP_BIN;
  camWsBuf[1] = 126;
  camWsBuf[2] = (uint8_t)(camDataSize / 256);
  camWsBuf[3] = (uint8_t)(camDataSize % 256);
  return true;
}

void camWsSendImg(uint16_t lineNo) {
  camWsBuf[4] = (uint8_t)(lineNo % 256);
  camWsBuf[5] = (uint8_t)(lineNo / 256);
  uint16_t len = camDataSize + 4;
  uint8_t* p = camWsBuf;
  const uint16_t unit = 2048;
  while (len) {
    uint16_t chunk = (len > unit) ? unit : len;
    camWsClient.write(p, chunk);
    len -= chunk;
    p += chunk;
  }
}

// Runs forever on core 0: keeps the camera streaming without ever touching
// the sensor/counter logic that runs in loop() on core 1.
void cameraTask(void* pvParameters) {
  uint16_t dy = CAM_HEIGHT / CAM_DIV;
  if (!camSetImgHeader(CAM_WIDTH, dy)) vTaskDelete(NULL);

  for (;;) {
    for (uint16_t y = 0; y < CAM_HEIGHT; y += dy) {
      cam.getLines(y + 1, &camWsBuf[6], dy);
      if (camWsOn) {
        if (camWsClient) {
          camWsSendImg(y);
        } else {
          camWsClient.stop();
          camWsOn = false;
          Serial.println("[CAM] client disconnected");
        }
      }
    }
    if (!camWsOn) camHandleHttp();
  }
}

bool cameraInit() {
  Wire.begin(CAM_SDA_PIN, CAM_SCL_PIN);
  Wire.setClock(400000);

  esp_err_t err = cam.init(&CAM_CONF, CAM_RES, RGB565);
  if (err != ESP_OK) {
    Serial.println("[CAM] cam.init failed");
    return false;
  }
  cam.vflip(false);
  Serial.printf("[CAM] MID=%X PID=%X\n", cam.getMID(), cam.getPID());

  camServer.begin();
  xTaskCreatePinnedToCore(cameraTask, "cameraTask", 8192, NULL, 1, NULL, 0);
  Serial.printf("[CAM] streaming at ws://<device-ip>:%u/\n", CAM_WS_PORT);
  return true;
}
// ================== end OV7670 camera (WebSocket stream) ==================

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  if (SENSOR_TEST_ONLY) {
    Serial.println("SENSOR_TEST_ONLY mode: skipping Wi-Fi, testing sensors only.");
    return;
  }

  connectToWiFi();
  cameraInit();  // camera stays on for the life of the sketch, on core 0

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.begin();
  Serial.println("Room 1 sensor server started");
}

void loop() {
  if (SENSOR_TEST_ONLY) {
    readSensors();
    return;
  }

  server.handleClient();
  readSensors();

  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) connectToWiFi();
  }
}
