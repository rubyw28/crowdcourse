// Crowdsource: single-board test firmware.
//
// Starts a Wi-Fi hotspot, serves index.html at http://192.168.4.1 and speaks the
// WebSocket contract on /ws with a *simulated* friend (no second bracelet needed).
// Use it to test the web page on real phones over the real hotspot.
//
// Serial monitor (115200) commands, same as the Node mock:
//   s = friend sends SOS   c = friend ends SOS   n = friend walks close
//   f = friend walks far   l = toggle signal lost
//
// Libraries: "ESP Async WebServer" and "Async TCP" (both by ESP32Async).
// Regenerate index_html.h after editing index.html: python3 firmware/embed_html.py

#include <WiFi.h>
#include <esp_mac.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include "index_html.h"

static const char *AP_PREFIX = "Crowdsource-";
static const char *AP_PASSWORD = "";          // open network for testing; 8+ chars to enable WPA2
static const char *FRIEND = "Alex";
static const int LED_PIN = 2;                 // onboard LED on most ESP32 dev boards

static const uint32_t RSSI_PERIOD_MS = 200;   // 5 Hz
static const uint32_t STATUS_PERIOD_MS = 2000;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------- simulated friend ----------
struct World {
  float d = 4;            // metres
  float dest = 4;
  float speed = 1;
  float shadow = 0;       // slow fading, dB
  uint32_t nextDest = 0;
  bool forceLost = false;
  bool friendSos = false;
  int lastRssi = 0;
  float battery = 92;
  float friendBattery = 67;
} world;

static float frand(float a, float b) { return a + (b - a) * (esp_random() / 4294967295.0f); }
static float gauss() {
  float u = frand(1e-6f, 1), v = frand(0, 1);
  return sqrtf(-2 * logf(u)) * cosf(2 * PI * v);
}

static void sendAll(const String &msg) {
  if (ws.count()) ws.textAll(msg);
}

static void sendStatus(AsyncWebSocketClient *client = nullptr) {
  String msg = String("{\"type\":\"status\",\"name\":\"") + FRIEND +
               "\",\"battery\":" + (int)world.battery +
               ",\"friendBattery\":" + (int)world.friendBattery + "}";
  if (client) client->text(msg); else sendAll(msg);
}

static void setFriendSos(bool on) {
  world.friendSos = on;
  sendAll(on ? "{\"type\":\"sos\"}" : "{\"type\":\"sos_clear\"}");
  Serial.printf("! %s %s SOS\n", FRIEND, on ? "sent" : "cleared");
}

static void stepWorld() {
  uint32_t now = millis();
  float dt = RSSI_PERIOD_MS / 1000.0f;
  if (now > world.nextDest) {
    world.dest = frand(0, 1) < 0.4f ? frand(0.3f, 2.5f) : frand(3, 45);
    world.speed = frand(0.4f, 1.4f);
    world.nextDest = now + (uint32_t)frand(8000, 20000);
  }
  float delta = world.dest - world.d;
  float stepM = fminf(fabsf(delta), world.speed * dt);
  world.d += delta > 0 ? stepM : -stepM;
  world.shadow += -world.shadow * 0.05f + gauss() * 0.6f;

  float ideal = -45 - 26 * log10f(fmaxf(world.d, 0.3f));
  int rssi = (int)lroundf(constrain(ideal + world.shadow + gauss() * 2.5f, -90.0f, -40.0f));
  world.lastRssi = rssi;

  float lossP = constrain((-rssi - 82) / 10.0f, 0.0f, 0.85f);
  if (!world.forceLost && frand(0, 1) >= lossP) {
    sendAll(String("{\"type\":\"rssi\",\"rssi\":") + rssi + "}");
  }
}

// ---------- incoming messages ----------
// Pulls the value of "type" out of a small JSON object without a JSON library.
static String messageType(const String &json) {
  int k = json.indexOf("\"type\"");
  if (k < 0) return "";
  int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  return (q1 < 0 || q2 < 0) ? "" : json.substring(q1 + 1, q2);
}

static uint32_t ackAt = 0;   // when to fake the friend acknowledging our SOS

static void onPhoneMessage(AsyncWebSocketClient *client, const String &msg) {
  Serial.printf("< #%u %s\n", client->id(), msg.c_str());
  String type = messageType(msg);
  if (type == "sos") {
    digitalWrite(LED_PIN, HIGH);
    ackAt = millis() + 3000;
  } else if (type == "sos_cancel") {
    digitalWrite(LED_PIN, LOW);
    ackAt = 0;
  } else if (type == "sos_ack") {
    world.friendSos = false;
  }
}

static void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("+ client #%u from %s (%u connected)\n", client->id(),
                  client->remoteIP().toString().c_str(), ws.count());
    sendStatus(client);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("- client #%u (%u connected)\n", client->id(), ws.count());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      onPhoneMessage(client, String((const char *)data, len));
    }
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(300);

  uint8_t mac[6];
  WiFi.mode(WIFI_AP);
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);   // read from eFuse; works before the AP is up
  char ssid[32];
  snprintf(ssid, sizeof ssid, "%s%02X%02X", AP_PREFIX, mac[4], mac[5]);
  WiFi.softAP(ssid, AP_PASSWORD);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  server.onNotFound([](AsyncWebServerRequest *req) { req->send(404, "text/plain", "not found"); });
  server.begin();

  Serial.printf("\nCrowdsource test firmware\n");
  Serial.printf("Join Wi-Fi \"%s\"%s, then open http://%s\n", ssid,
                strlen(AP_PASSWORD) ? " (password set)" : " (no password)",
                WiFi.softAPIP().toString().c_str());
  Serial.println("Keys: s=friend SOS  c=clear SOS  n=walk close  f=walk far  l=toggle lost");
}

void loop() {
  static uint32_t lastRssi = 0, lastStatus = 0, lastLog = 0, lastClean = 0;
  uint32_t now = millis();

  if (now - lastRssi >= RSSI_PERIOD_MS) { lastRssi = now; stepWorld(); }
  if (now - lastStatus >= STATUS_PERIOD_MS) {
    lastStatus = now;
    world.battery = fmaxf(0, world.battery - 0.02f);
    world.friendBattery = fmaxf(0, world.friendBattery - 0.03f);
    sendStatus();
  }
  if (now - lastClean >= 1000) { lastClean = now; ws.cleanupClients(); }
  if (now - lastLog >= 5000 && ws.count()) {
    lastLog = now;
    Serial.printf("  d=%.1fm rssi=%d lost=%d clients=%u heap=%u\n", world.d, world.lastRssi,
                  world.forceLost, ws.count(), ESP.getFreeHeap());
  }
  if (ackAt && now > ackAt) {
    ackAt = 0;
    sendAll("{\"type\":\"sos_ack\"}");
    Serial.printf("! %s acknowledged your SOS\n", FRIEND);
  }

  while (Serial.available()) {
    switch (Serial.read()) {
      case 's': setFriendSos(true); break;
      case 'c': setFriendSos(false); break;
      case 'n': world.dest = 0.5; world.speed = 3; world.nextDest = now + 30000; Serial.println("~ walking close"); break;
      case 'f': world.dest = 40; world.speed = 4; world.nextDest = now + 30000; Serial.println("~ walking far"); break;
      case 'l': world.forceLost = !world.forceLost; Serial.printf("~ signal lost: %d\n", world.forceLost); break;
    }
  }
}
