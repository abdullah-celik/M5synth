# M5synth – Faces Groovebox

A pocket groovebox and synth for the **M5Stack Faces** kit (ESP32 Core + Calculator panel).
It has 7 synthesized instruments, a 16-step sequencer and a live keyboard mode.
Sound comes from the built-in speaker or goes over **Bluetooth** to headphones and speakers.

▶ [`media/demo.wav`](media/demo.wav): the demo pattern, rendered by the same audio engine.

## Features

- **Real-time synth engine** at 44.1 kHz (mono), running on its own core
- **7 tracks**, 3 sound presets each, plus Tone / Decay / Volume / Echo controls:

  | Track | Presets |
  |---|---|
  | KICK  | 808 · Punch · Chip |
  | SNARE | Snare · Clap · Chip |
  | HAT   | Closed · Open · Metal |
  | BASS  | Acid (resonant filter) · Sub · Square |
  | LEAD  | Square · Pulse (vibrato) · Saw (detuned) |
  | PLUCK | Guitar · Harp · Muted (Karplus-Strong) |
  | FM    | E.Piano · Bell · Metal (2-operator FM) |

- **GROOVE mode**: 16-step sequencer with per-step notes, swing, scales and key
- **KEYS mode**: play notes or drum pads live. With REC on, playing while the loop runs records quantized notes into it; playing while stopped fills the pattern one step at a time.
- **Output chosen at boot**: built-in speaker or Bluetooth A2DP (pairs with headphones such as the Soundcore Q20i)
- Tempo-synced echo, crash and reset diagnostics on screen and on the serial port

## Hardware

- M5Stack Faces (Core ESP32, no PSRAM needed) with the **Calculator** panel
- **USB-A → USB-C cable.** The older M5Stack Core boards don't power up or flash from USB-C ↔ USB-C cables, because their USB-C port lacks the CC resistors that C-to-C chargers look for.

## Build & flash (PlatformIO)

1. Install VS Code and the **PlatformIO IDE** extension
2. Linux only, run once and then log out and back in:
   ```bash
   sudo usermod -a -G dialout $USER
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   sudo apt remove brltty   # Ubuntu: it grabs CH9102/CH340 USB-serial chips
   ```
3. Open this folder in VS Code, then click **→ Upload** in the status bar.
   `platformio.ini` pulls in M5Unified and ESP32-A2DP and selects the `huge_app` partition, because the firmware is about 1.3 MB with Bluetooth.

The warning `AudioTools library is not included` comes from inside ESP32-A2DP and can be ignored.

## Controls

**Boot menu:** button **A** = speaker, **C** = Bluetooth.
In the Bluetooth list: A/C select, B connects, hold B to go back.

**Calculator panel**

```
 [AC] menu   [M] mute   [%] value -   [/] value +
 [7 ] [8 ] [9 ] [* ]
 [4 ] [5 ] [6 ] [- ]    4x4 block = the 16 steps (GROOVE)
 [1 ] [2 ] [3 ] [+ ]             or notes / drum pads (KEYS)
 [. ] [0 ] [` ] [= ]
```

**AC menu.** It opens on the 4×4 block; pick what `%` and `/` change, or run an action:

```
 TEMPO   SWING   SCALE    KEY
 SOUND   TONE    DECAY    ECHO
 VOLUME  NOTE    OCTAVE   RANDOM
 CLR TRK CLR ALL REC      MODE
```

**Core buttons:** **A** previous track · **B** play/stop (hold: switch GROOVE ↔ KEYS) · **C** next track

Tips:
- In GROOVE mode on a synth track, placing a step selects NOTE, so `%` and `/` change that step's pitch right away.
- In KEYS mode, the 16 keys play notes of the current scale (bottom-left is lowest, root notes are shaded). On a drum track they become pads: rows from bottom to top are kick, snare, hat and high hat, each in 4 pitches.
- M (mute) silences only the track's pattern. You can still play the track live.
- The Calculator panel only reports key presses, not releases, so every note rings out according to its Decay setting.

## Project layout

```
src/main.cpp                  current firmware (Groovebox v2)
examples/key_tester/          prints the code of every panel key (hardware check)
examples/groovebox_v1/        first version: 4-track drum machine, pre-rendered samples
media/demo.wav                audio preview
```

To build an example, copy its `main.cpp` over `src/main.cpp`.

## Troubleshooting

- **The device restarts by itself:** the boot screen shows `last reset: …`.
  - *CRASH*: copy the decoded backtrace from the Serial Monitor.
  - *POWER DIP (brownout)*: the USB supply or a worn battery can't deliver the current. Try another port or charger, or lower the volume.
- **The Bluetooth device isn't listed:** put it in pairing mode and turn off Bluetooth on your phone, so the headphones don't reconnect there instead.
- **Serial health report:** every 5 s the firmware prints free heap and the stack headroom of the main and audio tasks.

## Roadmap

- Save and load patterns to flash
- Pattern chaining (song mode)
- Fire as a wireless motion controller (ESP-NOW)
- Clean line-out through an I2S DAC (PCM5102A)
