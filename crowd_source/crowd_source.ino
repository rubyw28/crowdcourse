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
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
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
// LCD: SDA = 21, SCL = 22 (I2C address is auto-detected)
#define LCD_COLS 16
#define LCD_ROWS 2

// Buzzer type:
//   0 = ACTIVE buzzer  (beeps by itself when powered — one fixed pitch)
//   1 = PASSIVE buzzer (needs a tone signal — different pitches per alert)
// Not sure? Apply 3.3 V directly: if it beeps, it's active.
#define BUZZER_PASSIVE 1

// ============================ Radio ==================================
#define WIFI_CHANNEL  1
// Full power for real use. For a small demo room, try WIFI_POWER_2dBm
// or WIFI_POWER_MINUS_1dBm so walking a few meters changes the color.
#define TX_POWER      WIFI_POWER_19_5dBm // change to 2dBm for small judging room
#define BROADCAST_MS  100   // ~10 packets/s

// ===================== RSSI tuning (CALIBRATE!) ======================
// Open Serial Monitor @115200, stand at known distances, adjust these.
#define RSSI_CLOSE  -55     // stronger than this = CLOSE
#define RSSI_NEAR   -70     // stronger than this = NEAR, else FAR
#define HYST        3       // dB of hysteresis so zones don't flicker
#define HEAT_HOT    -45     // RSSI where the strip is fully red
#define HEAT_COLD   -90     // RSSI where the strip is fully blue
#define EMA_ALPHA   0.2f    // RSSI smoothing: lower = smoother but slower to react.
  // weight given to each new RSSI sample in moving average. 0.2: average reflects last 5 samples
#define COLOR_GLIDE 0.08f   // LED color easing per frame: lower = slower fade
  // runs 50 times a second, 0.08 gives fade time of 1/4 second
#define LOST_TIMEOUT_MS 4000 // no packets this long = LOST

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

// packet sent over the air, 8 bytes total
typedef struct __attribute__((packed)) { // __attribute__((packed)) tells computer not to insert padding bytes btwn fields
  uint16_t magic;
  uint8_t  id;
  uint8_t  flags;
  uint32_t seq; // counter that goes up with every packet, not used right now, but could measure packet loss
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
};
RadioData radio[NUM_DEVICES]; // global array, starts out all zeros
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; 
// receive callback doesn't run inside loop() but in the ESP32's Wi-Fi task
// which can be on the other CPU core at the same moment. Without protection,
// loop() could read a half-updated record. portMUX_TYPE is a spinlock
// code between portENTER_CRITICAL(&mux) and portEXIT_CRITICAL(&mux) can't be
// interrupted so toher core waits until lock is released

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

hd44780_I2Cexp lcd;
bool lcdOK = true;

// Custom LCD characters 1-4: signal bars of increasing height (5x8 pixels)
const uint8_t BAR_GLYPHS[4][8] = {
  {0, 0, 0, 0, 0, 0, 0b01110, 0b01110},                          // 1 bar
  {0, 0, 0, 0, 0b01110, 0b01110, 0b01110, 0b01110},              // 2 bars
  {0, 0, 0b01110, 0b01110, 0b01110, 0b01110, 0b01110, 0b01110},  // 3 bars
  {0b01110, 0b01110, 0b01110, 0b01110, 0b01110, 0b01110, 0b01110, 0b01110}, // 4
};

// broadcast MAC address, packet sent to all-FF reaches every
// ESP-NOW device on the channel
uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int      selected = (MY_ID == 0) ? 1 : 0;   // friend currently being tracked
uint32_t selectFlashUntil = 0; // time at which signature color flash ends
bool     myLighthouse = false;
uint32_t lighthouseStart = 0;
bool     muted = false;
uint32_t muteStart = 0;

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
  portEXIT_CRITICAL(&mux);
}

void broadcast() {
  static uint32_t seq = 0; // keeps value between calls
  Packet p = {MAGIC, MY_ID, (uint8_t)(myLighthouse ? FLAG_LIGHTHOUSE : 0), seq++};
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
float heat(float r) {
  return constrain((r - HEAT_COLD) / float(HEAT_HOT - HEAT_COLD), 0.0f, 1.0f);
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
  // any press moves to enxt friend
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
    myLighthouse = !myLighthouse;
    lighthouseStart = now;
    buzz(1, 200, 0, TONE_UI);
    Serial.println(myLighthouse ? "Lighthouse ON" : "Lighthouse OFF");
  }
}

// ======================= Friend logic ================================

// copies radio data from loop(), recording every friend excpet you
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
      buzz(3, 300, 200, TONE_LIGHTHOUSE); // play three long beeps
      selectFriend(i, now); // automatically switch to tracking that friend
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
//
//  Layout (16 x 2):
//    Row 0:  >SAM     [b] CLOSE M      selected friend, bars, zone, flag
//    Row 1:  JORDAN:NEAR -62dB         other friends / mode messages
//
//  Flag in the last column of row 0:  B = your beacon on, M = muted.
//  Custom characters 1-4 are the signal-bar glyphs (char 0 is avoided
//  because it would end a C string).

char lcdShown[LCD_ROWS][LCD_COLS + 1];   // what's currently on the screen

// Pad/truncate to exactly 16 chars and only send it if it changed.
// Rewriting unchanged text is what makes character LCDs flicker,
// and every character costs I2C time through the PCF8574 backpack.
void lcdWriteRow(int row, const char* text) {
  char buf[LCD_COLS + 1];
  int n = strlen(text);
  for (int c = 0; c < LCD_COLS; c++) buf[c] = (c < n) ? text[c] : ' ';
  buf[LCD_COLS] = '\0';
  if (memcmp(buf, lcdShown[row], LCD_COLS) == 0) return;
  memcpy(lcdShown[row], buf, LCD_COLS + 1);
  lcd.setCursor(0, row);
  for (int c = 0; c < LCD_COLS; c++) lcd.write((uint8_t)buf[c]);
}

void drawLcd(const RadioData* snap, uint32_t now) {
  const FriendState& s = state[selected];
  const RadioData& r = snap[selected];
  char row0[32], row1[48], status[16];

  // ---- Row 0: selected friend ----
  if (s.lhActive) {
    snprintf(status, sizeof(status), "FIND ME");
  } else if (s.zone == Z_LOST) {
    if (!r.everSeen) {
      snprintf(status, sizeof(status), "unseen");
    } else {
      unsigned long secs = (now - r.lastSeen) / 1000;
      if (secs > 99) snprintf(status, sizeof(status), "lost99+");
      else           snprintf(status, sizeof(status), "lost%lus", secs);
    }
  } else {
    char bar = (char)(1 + (int)(heat(r.rssi) * 3.99f));   // glyph 1..4
    snprintf(status, sizeof(status), "%c %s", bar, ZONE_NAMES[s.zone]);
  }
  char flag = myLighthouse ? 'B' : (muted ? 'M' : ' ');
  // ">" + name in 7 cols, status in 7 cols, space, flag = 16
  snprintf(row0, sizeof(row0), ">%-7.7s%-7.7s%c", NAMES[selected], status, flag);
  lcdWriteRow(0, row0);

  // ---- Row 1: mode message, or the other friends + selected RSSI ----
  if (myLighthouse) {
    unsigned long left = (LIGHTHOUSE_MS - (now - lighthouseStart)) / 1000;
    snprintf(row1, sizeof(row1), "BEACON ON  %lus", left);
  } else {
    int pos = 0;
    row1[0] = '\0';
    for (int i = 0; i < NUM_DEVICES; i++) {
      if (i == MY_ID || i == selected) continue;
      pos += snprintf(row1 + pos, sizeof(row1) - pos, "%s:%s ",
                      NAMES[i], state[i].lhActive ? "HELP" : ZONE_NAMES[state[i].zone]);
    }
    // Raw dBm of the selected friend, right-aligned, if there's room (handy for calibrating)
    if (s.zone != Z_LOST && pos <= LCD_COLS - 6) {
      char dbm[8];
      snprintf(dbm, sizeof(dbm), "%ddB", (int)r.rssi);
      int col = LCD_COLS - strlen(dbm);
      while (pos < col) row1[pos++] = ' ';
      snprintf(row1 + pos, sizeof(row1) - pos, "%s", dbm);
    }
  }
  lcdWriteRow(1, row1);
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

  strip.begin(); // initialize LED driver
  strip.setBrightness(NORMAL_BRIGHTNESS);
  strip.fill(sigColor(MY_ID));   // boot flash in my own color
  strip.show();

  Wire.begin();                       // SDA 21, SCL 22
  Wire.setClock(100000);              // PCF8574 backpacks are rated for 100 kHz
  int lcdStatus = lcd.begin(LCD_COLS, LCD_ROWS);   // auto-detects address + pin map
  if (lcdStatus != 0) {
    Serial.printf("LCD not found (status %d) — continuing without display\n", lcdStatus);
    lcdOK = false;
  } else {
    for (int i = 0; i < 4; i++) lcd.createChar(i + 1, (uint8_t*)BAR_GLYPHS[i]);
    lcd.backlight();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Lighthouse");
    lcd.setCursor(0, 1);
    lcd.print("I am ");
    lcd.print(NAMES[MY_ID]);
    memset(lcdShown, 0, sizeof(lcdShown));   // force first full redraw
  }

  WiFi.mode(WIFI_STA); // station mode switches radio on for ESP-NOW
  WiFi.disconnect(); // makes sure board isn't trying to join saved network
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE); // fixes channel, 20 MHz width
  WiFi.setTxPower(TX_POWER); // set transmit power

  if (esp_now_init() != ESP_OK) { // starts ESP-NOW
    Serial.println("ESP-NOW init failed");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onRecv); // call onRecv for every incoming packet

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
  static uint32_t nextBroadcast = 0, lastLed = 0, lastLcd = 0, lastLog = 0;
  uint32_t now = millis();

  // handles button presses, switches Lighthouse mode off after 60s and mute off after 2 min
  updateButtons(now);
  if (myLighthouse && now - lighthouseStart > LIGHTHOUSE_MS) myLighthouse = false;
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

  // zomes, Lighthouse, and tether alerts are updated, buzzer pattern advances
  updateFriends(snap, now);
  updateBuzzer(now);

// rate-limited tasks: LEDs at 50 Hz, Serial log at 2 Hz
  if (now - lastLed >= 20)            { drawLeds(snap, now); lastLed = now; }
  if (lcdOK && now - lastLcd >= 250)  { drawLcd(snap, now); lastLcd = now; }
  if (now - lastLog >= 500)           { logSerial(snap, now); lastLog = now; }
}

// Notes:
// since buzz() replaces any pattern already playing, if a tether alert and Lighthouse alert
// fire at the same time, you only hear one of them
// long press only register when you release button
// unused seq field
// while the screen refreshes, the loops pauses for about 20 ms, so don't move OLED refresh
// into 50 Hz LED block
