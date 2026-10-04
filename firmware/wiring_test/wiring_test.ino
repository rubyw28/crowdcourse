// Wiring test: OLED (I2C), buttons on 12/13, passive buzzer on 14.
// Open Serial Monitor @115200. Report repeats every 5 s.

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define BTN_A   12
#define BTN_B   13
#define BUZZER  14

// Pins to try for the OLED if it is not on 32/33
const int CANDIDATES[] = {32, 33, 21, 22, 4, 5, 14, 15, 16, 17, 18, 19, 23, 26, 27};
const int NC = sizeof(CANDIDATES) / sizeof(int);

Adafruit_SSD1306 display(128, 64, &Wire, -1);
int oledSda = -1, oledScl = -1, oledAddr = 0;

bool probe(int sda, int scl, uint8_t addr) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setTimeOut(20);
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void reportIdle() {
  // A powered OLED module has pull-ups, so its SDA/SCL idle HIGH with no internal pull-up
  pinMode(32, INPUT_PULLDOWN); pinMode(33, INPUT_PULLDOWN);
  delay(5);
  Serial.printf("Idle level (internal pull-DOWN on): GPIO32=%d GPIO33=%d  -> %s\n",
                digitalRead(32), digitalRead(33),
                (digitalRead(32) && digitalRead(33)) ? "HIGH = OLED pull-ups present, so it has power and is wired to these pins"
                                                     : "LOW = OLED unpowered or not connected to that pin");
}

void findOled() {
  reportIdle();

  for (int i = 0; i < NC; i++)
    for (int j = 0; j < NC; j++) {
      if (i == j) continue;
      for (uint8_t a : {0x3C, 0x3D})
        if (probe(CANDIDATES[i], CANDIDATES[j], a)) {
          oledSda = CANDIDATES[i]; oledScl = CANDIDATES[j]; oledAddr = a;
          return;
        }
    }
}

void beep(int freq, int ms) { tone(BUZZER, freq, ms); }

void setup() {
  Serial.begin(115200);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  beep(1500, 150);

  findOled();
  if (oledSda >= 0) {
    Wire.end();
    Wire.begin(oledSda, oledScl);
    display.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 0);
    display.println("OLED OK");
    display.setTextSize(1);
    display.printf("SDA=%d SCL=%d 0x%02X\n", oledSda, oledScl, oledAddr);
    display.display();
  }
}

void loop() {
  static bool lastA = HIGH, lastB = HIGH;
  static uint32_t lastReport = 0;
  bool a = digitalRead(BTN_A), b = digitalRead(BTN_B);

  if (a != lastA) { Serial.printf("Button GPIO12 %s\n", a ? "released" : "PRESSED"); if (!a) beep(2000, 80); }
  if (b != lastB) { Serial.printf("Button GPIO13 %s\n", b ? "released" : "PRESSED"); if (!b) beep(3000, 80); }
  lastA = a; lastB = b;

  if (millis() - lastReport > 5000) {
    lastReport = millis();
    if (oledSda >= 0) Serial.printf("OLED FOUND at SDA=%d SCL=%d addr 0x%02X\n", oledSda, oledScl, oledAddr);
    else {
      Serial.println("OLED NOT FOUND on any pin pair (checked 0x3C/0x3D)");
      Wire.end();
      reportIdle();
      // Full address scan, both pin orders (an I2C LCD sits at 0x27 or 0x3F)
      for (int k = 0; k < 2; k++) {
        int sda = k ? 32 : 33, scl = k ? 33 : 32;
        Wire.end(); Wire.begin(sda, scl); Wire.setTimeOut(20);
        Serial.printf("Full scan SDA=%d SCL=%d:", sda, scl);
        for (uint8_t a = 1; a < 127; a++) {
          Wire.beginTransmission(a);
          if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
        }
        Serial.println();
      }
    }
    Serial.printf("Buttons now: GPIO12=%s GPIO13=%s\n", a ? "up" : "DOWN", b ? "up" : "DOWN");
  }

  if (oledSda >= 0) {
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 100) {
      lastDraw = millis();
      display.fillRect(0, 40, 128, 24, SSD1306_BLACK);
      display.setCursor(0, 40);
      display.printf("D12: %s\nD13: %s", a ? "up" : "PRESSED", b ? "up" : "PRESSED");
      display.display();
    }
  }
  delay(10);
}
