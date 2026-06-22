# BASS Player

A minimal native Windows player in C built on **BASS**, **BASS_FX** and **BASSenc**.
Status is shown in a real `SysListView32`, and everything is controlled from the keyboard.

## Features

- Playback via BASS with plugin loading (all `.dll` in the `plugins\` folder are loaded with `BASS_PluginLoad`, e.g. `bassflac.dll`, `bassopus.dll`, `bass_aac.dll`).
- Real-time tempo change with BASS_FX (`BASS_ATTRIB_TEMPO`) — without changing the pitch.
- Recording of what is currently playing to `recording.wav` with BASSenc (`BASS_Encode_Start` / `BASS_Encode_Stop`).
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
| `R` | Start recording → `recording_YYYYMMDD_HHMMSS.wav` |
| `E` | Stop recording |
| `Esc` | Quit |

> The list (`SysListView32`) is subclassed and gets focus automatically. Keys it doesn't use itself (e.g. arrow up/down) are passed on, so you can freely navigate the list.

## How to build

You need to fetch the BASS libraries yourself from un4seen (free for non-commercial use):

1. Download **BASS**, **BASS_FX** and **BASSenc** from https://www.un4seen.com
2. Place in the same folder as `player.c`:
   - Headers: `bass.h`, `bass_fx.h`, `bassenc.h`
   - Import libs for MinGW: `libbass.a`, `libbass_fx.a`, `libbassenc.a`
     (or use the `.lib` files; MinGW can often link against them directly, otherwise regenerate with `dlltool`)
   - The DLLs next to `player.exe`: `bass.dll`, `bass_fx.dll`, `bassenc.dll`
3. (Optional) Create the `plugins\` folder and put extra BASS add-on DLLs there.
4. Build:

```
build.bat
```

or

```
mingw32-make
```

### MSVC instead of MinGW
Use the import libs `bass.lib`, `bass_fx.lib`, `bassenc.lib`:

```
cl /O2 player.c /link bass.lib bass_fx.lib bassenc.lib comctl32.lib comdlg32.lib user32.lib gdi32.lib
```

## Notes

- The stream is created as a decoder channel (`BASS_STREAM_DECODE`) and wrapped in `BASS_FX_TempoCreate`, so the tempo can be changed live. `BASS_FX_FREESOURCE` ensures the source is freed automatically.
- The recording captures exactly the samples the playing channel delivers — including the tempo change — because the encoder is attached to the tempo stream.
- Each recording gets a unique name with date and time (`recording_20260620_143005.wav`), so earlier recordings are not overwritten. The file is written as 16-bit PCM WAV. The channel runs in float, so `BASS_ENCODE_FP_16BIT` converts to 16-bit integer during recording (otherwise it would have been a 32-bit float WAV). If you want MP3/OGG instead, BASSenc can be hooked up to a command-line encoder (`BASS_Encode_Start` with an encoder command).
