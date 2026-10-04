// =====================================================================
//  Features:
//    1. Whole strip glows ONE color for the selected friend, gliding
//       smoothly from blue (far) to red (close)
//    2. SELECT button cycles which friend you're tracking; the strip
//       flashes that friend's signature color for 1 s to confirm
//    3. Tether alert: buzzer beeps if ANY friend stays FAR/LOST too long
//    4. OLED: who you're tracking, their signal bars/zone, others' status
//    5. Lighthouse mode: your strip strobes white; friends' strips strobe
//       in YOUR color, they get beeped, and they auto-select you
//
//  Controls:
//    SELECT button  short press -> next friend
//    BEACON button  short press -> toggle Lighthouse (auto-off after 60 s)
//                   long press  -> toggle mute for tether alerts (2 min)
//
//  Board:     ESP32 Dev Module
//  Core:      Arduino-ESP32 3.x  (Boards Manager -> "esp32" by Espressif)
//  Libraries: Adafruit NeoPixel, Adafruit SSD1306, Adafruit GFX
// =====================================================================

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// ======================= CHANGE PER BOARD ============================
#define MY_ID 0          // 0, 1 or 2 — must be unique on each bracelet

// ================= Team config (same on every board) =================
#define NUM_DEVICES 3
const char* NAMES[NUM_DEVICES] = {"ALEX", "SAM", "JORDAN"};   // max ~8 chars
const uint8_t SIG_COLOR[NUM_DEVICES][3] = {
  {0,   255, 80},   // 0: mint
  {255, 0,   200},  // 1: pink
  {255, 150, 0},    // 2: amber
};

// ============================ Pins ===================================
#define LED_PIN        13
#define NUM_LEDS       12
#define BUZZER_PIN     25
#define BEACON_BTN_PIN 0     // BOOT button on most dev boards; or a button to GND
#define SELECT_BTN_PIN 27    // button between this pin and GND
#define OLED_ADDR      0x3C  // SDA = 21, SCL = 22 by default

// Buzzer type:
//   0 = ACTIVE buzzer  (beeps by itself when powered — one fixed pitch)
//   1 = PASSIVE buzzer (needs a tone signal — different pitches per alert)
// Not sure? Apply 3.3 V directly: if it beeps, it's active.
#define BUZZER_PASSIVE 1

// ============================ Radio ==================================
#define WIFI_CHANNEL  1
// Full power for real use. For a small demo room, try WIFI_POWER_2dBm
// or WIFI_POWER_MINUS_1dBm so walking a few meters changes the color.
#define TX_POWER      WIFI_POWER_19_5dBm
#define BROADCAST_MS  100   // ~10 packets/s

// ===================== RSSI tuning (CALIBRATE!) ======================
// Open Serial Monitor @115200, stand at known distances, adjust these.
#define RSSI_CLOSE  -55     // stronger than this = CLOSE
#define RSSI_NEAR   -70     // stronger than this = NEAR, else FAR
#define HYST        3       // dB of hysteresis so zones don't flicker
#define HEAT_HOT    -45     // RSSI where the strip is fully red
#define HEAT_COLD   -90     // RSSI where the strip is fully blue
#define EMA_ALPHA   0.2f    // RSSI smoothing: lower = smoother but slower
#define COLOR_GLIDE 0.08f   // LED color easing per frame: lower = slower fade
#define LOST_TIMEOUT_MS 4000 // no packets this long = LOST

// ============================ Alerts =================================
#define TETHER_MS        8000    // FAR/LOST this long before beeping
#define TETHER_REPEAT_MS 10000   // re-beep interval while still far
#define MUTE_MS          120000
#define LIGHTHOUSE_MS    60000
#define LONG_PRESS_MS    1000
#define SELECT_FLASH_MS  1000    // show friend's signature color after selecting
#define NORMAL_BRIGHTNESS     40   // keep low: battery + eyes
#define LIGHTHOUSE_BRIGHTNESS 200  // 12 LEDs white at 200 ≈ 0.6 A — use a power bank

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

#define MAGIC 0x4C48            // "LH"
#define FLAG_LIGHTHOUSE 0x01

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t  id;
  uint8_t  flags;
  uint32_t seq;
} Packet;

enum Zone { Z_CLOSE = 0, Z_NEAR = 1, Z_FAR = 2, Z_LOST = 3 };
const char* ZONE_NAMES[] = {"CLOSE", "NEAR", "FAR", "LOST"};

// Written by the radio callback (different task) — guarded by mux
struct RadioData {
  float    rssi;
  bool     everSeen;
  uint32_t lastSeen;
  bool     lighthouse;
};
RadioData radio[NUM_DEVICES];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Only touched by loop()
struct FriendState {
  Zone     zone = Z_LOST;
  uint32_t farSince = 0;
  uint32_t lastTetherBuzz = 0;
  bool     lhActive = false;
  float    shownHeat = 0;     // eased value the LEDs actually display
};
FriendState state[NUM_DEVICES];

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOK = true;

uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int      selected = (MY_ID == 0) ? 1 : 0;   // friend currently being tracked
uint32_t selectFlashUntil = 0;
bool     myLighthouse = false;
uint32_t lighthouseStart = 0;
bool     muted = false;
uint32_t muteStart = 0;

// ============================ Radio ==================================

void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != MAGIC || p.id >= NUM_DEVICES || p.id == MY_ID) return;

  int rssi = info->rx_ctrl->rssi;
  uint32_t now = millis();

  portENTER_CRITICAL(&mux);
  RadioData& r = radio[p.id];
  if (!r.everSeen || now - r.lastSeen > LOST_TIMEOUT_MS) {
    r.rssi = rssi;                          // fresh start after being lost
  } else {
    r.rssi += EMA_ALPHA * (rssi - r.rssi);  // exponential moving average
  }
  r.everSeen   = true;
  r.lastSeen   = now;
  r.lighthouse = p.flags & FLAG_LIGHTHOUSE;
  portEXIT_CRITICAL(&mux);
}

void broadcast() {
  static uint32_t seq = 0;
  Packet p = {MAGIC, MY_ID, (uint8_t)(myLighthouse ? FLAG_LIGHTHOUSE : 0), seq++};
  esp_now_send(BCAST, (uint8_t*)&p, sizeof(p));
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

// 0.0 = cold/far, 1.0 = hot/close
float heat(float r) {
  return constrain((r - HEAT_COLD) / float(HEAT_HOT - HEAT_COLD), 0.0f, 1.0f);
}

uint32_t sigColor(int i) {
  return strip.Color(SIG_COLOR[i][0], SIG_COLOR[i][1], SIG_COLOR[i][2]);
}

// ======================= Buzzer (non-blocking) =======================

uint8_t  buzzPulsesLeft = 0;
uint16_t buzzOnMs = 0, buzzOffMs = 0, buzzFreq = 0;
uint32_t buzzNext = 0;
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

void selectFriend(int i, uint32_t now) {
  selected = i;
  selectFlashUntil = now + SELECT_FLASH_MS;
  Serial.printf("Now tracking %s\n", NAMES[i]);
}

void selectNextFriend(uint32_t now) {
  int i = selected;
  do { i = (i + 1) % NUM_DEVICES; } while (i == MY_ID);
  selectFriend(i, now);
  buzz(1, 40, 0, TONE_SELECT);
}

void updateButtons(uint32_t now) {
  if (readButton(selectBtn, now) != NONE) selectNextFriend(now);

  Press p = readButton(beaconBtn, now);
  if (p == LONG_PRESS) {
    muted = !muted;
    muteStart = now;
    buzz(muted ? 1 : 2, 60, 80, TONE_UI);
    Serial.println(muted ? "Tether alerts muted" : "Tether alerts on");
  } else if (p == SHORT_PRESS) {
    myLighthouse = !myLighthouse;
    lighthouseStart = now;
    buzz(1, 200, 0, TONE_UI);
    Serial.println(myLighthouse ? "Lighthouse ON" : "Lighthouse OFF");
  }
}

// ======================= Friend logic ================================

void updateFriends(const RadioData* snap, uint32_t now) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i == MY_ID) continue;
    FriendState& s = state[i];
    const RadioData& r = snap[i];
    bool heard = r.everSeen && (now - r.lastSeen < LOST_TIMEOUT_MS);

    // Zone
    if (!heard)                s.zone = Z_LOST;
    else if (s.zone == Z_LOST) s.zone = rawZone(r.rssi);
    else                       s.zone = nextZone(s.zone, r.rssi);

    // Friend's lighthouse: beep + auto-select them on rising edge
    bool lh = heard && r.lighthouse;
    if (lh && !s.lhActive) {
      buzz(3, 300, 200, TONE_LIGHTHOUSE);
      selectFriend(i, now);
    }
    s.lhActive = lh;

    // Tether alert — any friend, not just the selected one
    if (r.everSeen && s.zone >= Z_FAR) {
      if (s.farSince == 0) s.farSince = now;
      if (!muted && now - s.farSince > TETHER_MS &&
          now - s.lastTetherBuzz > TETHER_REPEAT_MS) {
        buzz(2, 150, 150, TONE_TETHER);
        s.lastTetherBuzz = now;
      }
    } else {
      s.farSince = 0;
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
    bool on = (now % 300) < 80;   // fast strobe
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

// ============================ OLED ===================================

void drawBars(int x, int y, int bars) {
  for (int b = 0; b < 4; b++) {
    int h = 2 + b * 2;
    int bx = x + b * 6;
    if (b < bars) display.fillRect(bx, y + 8 - h, 4, h, SSD1306_WHITE);
    else          display.drawRect(bx, y + 8 - h, 4, h, SSD1306_WHITE);
  }
}

void drawOled(const RadioData* snap, uint32_t now) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Me: ");
  display.print(NAMES[MY_ID]);
  if (myLighthouse)  display.print(" [BEACON]");
  else if (muted)    display.print(" [MUTE]");

  // Selected friend, big
  display.setCursor(0, 12);
  display.print("Tracking:");
  display.setTextSize(2);
  display.setCursor(0, 22);
  display.print(NAMES[selected]);

  // Selected friend's status
  display.setTextSize(1);
  const FriendState& s = state[selected];
  const RadioData& r = snap[selected];
  display.setCursor(0, 42);
  if (s.lhActive) {
    display.print("FIND ME!");
  } else if (s.zone == Z_LOST) {
    if (!r.everSeen) {
      display.print("not seen yet");
    } else {
      display.print("lost ");
      display.print((now - r.lastSeen) / 1000);
      display.print("s ago");
    }
  } else {
    drawBars(0, 42, 1 + (int)(heat(r.rssi) * 3.99f));   // 1..4 bars
    display.setCursor(30, 42);
    display.print(ZONE_NAMES[s.zone]);
    display.print(" ");
    display.print((int)r.rssi);
    display.print("dBm");
  }

  // Everyone else, compact
  display.setCursor(0, 55);
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (i == MY_ID || i == selected) continue;
    display.print(NAMES[i]);
    display.print(":");
    display.print(state[i].lhActive ? "HELP" : ZONE_NAMES[state[i].zone]);
    display.print(" ");
  }
  display.display();
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

// ============================ Setup / loop ===========================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nLighthouse bracelet — I am #%d (%s)\n", MY_ID, NAMES[MY_ID]);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BEACON_BTN_PIN, INPUT_PULLUP);
  pinMode(SELECT_BTN_PIN, INPUT_PULLUP);

  strip.begin();
  strip.setBrightness(NORMAL_BRIGHTNESS);
  strip.fill(sigColor(MY_ID));   // boot flash in my own color
  strip.show();

  Wire.begin();
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found — continuing without display");
    oledOK = false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setTxPower(TX_POWER);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = WIFI_CHANNEL;
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) Serial.println("Failed to add broadcast peer");

  delay(500);
  selectFriend(selected, millis());   // flash first friend's color on boot
  buzz(1, 100, 0, TONE_UI);           // "I'm alive"
}

void loop() {
  static uint32_t nextBroadcast = 0, lastLed = 0, lastOled = 0, lastLog = 0;
  uint32_t now = millis();

  updateButtons(now);
  if (myLighthouse && now - lighthouseStart > LIGHTHOUSE_MS) myLighthouse = false;
  if (muted && now - muteStart > MUTE_MS) muted = false;

  if ((int32_t)(now - nextBroadcast) >= 0) {
    broadcast();
    nextBroadcast = now + BROADCAST_MS + random(0, 20);   // jitter avoids collisions
  }

  RadioData snap[NUM_DEVICES];
  portENTER_CRITICAL(&mux);
  memcpy(snap, radio, sizeof(snap));
  portEXIT_CRITICAL(&mux);

  updateFriends(snap, now);
  updateBuzzer(now);

  if (now - lastLed >= 20)            { drawLeds(snap, now); lastLed = now; }
  if (oledOK && now - lastOled >= 200){ drawOled(snap, now); lastOled = now; }
  if (now - lastLog >= 500)           { logSerial(snap, now); lastLog = now; }
}
