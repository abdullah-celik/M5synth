// =====================================================================
//  Faces Groovebox v1  -  16-step, 4-track drum machine
//  M5Stack Faces (Core ESP32) + Calculator panel, built-in speaker
//
//  Keypad:
//    [AC] clear track   [M] mute track   [%] tempo -   [/] tempo +
//    The 4x4 block below = the 16 steps (left->right, top->bottom)
//  Core buttons:
//    A = previous track   B = play / stop   C = next track
// =====================================================================
#include <M5Unified.h>
#include <Wire.h>
#include <math.h>

// ---------- hardware ----------
constexpr uint8_t FACES_ADDR = 0x08;
constexpr int     FACES_INT  = 5;

// ---------- sequencer ----------
constexpr int NUM_TRACKS = 4;
constexpr int NUM_STEPS  = 16;
constexpr int BPM_MIN = 60, BPM_MAX = 200, BPM_STEP = 5;

// Panel key -> step index, matching the physical 4x4 block
const char STEP_KEYS[NUM_STEPS] = {
  '7', '8', '9', '*',
  '4', '5', '6', '-',
  '1', '2', '3', '+',
  '.', '0', '`', '='
};

struct Track {
  const char* name;
  uint16_t    color;
  int16_t*    sample = nullptr;
  size_t      len    = 0;
  bool        steps[NUM_STEPS] = {};
  bool        muted  = false;
};

Track tracks[NUM_TRACKS];

int      bpm        = 110;
bool     playing    = false;
int      playStep   = -1;    // step currently sounding
int      curTrack   = 0;     // track shown in the big grid
uint32_t nextStepUs = 0;

// ---------- sound synthesis (pre-rendered at boot) ----------
constexpr uint32_t SR = 16000;   // sample rate of our drum samples
constexpr float    TWO_PI_F = 6.2831853f;

static uint32_t rngState = 0x1234567;
inline float noise() {           // fast white noise in [-1, 1)
  rngState = rngState * 1664525u + 1013904223u;
  return (int32_t)rngState / 2147483648.0f;
}

inline int16_t toPcm(float s) {
  if (s > 1.0f) s = 1.0f;
  if (s < -1.0f) s = -1.0f;
  return (int16_t)(s * 26000.0f);
}

int16_t* allocBuf(size_t n) {
  auto* b = (int16_t*)malloc(n * sizeof(int16_t));
  if (!b) { Serial.println("Out of memory!"); while (true) delay(1000); }
  return b;
}

// Kick: sine wave whose pitch dives from ~160 Hz to ~45 Hz, plus a click
void makeKick(Track& t) {
  t.len = SR * 0.30f;
  t.sample = allocBuf(t.len);
  float phase = 0;
  for (size_t i = 0; i < t.len; i++) {
    float tm   = (float)i / SR;
    float freq = 45.0f + 115.0f * expf(-tm * 30.0f);
    phase += TWO_PI_F * freq / SR;
    float s = sinf(phase) * expf(-tm * 9.0f);
    if (i < 60) s += noise() * 0.4f * (1.0f - i / 60.0f);   // attack click
    t.sample[i] = toPcm(s);
  }
}

// Snare: noise burst + short 185 Hz body tone
void makeSnare(Track& t) {
  t.len = SR * 0.20f;
  t.sample = allocBuf(t.len);
  for (size_t i = 0; i < t.len; i++) {
    float tm = (float)i / SR;
    float s = noise() * expf(-tm * 22.0f) * 0.75f
            + sinf(TWO_PI_F * 185.0f * tm) * expf(-tm * 35.0f) * 0.5f;
    t.sample[i] = toPcm(s);
  }
}

// Hi-hat: high-passed noise (difference of samples) with fast/slow decay
void makeHat(Track& t, float seconds, float decay) {
  t.len = SR * seconds;
  t.sample = allocBuf(t.len);
  float prev = 0;
  for (size_t i = 0; i < t.len; i++) {
    float tm = (float)i / SR;
    float n  = noise();
    float hp = n - prev;  prev = n;
    t.sample[i] = toPcm(hp * 0.45f * expf(-tm * decay));
  }
}

void triggerTrack(int i) {
  Track& t = tracks[i];
  if (i == 2) M5.Speaker.stop(3);   // closed hat chokes the open hat
  // channel i, stop whatever that channel was playing (retrigger)
  M5.Speaker.playRaw(t.sample, t.len, SR, false, 1, i, true);
}

// ---------- drawing ----------
constexpr uint16_t COL_OFF  = 0x31A6;   // dark grey for empty steps
constexpr uint16_t COL_BG   = TFT_BLACK;
constexpr int CELL = 44, GAP = 4, GX = 8, GY = 28;      // big grid
constexpr int MX = 212, MY = 28, MROW = 46;             // mini overview

void drawCell(int s) {
  int x = GX + (s % 4) * (CELL + GAP);
  int y = GY + (s / 4) * (CELL + GAP);
  Track& t = tracks[curTrack];
  bool on   = t.steps[s];
  bool head = playing && s == playStep;

  uint16_t fill = on ? (t.muted ? TFT_DARKGREY : t.color) : COL_OFF;
  M5.Display.fillRoundRect(x, y, CELL, CELL, 6, fill);

  // playhead outline (2 px, drawn in the gap around the cell)
  uint16_t ring = head ? TFT_WHITE : COL_BG;
  M5.Display.drawRoundRect(x - 2, y - 2, CELL + 4, CELL + 4, 8, ring);
  M5.Display.drawRoundRect(x - 1, y - 1, CELL + 2, CELL + 2, 7, ring);

  // key label in the corner so you know which key it is
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(on ? TFT_BLACK : TFT_LIGHTGREY, fill);
  M5.Display.setCursor(x + 4, y + 4);
  M5.Display.print(STEP_KEYS[s]);
}

void drawGrid() {
  for (int s = 0; s < NUM_STEPS; s++) drawCell(s);
}

void drawMiniStep(int ti, int s) {
  Track& t = tracks[ti];
  int x = MX + s * 6 + (s / 4) * 2;
  int y = MY + ti * MROW + 14;
  bool head = playing && s == playStep;
  uint16_t c;
  if (t.steps[s]) c = head ? TFT_WHITE : (t.muted ? TFT_DARKGREY : t.color);
  else            c = head ? 0x7BEF : COL_OFF;
  M5.Display.fillRect(x, y, 5, 14, c);
}

void drawMiniTrack(int ti) {
  Track& t = tracks[ti];
  int y = MY + ti * MROW;
  M5.Display.fillRect(MX - 4, y, 320 - MX + 4, 12, COL_BG);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(ti == curTrack ? t.color : TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(MX, y + 2);
  M5.Display.print(ti == curTrack ? "> " : "  ");
  M5.Display.print(t.name);
  if (t.muted) { M5.Display.setTextColor(TFT_RED, COL_BG); M5.Display.print(" MUTE"); }
  for (int s = 0; s < NUM_STEPS; s++) drawMiniStep(ti, s);
}

void drawHeader() {
  M5.Display.fillRect(0, 0, 320, 22, COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 4);
  M5.Display.setTextColor(playing ? TFT_GREEN : TFT_RED, COL_BG);
  M5.Display.print(playing ? "PLAY" : "STOP");
  M5.Display.setTextColor(TFT_WHITE, COL_BG);
  M5.Display.printf("  %3d BPM", bpm);
  Track& t = tracks[curTrack];
  M5.Display.setTextColor(t.color, COL_BG);
  M5.Display.setCursor(320 - 8 - M5.Display.textWidth(t.name), 4);
  M5.Display.print(t.name);
}

void drawHelp() {
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(MX, 214);  M5.Display.print("AC clr  M mute");
  M5.Display.setCursor(MX, 224);  M5.Display.print("% bpm-  / bpm+");
  // labels above the three Core buttons
  const char* labels[3] = {"< TRACK", "PLAY/STOP", "TRACK >"};
  const int   cx[3]     = {65, 160, 255};
  for (int i = 0; i < 3; i++) {
    M5.Display.setCursor(cx[i] - M5.Display.textWidth(labels[i]) / 2, 232);
    M5.Display.print(labels[i]);
  }
}

void drawAll() {
  M5.Display.fillScreen(COL_BG);
  drawHeader();
  drawGrid();
  for (int i = 0; i < NUM_TRACKS; i++) drawMiniTrack(i);
  drawHelp();
}

// ---------- sequencer logic ----------
uint32_t stepIntervalUs() { return 60000000UL / bpm / 4; }   // 16th notes

void advanceStep() {
  int prev = playStep;
  playStep = (playStep + 1) % NUM_STEPS;

  // sound first, so drawing never delays the beat
  for (int i = 0; i < NUM_TRACKS; i++)
    if (tracks[i].steps[playStep] && !tracks[i].muted) triggerTrack(i);

  if (prev >= 0) drawCell(prev);
  drawCell(playStep);
  for (int i = 0; i < NUM_TRACKS; i++) {
    if (prev >= 0) drawMiniStep(i, prev);
    drawMiniStep(i, playStep);
  }
}

void setPlaying(bool p) {
  playing = p;
  if (playing) {
    playStep   = -1;
    nextStepUs = micros();
  } else {
    int last = playStep;
    playStep = -1;
    if (last >= 0) {
      drawCell(last);
      for (int i = 0; i < NUM_TRACKS; i++) drawMiniStep(i, last);
    }
  }
  drawHeader();
}

void selectTrack(int t) {
  curTrack = (t + NUM_TRACKS) % NUM_TRACKS;
  drawHeader();
  drawGrid();
  for (int i = 0; i < NUM_TRACKS; i++) drawMiniTrack(i);
}

void handlePanelKey(char k) {
  Track& t = tracks[curTrack];

  // step keys
  for (int s = 0; s < NUM_STEPS; s++) {
    if (STEP_KEYS[s] == k) {
      t.steps[s] = !t.steps[s];
      if (t.steps[s] && !playing) triggerTrack(curTrack);   // preview
      drawCell(s);
      drawMiniStep(curTrack, s);
      return;
    }
  }

  // function keys (top row)
  switch (k) {
    case 'A':                                   // AC = clear track
      for (auto& st : t.steps) st = false;
      drawGrid(); drawMiniTrack(curTrack);
      break;
    case 'M':                                   // mute / unmute
      t.muted = !t.muted;
      drawGrid(); drawMiniTrack(curTrack);
      break;
    case '%':                                   // tempo down
      bpm = max(BPM_MIN, bpm - BPM_STEP); drawHeader();
      break;
    case '/':                                   // tempo up
      bpm = min(BPM_MAX, bpm + BPM_STEP); drawHeader();
      break;
  }
}

void loadDefaultBeat() {
  const int kick[]  = {0, 4, 8, 12};
  const int snare[] = {4, 12};
  for (int s : kick)  tracks[0].steps[s] = true;
  for (int s : snare) tracks[1].steps[s] = true;
  for (int s = 0; s < NUM_STEPS; s += 2) tracks[2].steps[s] = true;
  tracks[3].steps[14] = true;
}

// ---------- setup / loop ----------
void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.In_I2C.release();               // give pins 21/22 to Arduino Wire
  Wire.begin(21, 22, 400000);
  pinMode(FACES_INT, INPUT_PULLUP);

  M5.Display.fillScreen(COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE, COL_BG);
  M5.Display.setCursor(40, 110);
  M5.Display.print("Synthesising drums...");

  tracks[0].name = "KICK";   tracks[0].color = TFT_ORANGE;
  tracks[1].name = "SNARE";  tracks[1].color = TFT_YELLOW;
  tracks[2].name = "C.HAT";  tracks[2].color = TFT_CYAN;
  tracks[3].name = "O.HAT";  tracks[3].color = TFT_GREEN;
  makeKick(tracks[0]);
  makeSnare(tracks[1]);
  makeHat(tracks[2], 0.06f, 80.0f);
  makeHat(tracks[3], 0.30f, 14.0f);

  M5.Speaker.setVolume(200);         // 0-255

  loadDefaultBeat();
  drawAll();
}

void loop() {
  M5.update();

  // Core buttons
  if (M5.BtnA.wasPressed()) selectTrack(curTrack - 1);
  if (M5.BtnB.wasPressed()) setPlaying(!playing);
  if (M5.BtnC.wasPressed()) selectTrack(curTrack + 1);

  // Calculator panel
  if (digitalRead(FACES_INT) == LOW) {
    Wire.requestFrom(FACES_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t k = Wire.read();
      if (k != 0x00 && k != 0xFF) handlePanelKey((char)k);
    }
  }

  // Clock
  if (playing && (int32_t)(micros() - nextStepUs) >= 0) {
    nextStepUs += stepIntervalUs();
    advanceStep();
  }
}
