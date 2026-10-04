// LCD + buttons + buzzer check. 16x2 I2C LCD at 0x27, SDA=32, SCL=33.
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define BTN_A  12
#define BTN_B  13
#define BUZZER 25

LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  Serial.begin(115200);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  Wire.begin(32, 33);
  lcd.init();
  lcd.backlight();
  lcd.print("LCD OK!");
  tone(BUZZER, 1500, 150);
}

void loop() {
  static bool lastA = HIGH, lastB = HIGH;
  bool a = digitalRead(BTN_A), b = digitalRead(BTN_B);
  if (a != lastA || b != lastB) {
    if (!a && lastA) tone(BUZZER, 2000, 80);
    if (!b && lastB) tone(BUZZER, 3000, 80);
    Serial.printf("D12=%s D13=%s\n", a ? "up" : "DOWN", b ? "up" : "DOWN");
  }
  lastA = a; lastB = b;
  lcd.setCursor(0, 1);
  lcd.printf("D12:%-4s D13:%-4s", a ? "up" : "DOWN", b ? "up" : "DOWN");
  delay(50);
}
