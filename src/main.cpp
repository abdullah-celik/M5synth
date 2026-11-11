// =====================================================================
//  Faces Groovebox v2
//  M5Stack Faces (Core ESP32) + Calculator panel
//
//  - Real-time synth engine, output to built-in speaker OR Bluetooth
//    headphones / speaker (chosen at boot)
//  - 7 tracks: KICK, SNARE, HAT  (drums)  BASS, LEAD, PLUCK, FM  (synths)
//    each track has 3 sound presets + TONE / DECAY / VOLUME / ECHO
//  - GROOVE mode: 16-step sequencer        KEYS mode: play live,
//    optionally recording into the sequence (live or step-by-step)
//
//  Calculator panel:
//    AC = function menu   M = mute track   % = value -   / = value +
//    4x4 block = steps (GROOVE) or notes / drum pads (KEYS)
//  Core buttons:
//    A = previous track   B = play/stop (hold: GROOVE <-> KEYS)   C = next track
// =====================================================================
#include <M5Unified.h>
#include <Wire.h>
#include <math.h>
#include "BluetoothA2DPSource.h"

// ---------------------------------------------------------------------
//  Hardware
// ---------------------------------------------------------------------
constexpr uint8_t FACES_ADDR = 0x08;
constexpr int     FACES_INT  = 5;

// ---------------------------------------------------------------------
//  Tracks, sounds, scales
// ---------------------------------------------------------------------
enum TrackId { T_KICK, T_SNARE, T_HAT, T_BASS, T_LEAD, T_PLUCK, T_FM, NT };
inline bool isDrum(int t) { return t <= T_HAT; }

constexpr int NUM_STEPS = 16;

const char* TRACK_NAMES[NT] = {"KICK", "SNARE", "HAT", "BASS", "LEAD", "PLUCK", "FM"};
const uint16_t TRACK_COLORS[NT] = {
  TFT_ORANGE, TFT_YELLOW, TFT_CYAN, TFT_VIOLET, TFT_GREENYELLOW, TFT_SKYBLUE, TFT_PINK
};
const char* SOUND_NAMES[NT][3] = {
  {"808",     "Punch", "Chip"},
  {"Snare",   "Clap",  "Chip"},
  {"Closed",  "Open",  "Metal"},
  {"Acid",    "Sub",   "Square"},
  {"Square",  "Pulse", "Saw"},
  {"Guitar",  "Harp",  "Muted"},
  {"E.Piano", "Bell",  "Metal"},
};
const float TRACK_GAIN[NT] = {1.0f, 0.65f, 0.35f, 0.55f, 0.32f, 0.7f, 0.45f};
const int   TRACK_BASE_MIDI[NT] = {0, 0, 0, 36, 60, 48, 48};   // + key

struct Scale { const char* name; uint8_t len; uint8_t semis[12]; };
const Scale SCALES[] = {
  {"MinPent", 5,  {0, 3, 5, 7, 10}},
  {"MajPent", 5,  {0, 2, 4, 7, 9}},
  {"Minor",   7,  {0, 2, 3, 5, 7, 8, 10}},
  {"Major",   7,  {0, 2, 4, 5, 7, 9, 11}},
  {"Dorian",  7,  {0, 2, 3, 5, 7, 9, 10}},
  {"Blues",   6,  {0, 3, 5, 6, 7, 10}},
  {"Chroma",  12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
constexpr int NUM_SCALES = sizeof(SCALES) / sizeof(SCALES[0]);
const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// ---------------------------------------------------------------------
//  Shared state (written by UI, read by audio; bytes = atomic enough)
// ---------------------------------------------------------------------
struct TrackParams {
  uint8_t sound, tone, decay, vol, echo;   // tone/decay/vol/echo: 0..10
  int8_t  oct;                             // -2..+2
  bool    muted;
};
volatile TrackParams tp[NT] = {
  {0, 5, 5, 9, 0, 0, false},   // KICK
  {0, 5, 4, 7, 1, 0, false},   // SNARE
  {0, 6, 3, 6, 0, 0, false},   // HAT
  {0, 4, 4, 8, 0, 0, false},   // BASS
  {0, 6, 5, 6, 3, 0, false},   // LEAD
  {0, 6, 6, 7, 3, 0, false},   // PLUCK
  {0, 5, 6, 7, 2, 0, false},   // FM
};

struct Step { bool on; int8_t note; };   // synths: scale degree, drums: semitones
volatile Step pattern[NT][NUM_STEPS];

volatile int  bpm      = 112;
volatile int  swing    = 0;      // 0..6
volatile int  scaleIdx = 0;
volatile int  keyRoot  = 9;      // A

// audio -> UI
volatile int8_t   uiStep     = -1;
volatile bool     uiPlaying  = false;
volatile uint32_t patternRev = 0;    // bumped when audio thread edits the pattern

int degreeToSemis(int deg) {
  const Scale& sc = SCALES[scaleIdx];
  if (deg < 0) deg = 0;
  return (deg / sc.len) * 12 + sc.semis[deg % sc.len];
}
int noteMidi(int track, int deg) {
  return TRACK_BASE_MIDI[track] + keyRoot + 12 * tp[track].oct + degreeToSemis(deg);
}
int maxDegree() { int m = SCALES[scaleIdx].len * 3; return m > 24 ? 24 : m; }

// ---------------------------------------------------------------------
//  Event queue  UI -> audio
// ---------------------------------------------------------------------
enum EvType : uint8_t { EV_NOTE, EV_REC, EV_PLAY, EV_STOP };
struct Event { EvType type; uint8_t track; int8_t note; };
QueueHandle_t evq;

void sendEvent(EvType type, int track = 0, int note = 0) {
  Event e{type, (uint8_t)track, (int8_t)note};
  xQueueSend(evq, &e, 0);
}

// =====================================================================
//  AUDIO ENGINE
// =====================================================================
constexpr float SR      = 44100.0f;
constexpr float INV_SR  = 1.0f / SR;
constexpr float PI_F    = 3.14159265f;

// ---- helpers ----
float sineTbl[1025];
inline float sinT(float ph) {                // ph in cycles
  ph -= floorf(ph);
  float x = ph * 1024.0f;
  int i = (int)x;
  float f = x - i;
  return sineTbl[i] + (sineTbl[i + 1] - sineTbl[i]) * f;
}
static uint32_t rng = 0x9E3779B9;
inline float noise() {
  rng = rng * 1664525u + 1013904223u;
  return (int32_t)rng * (1.0f / 2147483648.0f);
}
inline float polyblep(float t, float dt) {
  if (t < dt)        { t /= dt;          return t + t - t * t - 1.0f; }
  if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
  return 0.0f;
}
inline float softclip(float x) {
  if (x > 3.0f) x = 3.0f; else if (x < -3.0f) x = -3.0f;
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
inline float decayCoef(float seconds) { return expf(-1.0f / (SR * seconds)); }
inline float midiHz(float m) { return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f); }

// ---- state-variable filter (Zavalishin TPT, stable at any cutoff) ----
struct SVF {
  float ic1 = 0, ic2 = 0, a1 = 1, a2 = 0, a3 = 0, k = 2;
  void set(float fc, float res) {            // res 0..0.98
    if (fc < 20) fc = 20;
    if (fc > SR * 0.45f) fc = SR * 0.45f;
    float g = tanf(PI_F * fc * INV_SR);
    k  = 2.0f - 2.0f * res;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  inline float lp(float x, float* bp = nullptr) {
    float v3 = x - ic2;
    float v1 = a1 * ic1 + a2 * v3;
    float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
    if (bp) *bp = v1;
    return v2;
  }
  void reset() { ic1 = ic2 = 0; }
};

// ---- voices ----
constexpr int KS_MAX = 900;                  // Karplus-Strong buffer (min ~49 Hz)
constexpr int NUM_PLUCK = 4;
float ksBuf[NUM_PLUCK][KS_MAX];

struct Voice {
  bool    active = false;
  uint8_t track = 0, sound = 0;
  uint32_t t = 0;                 // samples since trigger
  float   env = 0, envCoef = 0;   // amplitude envelope
  float   env2 = 0, env2Coef = 0; // pitch / filter / mod envelope
  float   gain = 1;
  float   freq = 0, dt = 0;
  float   ph1 = 0, ph2 = 0, ph3 = 0;
  float   fA = 0, fB = 0, fC = 0; // per-instrument parameters
  float   hpX = 0, hpY = 0, hold = 0;
  float   metal[6] = {};
  SVF     svf;
  // Karplus
  float*  ks = nullptr; int ksLen = 0, ksIdx = 0; float ksFb = 0, ksMix = 0;
};

// voice ranges per track
const uint8_t V_FIRST[NT] = {0, 1, 2, 3, 4, 6, 10};
const uint8_t V_COUNT[NT] = {1, 1, 1, 1, 2, 4, 4};
constexpr int NUM_VOICES = 14;
Voice voices[NUM_VOICES];
uint8_t rrNext[NT];

// echo
constexpr int ECHO_LEN = 17640;              // 0.4 s
int16_t echoBuf[ECHO_LEN];
int     echoW = 0;
float   echoLp = 0;

// sequencer (audio thread only)
bool    seqPlaying = false;
int     seqStep = -1;
float   samplesLeft = 0, curStepLen = 1;
uint8_t skipNext[NT];

inline float tauFromDecay(int d, float lo, float hi) {   // exponential map 0..10
  return lo * powf(hi / lo, d / 10.0f);
}

void triggerVoice(int track, int note) {
  int vi = V_FIRST[track] + rrNext[track];
  rrNext[track] = (rrNext[track] + 1) % V_COUNT[track];
  Voice& v = voices[vi];

  const int snd = tp[track].sound, tone = tp[track].tone, dec = tp[track].decay;
  v.track = track; v.sound = snd; v.t = 0;
  v.env = 1.0f; v.env2 = 1.0f;
  v.gain = TRACK_GAIN[track];
  v.ph1 = v.ph2 = v.ph3 = 0;
  v.hpX = v.hpY = v.hold = 0;
  v.svf.reset();

  float pitchMul = powf(2.0f, note / 12.0f);   // drums: note = semitones
  switch (track) {
    case T_KICK: {
      v.fB = (40.0f + tone * 4.0f) * pitchMul;             // end frequency
      v.fA = v.fB * (snd == 1 ? 6.0f : 4.0f);               // start frequency
      v.env2Coef = decayCoef(snd == 1 ? 0.018f : (snd == 2 ? 0.03f : 0.045f));
      v.envCoef  = decayCoef(tauFromDecay(dec, 0.05f, 0.9f) * (snd == 0 ? 1.4f : 1.0f));
      if (snd == 2) v.gain *= 0.55f;
    } break;
    case T_SNARE: {
      v.freq = (160.0f + tone * 12.0f) * pitchMul;
      v.envCoef  = decayCoef(tauFromDecay(dec, 0.03f, 0.45f));
      v.env2Coef = decayCoef(0.035f);
      if (snd == 1) v.svf.set(1100.0f * pitchMul, 0.45f);    // clap band
      if (snd == 2) v.freq *= 1.4f;
    } break;
    case T_HAT: {
      float tau = tauFromDecay(dec, 0.015f, 0.12f) * (snd == 1 ? 5.0f : 1.0f);
      v.envCoef = decayCoef(tau);
      v.fA = 0.55f + tone * 0.04f;                           // high-pass amount
      for (int i = 0; i < 6; i++) v.metal[i] = 0;
      v.freq = 40.0f * (1.0f + tone * 0.12f) * pitchMul;      // metal base pitch
    } break;
    case T_BASS: {
      v.freq = midiHz(noteMidi(track, note)); v.dt = v.freq * INV_SR;
      v.envCoef  = decayCoef(tauFromDecay(dec, 0.08f, 1.5f));
      v.env2Coef = decayCoef(tauFromDecay(dec, 0.04f, 0.5f));
      v.fA = 60.0f * powf(2.0f, tone * 0.55f);               // base cutoff
      v.fB = (snd == 0) ? 8.0f : (snd == 2 ? 3.0f : 0.0f);   // env amount
      v.fC = (snd == 0) ? 0.88f : 0.35f;                     // resonance
    } break;
    case T_LEAD: {
      v.freq = midiHz(noteMidi(track, note)); v.dt = v.freq * INV_SR;
      v.envCoef  = decayCoef(tauFromDecay(dec, 0.08f, 2.5f));
      v.env2Coef = decayCoef(0.12f);
      v.fA = 300.0f * powf(2.0f, tone * 0.45f);
      v.ph2 = 0.37f;                                          // detune osc start
    } break;
    case T_PLUCK: {
      v.freq = midiHz(noteMidi(track, note));
      int pv = vi - V_FIRST[T_PLUCK];
      v.ks = ksBuf[pv];
      v.ksLen = (int)(SR / v.freq);
      if (v.ksLen > KS_MAX) v.ksLen = KS_MAX;
      if (v.ksLen < 4) v.ksLen = 4;
      v.ksIdx = 0;
      float fb = 0.985f + dec * 0.0014f;
      if (snd == 1) fb = fminf(0.9995f, fb + 0.004f);
      if (snd == 2) fb -= 0.02f;
      v.ksFb  = fb;
      v.ksMix = (snd == 2) ? 0.5f : (0.5f - tone * 0.04f);
      float y = 0, a = 0.2f + tone * 0.08f;
      for (int i = 0; i < v.ksLen; i++) { y += (noise() - y) * a; v.ks[i] = y * 1.6f; }
      v.envCoef = decayCoef(tauFromDecay(dec, 0.4f, 4.0f));
    } break;
    case T_FM: {
      v.freq = midiHz(noteMidi(track, note));
      v.dt = v.freq * INV_SR;
      v.fA = (snd == 0) ? 1.0f : (snd == 1 ? 3.5f : 1.414f);  // mod ratio
      v.fB = (0.4f + tone * 0.3f) * (snd == 1 ? 1.4f : 1.0f); // mod index
      float tau = tauFromDecay(dec, 0.1f, 3.0f);
      v.envCoef  = decayCoef(tau);
      v.env2Coef = decayCoef(tau * (snd == 0 ? 0.25f : 0.5f));
    } break;
  }
  v.active = true;
}

inline float renderVoice(Voice& v) {
  float s = 0;
  const float atk = (v.t < 88) ? v.t / 88.0f : 1.0f;      // 2 ms anti-click
  switch (v.track) {
    case T_KICK: {
      float f = v.fB + (v.fA - v.fB) * v.env2;
      v.ph1 += f * INV_SR;
      s = sinT(v.ph1);
      if (v.sound == 2) s = s > 0 ? 0.8f : -0.8f;
      if (v.sound == 1 && v.t < 132) s += noise() * 0.5f * (1.0f - v.t / 132.0f);
      s *= v.env;
      v.env2 *= v.env2Coef;
      return s * v.gain;   // no attack ramp: the kick needs its punch
    }
    case T_SNARE: {
      float n = noise();
      if (v.sound == 2) { if ((v.t & 7) == 0) v.hold = n; n = v.hold; }   // crushed
      v.ph1 += v.freq * INV_SR;
      if (v.sound == 1) {                                  // clap: 3 quick bursts
        float bp; v.svf.lp(n, &bp);
        float e = (v.t < 1323) ? 1.0f - (v.t % 441) / 441.0f : v.env;
        s = bp * 3.3f * e;
      } else {
        float hp = n - v.hpX; v.hpX = n;
        float tone = (v.sound == 2) ? (sinT(v.ph1) > 0 ? 0.5f : -0.5f) : sinT(v.ph1);
        s = hp * 0.55f * v.env + tone * 0.5f * v.env2;
        v.env2 *= v.env2Coef;
      }
      break;
    }
    case T_HAT: {
      float x;
      if (v.sound == 2) {                                  // 808-style metal
        static const float R[6] = {2.0f, 3.0f, 4.16f, 5.43f, 6.79f, 8.21f};
        x = 0;
        for (int i = 0; i < 6; i++) {
          v.metal[i] += v.freq * R[i] * INV_SR;
          if (v.metal[i] >= 1.0f) v.metal[i] -= 1.0f;
          x += v.metal[i] < 0.5f ? 1.0f : -1.0f;
        }
        x *= 0.6f;
      } else {
        x = noise();
      }
      v.hpY = v.fA * (v.hpY + x - v.hpX);                 // one-pole high-pass
      v.hpX = x;
      s = v.hpY * v.env;
      break;
    }
    case T_BASS: {
      v.ph1 += v.dt; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
      float o;
      if (v.sound == 0)      o = 2.0f * v.ph1 - 1.0f - polyblep(v.ph1, v.dt);       // saw
      else if (v.sound == 2) {                                                       // square
        float t2 = v.ph1 + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
        o = (v.ph1 < 0.5f ? 1.0f : -1.0f) + polyblep(v.ph1, v.dt) - polyblep(t2, v.dt);
      } else                 o = 0.7f * sinT(v.ph1) + 0.3f * (4.0f * fabsf(v.ph1 - 0.5f) - 1.0f); // sub
      if (v.fB > 0) {
        if ((v.t & 15) == 0) v.svf.set(v.fA * (1.0f + v.fB * v.env2), v.fC);
        o = softclip(v.svf.lp(o * 1.3f) * 1.4f);
        v.env2 *= v.env2Coef;
      }
      s = o * v.env;
      break;
    }
    case T_LEAD: {
      float dt = v.dt;
      if (v.sound == 1 && v.t > 6000) {                    // delayed vibrato
        v.ph3 += 5.5f * INV_SR;
        dt *= 1.0f + 0.006f * sinT(v.ph3);
      }
      v.ph1 += dt; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
      float o;
      if (v.sound == 2) {                                  // 2 detuned saws
        float dt2 = dt * 1.007f;
        v.ph2 += dt2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
        o = 0.8f * ((2.0f * v.ph1 - 1.0f - polyblep(v.ph1, dt)) +
                    (2.0f * v.ph2 - 1.0f - polyblep(v.ph2, dt2)));
      } else {                                             // square / 25% pulse
        float pw = (v.sound == 1) ? 0.25f : 0.5f;
        float t2 = v.ph1 + (1.0f - pw); if (t2 >= 1.0f) t2 -= 1.0f;
        o = (v.ph1 < pw ? 1.0f : -1.0f) + polyblep(v.ph1, dt) - polyblep(t2, dt);
      }
      if ((v.t & 15) == 0) v.svf.set(v.fA * (1.0f + 1.5f * v.env2), 0.3f);
      o = v.svf.lp(o);
      v.env2 *= v.env2Coef;
      s = o * v.env;
      break;
    }
    case T_PLUCK: {
      float out = v.ks[v.ksIdx];
      int nx = v.ksIdx + 1; if (nx >= v.ksLen) nx = 0;
      v.ks[v.ksIdx] = v.ksFb * (out * (1.0f - v.ksMix) + v.ks[nx] * v.ksMix);
      v.ksIdx = nx;
      s = out * v.env;
      break;
    }
    case T_FM: {
      v.ph1 += v.dt;
      v.ph2 += v.dt * v.fA;
      if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
      if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
      float mod = v.fB * v.env2 * sinT(v.ph2);
      s = sinT(v.ph1 + mod) * v.env;
      v.env2 *= v.env2Coef;
      break;
    }
  }
  return s * atk * v.gain;
}

// ---- sequencer ----
float stepLength(int step) {
  float base = SR * 60.0f / bpm / 4.0f;
  float sw = swing * 0.055f;
  return (step & 1) ? base * (1.0f - sw) : base * (1.0f + sw);
}

// HAT has a single voice, so a new hit always chokes a ringing open hat
inline void triggerTrack(int track, int note) { triggerVoice(track, note); }

void seqAdvance() {
  seqStep = (seqStep + 1) % NUM_STEPS;
  for (int t = 0; t < NT; t++) {
    if (pattern[t][seqStep].on && !tp[t].muted) {
      if (skipNext[t]) continue;
      triggerTrack(t, pattern[t][seqStep].note);
    }
  }
  for (int t = 0; t < NT; t++) skipNext[t] = 0;
  curStepLen = stepLength(seqStep);
  samplesLeft += curStepLen;
  uiStep = seqStep;
}

void handleEvent(const Event& e) {
  switch (e.type) {
    case EV_NOTE:
      triggerTrack(e.track, e.note);
      break;
    case EV_REC: {
      triggerTrack(e.track, e.note);
      if (!seqPlaying) break;
      float phase = 1.0f - samplesLeft / curStepLen;      // 0..1 through the current step
      if (seqStep < 0) break;                             // sequence not started yet
      int target = seqStep;
      if (phase > 0.5f) { target = (seqStep + 1) % NUM_STEPS; skipNext[e.track] = 1; }
      pattern[e.track][target].on   = true;
      pattern[e.track][target].note = e.note;
      patternRev = patternRev + 1;
    } break;
    case EV_PLAY:
      seqPlaying = true; seqStep = -1; samplesLeft = 0; curStepLen = 1;
      uiPlaying = true;
      break;
    case EV_STOP:
      seqPlaying = false; seqStep = -1;
      uiPlaying = false; uiStep = -1;
      break;
  }
}

// ---- main render: mono int16 ----
void engineRender(int16_t* out, int n) {
  Event e;
  while (xQueueReceive(evq, &e, 0) == pdTRUE) handleEvent(e);

  // per-block constants
  float trackVol[NT], trackSend[NT];
  for (int t = 0; t < NT; t++) {
    float v = tp[t].vol / 10.0f;
    trackVol[t]  = v;                 // (mute only silences the sequence, not live play)
    trackSend[t] = tp[t].echo / 10.0f;
  }
  // echo time: dotted 8th, shortened if it doesn't fit the buffer
  float stepS = SR * 60.0f / bpm / 4.0f;
  int echoDelay = (int)(stepS * 3);
  while (echoDelay >= ECHO_LEN) echoDelay = (int)(echoDelay * 0.66f);

  for (int i = 0; i < n; i++) {
    if (seqPlaying) {
      if (samplesLeft <= 0) seqAdvance();
      samplesLeft -= 1.0f;
    }

    float dry = 0, send = 0;
    for (int vi = 0; vi < NUM_VOICES; vi++) {
      Voice& v = voices[vi];
      if (!v.active) continue;
      float s = renderVoice(v) * trackVol[v.track];
      dry  += s;
      send += s * trackSend[v.track];
      v.env *= v.envCoef;
      v.t++;
      if (v.env < 0.0004f && v.t > 200) v.active = false;
    }

    // echo
    int r = echoW - echoDelay; if (r < 0) r += ECHO_LEN;
    float d = echoBuf[r] * (1.0f / 32767.0f);
    echoLp += (d - echoLp) * 0.35f;                      // darken repeats
    float w = send + echoLp * 0.45f;
    if (w > 1.0f) w = 1.0f; else if (w < -1.0f) w = -1.0f;
    echoBuf[echoW] = (int16_t)(w * 32767.0f);
    if (++echoW >= ECHO_LEN) echoW = 0;

    float mix = softclip((dry + d * 0.6f) * 0.7f);
    out[i] = (int16_t)(mix * 30000.0f);
  }
}

void engineInit() {
  for (int i = 0; i <= 1024; i++) sineTbl[i] = sinf(2.0f * PI_F * i / 1024.0f);
  evq = xQueueCreate(32, sizeof(Event));
}

// ---------------------------------------------------------------------
//  Output: speaker task  /  Bluetooth callback
// ---------------------------------------------------------------------
constexpr int SPK_BLOCK = 256;
bool useBluetooth = false;
BluetoothA2DPSource a2dp;

TaskHandle_t audioTaskHandle = nullptr;
void speakerTask(void*) {
  static int16_t bufs[3][SPK_BLOCK];
  int bi = 0;
  for (;;) {
    while (M5.Speaker.isPlaying(0) >= 2) vTaskDelay(1);
    engineRender(bufs[bi], SPK_BLOCK);
    M5.Speaker.playRaw(bufs[bi], SPK_BLOCK, (uint32_t)SR, false, 1, 0, false);
    bi = (bi + 1) % 3;
  }
}

int32_t btFrames(Frame* frames, int32_t count) {
  static int16_t tmp[256];
  int32_t done = 0;
  while (done < count) {
    int32_t c = count - done; if (c > 256) c = 256;
    engineRender(tmp, c);
    for (int32_t i = 0; i < c; i++) {
      frames[done + i].channel1 = tmp[i];
      frames[done + i].channel2 = tmp[i];
    }
    done += c;
  }
  return count;
}

// Bluetooth device discovery
struct FoundDev { char name[32]; uint8_t addr[6]; };
constexpr int MAX_FOUND = 8;
FoundDev found[MAX_FOUND];
volatile int  foundCount = 0;
volatile bool haveSel = false;
uint8_t selAddr[6];

bool onSsid(const char* ssid, esp_bd_addr_t address, int rssi) {
  if (haveSel && memcmp(address, selAddr, 6) == 0) return true;
  for (int i = 0; i < foundCount; i++)
    if (memcmp(found[i].addr, address, 6) == 0) return false;
  if (foundCount < MAX_FOUND) {
    FoundDev& f = found[foundCount];
    strncpy(f.name, (ssid && *ssid) ? ssid : "(no name)", sizeof(f.name) - 1);
    f.name[sizeof(f.name) - 1] = 0;
    memcpy(f.addr, address, 6);
    foundCount = foundCount + 1;
  }
  return false;
}

// =====================================================================
//  USER INTERFACE
// =====================================================================
const char STEP_KEYS[NUM_STEPS] = {
  '7', '8', '9', '*',
  '4', '5', '6', '-',
  '1', '2', '3', '+',
  '.', '0', '`', '='
};

enum Mode { MODE_GROOVE, MODE_KEYS };
enum Target { TG_TEMPO, TG_SWING, TG_SCALE, TG_KEY, TG_SOUND, TG_TONE, TG_DECAY, TG_ECHO,
              TG_VOLUME, TG_NOTE, TG_OCTAVE };
enum FnAction { FN_TEMPO, FN_SWING, FN_SCALE, FN_KEY, FN_SOUND, FN_TONE, FN_DECAY, FN_ECHO,
                FN_VOLUME, FN_NOTE, FN_OCTAVE, FN_RANDOM, FN_CLRTRK, FN_CLRALL, FN_REC, FN_MODE };
const char* FN_LABELS[16] = {
  "TEMPO", "SWING", "SCALE", "KEY",
  "SOUND", "TONE",  "DECAY", "ECHO",
  "VOLUME","NOTE",  "OCTAVE","RANDOM",
  "CLR TRK","CLR ALL","REC", "MODE"
};
const char* TARGET_NAMES[] = {"TEMPO", "SWING", "SCALE", "KEY", "SOUND", "TONE", "DECAY",
                              "ECHO", "VOLUME", "NOTE", "OCTAVE"};

Mode   mode     = MODE_GROOVE;
Target target   = TG_TEMPO;
bool   fnLayer  = false;
bool   recOn    = false;
int    curTrack = T_KICK;
int    lastStep = -1;          // last step touched (for NOTE editing)
int    brush[NT] = {0};        // note used when placing a new step
int    recCursor = 0;          // step-record position (KEYS + REC while stopped)
int    flashKey = -1; uint32_t flashUntil = 0;

// layout
constexpr uint16_t COL_BG  = TFT_BLACK;
constexpr uint16_t COL_OFF = 0x31A6;
constexpr uint16_t COL_KEY = 0x2945;
constexpr uint16_t COL_FN  = 0x18E3;
constexpr int CELL = 44, GAP = 4, GX = 8, GY = 28;
constexpr int MX = 210, MY = 28, MROW = 25;

void noteName(int midi, char* buf) {
  int oct = midi / 12 - 1;
  if (oct < -1) oct = -1;
  if (oct > 9) oct = 9;
  snprintf(buf, 6, "%s%d", NOTE_NAMES[((midi % 12) + 12) % 12], oct);
}

// ---- cells ----
void cellXY(int s, int& x, int& y) {
  x = GX + (s % 4) * (CELL + GAP);
  y = GY + (s / 4) * (CELL + GAP);
}
void drawRing(int s, uint16_t c) {
  int x, y; cellXY(s, x, y);
  M5.Display.drawRoundRect(x - 2, y - 2, CELL + 4, CELL + 4, 8, c);
  M5.Display.drawRoundRect(x - 1, y - 1, CELL + 2, CELL + 2, 7, c);
}
void centerText(const char* txt, int cx, int cy, int size, uint16_t fg, uint16_t bg) {
  M5.Display.setTextSize(size);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setCursor(cx - M5.Display.textWidth(txt) / 2, cy - 4 * size);
  M5.Display.print(txt);
}

// drum pad layout in KEYS mode: rows bottom->top = KICK, SNARE, HAT, HAT high
const int8_t PAD_SEMIS[4] = {-3, 0, 3, 7};
int padTrack(int s) { int row = s / 4; return row == 3 ? T_KICK : row == 2 ? T_SNARE : T_HAT; }
int padNote(int s)  { return PAD_SEMIS[s % 4] + (s / 4 == 0 ? 12 : 0); }
int keyDegree(int s) { return (3 - s / 4) * 4 + (s % 4); }   // bottom-left = lowest

void drawCell(int s) {
  int x, y; cellXY(s, x, y);
  char buf[12];

  if (fnLayer) {
    bool sel = (s <= FN_OCTAVE) && (int)target == s;
    uint16_t fill = sel ? TFT_DARKCYAN : COL_FN;
    if (s == FN_REC && recOn) fill = TFT_RED;
    M5.Display.fillRoundRect(x, y, CELL, CELL, 6, fill);
    centerText(FN_LABELS[s], x + CELL / 2, y + CELL / 2, 1, TFT_WHITE, fill);
    drawRing(s, COL_BG);
    return;
  }

  if (mode == MODE_KEYS) {
    bool lit = (s == flashKey);
    if (isDrum(curTrack)) {
      int pt = padTrack(s);
      uint16_t fill = lit ? TRACK_COLORS[pt] : COL_KEY;
      M5.Display.fillRoundRect(x, y, CELL, CELL, 6, fill);
      centerText(TRACK_NAMES[pt], x + CELL / 2, y + 15, 1, lit ? TFT_BLACK : TRACK_COLORS[pt], fill);
      snprintf(buf, sizeof(buf), "%+d", padNote(s));
      centerText(buf, x + CELL / 2, y + 31, 1, lit ? TFT_BLACK : TFT_LIGHTGREY, fill);
    } else {
      int deg = keyDegree(s);
      bool root = (deg % SCALES[scaleIdx].len) == 0;
      uint16_t fill = lit ? TRACK_COLORS[curTrack] : (root ? 0x4208 : COL_KEY);
      M5.Display.fillRoundRect(x, y, CELL, CELL, 6, fill);
      noteName(noteMidi(curTrack, deg), buf);
      centerText(buf, x + CELL / 2, y + CELL / 2, 2, lit ? TFT_BLACK : TFT_WHITE, fill);
    }
    drawRing(s, COL_BG);
    return;
  }

  // GROOVE
  volatile Step& st = pattern[curTrack][s];
  bool head = uiPlaying && s == uiStep;
  uint16_t fill = st.on ? (tp[curTrack].muted ? TFT_DARKGREY : TRACK_COLORS[curTrack]) : COL_OFF;
  M5.Display.fillRoundRect(x, y, CELL, CELL, 6, fill);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(st.on ? TFT_BLACK : TFT_LIGHTGREY, fill);
  M5.Display.setCursor(x + 4, y + 4);
  M5.Display.print(STEP_KEYS[s]);
  if (st.on) {
    if (isDrum(curTrack)) {
      if (st.note != 0) { snprintf(buf, sizeof(buf), "%+d", st.note); centerText(buf, x + CELL / 2, y + 26, 1, TFT_BLACK, fill); }
    } else {
      noteName(noteMidi(curTrack, st.note), buf);
      centerText(buf, x + CELL / 2, y + 26, 2, TFT_BLACK, fill);
    }
  }
  if (s == lastStep && st.on) M5.Display.drawRoundRect(x + 2, y + 2, CELL - 4, CELL - 4, 5, TFT_BLACK);
  drawRing(s, head ? TFT_WHITE : COL_BG);
}

void drawGrid() { for (int s = 0; s < NUM_STEPS; s++) drawCell(s); }

// ---- right-hand overview ----
void drawMiniStep(int t, int s) {
  int x = MX + s * 6 + (s / 4) * 2;
  int y = MY + t * MROW + 10;
  bool head = uiPlaying && s == uiStep;
  bool cursor = (mode == MODE_KEYS && recOn && !uiPlaying && t == curTrack && s == recCursor);
  uint16_t c;
  if (pattern[t][s].on) c = head ? TFT_WHITE : (tp[t].muted ? TFT_DARKGREY : TRACK_COLORS[t]);
  else                  c = head ? 0x7BEF : COL_OFF;
  if (cursor) c = TFT_RED;
  M5.Display.fillRect(x, y, 5, 11, c);
}
void drawMiniTrack(int t) {
  int y = MY + t * MROW;
  M5.Display.fillRect(MX - 4, y, 320 - MX + 4, 9, COL_BG);
  M5.Display.setTextSize(1);
  bool cur = (t == curTrack);
  M5.Display.setTextColor(cur ? TRACK_COLORS[t] : TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(MX - 4, y);
  M5.Display.print(cur ? ">" : " ");
  M5.Display.print(TRACK_NAMES[t]);
  M5.Display.print(" ");
  M5.Display.print(SOUND_NAMES[t][tp[t].sound]);
  if (tp[t].muted) { M5.Display.setTextColor(TFT_RED, COL_BG); M5.Display.print(" M"); }
  for (int s = 0; s < NUM_STEPS; s++) drawMiniStep(t, s);
}
void drawAllMinis() { for (int t = 0; t < NT; t++) drawMiniTrack(t); }

// ---- header ----
void targetValue(char* buf, size_t n) {
  volatile TrackParams& p = tp[curTrack];
  switch (target) {
    case TG_TEMPO:  snprintf(buf, n, "%d BPM", bpm); break;
    case TG_SWING:  snprintf(buf, n, "%d%%", 50 + swing * 4); break;
    case TG_SCALE:  snprintf(buf, n, "%s", SCALES[scaleIdx].name); break;
    case TG_KEY:    snprintf(buf, n, "%s", NOTE_NAMES[keyRoot]); break;
    case TG_SOUND:  snprintf(buf, n, "%s", SOUND_NAMES[curTrack][p.sound]); break;
    case TG_TONE:   snprintf(buf, n, "%d/10", p.tone); break;
    case TG_DECAY:  snprintf(buf, n, "%d/10", p.decay); break;
    case TG_ECHO:   snprintf(buf, n, "%d/10", p.echo); break;
    case TG_VOLUME: snprintf(buf, n, "%d/10", p.vol); break;
    case TG_OCTAVE: snprintf(buf, n, "%+d", p.oct); break;
    case TG_NOTE: {
      int note = (lastStep >= 0 && pattern[curTrack][lastStep].on) ? pattern[curTrack][lastStep].note
                                                                    : brush[curTrack];
      if (isDrum(curTrack)) snprintf(buf, n, "pitch %+d", note);
      else { char nn[6]; noteName(noteMidi(curTrack, note), nn); snprintf(buf, n, "%s", nn); }
    } break;
  }
}

void drawHeader() {
  M5.Display.fillRect(0, 0, 320, 24, COL_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 5);
  M5.Display.setTextColor(uiPlaying ? TFT_GREEN : TFT_RED, COL_BG);
  M5.Display.print(uiPlaying ? "PLAY" : "STOP");
  M5.Display.setTextColor(TFT_WHITE, COL_BG);
  M5.Display.printf(" %d", bpm);
  M5.Display.setCursor(118, 5);
  M5.Display.setTextColor(mode == MODE_GROOVE ? TFT_ORANGE : TFT_SKYBLUE, COL_BG);
  M5.Display.print(mode == MODE_GROOVE ? "GRV" : "KEY");
  if (recOn) { M5.Display.setTextColor(TFT_RED, COL_BG); M5.Display.print(" R"); }

  char val[20];
  targetValue(val, sizeof(val));
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(MX - 4, 2);
  M5.Display.print(fnLayer ? "MENU: pick one" : TARGET_NAMES[target]);
  M5.Display.setTextColor(TFT_WHITE, COL_BG);
  M5.Display.setCursor(MX - 4, 13);
  M5.Display.print(val);
}

void drawFooter() {
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(MX - 4, 206); M5.Display.print("AC menu  M mute");
  M5.Display.setCursor(MX - 4, 216); M5.Display.print("%  -     /  +");
  const char* labels[3] = {"< TRACK", "PLAY  (hold: MODE)", "TRACK >"};
  const int cx[3] = {65, 160, 255};
  M5.Display.fillRect(0, 228, 320, 12, COL_BG);
  for (int i = 0; i < 3; i++) {
    M5.Display.setCursor(cx[i] - M5.Display.textWidth(labels[i]) / 2, 230);
    M5.Display.print(labels[i]);
  }
}

void drawAll() {
  M5.Display.fillScreen(COL_BG);
  drawHeader();
  drawGrid();
  drawAllMinis();
  drawFooter();
}

// ---- actions ----
void togglePlay() {
  sendEvent(uiPlaying ? EV_STOP : EV_PLAY);
  // the audio thread flips uiPlaying; give it a moment so the header is right
  uint32_t t0 = millis(); bool was = uiPlaying;
  while (uiPlaying == was && millis() - t0 < 100) delay(1);
  drawHeader(); drawGrid(); drawAllMinis();
}

void setMode(Mode m) {
  mode = m; fnLayer = false; flashKey = -1;
  if (mode == MODE_GROOVE) recOn = false;
  drawAll();
}

void selectTrack(int t) {
  curTrack = (t + NT) % NT;
  lastStep = -1;
  drawHeader(); drawGrid(); drawAllMinis();
}

void randomizeTrack(int t) {
  for (int s = 0; s < NUM_STEPS; s++) {
    bool on;
    switch (t) {
      case T_KICK:  on = (s % 4 == 0) || random(100) < 15; break;
      case T_SNARE: on = (s % 8 == 4) || random(100) < 8;  break;
      case T_HAT:   on = random(100) < 60; break;
      default:      on = random(100) < 40; break;
    }
    pattern[t][s].on = on;
    pattern[t][s].note = isDrum(t) ? 0 : (int8_t)random(0, 8);
  }
}

void adjustTarget(int dir) {
  volatile TrackParams& p = tp[curTrack];
  auto clampAdd = [](int v, int d, int lo, int hi) { v += d; return v < lo ? lo : (v > hi ? hi : v); };
  bool redrawGrid = false, redrawMini = false, preview = false;
  switch (target) {
    case TG_TEMPO:  bpm = clampAdd(bpm, dir * 2, 60, 200); break;
    case TG_SWING:  swing = clampAdd(swing, dir, 0, 6); break;
    case TG_SCALE:  scaleIdx = (scaleIdx + dir + NUM_SCALES) % NUM_SCALES; redrawGrid = true; break;
    case TG_KEY:    keyRoot = (keyRoot + dir + 12) % 12; redrawGrid = true; break;
    case TG_SOUND:  p.sound = (p.sound + dir + 3) % 3; redrawMini = true; preview = true; break;
    case TG_TONE:   p.tone  = clampAdd(p.tone, dir, 0, 10);  preview = true; break;
    case TG_DECAY:  p.decay = clampAdd(p.decay, dir, 0, 10); preview = true; break;
    case TG_ECHO:   p.echo  = clampAdd(p.echo, dir, 0, 10);  preview = true; break;
    case TG_VOLUME: p.vol   = clampAdd(p.vol, dir, 0, 10);   preview = true; break;
    case TG_OCTAVE: if (!isDrum(curTrack)) { p.oct = clampAdd(p.oct, dir, -2, 2); redrawGrid = true; preview = true; } break;
    case TG_NOTE: {
      int lo = isDrum(curTrack) ? -12 : 0, hi = isDrum(curTrack) ? 12 : maxDegree();
      int n;
      if (lastStep >= 0 && pattern[curTrack][lastStep].on) {
        n = clampAdd(pattern[curTrack][lastStep].note, dir, lo, hi);
        pattern[curTrack][lastStep].note = n;
        drawCell(lastStep);
      } else {
        n = clampAdd(brush[curTrack], dir, lo, hi);
      }
      brush[curTrack] = n;
      sendEvent(EV_NOTE, curTrack, n);
    } break;
  }
  if (preview && !uiPlaying) sendEvent(EV_NOTE, curTrack, isDrum(curTrack) ? 0 : brush[curTrack]);
  drawHeader();
  if (redrawGrid) drawGrid();
  if (redrawMini) drawMiniTrack(curTrack);
}

void runFn(int f) {
  fnLayer = false;
  switch (f) {
    case FN_RANDOM: randomizeTrack(curTrack); break;
    case FN_CLRTRK: for (int s = 0; s < NUM_STEPS; s++) pattern[curTrack][s].on = false; break;
    case FN_CLRALL: for (int t = 0; t < NT; t++) for (int s = 0; s < NUM_STEPS; s++) pattern[t][s].on = false; break;
    case FN_REC:
      recOn = !recOn; recCursor = 0;
      if (recOn && mode != MODE_KEYS) { mode = MODE_KEYS; }
      break;
    case FN_MODE: setMode(mode == MODE_GROOVE ? MODE_KEYS : MODE_GROOVE); return;
    default: target = (Target)f; break;   // FN_TEMPO..FN_OCTAVE map 1:1 to targets
  }
  drawHeader(); drawGrid(); drawAllMinis();
}

void stepKeyGroove(int s) {
  volatile Step& st = pattern[curTrack][s];
  if (st.on) {
    st.on = false;
    if (lastStep == s) lastStep = -1;
  } else {
    st.on = true;
    st.note = isDrum(curTrack) ? 0 : brush[curTrack];
    int prev = lastStep;
    lastStep = s;
    if (prev >= 0) drawCell(prev);
    if (!isDrum(curTrack)) target = TG_NOTE;      // ready to pick the pitch with % and /
    if (!uiPlaying) sendEvent(EV_NOTE, curTrack, st.note);
  }
  drawCell(s);
  drawMiniStep(curTrack, s);
  drawHeader();
}

void stepKeyKeys(int s) {
  int track = isDrum(curTrack) ? padTrack(s) : curTrack;
  int note  = isDrum(curTrack) ? padNote(s)  : keyDegree(s);

  if (recOn && uiPlaying) {
    sendEvent(EV_REC, track, note);                // audio thread quantises & writes
  } else {
    sendEvent(EV_NOTE, track, note);
    if (recOn) {                                   // step record while stopped
      pattern[track][recCursor].on = true;
      pattern[track][recCursor].note = note;
      int old = recCursor;
      recCursor = (recCursor + 1) % NUM_STEPS;
      drawMiniStep(track, old);
      drawMiniStep(curTrack, recCursor);
    }
  }
  int old = flashKey;
  flashKey = s; flashUntil = millis() + 140;
  if (old >= 0 && old != s) drawCell(old);
  drawCell(s);
}

void handlePanelKey(char k) {
  // top row
  switch (k) {
    case 'A': fnLayer = !fnLayer; drawHeader(); drawGrid(); return;
    case 'M': tp[curTrack].muted = !tp[curTrack].muted; drawGrid(); drawMiniTrack(curTrack); return;
    case '%': adjustTarget(-1); return;
    case '/': adjustTarget(+1); return;
  }
  for (int s = 0; s < NUM_STEPS; s++) {
    if (STEP_KEYS[s] != k) continue;
    if (fnLayer)                 runFn(s);
    else if (mode == MODE_GROOVE) stepKeyGroove(s);
    else                         stepKeyKeys(s);
    return;
  }
}

void loadDemo() {
  for (int t = 0; t < NT; t++) for (int s = 0; s < NUM_STEPS; s++) { pattern[t][s].on = false; pattern[t][s].note = 0; }
  for (int s : {0, 4, 8, 12})            pattern[T_KICK][s].on = true;
  for (int s : {4, 12})                  pattern[T_SNARE][s].on = true;
  for (int s = 2; s < NUM_STEPS; s += 4) pattern[T_HAT][s].on = true;
  const int bs[]  = {0, 3, 6, 8, 10, 14};
  const int bn[]  = {0, 0, 2, 0, 3, 4};
  for (int i = 0; i < 6; i++) { pattern[T_BASS][bs[i]].on = true; pattern[T_BASS][bs[i]].note = bn[i]; }
  const int ps[]  = {0, 3, 6, 10, 12};
  const int pn[]  = {7, 9, 8, 10, 7};
  for (int i = 0; i < 5; i++) { pattern[T_PLUCK][ps[i]].on = true; pattern[T_PLUCK][ps[i]].note = pn[i]; }
}

// =====================================================================
//  Boot: choose output
// =====================================================================
const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "CRASH (panic)";
    case ESP_RST_INT_WDT:  return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT:      return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "POWER DIP (brownout)";
    case ESP_RST_EXT:      return "reset button";
    default:               return "other";
  }
}

void bootMenu() {
  M5.Display.fillScreen(COL_BG);
  {
    char rr[48];
    snprintf(rr, sizeof(rr), "last reset: %s", resetReasonText());
    bool bad = esp_reset_reason() != ESP_RST_POWERON && esp_reset_reason() != ESP_RST_SW;
    centerText(rr, 160, 120, 1, bad ? TFT_RED : TFT_DARKGREY, COL_BG);
  }
  centerText("FACES GROOVEBOX", 160, 40, 2, TFT_ORANGE, COL_BG);
  centerText("Where should the sound go?", 160, 90, 1, TFT_LIGHTGREY, COL_BG);
  centerText("SPEAKER", 65, 180, 2, TFT_GREEN, COL_BG);
  centerText("BLUETOOTH", 255, 180, 2, TFT_SKYBLUE, COL_BG);
  centerText("v", 65, 210, 2, TFT_GREEN, COL_BG);
  centerText("v", 255, 210, 2, TFT_SKYBLUE, COL_BG);
  for (;;) {
    M5.update();
    if (M5.BtnA.wasPressed()) { useBluetooth = false; return; }
    if (M5.BtnC.wasPressed()) { useBluetooth = true;  return; }
    delay(10);
  }
}

void drawBtList(int cursor, const char* status) {
  M5.Display.fillScreen(COL_BG);
  centerText("BLUETOOTH", 160, 14, 2, TFT_SKYBLUE, COL_BG);
  centerText("Put your earbuds/speaker in pairing mode", 160, 36, 1, TFT_LIGHTGREY, COL_BG);
  int n = foundCount;
  if (n == 0) centerText("Searching...", 160, 110, 2, TFT_DARKGREY, COL_BG);
  for (int i = 0; i < n; i++) {
    int y = 52 + i * 20;
    bool sel = (i == cursor);
    if (sel) M5.Display.fillRect(20, y - 2, 280, 18, 0x2945);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? 0x2945 : COL_BG);
    M5.Display.setCursor(28, y);
    M5.Display.print(found[i].name);
  }
  centerText(status, 160, 214, 1, TFT_YELLOW, COL_BG);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_DARKGREY, COL_BG);
  M5.Display.setCursor(40, 230);  M5.Display.print("UP");
  M5.Display.setCursor(110, 230); M5.Display.print("CONNECT (hold: back)");
  M5.Display.setCursor(250, 230); M5.Display.print("DOWN");
}

void startBluetooth() {
  M5.Speaker.end();
  a2dp.set_data_callback_in_frames(btFrames);
  a2dp.set_ssid_callback(onSsid);
  a2dp.set_auto_reconnect(true);
  a2dp.set_local_name("Faces Groovebox");
  a2dp.start();                       // also retries the last device automatically

  int cursor = 0, lastCount = -1, lastCursor = -1;
  const char* status = "Looking for devices (last one reconnects by itself)";
  const char* lastStatus = nullptr;
  while (!a2dp.is_connected()) {
    M5.update();
    int n = foundCount;
    if (M5.BtnA.wasPressed() && n) cursor = (cursor + n - 1) % n;
    if (M5.BtnC.wasPressed() && n) cursor = (cursor + 1) % n;
    if (M5.BtnB.wasHold()) ESP.restart();          // back to the output menu
    if (M5.BtnB.wasClicked() && n) {
      memcpy(selAddr, found[cursor].addr, 6);
      haveSel = true;
      status = "Connecting... (can take ~10 s)";
    }
    if (n != lastCount || cursor != lastCursor || status != lastStatus) {
      drawBtList(cursor, status);
      lastCount = n; lastCursor = cursor; lastStatus = status;
    }
    delay(20);
  }
}

// =====================================================================
//  setup / loop
// =====================================================================
void setup() {
  M5.begin();
  Serial.begin(115200);
  Serial.printf("\n=== Faces Groovebox boot, last reset: %s ===\n", resetReasonText());
  M5.In_I2C.release();                       // give pins 21/22 to Arduino Wire
  Wire.begin(21, 22, 400000);
  pinMode(FACES_INT, INPUT_PULLUP);
  randomSeed(esp_random());

  engineInit();
  loadDemo();

  bootMenu();
  if (useBluetooth) {
    startBluetooth();
  } else {
    M5.Speaker.setVolume(200);
    xTaskCreatePinnedToCore(speakerTask, "audio", 8192, nullptr, 5, &audioTaskHandle, 0);
  }
  drawAll();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) selectTrack(curTrack - 1);
  if (M5.BtnC.wasPressed()) selectTrack(curTrack + 1);
  if (M5.BtnB.wasClicked()) togglePlay();
  if (M5.BtnB.wasHold())    setMode(mode == MODE_GROOVE ? MODE_KEYS : MODE_GROOVE);

  if (digitalRead(FACES_INT) == LOW) {
    Wire.requestFrom(FACES_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t k = Wire.read();
      if (k != 0x00 && k != 0xFF) handlePanelKey((char)k);
    }
  }

  // follow the playhead
  static int8_t drawnStep = -1;
  int8_t st = uiStep;
  if (st != drawnStep) {
    int8_t prev = drawnStep;
    drawnStep = st;
    if (mode == MODE_GROOVE && !fnLayer) {
      if (prev >= 0) drawCell(prev);
      if (st >= 0)   drawCell(st);
    }
    for (int t = 0; t < NT; t++) {
      if (prev >= 0) drawMiniStep(t, prev);
      if (st >= 0)   drawMiniStep(t, st);
    }
  }

  // pattern edited by live recording
  static uint32_t seenRev = 0;
  if (patternRev != seenRev) {
    seenRev = patternRev;
    if (mode == MODE_GROOVE && !fnLayer) drawGrid();
    drawAllMinis();
  }

  // un-flash pressed key
  if (flashKey >= 0 && millis() > flashUntil) {
    int k = flashKey; flashKey = -1;
    if (mode == MODE_KEYS && !fnLayer) drawCell(k);
  }

  // Bluetooth dropped? show it
  static bool btWarned = false;
  if (useBluetooth) {
    bool c = a2dp.is_connected();
    if (!c && !btWarned) { centerText("BT disconnected - reconnecting", 104, 223, 1, TFT_RED, COL_BG); btWarned = true; }
    if (c && btWarned)   { M5.Display.fillRect(0, 218, 205, 10, COL_BG); btWarned = false; }
  }

  // health report on the serial monitor every 5 s
  static uint32_t lastReport = 0;
  if (millis() - lastReport > 5000) {
    lastReport = millis();
    Serial.printf("[health] heap free %u (min %u)  loop stack left %u  audio stack left %u  %s\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                  audioTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(audioTaskHandle) : 0u,
                  useBluetooth ? (a2dp.is_connected() ? "BT connected" : "BT NOT connected") : "speaker");
  }

  delay(2);
}
