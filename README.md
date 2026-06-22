# BASS Player

En minimal native Windows-afspiller i C bygget på **BASS**, **BASS_FX** og **BASSenc**.
Status vises i en rigtig `SysListView32`, og alt styres fra tastaturet.

## Funktioner

- Afspilning via BASS med pluginindlæsning (alle `.dll` i mappen `plugins\` indlæses med `BASS_PluginLoad`, fx `bassflac.dll`, `bassopus.dll`, `bass_aac.dll`).
- Tempoændring i realtid med BASS_FX (`BASS_ATTRIB_TEMPO`) — uden at ændre toneleje.
- Optagelse af det der afspilles lige nu til `recording.wav` med BASSenc (`BASS_Encode_Start` / `BASS_Encode_Stop`).
- Tid, status, længde, tempo m.m. vises løbende i en `SysListView32` (report-view).

## Tastaturgenveje

| Tast | Handling |
|------|----------|
| `O` | Åbn fil |
| `Mellemrum` | Play / pause |
| `S` | Stop (spol til start) |
| `←` / `→` | Spol −5 / +5 sek |
| `Ctrl+←` / `Ctrl+→` | Spol −30 / +30 sek |
| `G` | Gå til tidspunkt (indtast minutter, fx `3` eller `3.5`) |
| `↑` / `↓` | Naviger i listen |
| `T` / `Shift+T` | Tempo ned / op (−5 % / +5 %) |
| `Ctrl+T` | Nulstil tempo |
| `I` | Indtast præcis tempoværdi (fx `-10` eller `20`) |
| `V` / `Shift+V` | Volumen ned / op (−5 % / +5 %) |
| `Ctrl+V` | Nulstil volumen (100 %) |
| `R` | Start optagelse → `recording_ÅÅÅÅMMDD_TTMMSS.wav` |
| `E` | Stop optagelse |
| `Esc` | Afslut |

> Listen (`SysListView32`) er subklasset og får automatisk fokus. Taster den ikke selv bruger (fx pil op/ned) sendes videre, så du frit kan navigere i listen.

## Sådan bygger du

Du skal selv hente BASS-bibliotekerne fra un4seen (gratis til ikke-kommerciel brug):

1. Hent **BASS**, **BASS_FX** og **BASSenc** fra https://www.un4seen.com
2. Læg i samme mappe som `player.c`:
   - Headere: `bass.h`, `bass_fx.h`, `bassenc.h`
   - Import-libs til MinGW: `libbass.a`, `libbass_fx.a`, `libbassenc.a`
     (eller brug `.lib`-filerne; MinGW kan ofte linke mod dem direkte, ellers gen-generér med `dlltool`)
   - DLL'erne ved siden af `player.exe`: `bass.dll`, `bass_fx.dll`, `bassenc.dll`
3. (Valgfrit) Opret mappen `plugins\` og læg ekstra BASS-add-on-DLL'er der.
4. Byg:

```
build.bat
```

eller

```
mingw32-make
```

### MSVC i stedet for MinGW
Brug import-libsene `bass.lib`, `bass_fx.lib`, `bassenc.lib`:

```
cl /O2 player.c /link bass.lib bass_fx.lib bassenc.lib comctl32.lib comdlg32.lib user32.lib gdi32.lib
```

## Bemærkninger

- Streamen oprettes som dekoderkanal (`BASS_STREAM_DECODE`) og pakkes ind i `BASS_FX_TempoCreate`, så tempo kan ændres live. `BASS_FX_FREESOURCE` sørger for at kilden frigives automatisk.
- Optagelsen fanger præcis de samples den afspillede kanal leverer — inkl. tempoændringen — fordi encoderen hænges på tempo-streamen.
- Hver optagelse får et unikt navn med dato og tid (`recording_20260620_143005.wav`), så tidligere optagelser ikke overskrives. Filen skrives som 16-bit PCM-WAV. Kanalen kører i float, så `BASS_ENCODE_FP_16BIT` konverterer til 16-bit heltal under optagelsen (ellers var det blevet en 32-bit float-WAV). Vil du have MP3/OGG i stedet, kan BASSenc kobles til en kommandolinje-encoder (`BASS_Encode_Start` med en encoder-kommando).
