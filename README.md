# BASS Player

A minimal native Windows player in C built on **BASS**, **BASS_FX** and **BASSenc**.
Status is shown in a real `SysListView32`, and everything is controlled from the keyboard.

Please Note: Virtually all of this code is written by AI, so there might be some bugs here and there. The program has been tested and found to be working.

## Features

- Playback via BASS with plugin loading (all `.dll` files in the `plugins\` folder are loaded with `BASS_PluginLoad`, e.g. `bassflac.dll`, `bassopus.dll`, `bass_aac.dll`).
- Real-time tempo change with BASS_FX (`BASS_ATTRIB_TEMPO`) — without changing the pitch.
- 10-band graphic equalizer with BASS_FX (`BASS_FX_BFX_PEAKEQ`) at the ISO octave centres (31 Hz … 16 kHz). The band gains are also applied to the recording.
- Recording of what is currently playing to a `.wav` file with BASSenc (`BASS_Encode_Start` / `BASS_Encode_Stop`).
- Time, status, length, tempo etc. are shown continuously in a `SysListView32` (report view).

## Keyboard shortcuts

| Key | Action |
|------|----------|
| `O` | Open file |
| `Space` | Play / pause |
| `S` | Stop (rewind to start) |
| `←` / `→` | Seek −5 / +5 sec |
| `Ctrl+←` / `Ctrl+→` | Seek −30 / +30 sec |
| `G` | Go to time (enter minutes, e.g. `3` or `3.5`) |
| `↑` / `↓` | Navigate the list |
| `T` / `Shift+T` | Tempo down / up (−5 % / +5 %) |
| `Ctrl+T` | Reset tempo |
| `I` | Enter exact tempo value (e.g. `-10` or `20`) |
| `V` / `Shift+V` | Volume down / up (−5 % / +5 %) |
| `Ctrl+V` | Reset volume (100 %) |
| `1`…`9`, `0` | Cut EQ band 1 dB (`1` = 31 Hz … `0` = 16 kHz); top row or numpad |
| `Shift`+`1`…`9`, `0` | Boost that EQ band 1 dB (range ±15 dB) |
| `Backspace` | Reset all EQ bands to flat |
| `R` | Start recording |
| `E` | Stop recording |
| `Esc` | Quit |

> **Equalizer:** the 10 `EQ …` rows in the list are the equalizer bands. Each band has its own number key (`1` = 31 Hz … `0` = 16 kHz): press it to cut that band, or `Shift`+the number to boost it — just like the volume keys. `Backspace` flattens everything. The setting persists when you open another file.

> The list (`SysListView32`) is subclassed and gets focus automatically. Keys it doesn't use itself (e.g. arrow up/down) are passed on, so you can freely navigate the list.

## How to build

You need to fetch the BASS libraries yourself from un4seen (free for non-commercial use):

1. Download **BASS**, **BASS_FX** and **BASSenc** from https://www.un4seen.com
2. Place in the same folder as `player.c`:
   - Headers: `bass.h`, `bass_fx.h`, `bassenc.h`
   - Import libs: `bass.lib`, `bass_fx.lib`, `bassenc.lib`
   - The DLLs next to `BASSPlayer.exe`: `bass.dll`, `bass_fx.dll`, `bassenc.dll`
3. (Optional) Create the `plugins\` folder and put extra BASS add-on DLLs there.
4. Build with MSVC (from a Developer Command Prompt):

```
build.bat
```

which runs:

```
cl /O2 player.c /link bass.lib bass_fx.lib bassenc.lib comctl32.lib comdlg32.lib user32.lib gdi32.lib /OUT:BASSPlayer.exe
```

## Notes

- The stream is created as a decoder channel (`BASS_STREAM_DECODE`) and wrapped in `BASS_FX_TempoCreate`, so the tempo can be changed live. `BASS_FX_FREESOURCE` ensures the source is freed automatically.
- The recording captures exactly the samples the playing channel delivers — including the tempo change — because the encoder is attached to the tempo stream.
- Each recording gets a unique name with date and time (`recording_20260620_143005.wav`), so earlier recordings are not overwritten. The file is written as a WAV in the channel's own format — the channel runs in float, so the result is a 32-bit float WAV. Add `BASS_ENCODE_FP_16BIT` to `BASS_Encode_Start` if you want 16-bit integer instead. If you want MP3/OGG instead, BASSenc can be hooked up to a command-line encoder (`BASS_Encode_Start` with an encoder command).
