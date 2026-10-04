// =====================================================================
//  Features:
//    1. Whole strip glows ONE color for the selected friend, gliding
//       smoothly from blue (far) to red (close)
//    2. SELECT button cycles which friend you're tracking; the strip
//       flashes that friend's signature color for 1 s to confirm
//    3. Tether alert: buzzer beeps if ANY friend stays FAR/LOST too long
//    4. LCD (16x2): who you're tracking, their signal bars/zone, others' status
//    5. Lighthouse mode: your strip strobes white; friends' strips strobe
//       in YOUR color, they get beeped, and they auto-select you
//    6. Phone page: join Wi-Fi "CrowdCourse-<NAME>" and open http://192.168.4.1
//       - shows the selected friend's closeness score (index.html)
//       - the page's SOS = Lighthouse; a friend's Lighthouse shows as their SOS,
//         and acknowledging it on the phone tells them help is coming
//       - calibrating on the page also retunes the LED colors and LCD bars
//
//  Controls:
//    SELECT button  short press -> next friend
//    BEACON button  short press -> toggle Lighthouse (auto-off after 60 s,
//                                  unless it was started from the phone)
//                   long press  -> toggle mute for tether alerts (2 min)
//
//  Board:     ESP32 Dev Module
//  Core:      Arduino-ESP32 3.x  (Boards Manager -> "esp32" by Espressif)
//  Libraries: Adafruit NeoPixel, LiquidCrystal I2C (Frank de Brabander),
//             ESP Async WebServer + Async TCP (both by ESP32Async)
//  Regenerate index_html.h after editing index.html: python3 crowdcourse/embed_html.py
// =====================================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "index_html.h"

// ======================= CHANGE PER BOARD ============================
#ifndef MY_ID
#define MY_ID 0          // 0, 1 or 2 — must be unique on each bracelet (or pass -DMY_ID=1 when compiling)
#endif

// ================= Team config (same on every board) =================
#define NUM_DEVICES 3
const char* NAMES[NUM_DEVICES] = {"Ruby", "Kate", "Svet"};   // max ~8 chars
const uint8_t SIG_COLOR[NUM_DEVICES][3] = {
  {0,   255, 80},   // 0: mint
  {255, 0,   200},  // 1: pink
  {255, 150, 0},    // 2: amber
};

// ============================ Pins ===================================
#define LED_PIN        26
#define NUM_LEDS       12
#define BUZZER_PIN     14
#define BEACON_BTN_PIN 13    // button between this pin and GND
#define SELECT_BTN_PIN 12    // button between this pin and GND
#define LCD_ADDR       0x27  // 16x2 I2C LCD; power it from VIN (5 V)
#define LCD_SDA        32
#define LCD_SCL        33

// Buzzer type:
//   0 = ACTIVE buzzer  (beeps by itself when powered — one fixed pitch)
//   1 = PASSIVE buzzer (needs a tone signal — different pitches per alert)
// Not sure? Apply 3.3 V directly: if it beeps, it's active.
#define BUZZER_PASSIVE 1

// ============================ Radio ==================================
#define WIFI_CHANNEL  1     // ESP-NOW and the phone hotspot share this channel
#define AP_PASSWORD   ""    // open hotspot; 8+ chars to enable WPA2
// Full power for real use. For a small demo room, try WIFI_POWER_2dBm
// or WIFI_POWER_MINUS_1dBm so walking a few meters changes the color.
#define TX_POWER      WIFI_POWER_8_5dBm // middle ground: 2dBm drops out within a few meters, 19.5dBm reads "close" across a room
#define BROADCAST_MS  100   // ~10 packets/s

// ===================== RSSI tuning (CALIBRATE!) ======================
// Open Serial Monitor @115200, stand at known distances, adjust these.
#define RSSI_CLOSE  -70     // stronger than this = CLOSE
#define RSSI_NEAR   -75     // stronger than this = NEAR, else FAR
#define HYST        2       // dB of hysteresis so zones don't flicker
#define HEAT_HOT    -65     // RSSI where the strip is fully red
#define HEAT_COLD   -80     // RSSI where the strip is fully blue
#define EMA_ALPHA   0.1f    // RSSI smoothing: lower = smoother but slower to react.
  // weight given to each new RSSI sample in moving average. 0.1: average reflects last ~10 samples
#define COLOR_GLIDE 0.08f   // LED color easing per frame: lower = slower fade
  // runs 50 times a second, 0.08 gives fade time of 1/4 second
#define LOST_TIMEOUT_MS 4000 // no packets this long = LOST
#define RSSI_DEBUG  1       // 1 = print raw + filtered RSSI of every packet from the selected friend

// ============================ Alerts =================================
#define TETHER_MS        8000    // FAR/LOST this long before beeping
#define TETHER_REPEAT_MS 10000   // re-beep interval while still far
#define MUTE_MS          120000  // lasts 2 min
#define LIGHTHOUSE_MS    60000   // lighthouse switches itself off after 60s
#define LONG_PRESS_MS    1000    // a press of 1s or more is long press
#define SELECT_FLASH_MS  1000    // show friend's signature color after selecting
#define NORMAL_BRIGHTNESS     40   // keep low: battery + eyes
#define LIGHTHOUSE_BRIGHTNESS 100  // 12 LEDs white at 200 ≈ 0.3 A 

// Buzzer pitches (only used with a passive buzzer)
#define TONE_SELECT     3000
#define TONE_TETHER     1000
#define TONE_LIGHTHOUSE 2000
#define TONE_UI         1500

// =====================================================================

struct Button {
  uint8_t  pin;
  bool     prev;
  uint32_t downAt;
  uint32_t changeAt;
};

enum Press { NONE, SHORT_PRESS, LONG_PRESS };

#define MAGIC 0x4C48  // ASCII for "LH": ID stamped on every packet, 
#define FLAG_LIGHTHOUSE 0x01 // one bit in flags byte, room for 7 more flags such as SOS flag
#define NO_ACK 0xFF

// packet sent over the air, 9 bytes total
typedef struct __attribute__((packed)) { // __attribute__((packed)) tells computer not to insert padding bytes btwn fields
  uint16_t magic;
  uint8_t  id;
  uint8_t  flags;
  uint32_t seq; // counter that goes up with every packet, not used right now, but could measure packet loss
  uint8_t  ackFor; // ID of the friend whose Lighthouse this bracelet's phone acknowledged, or NO_ACK
} Packet;

enum Zone { Z_CLOSE = 0, Z_NEAR = 1, Z_FAR = 2, Z_LOST = 3 };
const char* ZONE_NAMES[] = {"CLOSE", "NEAR", "FAR", "LOST"};

// Written by the radio callback (different task) — guarded by mux
// holds what was heard over radio about each friend
struct RadioData {
  float    rssi; // smoothed signal strength
  bool     everSeen; // whether we've heard them at least once
  uint32_t lastSeen; // when the last packet arrived, as ms since boot
  bool     lighthouse; // whether lighthouse flag is set
  int8_t   rawRssi; // unsmoothed RSSI of the last packet (the phone page does its own smoothing)
  uint32_t count; // packets received, so loop() can tell when a new one arrived
  uint8_t  ackFor; // whose Lighthouse they acknowledged, or NO_ACK
};
RadioData radio[NUM_DEVICES]; // global array, starts out all zeros
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; 
// receive callback doesn't run inside loop() but in the ESP32's Wi-Fi task
// which can be on the other CPU core at the same moment. Without protection,
// loop() could read a half-updated record. portMUX_TYPE is a spinlock
// code between portENTER_CRITICAL(&mux) and portEXIT_CRITICAL(&mux) can't be
// interrupted so the other core waits until lock is released

// Only touched by loop()
struct FriendState {
  Zone     zone = Z_LOST;
  uint32_t farSince = 0; // when friend first went FAR
  uint32_t lastTetherBuzz = 0; // when last tether alert for friend was sounded
  bool     lhActive = false; // is lighthouse active
  float    shownHeat = 0;     // eased value the LEDs actually display
};
FriendState state[NUM_DEVICES];

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
bool lcdOK = true;

// broadcast MAC address, packet sent to all-FF reaches every
// ESP-NOW device on the channel
uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int      selected = (MY_ID == 0) ? 1 : 0;   // friend currently being tracked
uint32_t selectFlashUntil = 0; // time at which signature color flash ends
bool     myLighthouse = false;
uint32_t lighthouseStart = 0;
bool     muted = false;
uint32_t muteStart = 0;

// Phone page
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool     lighthouseFromPhone = false; // phone SOS: no 60 s auto-off (the page can't be told it ended)
bool     myLighthouseAcked = false;   // a friend's phone acknowledged my Lighthouse
int      pageSosFor = -1;             // friend whose Lighthouse the page is showing as an SOS
int      ackedFor = -1;               // friend whose Lighthouse my phone acknowledged
float    calNear = HEAT_HOT, calFar = HEAT_COLD;   // RSSI for fully close / fully far; set from the page

// ========================= USB telemetry =============================
// One JSON object per line, prefixed with '@', for the laptop bridge (bridge/bridge.js).
// Only call from loop() so lines from two tasks never interleave.
void telemetry(const char* ev, int friendId, const char* extra) {
  Serial.printf("@{\"ev\":\"%s\",\"me\":\"%s\",\"friend\":\"%s\"%s}\n",
                ev, NAMES[MY_ID], NAMES[friendId], extra);
}

// ============================ Radio ==================================

// called every time a packet arrives
// packet - magic, id, flags, seq
// radioData - rssi, everSeen, lastSeen, lighthouse
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != MAGIC || p.id >= NUM_DEVICES || p.id == MY_ID) return;

// rx_ctrl: radio's receive-control info for packet
  int rssi = info->rx_ctrl->rssi;
  uint32_t now = millis(); // return ms since boot

  portENTER_CRITICAL(&mux);
  RadioData& r = radio[p.id]; // r is a reference, changing r changes array entry
  if (!r.everSeen || now - r.lastSeen > LOST_TIMEOUT_MS) {
    r.rssi = rssi;                          // fresh start after being lost
  } else {
    r.rssi += EMA_ALPHA * (rssi - r.rssi);  // exponential moving average: new avg = old avg + 20% of diff btwn new sample & old avg
  }
  r.everSeen   = true;
  r.lastSeen   = now;
  r.lighthouse = p.flags & FLAG_LIGHTHOUSE;
  r.rawRssi    = rssi;
  r.count++;
  r.ackFor     = p.ackFor;
  portEXIT_CRITICAL(&mux);
}

void broadcast() {
  static uint32_t seq = 0; // keeps value between calls
  Packet p = {MAGIC, MY_ID, (uint8_t)(myLighthouse ? FLAG_LIGHTHOUSE : 0), seq++,
              (uint8_t)(ackedFor >= 0 ? ackedFor : NO_ACK)};
  esp_now_send(BCAST, (uint8_t*)&p, sizeof(p)); // sends 8 bytes to broadcast addr
}

// ============================ Zones ==================================

Zone rawZone(float r) {
  if (r >= RSSI_CLOSE) return Z_CLOSE;
  if (r >= RSSI_NEAR)  return Z_NEAR;
  return Z_FAR;
}

// Must beat a threshold by HYST dB to change zone
Zone nextZone(Zone cur, float r) {
  Zone closer  = rawZone(r - HYST);
  Zone farther = rawZone(r + HYST);
  if (closer < cur)  return closer;
  if (farther > cur) return farther;
  return cur;
}

// maps RSSI linearly onto range 0 to 1
// At -90: (-90 + 90)/45 = 0; at -45: 45/45 = 1
// constrain clamps anything outside this range
// 0.0 = cold/far, 1.0 = hot/close
// calFar/calNear start at HEAT_COLD/HEAT_HOT; calibrating on the phone replaces them
float heat(float r) {
  return constrain((r - calFar) / (calNear - calFar), 0.0f, 1.0f);
}

// packs 3 bytes into one 32-bit number: format the NeoPixel library uses for colors
uint32_t sigColor(int i) { // i is ID index (0,1,2)
  return strip.Color(SIG_COLOR[i][0], SIG_COLOR[i][1], SIG_COLOR[i][2]);
}

// ======================= Buzzer (non-blocking) =======================
// code keeps buzzer state in variables and checks it on every pass thru loop()

uint8_t  buzzPulsesLeft = 0; // how many beeps remain
uint16_t buzzOnMs = 0, buzzOffMs = 0, buzzFreq = 0; // on off durations, pitch
uint32_t buzzNext = 0; // time of next changeAt
bool     buzzerOn = false;

void buzzerWrite(bool on) {
#if BUZZER_PASSIVE
  if (on) tone(BUZZER_PIN, buzzFreq);
  else    noTone(BUZZER_PIN);
#else
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
#endif
  buzzerOn = on;
}

// stops any current sound, stores pattern, and schedules first change for now
void buzz(uint8_t pulses, uint16_t onMs, uint16_t offMs, uint16_t freq) {
  buzzerWrite(false);
  buzzPulsesLeft = pulses;
  buzzOnMs = onMs;
  buzzOffMs = offMs;
  buzzFreq = freq;
  buzzNext = millis();
}

void updateBuzzer(uint32_t now) {
  if (buzzPulsesLeft == 0 && !buzzerOn) return;
  if ((int32_t)(now - buzzNext) < 0) return;
  if (buzzerOn) {
    buzzerWrite(false);
    buzzNext = now + buzzOffMs;
  } else if (buzzPulsesLeft > 0) {
    buzzerWrite(true);
    buzzPulsesLeft--;
    buzzNext = now + buzzOnMs;
  }
}

// ============================ Buttons ================================

Button beaconBtn = {BEACON_BTN_PIN, HIGH, 0, 0};
Button selectBtn = {SELECT_BTN_PIN, HIGH, 0, 0};

Press readButton(Button& b, uint32_t now) {
  bool level = digitalRead(b.pin);
  if (level == b.prev || now - b.changeAt < 30) return NONE;  // 30 ms debounce
  b.changeAt = now;
  b.prev = level;
  if (level == LOW) { b.downAt = now; return NONE; }          // pressed
  return (now - b.downAt >= LONG_PRESS_MS) ? LONG_PRESS : SHORT_PRESS;  // released
}

// switches the tracked friend, starts 1-second color flash
void selectFriend(int i, uint32_t now) {
  selected = i;
  selectFlashUntil = now + SELECT_FLASH_MS;
  Serial.printf("Now tracking %s\n", NAMES[i]);
}

// steps to next ID (friend) then gives short 40 ms chirp
void selectNextFriend(uint32_t now) {
  int i = selected;
  do { i = (i + 1) % NUM_DEVICES; } while (i == MY_ID);
  selectFriend(i, now);
  buzz(1, 40, 0, TONE_SELECT);
}

void updateButtons(uint32_t now) {
  // any press moves to next friend
  if (readButton(selectBtn, now) != NONE) selectNextFriend(now);

  Press p = readButton(beaconBtn, now);
  // flips muted and records when; beeps once if muting or twice
  // or unmuting
  if (p == LONG_PRESS) {
    muted = !muted;
    muteStart = now;
    buzz(muted ? 1 : 2, 60, 80, TONE_UI);
    Serial.println(muted ? "Tether alerts muted" : "Tether alerts on");
  } else if (p == SHORT_PRESS) { // flips Lighthouse mode, records start time, gives a longer beep
    setLighthouse(!myLighthouse, false, now);
    buzz(1, 200, 0, TONE_UI);
  }
}

void setLighthouse(bool on, bool fromPhone, uint32_t now) {
  myLighthouse = on;
  lighthouseStart = now;
  lighthouseFromPhone = on && fromPhone;
  myLighthouseAcked = false;
  Serial.printf("Lighthouse %s%s\n", on ? "ON" : "OFF", fromPhone ? " (phone)" : "");
  telemetry(on ? "my_sos" : "my_sos_end", selected, fromPhone ? ",\"from\":\"phone\"" : ",\"from\":\"button\"");
}

// ======================= Friend logic ================================

// copies radio data from loop(), recording every friend except you
void updateFriends(const RadioData* snap, uint32_t now) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i == MY_ID) continue;
    FriendState& s = state[i];
    const RadioData& r = snap[i];
    bool heard = r.everSeen && (now - r.lastSeen < LOST_TIMEOUT_MS);

    // Zone
    if (heard != (s.zone != Z_LOST)) telemetry(heard ? "found" : "lost", i, "");
    if (!heard)                s.zone = Z_LOST;
    else if (s.zone == Z_LOST) s.zone = rawZone(r.rssi); 
    else                       s.zone = nextZone(s.zone, r.rssi);

    // Friend's lighthouse: beep + auto-select them on rising edge
    bool lh = heard && r.lighthouse;
    if (lh && !s.lhActive) {
      buzz(3, 300, 200, TONE_LIGHTHOUSE); // play three long beeps
      selectFriend(i, now); // automatically switch to tracking that friend
      telemetry("friend_sos", i, "");
    } else if (!lh && s.lhActive) {
      telemetry("friend_sos_end", i, "");
    }
    s.lhActive = lh;

    // Tether alert — any friend, not just the selected one
    if (r.everSeen && s.zone >= Z_FAR) { // checks that we've seen this friend at some point (so board that was never on doesn't trigger alarms)
      if (s.farSince == 0) s.farSince = now; // if timer isn't running, start timer now
      if (!muted && now - s.farSince > TETHER_MS && // if alerts aren't muted, friend has been far for more than 8 seconds
          now - s.lastTetherBuzz > TETHER_REPEAT_MS) { // we haven't alerted for them in the last 10 seconds: plays two beeps + record time
        buzz(2, 150, 150, TONE_TETHER);
        s.lastTetherBuzz = now;
      }
    } else {
      s.farSince = 0; // friend back in range: reset timer
    }
  }
}

// ============================ LEDs ===================================

void drawLeds(const RadioData* snap, uint32_t now) {
  // Ease the selected friend's displayed heat toward the real value (50 Hz)
  FriendState& s = state[selected];
  if (s.zone != Z_LOST) {
    s.shownHeat += COLOR_GLIDE * (heat(snap[selected].rssi) - s.shownHeat);
  }

  // 1. Lighthouse overrides everything
  int lhFriend = -1;
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i != MY_ID && state[i].lhActive) { lhFriend = i; break; }
  }
  if (myLighthouse || lhFriend >= 0) {
    bool on = (now % 300) < 80;   // fast strobe, 3.3 flashes per second
    uint32_t c = myLighthouse ? strip.Color(255, 255, 255) : sigColor(lhFriend);
    strip.setBrightness(LIGHTHOUSE_BRIGHTNESS);
    strip.fill(on ? c : 0);
    strip.show();
    return;
  }

  strip.setBrightness(NORMAL_BRIGHTNESS);

  // 2. Just switched friends: show their signature color
  if ((int32_t)(selectFlashUntil - now) > 0) {
    strip.fill(sigColor(selected));
  }
  // 3. Selected friend lost: slow blink of their signature color
  else if (s.zone == Z_LOST) {
    strip.fill((now % 1000) < 150 ? sigColor(selected) : 0);
  }
  // 4. Normal: whole strip one color, blue (far) -> red (close)
  else {
    uint16_t hue = (uint16_t)((1.0f - s.shownHeat) * 43690);   // 43690 = blue, 0 = red
    strip.fill(strip.gamma32(strip.ColorHSV(hue)));
  }
  strip.show();
}

// ============================ LCD ====================================

// Custom characters 1-4: signal bars of increasing height (0 would end a C string)
const uint8_t BAR_GLYPHS[4][8] = {
  {0, 0, 0, 0, 0, 0, 0, 31},
  {0, 0, 0, 0, 0, 31, 31, 31},
  {0, 0, 0, 31, 31, 31, 31, 31},
  {0, 31, 31, 31, 31, 31, 31, 31},
};

// Only rewrite a row when its text changed, so the LCD doesn't flicker
char lcdShown[2][17];

void lcdRow(int row, const char* text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);
  if (strcmp(buf, lcdShown[row]) == 0) return;
  strcpy(lcdShown[row], buf);
  lcd.setCursor(0, row);
  for (int i = 0; i < 16; i++) lcd.write((uint8_t)buf[i]);   // write() so bytes 1-4 map to bar glyphs
}

void drawLcd(const RadioData* snap, uint32_t now) {
  char top[17], bottom[17];
  const FriendState& s = state[selected];
  const RadioData& r = snap[selected];

  // Top row: selected friend and how close they are
  if (s.lhActive) {
    snprintf(top, sizeof(top), "%-6s FIND ME!", NAMES[selected]);
  } else if (s.zone == Z_LOST) {
    if (!r.everSeen) snprintf(top, sizeof(top), "%-6s not seen", NAMES[selected]);
    else             snprintf(top, sizeof(top), "%-6s lost %lus", NAMES[selected],
                              (unsigned long)((now - r.lastSeen) / 1000));
  } else {
    int bars = 1 + (int)(heat(r.rssi) * 3.99f);   // 1..4
    char b[5];
    for (int i = 0; i < 4; i++) b[i] = i < bars ? (char)(i + 1) : ' ';
    b[4] = 0;
    snprintf(top, sizeof(top), "%-6s %s %s", NAMES[selected], b, ZONE_NAMES[s.zone]);
  }

  // Bottom row: my mode, or everyone else's status
  if (myLighthouse) {
    snprintf(bottom, sizeof(bottom), myLighthouseAcked ? "HELP IS COMING" : "** BEACON ON **");
  } else {
    int n = 0;
    bottom[0] = 0;
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (i == MY_ID || i == selected) continue;
      n += snprintf(bottom + n, sizeof(bottom) - n, "%s:%s ", NAMES[i],
                    state[i].lhActive ? "HELP" : ZONE_NAMES[state[i].zone]);
      if (n >= (int)sizeof(bottom)) break;
    }
    if (muted) {
      bottom[11] = 0;
      strcat(bottom, " MUTE");
    }
  }

  lcdRow(0, top);
  lcdRow(1, bottom);
}

// ============================ Phone page =============================
// The page (index.html) knows one friend: whoever SELECT is tracking.
// WebSocket contract: README.md, "WebSocket contract".

void sendAll(const String& msg) {
  if (ws.count()) ws.textAll(msg);
}

String statusJson() {
  return String("{\"type\":\"status\",\"name\":\"") + NAMES[selected] + "\"}";
}

// Minimal JSON readers: the page only sends small flat objects
String jsonType(const String& json) {
  int k = json.indexOf("\"type\"");
  if (k < 0) return "";
  int q1 = json.indexOf('"', json.indexOf(':', k) + 1);
  int q2 = json.indexOf('"', q1 + 1);
  return (q1 < 0 || q2 < 0) ? "" : json.substring(q1 + 1, q2);
}

bool jsonInt(const String& json, const char* key, int& out) {
  int k = json.indexOf(String("\"") + key + "\"");
  if (k < 0) return false;
  int c = json.indexOf(':', k);
  if (c < 0) return false;
  out = json.substring(c + 1).toInt();
  return true;
}

// Messages arrive on the web server's task; they're queued and handled in loop()
struct PhoneMsg { char text[96]; };
QueueHandle_t phoneQueue;

void onPhoneMessage(const String& msg) {
  Serial.printf("< phone %s\n", msg.c_str());
  String type = jsonType(msg);
  uint32_t now = millis();
  if (type == "sos") {
    if (!myLighthouse) { setLighthouse(true, true, now); buzz(1, 200, 0, TONE_UI); }
  } else if (type == "sos_cancel") {
    if (myLighthouse) setLighthouse(false, true, now);
  } else if (type == "sos_ack") {
    if (pageSosFor >= 0) {
      ackedFor = pageSosFor;
      Serial.printf("Acknowledged %s's Lighthouse\n", NAMES[ackedFor]);
      telemetry("friend_sos_acked", ackedFor, "");
    }
  } else if (type == "calibrate") {
    int n, f;
    if (jsonInt(msg, "near", n) && jsonInt(msg, "far", f) && n - f >= 5) {
      calNear = n;
      calFar = f;
      Serial.printf("Calibrated from phone: near=%d far=%d\n", n, f);
      char extra[40];
      snprintf(extra, sizeof(extra), ",\"near\":%d,\"far\":%d", n, f);
      telemetry("calibrate", selected, extra);
    }
  }
}

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
               void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("+ phone #%u\n", client->id());
    client->text(statusJson());
    // a phone that (re)connects mid-alert still needs to see it
    if (pageSosFor >= 0 && ackedFor != pageSosFor) client->text("{\"type\":\"sos\"}");
    if (myLighthouse && myLighthouseAcked) client->text("{\"type\":\"sos_ack\"}");
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("- phone #%u\n", client->id());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      PhoneMsg m = {};
      memcpy(m.text, data, min(len, sizeof(m.text) - 1));
      xQueueSend(phoneQueue, &m, 0);
    }
  }
}

void updatePhone(const RadioData* snap, uint32_t now) {
  static uint32_t lastStatus = 0, sentCount = 0;
  static int shownFriend = -1;

  PhoneMsg m;
  while (xQueueReceive(phoneQueue, &m, 0) == pdTRUE) onPhoneMessage(m.text);

  // Friend changed: tell the page right away so the name updates
  if (selected != shownFriend || now - lastStatus >= 1000) {
    shownFriend = selected;
    lastStatus = now;
    sendAll(statusJson());
  }

  // Every new packet from the selected friend -> raw RSSI
  if (snap[selected].count != sentCount) {
    sentCount = snap[selected].count;
    sendAll(String("{\"type\":\"rssi\",\"rssi\":") + snap[selected].rawRssi + "}");
#if RSSI_DEBUG
    // raw = this one packet, filtered = smoothed average (what zones and LEDs use)
    Serial.printf("%s raw=%d filtered=%.1f\n", NAMES[selected], snap[selected].rawRssi, snap[selected].rssi);
#endif
  }

  // Friend's Lighthouse -> SOS on the page (updateFriends auto-selects them)
  if (pageSosFor < 0 && state[selected].lhActive && ackedFor != selected) {
    pageSosFor = selected;
    sendAll("{\"type\":\"sos\"}");
  } else if (pageSosFor >= 0 && !state[pageSosFor].lhActive) {
    if (ackedFor == pageSosFor) ackedFor = -1;
    pageSosFor = -1;
    sendAll("{\"type\":\"sos_clear\"}");
  }
  if (ackedFor >= 0 && !state[ackedFor].lhActive) ackedFor = -1;

  // A friend's phone acknowledged my Lighthouse
  if (myLighthouse && !myLighthouseAcked) {
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (i == MY_ID || state[i].zone == Z_LOST || snap[i].ackFor != MY_ID) continue;
      myLighthouseAcked = true;
      sendAll("{\"type\":\"sos_ack\"}");
      buzz(3, 80, 80, TONE_SELECT);
      Serial.printf("%s acknowledged your Lighthouse\n", NAMES[i]);
      telemetry("my_sos_acked", i, "");
      break;
    }
  }
}

// ========================= Serial calibration ========================

void logSerial(const RadioData* snap, uint32_t now) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i == MY_ID) continue;
    Serial.printf("%s%-8s rssi=%6.1f  zone=%-5s  age=%lums%s   ",
                  i == selected ? "*" : " ",
                  NAMES[i], snap[i].rssi, ZONE_NAMES[state[i].zone],
                  snap[i].everSeen ? (unsigned long)(now - snap[i].lastSeen) : 0UL,
                  state[i].lhActive ? " LH" : "");
  }
  Serial.println();
}

// latest RSSI of every friend heard since the last call -> bridge readings
void logTelemetry(const RadioData* snap) {
  static uint32_t lastCount[NUM_DEVICES] = {};
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i == MY_ID || snap[i].count == lastCount[i]) continue;
    lastCount[i] = snap[i].count;
    char extra[16];
    snprintf(extra, sizeof(extra), ",\"rssi\":%d", snap[i].rawRssi);
    telemetry("rssi", i, extra);
  }
}

// ============================ Setup / loop ===========================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nLighthouse bracelet — I am #%d (%s)\n", MY_ID, NAMES[MY_ID]);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BEACON_BTN_PIN, INPUT_PULLUP);
  pinMode(SELECT_BTN_PIN, INPUT_PULLUP);

  strip.begin(); // initialize LED driver
  strip.setBrightness(NORMAL_BRIGHTNESS);
  strip.fill(sigColor(MY_ID));   // boot flash in my own color
  strip.show();

  Wire.begin(LCD_SDA, LCD_SCL);
  Wire.beginTransmission(LCD_ADDR);
  lcdOK = Wire.endTransmission() == 0;
  if (lcdOK) {
    lcd.init();
    lcd.backlight();
    for (int i = 0; i < 4; i++) lcd.createChar(i + 1, (uint8_t*)BAR_GLYPHS[i]);
    lcd.clear();
    lcd.print("I am ");
    lcd.print(NAMES[MY_ID]);
  } else {
    Serial.println("LCD not found — continuing without display");
  }

  // AP+STA: the hotspot serves the phone page, and ESP-NOW rides on the hotspot's channel
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "CrowdCourse-%s", NAMES[MY_ID]);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false); // keeps ESP-NOW and the phone link responsive
  WiFi.softAP(ssid, AP_PASSWORD, WIFI_CHANNEL);
  WiFi.setTxPower(TX_POWER); // set transmit power

  if (esp_now_init() != ESP_OK) { // starts ESP-NOW
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv); // call onRecv for every incoming packet

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;            // whatever channel the hotspot is on
  peer.ifidx   = WIFI_IF_AP;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("Failed to add broadcast peer");

  phoneQueue = xQueueCreate(8, sizeof(PhoneMsg));
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(200, "text/html", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "not found"); });
  server.begin();
  Serial.printf("Phone: join Wi-Fi \"%s\", then open http://%s\n", ssid, WiFi.softAPIP().toString().c_str());

  delay(500);
  selectFriend(selected, millis());   // flash first friend's color on boot
  buzz(1, 100, 0, TONE_UI);           // "I'm alive"
}

void loop() {
  static uint32_t nextBroadcast = 0, lastLed = 0, lastLcd = 0, lastLog = 0, lastTele = 0, lastClean = 0;
  uint32_t now = millis();

  // handles button presses, switches Lighthouse mode off after 60s and mute off after 2 min
  updateButtons(now);
  if (myLighthouse && !lighthouseFromPhone && now - lighthouseStart > LIGHTHOUSE_MS) setLighthouse(false, false, now);
  if (muted && now - muteStart > MUTE_MS) muted = false;

  // sends a packet when it's due
  if ((int32_t)(now - nextBroadcast) >= 0) {
    broadcast();
    nextBroadcast = now + BROADCAST_MS + random(0, 20);   // jitter avoids collisions
  }

  // takes a snapshot
  RadioData snap[NUM_DEVICES];
  portENTER_CRITICAL(&mux);
  memcpy(snap, radio, sizeof(snap));
  portEXIT_CRITICAL(&mux);

  // zones, Lighthouse, and tether alerts are updated, buzzer pattern advances
  updateFriends(snap, now);
  updatePhone(snap, now);
  updateBuzzer(now);

// rate-limited tasks: LEDs at 50 Hz, Serial log at 2 Hz
  if (now - lastLed >= 20)            { drawLeds(snap, now); lastLed = now; }
  if (lcdOK && now - lastLcd >= 200)  { drawLcd(snap, now); lastLcd = now; }
  if (!RSSI_DEBUG && now - lastLog >= 500) { logSerial(snap, now); lastLog = now; }
  if (now - lastTele >= 500)          { logTelemetry(snap); lastTele = now; }
  if (now - lastClean >= 1000)      { ws.cleanupClients(); lastClean = now; }
}

// Notes:
// since buzz() replaces any pattern already playing, if a tether alert and Lighthouse alert
// fire at the same time, you only hear one of them
// long press only register when you release button
// unused seq field
// while the screen refreshes, the loops pauses for about 20 ms, so don't move LCD refresh
// into 50 Hz LED block
