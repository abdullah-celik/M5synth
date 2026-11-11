// Faces Calculator key tester
// Shows the code each key sends, beeps on every press, and logs to Serial.
#include <M5Unified.h>
#include <Wire.h>

constexpr uint8_t FACES_ADDR = 0x08;  // I2C address of the Faces panels
constexpr int     FACES_INT  = 5;     // goes LOW when a key is waiting

void drawHeader() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("Faces key tester");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(10, 40);
  M5.Display.println("Press any key...");
}

void showKey(const char* source, uint8_t code) {
  // Big character in the middle
  M5.Display.fillRect(0, 70, 320, 140, TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setTextSize(6);
  M5.Display.setCursor(20, 80);
  if (code >= 32 && code < 127) M5.Display.printf("'%c'", code);
  else                          M5.Display.print("--");

  // Details underneath
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(20, 160);
  M5.Display.printf("%s  dec %3u  hex 0x%02X", source, code, code);

  Serial.printf("%s  dec %3u  hex 0x%02X  char '%c'\n",
                source, code, code, (code >= 32 && code < 127) ? code : '?');

  M5.Speaker.tone(300 + code * 8, 60);  // different pitch per key
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  // M5Unified grabs pins 21/22 for its own I2C; hand them to Arduino Wire.
  M5.In_I2C.release();
  Wire.begin(21, 22, 400000);
  pinMode(FACES_INT, INPUT_PULLUP);

  M5.Speaker.setVolume(140);  // 0-255
  drawHeader();
  M5.Speaker.tone(880, 80);   // startup beep = speaker works
}

void loop() {
  M5.update();

  // Faces panel key
  if (digitalRead(FACES_INT) == LOW) {
    Wire.requestFrom(FACES_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t code = Wire.read();
      if (code != 0x00 && code != 0xFF) showKey("PANEL", code);
    }
  }

  // The three buttons on the Core
  if (M5.BtnA.wasPressed()) showKey("BTN A", 'A');
  if (M5.BtnB.wasPressed()) showKey("BTN B", 'B');
  if (M5.BtnC.wasPressed()) showKey("BTN C", 'C');

  delay(5);
}
