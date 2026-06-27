# BASS PlAIer

A minimal native Windows player in C built on **BASS**, **BASS_FX** and **BASSenc**.
Status is shown in a real `SysListView32`, and everything is controlled from the keyboard.

Please Note: Virtually all of this code is written by AI, so there might be some bugs here and there. The program has been tested and found to be working.

## Features

- Playback via BASS with plugin loading (all `.dll` files in the `plugins\` folder are loaded with `BASS_PluginLoad`, e.g. `bassflac.dll`, `bassopus.dll`, `bass_aac.dll`).
- Real-time tempo change with BASS_FX (`BASS_ATTRIB_TEMPO`) — without changing the pitch.
- Reverse playback (`BASS_FX_ReverseCreate`) and tape-recorder-style fast cue/review: hold `F11`/`F12` to scrub backward/forward at speed.
- Independent pitch shift in semitones (`BASS_ATTRIB_TEMPO_PITCH`) and playback sample-rate / frequency control in 100 Hz steps (`BASS_ATTRIB_TEMPO_FREQ`).
- Command box (`C`): type `30` to jump to 30 minutes, `+5` / `-3` to seek relative, `t75` to set tempo, `p6` for pitch, `q44100` for frequency, `v150` for volume.
- 10-band graphic equalizer with BASS_FX (`BASS_FX_BFX_PEAKEQ`) at centres 80, 160, 320, 450, 900 Hz, 1.8, 3.6, 7, 10, 14 kHz. The band gains are also applied to the recording.
- Recording of what is currently playing to a `.wav` file with BASSenc (`BASS_Encode_Start` / `BASS_Encode_Stop`).
- Time, status, length, tempo etc. are shown continuously in a `SysListView32` (report view).

## Keyboard shortcuts

| Key | Action |
|------|----------|
| `O` | Open file |
| `Space` | Play / pause |
| `Enter` | Play from the start |
| `Pause`/`Break` | Play / pause — **global** hotkey, works even when the window is not focused |
| `←` / `→` | Seek −5 / +5 sec |
| `Ctrl+←` / `Ctrl+→` | Seek −30 / +30 sec |
| `B` | Play backwards (toggle) |
| `F11` / `F12` (hold) | Fast rewind / forward like a tape recorder — releases back to normal |
| `↑` / `↓` | Navigate the list |
| `T` / `Shift+T` | Tempo down / up |
| `Ctrl+T` | Reset tempo to 0 % |
| `P` / `Shift+P` | Pitch down / up (1 semitone) |
| `Ctrl+P` | Reset pitch to 0 |
| `Q` / `Shift+Q` | Frequency down / up (100 Hz) |
| `Ctrl+Q` | Reset frequency to the file's native rate |
| `Backspace` | Reset tempo, pitch and frequency at once |
| `C` | Command box (`30` = go to 30 min, `+5`/`-3` = seek relative, `t75`, `p6`, `q44100`, `v150`) |
| `V` / `Shift+V` | Volume down / up |
| `Ctrl+V` | Reset volume (100 %) |
| `1`…`9`, `0` | Cut EQ band 1 dB (`1` = 80 Hz … `0` = 14 kHz); top row or numpad |
| `Shift`+`1`…`9`, `0` | Boost that EQ band 1 dB (range ±15 dB) |
| `Ctrl`+`1`…`9`, `0` | Reset that EQ band to 0 dB |
| `I` / `Shift+I` | Cut / boost all EQ bands 1 dB |
| `Ctrl+I` | Reset all EQ bands to flat |
| `R` | Start recording |
| `E` | Stop recording |
| `Alt+F4` | Quit |

> **Equalizer:** the two `EQ …` rows show all 10 bands as `freq:gain` cells — bands 1-5 (80-900 Hz) on the first row, 6-10 (1.8-14 kHz) on the second. Each band has its own number key (`1` = 80 Hz … `0` = 14 kHz): press it to cut that band, or `Shift`+the number to boost it — just like the volume keys. `Ctrl`+the number resets that single band, and `Ctrl+I` flattens everything. The setting persists when you open another file. The two EQ rows are only shown while at least one band is non-zero, and the `Tempo` row only while the tempo isn't 0 %.

> The list (`SysListView32`) is subclassed and gets focus automatically. Keys it doesn't use itself (e.g. arrow up/down) are passed on, so you can freely navigate the list.

## Download

Prebuilt x64 packages are attached to each [GitHub release](../../releases): a portable
`.zip` and a Windows installer (`BASSPlAIer-Setup.exe`, built with NSIS) that adds a
Start-menu shortcut, an optional desktop shortcut and an uninstaller. The BASS DLLs are bundled in.

## How to build

You need to fetch the BASS libraries yourself from un4seen (free for non-commercial use):

1. Download **BASS**, **BASS_FX** and **BASSenc** from https://www.un4seen.com
2. Place in the same folder as `player.c`:
   - Headers: `bass.h`, `bass_fx.h`, `bassenc.h`
   - Import libs: `bass.lib`, `bass_fx.lib`, `bassenc.lib`
   - The DLLs next to `BASSPlAIer.exe`: `bass.dll`, `bass_fx.dll`, `bassenc.dll`
3. (Optional) Create the `plugins\` folder and put extra BASS add-on DLLs there.
4. Build with MSVC (from a Developer Command Prompt):

```
build.bat
```

which runs:

```
cl /O2 player.c /link bass.lib bass_fx.lib bassenc.lib comctl32.lib comdlg32.lib user32.lib gdi32.lib /OUT:BASSPlAIer.exe
```

## Notes

- The stream is created as a decoder channel (`BASS_STREAM_DECODE`) and wrapped in `BASS_FX_TempoCreate`, so the tempo can be changed live. `BASS_FX_FREESOURCE` ensures the source is freed automatically.
- The recording captures exactly the samples the playing channel delivers — including the tempo change — because the encoder is attached to the tempo stream.
- Each recording gets a unique name with date and time (`recording_20260620_143005.wav`), so earlier recordings are not overwritten. The file is written as a WAV in the channel's own format — the channel runs in float, so the result is a 32-bit float WAV. Add `BASS_ENCODE_FP_16BIT` to `BASS_Encode_Start` if you want 16-bit integer instead. If you want MP3/OGG instead, BASSenc can be hooked up to a command-line encoder (`BASS_Encode_Start` with an encoder command).
