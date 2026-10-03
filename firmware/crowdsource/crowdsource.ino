// Crowdsource bracelet firmware (flash the same sketch to both bracelets).
//
// Each bracelet:
//   - broadcasts a small ESP-NOW beacon 10x per second (name, battery, SOS state)
//   - measures the RSSI of its friend's beacons and forwards it to the phone
//   - runs a Wi-Fi hotspot "Crowdsource-XXXX" serving index.html at http://192.168.4.1
//   - buzzes faster as the friend gets closer, and hard during the friend's SOS
//
// SOS is carried in every beacon (flag + sequence number) so a lost packet never loses it:
//   A starts SOS  -> A.flags=SOS, A.sosSeq++   -> B tells its phone {"type":"sos"}
//   B's phone acks -> B.ackSeq = A.sosSeq       -> A tells its phone {"type":"sos_ack"}
//   A ends SOS    -> A.flags=0                  -> B tells its phone {"type":"sos_clear"}
//
// The first bracelet heard with the same GROUP_ID becomes the friend. Send 'p' over
// serial to forget it and pair again ('s' and 'a' test SOS without a phone).
//
// Libraries: "ESP Async WebServer" and "Async TCP" (both by ESP32Async). Board: ESP32 Dev Module.
// Regenerate index_html.h after editing index.html: python3 firmware/embed_html.py

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <math.h>
#include "index_html.h"

// ---------- config ----------
static const char *MY_NAME = "";            // shown on your friend's phone; "" = "Band XXXX"
static const uint8_t GROUP_ID = 0x42;       // both bracelets must match; change if another team uses this code
static const uint8_t WIFI_CHANNEL = 6;      // both bracelets must match (ESP-NOW shares the hotspot's channel)
static const char *AP_PASSWORD = "";        // open hotspot; 8+ chars to enable WPA2
static const int BUTTON_PIN = 0;            // BOOT button. Hold 1.5 s to start or end SOS
static const int HAPTIC_PIN = 2;            // vibration motor driver, or the onboard LED as a stand-in
static const int BATTERY_PIN = -1;          // ADC pin on a 1:2 divider from the LiPo, or -1 if not wired

// Pins left open for extra hardware. Taken: 0 (SOS button), 1 and 3 (USB serial
// telemetry — do not touch), 2 (haptic). Also leave 6–11 alone (flash) and avoid
// 12 (it is read at boot). Free and safe: 4, 5, 13–19, 21–23, 25–27, 32, 33.
// A sensible add-on set, matching crowd_source/'s wiring: NeoPixel data on 13,
// buzzer on 25, a second button on 27, OLED on the default I2C pins 21 (SDA) and 22 (SCL).
// Add parts in setup() and loop() below the radio. Keep Beacon, GROUP_ID, and
// WIFI_CHANNEL as they are, or the two bracelets stop hearing each other.
static const bool HAPTIC_PROXIMITY = true;  // pulse faster as the friend gets closer

static const uint32_t BEACON_MS = 100;
static const uint32_t STATUS_MS = 1000;
static const uint32_t LOST_MS = 5000;
static const uint32_t HOLD_MS = 1500;

// ---------- beacon ----------
static const uint32_t MAGIC = 0x44575243;   // "CRWD"
static const uint8_t VERSION = 1;
static const uint8_t FLAG_SOS = 0x01;

struct __attribute__((packed)) Beacon {
  uint32_t magic;
  uint8_t group;
  uint8_t version;
  uint8_t battery;     // 0-100, 255 = unknown
  uint8_t flags;
  uint16_t sosSeq;     // bumps every time this bracelet starts an SOS
  uint16_t ackSeq;     // the friend's sosSeq this bracelet last acknowledged
  char name[16];
};

struct Rx {
  uint8_t mac[6];
  int8_t rssi;
  Beacon b;
};

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static QueueHandle_t rxQueue;

// ---------- state (touched only from loop()) ----------
static char myName[16];
static uint16_t mySosSeq = 0, myAckSeq = 0;
static bool mySosActive = false, mySosAcked = false;

static bool paired = false;
static uint8_t friendMac[6];
static char friendName[16] = "Friend";
static int friendBattery = -1;
static uint32_t friendLastAt = 0;
static float rssiAvg = NAN;
static uint16_t friendSosSeq = 0;
static bool friendSosOn = false;        // friend's beacon currently says SOS
static bool friendSosPending = false;   // ...and we haven't acknowledged it yet

static int calNear = -45, calFar = -85;
static uint32_t celebrateUntil = 0;     // short buzz pattern when our SOS is acknowledged

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------- helpers ----------
static void sendAll(const String &msg) {
  if (ws.count()) ws.textAll(msg);
}

// Telemetry for the laptop bridge (bridge/): one JSON object per line, prefixed with '@'.
static void emit(const char *ev, const String &extra = "") {
  Serial.printf("@{\"ev\":\"%s\",\"me\":\"%s\",\"friend\":\"%s\"%s%s}\n", ev, myName, friendName,
                extra.length() ? "," : "", extra.c_str());
}

static int readBattery() {
  if (BATTERY_PIN < 0) return -1;
  int mv = analogReadMilliVolts(BATTERY_PIN) * 2;      // 1:2 divider
  return constrain(map(mv, 3300, 4200, 0, 100), 0, 100);
}

static String statusJson() {
  String s = String("{\"type\":\"status\",\"name\":\"") + friendName + "\"";
  int b = readBattery();
  if (b >= 0) s += String(",\"battery\":") + b;
  if (friendBattery >= 0) s += String(",\"friendBattery\":") + friendBattery;
  return s + "}";
}

static String jsonType(const String &json) {
  int k = json.indexOf("\"type\"");
  if (k < 0) return "";
  int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  return (q1 < 0 || q2 < 0) ? "" : json.substring(q1 + 1, q2);
}

static bool jsonInt(const String &json, const char *key, int &out) {
  int k = json.indexOf(String("\"") + key + "\"");
  if (k < 0) return false;
  int c = json.indexOf(':', k);
  if (c < 0) return false;
  out = json.substring(c + 1).toInt();
  return true;
}

static String macStr(const uint8_t *m) {
  char s[18];
  snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return s;
}

// ---------- SOS ----------
static void startMySos(const char *from) {
  mySosSeq++;
  mySosActive = true;
  mySosAcked = false;
  Serial.printf("! SOS started from %s (seq %u)\n", from, mySosSeq);
  emit("my_sos", String("\"seq\":") + mySosSeq + ",\"from\":\"" + from + "\"");
}

static void endMySos(const char *from) {
  mySosActive = false;
  Serial.printf("! SOS ended from %s\n", from);
  emit("my_sos_end", String("\"from\":\"") + from + "\"");
}

static void ackFriendSos(const char *from) {
  if (!friendSosPending) return;
  myAckSeq = friendSosSeq;
  friendSosPending = false;
  Serial.printf("! you acknowledged %s's SOS\n", friendName);
  emit("friend_sos_acked", String("\"seq\":") + friendSosSeq + ",\"from\":\"" + from + "\"");
}

// ---------- ESP-NOW ----------
// Runs on the Wi-Fi task: copy and hand off, nothing else.
static void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Beacon)) return;
  Rx rx;
  memcpy(&rx.b, data, sizeof(Beacon));
  if (rx.b.magic != MAGIC || rx.b.group != GROUP_ID || rx.b.version != VERSION) return;
  memcpy(rx.mac, info->src_addr, 6);
  rx.rssi = info->rx_ctrl->rssi;
  xQueueSend(rxQueue, &rx, 0);
}

static void handleBeacon(const Rx &rx, uint32_t now) {
  bool justPaired = false;
  if (!paired) {
    memcpy(friendMac, rx.mac, 6);
    paired = justPaired = true;
    Serial.printf("~ paired with %.16s (%s)\n", rx.b.name, macStr(rx.mac).c_str());
  }
  if (memcmp(rx.mac, friendMac, 6) != 0) {
    static uint32_t lastWarn = 0;
    if (now - lastWarn > 10000) {
      lastWarn = now;
      Serial.printf("~ ignoring another bracelet %s (send 'p' to re-pair)\n", macStr(rx.mac).c_str());
    }
    return;
  }

  friendLastAt = now;
  strncpy(friendName, rx.b.name, sizeof friendName);
  friendName[sizeof friendName - 1] = 0;
  friendBattery = rx.b.battery == 255 ? -1 : rx.b.battery;
  rssiAvg = isnan(rssiAvg) ? rx.rssi : rssiAvg * 0.7f + rx.rssi * 0.3f;
  if (justPaired) emit("paired", String("\"mac\":\"") + macStr(rx.mac) + "\"");
  emit("rssi", String("\"rssi\":") + rx.rssi);
  sendAll(String("{\"type\":\"rssi\",\"rssi\":") + rx.rssi + "}");

  // friend's SOS
  bool sos = rx.b.flags & FLAG_SOS;
  if (sos && (!friendSosOn || rx.b.sosSeq != friendSosSeq)) {
    friendSosSeq = rx.b.sosSeq;
    friendSosPending = (myAckSeq != friendSosSeq);
    if (friendSosPending) {
      sendAll("{\"type\":\"sos\"}");
      Serial.printf("! %s sent SOS (seq %u)\n", friendName, friendSosSeq);
      emit("friend_sos", String("\"seq\":") + friendSosSeq);
    }
  } else if (!sos && friendSosOn) {
    friendSosPending = false;
    sendAll("{\"type\":\"sos_clear\"}");
    Serial.printf("! %s ended SOS\n", friendName);
    emit("friend_sos_end");
  }
  friendSosOn = sos;

  // friend acknowledged our SOS
  if (mySosActive && !mySosAcked && rx.b.ackSeq == mySosSeq) {
    mySosAcked = true;
    celebrateUntil = now + 900;
    sendAll("{\"type\":\"sos_ack\"}");
    Serial.printf("! %s acknowledged your SOS\n", friendName);
    emit("my_sos_acked", String("\"seq\":") + mySosSeq);
  }
}

static void sendBeacon() {
  Beacon b = {};
  b.magic = MAGIC;
  b.group = GROUP_ID;
  b.version = VERSION;
  int bat = readBattery();
  b.battery = bat < 0 ? 255 : bat;
  b.flags = mySosActive ? FLAG_SOS : 0;
  b.sosSeq = mySosSeq;
  b.ackSeq = myAckSeq;
  strncpy(b.name, myName, sizeof b.name);
  esp_now_send(BROADCAST, (const uint8_t *)&b, sizeof b);
}

// ---------- phone ----------
static void onPhoneMessage(AsyncWebSocketClient *client, const String &msg) {
  Serial.printf("< phone #%u %s\n", client->id(), msg.c_str());
  String type = jsonType(msg);
  if (type == "sos") {
    if (!mySosActive) startMySos("phone");
  } else if (type == "sos_cancel") {
    if (mySosActive) endMySos("phone");
  } else if (type == "sos_ack") {
    ackFriendSos("phone");
  } else if (type == "calibrate") {
    int n, f;
    if (jsonInt(msg, "near", n) && jsonInt(msg, "far", f) && n - f >= 5) {
      calNear = n; calFar = f;
      int nearSpread = 0, farSpread = 0;
      String extra = String("\"near\":") + calNear + ",\"far\":" + calFar;
      if (jsonInt(msg, "nearSpread", nearSpread) && jsonInt(msg, "farSpread", farSpread)) {
        extra += String(",\"nearSpread\":") + nearSpread + ",\"farSpread\":" + farSpread;
      }
      Serial.printf("~ calibration near=%d far=%d\n", calNear, calFar);
      emit("calibrate", extra);
    }
  }
}

static void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type,
                      void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("+ phone #%u from %s\n", client->id(), client->remoteIP().toString().c_str());
    emit("phone", String("\"phones\":") + ws.count());
    client->text(statusJson());
    // a phone that (re)connects mid-alert still needs to see it
    if (friendSosPending) client->text("{\"type\":\"sos\"}");
    if (mySosActive && mySosAcked) client->text("{\"type\":\"sos_ack\"}");
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("- phone #%u\n", client->id());
    emit("phone", String("\"phones\":") + ws.count());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      onPhoneMessage(client, String((const char *)data, len));
    }
  }
}

// ---------- haptics ----------
static bool hapticOn(uint32_t now) {
  if (friendSosPending) return (now / 150) % 2 == 0;                    // urgent: fast, until acknowledged
  if (now < celebrateUntil) return ((celebrateUntil - now) / 150) % 2 == 0;  // 3 short pulses
  if (!HAPTIC_PROXIMITY || !paired || isnan(rssiAvg) || now - friendLastAt > LOST_MS) return false;
  float score = constrain((rssiAvg - calFar) / float(calNear - calFar), 0.0f, 1.0f);
  uint32_t period = 1800 - (uint32_t)(1450 * score);                    // same curve as the page
  return now % period < 60;
}

// ---------- button ----------
static void pollButton(uint32_t now) {
  static uint32_t downAt = 0;
  static bool fired = false;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !downAt) { downAt = now; fired = false; }
  if (!down) downAt = 0;
  if (down && !fired && now - downAt >= HOLD_MS) {
    fired = true;
    if (mySosActive) endMySos("button"); else startMySos("button");
  }
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(300);

  // Random start so a rebooted bracelet never reuses a sequence number its friend already acknowledged.
  mySosSeq = esp_random() & 0xFFFF;

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  if (MY_NAME[0]) strncpy(myName, MY_NAME, sizeof myName - 1);
  else snprintf(myName, sizeof myName, "Band %02X%02X", mac[4], mac[5]);
  char ssid[32];
  snprintf(ssid, sizeof ssid, "Crowdsource-%02X%02X", mac[4], mac[5]);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAP(ssid, AP_PASSWORD, WIFI_CHANNEL);

  rxQueue = xQueueCreate(16, sizeof(Rx));
  if (esp_now_init() != ESP_OK) Serial.println("ESP-NOW init failed");
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;            // whatever channel the hotspot is on
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

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

  Serial.printf("\nCrowdsource bracelet \"%s\"\n", myName);
  Serial.printf("Join Wi-Fi \"%s\", then open http://%s\n", ssid, WiFi.softAPIP().toString().c_str());
  Serial.println("Serial: p = re-pair  s = start/end SOS  a = acknowledge friend SOS");
}

void loop() {
  static uint32_t lastBeacon = 0, lastStatus = 0, lastLog = 0, lastClean = 0;
  uint32_t now = millis();

  Rx rx;
  while (xQueueReceive(rxQueue, &rx, 0) == pdTRUE) handleBeacon(rx, now);

  if (now - lastBeacon >= BEACON_MS) { lastBeacon = now; sendBeacon(); }
  if (now - lastStatus >= STATUS_MS) { lastStatus = now; sendAll(statusJson()); }
  if (now - lastClean >= 1000) { lastClean = now; ws.cleanupClients(); }

  pollButton(now);

  static bool wasHeard = false;
  bool heard = paired && now - friendLastAt < LOST_MS;
  if (heard != wasHeard) {
    wasHeard = heard;
    if (paired) emit(heard ? "found" : "lost");
  }
  digitalWrite(HAPTIC_PIN, hapticOn(now) ? HIGH : LOW);

  if (now - lastLog >= 2000) {
    lastLog = now;
    if (!paired) Serial.println("  searching for a friend bracelet...");
    else Serial.printf("  %s rssi avg=%.1f last heard %.1fs ago | sos me=%d friend=%d | phones=%u heap=%u\n",
                       friendName, rssiAvg, (now - friendLastAt) / 1000.0f, mySosActive, friendSosPending,
                       ws.count(), ESP.getFreeHeap());
  }

  while (Serial.available()) {
    switch (Serial.read()) {
      case 'p':
        paired = false;
        rssiAvg = NAN;
        friendSosOn = friendSosPending = false;
        Serial.println("~ forgot friend, pairing again");
        break;
      case 's':   // same as holding the button
        if (mySosActive) endMySos("serial"); else startMySos("serial");
        break;
      case 'a':   // same as the phone's Acknowledge button
        ackFriendSos("serial");
        break;
    }
  }
}
