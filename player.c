/*
 * Minimal BASS player with tempo (BASS_FX), recording (BASSenc),
 * plugin loading and status display in a SysListView32.
 *
 * Keyboard:
 *   O           Open file
 *   Space       Play / Pause
 *   Enter       Play from the start
 *   Pause/Break Play / Pause (global hotkey, works even without focus)
 *   Left/Right  Seek -5 / +5 sec
 *   B           Play backwards (toggle)
 *   F11 / F12   Hold to fast rewind / forward (tape-style cue/review)
 *   T / Shift+T Tempo down / up;   Ctrl+T reset
 *   P / Shift+P Pitch down / up (1 semitone);   Ctrl+P reset
 *   Q / Shift+Q Frequency down / up (100 Hz);   Ctrl+Q reset to native
 *   Backspace   Reset tempo, pitch and frequency
 *   C           Command box (e.g. 30, +5, t75, p6, q44100, v150)
 *   R           Start recording what is playing (-> recording.wav)
 *   E           Stop recording
 *   1..9, 0          Cut EQ band 1 dB (1 = 80 Hz .. 0 = 14 kHz)  [10-band EQ]
 *   Shift+1..9, 0    Boost that EQ band 1 dB
 *   Ctrl+1..9, 0     Reset that EQ band to 0 dB
 *   Ctrl+I           Reset all EQ bands to flat
 *   Alt+F4         Quit
 *
 * Build with MinGW-w64 (see Makefile / build.bat).
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include "bass.h"
#include "bass_fx.h"
#include "bassenc.h"

#define APP_TITLE   "BASS PlAIer"
#define ID_LISTVIEW 1001
#define ID_TIMER    1
#define ID_HOTKEY_PAUSE 1
#define PLUGIN_DIR  "plugins"

/* ---- global state ---- */
static HWND      g_hList   = NULL;     /* SysListView32 */
static WNDPROC   g_listProc = NULL;    /* original listview procedure */
static DWORD     g_stream  = 0;        /* tempo stream (what we play) */
static DWORD     g_revStream = 0;      /* reverse decoder (direction control) */
static BOOL      g_reverse = FALSE;    /* playing backwards continuously? */
static BOOL      g_cueing  = FALSE;    /* fast cue/review while F11/F12 held? */
#define CUE_RATE  4.0f                 /* playback rate multiplier while cueing */
#define CUE_SLIDE 350                  /* ms to glide the rate up/down (spin-up feel) */
static float     g_tempo   = 0.0f;     /* tempo change in percent */
static float     g_pitch   = 0.0f;     /* pitch change in semitones */
static float     g_freq    = 0.0f;     /* playback sample rate in Hz */
static float     g_freqDef = 0.0f;     /* file's native sample rate (for reset) */
static BOOL      g_rec     = FALSE;    /* recording right now? */
static char      g_file[MAX_PATH] = "";
static char      g_recFile[MAX_PATH] = "";  /* current recording filename */

/* ---- 10-band peaking equalizer (BASS_FX) ---- */
#define EQ_BANDS    10
#define EQ_PER_ROW  5     /* shown 5 bands per list row */
static HFX        g_eqFx = 0;               /* peaking EQ effect on the stream */
static float      g_eqGain[EQ_BANDS] = {0}; /* per-band gain in dB; persists across files */
/* band centre frequencies (Hz) */
static const float g_eqFreq[EQ_BANDS] = {
    80.0f, 160.0f, 320.0f, 450.0f, 900.0f,
    1800.0f, 3600.0f, 7000.0f, 10000.0f, 14000.0f
};

/* ---- ListView rows ---- */
enum {
    ROW_FILE = 0, ROW_STATUS, ROW_POS, ROW_REMAIN, ROW_LEN,
    ROW_TEMPO, ROW_PITCH, ROW_FREQ, ROW_VOL, ROW_REC,
    ROW_EQ_LO,                     /* EQ bands 1-5 on one row, 6-10 on the next */
    ROW_EQ_HI,
    ROW_COUNT
};

/* column-0 label for each logical row */
static const char *g_rowName[ROW_COUNT] = {
    "File:", "Status:", "Position:", "Remaining:", "Length:",
    "Speed:", "Pitch:", "Frequency:", "Volume:", "Recording:",
    "Equalizer 1:", "Equalizer 2:"
};
/* current listview index of each logical row, or -1 while hidden */
static int  g_rowIdx[ROW_COUNT];
static BOOL g_showTempo = FALSE;   /* Tempo row hidden while tempo == 0 % */
static BOOL g_showPitch = FALSE;   /* Pitch row hidden while pitch == 0 */
static BOOL g_showEq    = FALSE;   /* EQ rows hidden while every band == 0 dB */

static BOOL handleKey(HWND hwnd, WPARAM key);   /* forward */
static void cueStop(void);                      /* forward (used by ListProc) */
static void updateList(void);                   /* forward (used by ListProc) */

/* Subclass: the list sends keys to our handler first.
 * Keys we don't use (e.g. arrow up/down) pass through to the list itself. */
static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (handleKey(GetParent(hwnd), wp))
            return 0;                 /* used -> swallow the key */
        /* otherwise: fall through to the list (navigation) */
    }
    if (msg == WM_KEYUP && (wp == VK_F11 || wp == VK_F12)) {
        cueStop();                    /* released -> stop fast cue/review */
        updateList();
        return 0;
    }
    /* Swallow WM_CHAR so the list's type-to-search doesn't move the
     * selection when our hotkeys (T, V, O, ...) are pressed. Arrow
     * navigation uses WM_KEYDOWN, so it is unaffected. */
    if (msg == WM_CHAR)
        return 0;
    return CallWindowProc(g_listProc, hwnd, msg, wp, lp);
}

static void lvSetText(int row, const char *txt)
{
    if (g_rowIdx[row] >= 0)
        ListView_SetItemText(g_hList, g_rowIdx[row], 1, (LPSTR)txt);
}

/* (re)insert the visible rows; the Tempo and EQ rows can be hidden.
 * Only called when the visible set actually changes, not every tick. */
static void rebuildRows(void)
{
    ListView_DeleteAllItems(g_hList);
    for (int r = 0; r < ROW_COUNT; r++) g_rowIdx[r] = -1;

    int idx = 0;
    for (int r = 0; r < ROW_COUNT; r++) {
        if (r == ROW_TEMPO && !g_showTempo) continue;
        if (r == ROW_PITCH && !g_showPitch) continue;
        if ((r == ROW_EQ_LO || r == ROW_EQ_HI) && !g_showEq) continue;
        LVITEM it = {0};
        it.mask = LVIF_TEXT;
        it.iItem = idx;
        it.iSubItem = 0;
        it.pszText = (LPSTR)g_rowName[r];
        ListView_InsertItem(g_hList, &it);
        ListView_SetItemText(g_hList, idx, 1, (LPSTR)"");
        g_rowIdx[r] = idx;
        idx++;
    }
}

static void buildList(HWND hwnd)
{
    g_hList = CreateWindowEx(0, WC_LISTVIEW, "",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
        0, 0, 480, 220, hwnd, (HMENU)ID_LISTVIEW,
        GetModuleHandle(NULL), NULL);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMN col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 130; col.pszText = (LPSTR)"Property";
    ListView_InsertColumn(g_hList, 0, &col);
    col.cx = 350; col.pszText = (LPSTR)"Value";
    ListView_InsertColumn(g_hList, 1, &col);

    rebuildRows();   /* Tempo and EQ rows start hidden (everything is zeroed) */

    /* subclass the list so it handles the keyboard itself */
    g_listProc = (WNDPROC)SetWindowLongPtr(g_hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
    SetFocus(g_hList);
}

/* ---- plugin loading: all .dll in ./plugins ---- */
static void loadPlugins(void)
{
    WIN32_FIND_DATA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.dll", PLUGIN_DIR);
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s\\%s", PLUGIN_DIR, fd.cFileName);
        BASS_PluginLoad(full, 0);   /* ignore errors for non-bass dlls */
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

/* ---- helper: format seconds as h:mm:ss (hours only when needed) ---- */
static void fmtTime(double secs, char *out, size_t n)
{
    if (secs < 0) secs = 0;
    int s = (int)(secs + 0.5);
    int h = s / 3600;
    if (h > 0)
        snprintf(out, n, "%d:%02d:%02d", h, (s % 3600) / 60, s % 60);
    else
        snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void stopEncode(void)
{
    if (g_rec && g_stream) {
        BASS_Encode_Stop(g_stream);
        g_rec = FALSE;
    }
}

static void closeStream(void)
{
    stopEncode();
    if (g_stream) {
        BASS_ChannelStop(g_stream);
        BASS_StreamFree(g_stream);   /* BASS_FX_FREESOURCE frees the whole chain */
        g_stream = 0;
        g_revStream = 0;             /* freed together with the stream */
        g_eqFx = 0;                  /* effect is freed together with the stream */
        g_cueing = FALSE;
    }
}

/* ---- attach the 10-band peaking EQ to the current stream ---- */
static void setupEq(void)
{
    if (!g_stream) return;
    g_eqFx = BASS_ChannelSetFX(g_stream, BASS_FX_BFX_PEAKEQ, 0);
    if (!g_eqFx) return;

    BASS_BFX_PEAKEQ eq;
    eq.fQ        = 0.0f;            /* 0 -> use fBandwidth instead */
    eq.fBandwidth = 1.0f;          /* one octave per band */
    eq.lChannel  = BASS_BFX_CHANALL;
    for (int b = 0; b < EQ_BANDS; b++) {
        eq.lBand   = b;
        eq.fCenter = g_eqFreq[b];
        eq.fGain   = g_eqGain[b];  /* reapply any previously chosen gains */
        BASS_FXSetParameters(g_eqFx, &eq);
    }
}

static void changeEqBand(int band, float delta)
{
    if (band < 0 || band >= EQ_BANDS) return;
    float g = g_eqGain[band] + delta;
    if (g < -15.0f) g = -15.0f;
    if (g >  15.0f) g =  15.0f;
    g_eqGain[band] = g;
    if (g_eqFx) {
        BASS_BFX_PEAKEQ eq;
        eq.lBand = band;
        BASS_FXGetParameters(g_eqFx, &eq);   /* keep centre/bandwidth, change gain */
        eq.fGain = g;
        BASS_FXSetParameters(g_eqFx, &eq);
    }
}

static void resetEq(void)
{
    for (int b = 0; b < EQ_BANDS; b++) {
        g_eqGain[b] = 0.0f;
        if (g_eqFx) {
            BASS_BFX_PEAKEQ eq;
            eq.lBand = b;
            BASS_FXGetParameters(g_eqFx, &eq);
            eq.fGain = 0.0f;
            BASS_FXSetParameters(g_eqFx, &eq);
        }
    }
}

/* any EQ band away from 0 dB? (decides whether the EQ rows are shown) */
static BOOL eqActive(void)
{
    for (int b = 0; b < EQ_BANDS; b++)
        if (g_eqGain[b] != 0.0f) return TRUE;
    return FALSE;
}

/* ---- open file and create tempo stream ---- */
static void openFile(HWND hwnd)
{
    char path[MAX_PATH] = "";
    OPENFILENAME ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Audio files\0*.mp3;*.ogg;*.wav;*.flac;*.aac;*.m4a;*.opus\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileName(&ofn)) return;

    closeStream();

    /* chain: file decoder -> reverse decoder -> tempo wrapper.
     * The reverse stage gives live forward/backward direction control. */
    DWORD dec = BASS_StreamCreateFile(FALSE, path, 0, 0,
        BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (!dec) {
        MessageBox(hwnd, "Could not open the file.", APP_TITLE, MB_ICONERROR);
        return;
    }
    g_revStream = BASS_FX_ReverseCreate(dec, 2.0f,
        BASS_STREAM_DECODE | BASS_FX_FREESOURCE);
    if (!g_revStream) {
        BASS_StreamFree(dec);
        MessageBox(hwnd, "Could not create reverse stream.", APP_TITLE, MB_ICONERROR);
        return;
    }
    g_stream = BASS_FX_TempoCreate(g_revStream, BASS_FX_FREESOURCE);
    if (!g_stream) {
        BASS_StreamFree(g_revStream);
        g_revStream = 0;
        MessageBox(hwnd, "Could not create tempo stream.", APP_TITLE, MB_ICONERROR);
        return;
    }

    lstrcpyn(g_file, path, MAX_PATH);
    g_tempo = 0.0f;
    g_pitch = 0.0f;
    g_reverse = FALSE;
    /* reverse streams default to playing backwards: force forward from the start */
    BASS_ChannelSetAttribute(g_revStream, BASS_ATTRIB_REVERSE_DIR, BASS_FX_RVS_FORWARD);
    BASS_ChannelSetPosition(g_stream, 0, BASS_POS_BYTE);
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, 0.0f);
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO_PITCH, 0.0f);
    /* default frequency = the file's native sample rate */
    g_freq = 0.0f;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_TEMPO_FREQ, &g_freq);
    if (g_freq <= 0.0f) {            /* fallback to the channel's reported rate */
        BASS_CHANNELINFO info;
        if (BASS_ChannelGetInfo(g_stream, &info)) g_freq = (float)info.freq;
    }
    g_freqDef = g_freq;
    setupEq();                 /* 10-band EQ (reapplies current gains) */
    BASS_ChannelPlay(g_stream, FALSE);
}

/* ---- seek relative ---- */
static void seekBy(double deltaSecs)
{
    if (!g_stream) return;
    QWORD pos = BASS_ChannelGetPosition(g_stream, BASS_POS_BYTE);
    double cur = BASS_ChannelBytes2Seconds(g_stream, pos);
    double nw = cur + deltaSecs;
    if (nw < 0) nw = 0;
    BASS_ChannelSetPosition(g_stream,
        BASS_ChannelSeconds2Bytes(g_stream, nw), BASS_POS_BYTE);
}

static void setTempo(float v)
{
    if (!g_stream) return;
    if (v < -95.0f)  v = -95.0f;   /* BASS_FX limit */
    if (v > 5000.0f) v = 5000.0f;
    g_tempo = v;
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, g_tempo);
}
static void changeTempo(float delta) { setTempo(g_tempo + delta); }
static void resetTempo(void)         { setTempo(0.0f); }

/* pitch in semitones (independent of tempo) */
static void setPitch(float v)
{
    if (!g_stream) return;
    if (v < -60.0f) v = -60.0f;
    if (v >  60.0f) v =  60.0f;
    g_pitch = v;
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO_PITCH, g_pitch);
}
static void changePitch(float delta) { setPitch(g_pitch + delta); }
static void resetPitch(void)         { setPitch(0.0f); }

/* playback sample rate in Hz (changes speed and pitch together) */
static void setFreq(float v)
{
    if (!g_stream) return;
    if (v < 1000.0f)   v = 1000.0f;
    if (v > 192000.0f) v = 192000.0f;
    g_freq = v;
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO_FREQ, g_freq);
}
static void changeFreq(float delta) { setFreq(g_freq + delta); }
static void resetFreq(void)         { if (g_freqDef > 0.0f) setFreq(g_freqDef); }

static void setVol(float v)   /* linear factor: 1.0 = 100 % */
{
    if (!g_stream) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 3.0f) v = 3.0f;   /* up to 300 % */
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, v);
}

static void changeVol(float delta)
{
    if (!g_stream) return;
    float v = 1.0f;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_VOL, &v);
    setVol(v + delta);
}

static void resetVol(void) { setVol(1.0f); }

/* ---- reverse playback & tape-style fast cue/review ---- */
static void setReverseDir(int dir)   /* +1 = forward, -1 = backward */
{
    if (g_revStream)
        BASS_ChannelSetAttribute(g_revStream, BASS_ATTRIB_REVERSE_DIR,
            dir < 0 ? BASS_FX_RVS_REVERSE : BASS_FX_RVS_FORWARD);
}

/* toggle continuous backwards playback at normal speed */
static void toggleReverse(void)
{
    if (!g_revStream) return;
    g_reverse = !g_reverse;
    if (!g_cueing) setReverseDir(g_reverse ? -1 : +1);
    if (BASS_ChannelIsActive(g_stream) != BASS_ACTIVE_PLAYING)
        BASS_ChannelPlay(g_stream, FALSE);
}

/* hold F11/F12: scrub fast in the given direction, like a tape recorder.
 * Speed comes from cranking the playback rate up, so the pitch rises too. */
static void cueStart(int dir)        /* +1 = forward, -1 = backward */
{
    if (!g_stream || g_cueing) return;
    g_cueing = TRUE;
    setReverseDir(dir);
    /* glide the rate up rather than jumping, so it spins up like a tape.
     * Slide the attribute directly so the user's g_freq setting is untouched. */
    BASS_ChannelSlideAttribute(g_stream, BASS_ATTRIB_TEMPO_FREQ,
        g_freq * CUE_RATE, CUE_SLIDE);
    if (BASS_ChannelIsActive(g_stream) != BASS_ACTIVE_PLAYING)
        BASS_ChannelPlay(g_stream, FALSE);
}

static void cueStop(void)
{
    if (!g_cueing) return;
    g_cueing = FALSE;
    setReverseDir(g_reverse ? -1 : +1);   /* back to the chosen direction */
    BASS_ChannelSlideAttribute(g_stream, BASS_ATTRIB_TEMPO_FREQ,
        g_freq, CUE_SLIDE);               /* glide back down to normal */
}

static void togglePlay(void)
{
    if (!g_stream) return;
    if (BASS_ChannelIsActive(g_stream) == BASS_ACTIVE_PLAYING)
        BASS_ChannelPause(g_stream);
    else
        BASS_ChannelPlay(g_stream, FALSE);
}

static void playFromStart(void)
{
    if (!g_stream) return;
    BASS_ChannelPlay(g_stream, TRUE);   /* TRUE = restart from the beginning */
}

/* ---- start recording what is playing ---- */
static void startEncode(HWND hwnd)
{
    if (!g_stream || g_rec) return;
    /* unique filename with date+time so nothing is overwritten */
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(g_recFile, sizeof(g_recFile),
        "recording_%04d%02d%02d_%02d%02d%02d.wav",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    /* BASS_ENCODE_PCM writes a WAV in the channel's own format. The
     * channel is float, so this produces a 32-bit float WAV. (Add
     * BASS_ENCODE_FP_16BIT to convert to 16-bit integer instead.) */
    if (BASS_Encode_Start(g_stream, g_recFile,
            BASS_ENCODE_PCM, NULL, NULL))
        g_rec = TRUE;
    else
        MessageBox(hwnd, "Could not start recording.", APP_TITLE, MB_ICONERROR);
}

/* ---- update ListView ---- */
static void updateList(void)
{
    char buf[256];

    /* show the Tempo row only when tempo != 0, the Pitch row only when
     * pitch != 0, and the EQ rows only when at least one band is boosted/cut.
     * Rebuild only on a real change. */
    BOOL wantTempo = (g_tempo != 0.0f);
    BOOL wantPitch = (g_pitch != 0.0f);
    BOOL wantEq    = eqActive();
    if (wantTempo != g_showTempo || wantPitch != g_showPitch || wantEq != g_showEq) {
        g_showTempo = wantTempo;
        g_showPitch = wantPitch;
        g_showEq    = wantEq;
        rebuildRows();
    }

    if (g_stream) {
        const char *base = strrchr(g_file, '\\');
        lvSetText(ROW_FILE, base ? base + 1 : g_file);

        DWORD st = BASS_ChannelIsActive(g_stream);
        snprintf(buf, sizeof(buf), "%s%s",
            st == BASS_ACTIVE_PLAYING ? "Playing" :
            st == BASS_ACTIVE_PAUSED  ? "Paused"  :
            st == BASS_ACTIVE_STALLED ? "Buffering" : "Stopped",
            g_cueing ? "  <<>> cue" : g_reverse ? "  << reverse" : "");
        lvSetText(ROW_STATUS, buf);

        char t[32];
        QWORD pos = BASS_ChannelGetPosition(g_stream, BASS_POS_BYTE);
        QWORD len = BASS_ChannelGetLength(g_stream, BASS_POS_BYTE);
        /* BASS reports time on the original timeline; scale it to real
         * playback time at the current tempo. +100 % = double speed
         * (half the time), -50 % = half speed (double the time). */
        double factor = (g_tempo + 100.0) / 100.0;
        if (factor < 0.01) factor = 0.01;        /* guard (tempo is clamped >= -90) */
        double posSec = BASS_ChannelBytes2Seconds(g_stream, pos) / factor;
        double lenSec = BASS_ChannelBytes2Seconds(g_stream, len) / factor;
        fmtTime(posSec, t, sizeof(t));
        lvSetText(ROW_POS, t);
        fmtTime(lenSec - posSec, t, sizeof(t));   /* time remaining */
        lvSetText(ROW_REMAIN, t);
        fmtTime(lenSec, t, sizeof(t));
        lvSetText(ROW_LEN, t);

        snprintf(buf, sizeof(buf), "%.0f %%", g_tempo);
        lvSetText(ROW_TEMPO, buf);

        snprintf(buf, sizeof(buf), "%g", g_pitch);
        lvSetText(ROW_PITCH, buf);

        snprintf(buf, sizeof(buf), "%.0f Hz", g_freq);
        lvSetText(ROW_FREQ, buf);

        float vol = 1.0f;
        BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_VOL, &vol);
        snprintf(buf, sizeof(buf), "%.0f %%", vol * 100.0f);
        lvSetText(ROW_VOL, buf);
    } else {
        lvSetText(ROW_FILE, "(none)");
        lvSetText(ROW_STATUS, "Stopped");
        lvSetText(ROW_POS, "0:00");
        lvSetText(ROW_REMAIN, "0:00");
        lvSetText(ROW_LEN, "0:00");
        lvSetText(ROW_TEMPO, "0 %");
        lvSetText(ROW_FREQ, "-");
        lvSetText(ROW_VOL, "100 %");
    }
    if (g_rec) {
        char rb[MAX_PATH + 8];
        snprintf(rb, sizeof(rb), "YES -> %s", g_recFile);
        lvSetText(ROW_REC, rb);
    } else {
        lvSetText(ROW_REC, "no");
    }

    /* EQ gains: 5 bands per row as "freq:gain" cells (persist always) */
    for (int r = 0; r < 2; r++) {
        char line[256];
        line[0] = 0;
        for (int i = 0; i < EQ_PER_ROW; i++) {
            int b = r * EQ_PER_ROW + i;
            char cell[32];
            if (g_eqFreq[b] >= 1000.0f)
                snprintf(cell, sizeof(cell), "%gk:%+.0f",
                         g_eqFreq[b] / 1000.0, g_eqGain[b]);
            else
                snprintf(cell, sizeof(cell), "%g:%+.0f",
                         (double)g_eqFreq[b], g_eqGain[b]);
            if (i) strncat(line, ",   ", sizeof(line) - strlen(line) - 1);
            strncat(line, cell, sizeof(line) - strlen(line) - 1);
        }
        lvSetText(ROW_EQ_LO + r, line);
    }
}

/* ---- small modal input dialog (returns the entered text) ---- */
static char g_inText[64];
static BOOL g_inDone, g_inOk;

static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND) {
        if (LOWORD(w) == IDOK) {
            GetWindowText(GetDlgItem(h, 100), g_inText, sizeof(g_inText));
            g_inOk = TRUE; g_inDone = TRUE;
        } else if (LOWORD(w) == IDCANCEL) {
            g_inDone = TRUE;
        }
        return 0;
    }
    if (m == WM_CLOSE) { g_inDone = TRUE; return 0; }
    return DefWindowProc(h, m, w, l);
}

static BOOL inputBox(HWND parent, const char *prompt, char *out, int outlen)
{
    static BOOL reg = FALSE;
    HINSTANCE hi = GetModuleHandle(NULL);
    if (!reg) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc   = InputProc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "InputBox";
        RegisterClass(&wc);
        reg = TRUE;
    }
    g_inDone = g_inOk = FALSE; g_inText[0] = 0;

    RECT pr; GetWindowRect(parent, &pr);
    HWND dlg = CreateWindowEx(WS_EX_DLGMODALFRAME, "InputBox", "Input",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        pr.left + 50, pr.top + 60, 300, 150, parent, NULL, hi, NULL);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND st = CreateWindow("STATIC", prompt, WS_CHILD | WS_VISIBLE,
        14, 14, 260, 20, dlg, NULL, hi, NULL);
    HWND ed = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        14, 40, 258, 24, dlg, (HMENU)100, hi, NULL);
    HWND ok = CreateWindow("BUTTON", "OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        56, 78, 80, 28, dlg, (HMENU)IDOK, hi, NULL);
    HWND ca = CreateWindow("BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        146, 78, 100, 28, dlg, (HMENU)IDCANCEL, hi, NULL);
    SendMessage(st, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(ed, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(ok, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(ca, WM_SETFONT, (WPARAM)font, TRUE);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(ed);

    MSG msg;
    while (!g_inDone && GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    DestroyWindow(dlg);
    SetForegroundWindow(parent);
    if (g_inOk && out) lstrcpyn(out, g_inText, outlen);
    return g_inOk;
}

/* jump to a specific time in minutes (decimals allowed: 3.5 = 3:30) */
static void seekMinutes(double minutes)
{
    if (!g_stream) return;
    double secs = minutes * 60.0;
    if (secs < 0) secs = 0;
    BASS_ChannelSetPosition(g_stream,
        BASS_ChannelSeconds2Bytes(g_stream, secs), BASS_POS_BYTE);
}

/* command box: a bare number jumps to that minute, a signed number seeks
 * relative, and t/p/q set tempo/pitch/freq.
 *   30        -> go to 30 minutes      +5  -> 5 minutes forward
 *   t75       -> tempo 75 %            -3  -> 3 minutes back
 *   p6        -> pitch +6 semitones    q44100 -> frequency 44100 Hz
 *   v150      -> volume 150 %                                                */
static void runCommand(HWND hwnd)
{
    if (!g_stream) return;
    char buf[64];
    if (!inputBox(hwnd, "Enter command:", buf, sizeof(buf)))
        return;
    const char *s = buf;
    while (*s == ' ') s++;
    switch (*s) {
    case 't': case 'T': setTempo((float)atof(s + 1)); break;
    case 'p': case 'P': setPitch((float)atof(s + 1)); break;
    case 'q': case 'Q': setFreq((float)atof(s + 1));  break;
    case 'v': case 'V': setVol((float)atof(s + 1) / 100.0f); break;  /* percent */
    case '+': case '-':
        seekBy(atof(s) * 60.0);   /* signed -> relative minutes */
        break;
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case '.':
        seekMinutes(atof(s));     /* bare number -> absolute minute */
        break;
    }
}

/* Returns TRUE if the key was used (and should be swallowed),
 * FALSE if the list should get it itself (e.g. arrow up/down for navigation). */
static BOOL handleKey(HWND hwnd, WPARAM key)
{
    BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL used  = TRUE;

    /* number keys drive the EQ directly, like the volume keys: the plain
     * number cuts its band, Shift+number boosts it, Ctrl+number resets it
     * to 0 dB. 1..9 -> bands 1..9, 0 -> the last band (14 kHz). Top row
     * and numpad both work. */
    if ((key >= '0' && key <= '9') ||
        (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)) {
        int d = (key >= VK_NUMPAD0) ? (int)(key - VK_NUMPAD0)
                                    : (int)(key - '0');
        int band = (d == 0) ? EQ_BANDS - 1 : d - 1;
        if (ctrl)
            changeEqBand(band, -g_eqGain[band]);   /* delta back to 0 dB */
        else
            changeEqBand(band, shift ? +1.0f : -1.0f);
        updateList();
        return TRUE;
    }

    switch (key) {
    case 'O':       openFile(hwnd); break;
    case VK_SPACE:  togglePlay(); break;
    case VK_RETURN: playFromStart(); break;
    case VK_LEFT:   seekBy(ctrl ? -30.0 : -5.0); break;
    case VK_RIGHT:  seekBy(ctrl ? +30.0 : +5.0); break;
    case 'C':       runCommand(hwnd); break;
    case 'I':       if (ctrl) resetEq(); else used = FALSE; break;  /* Ctrl+I: flatten EQ */
    case 'T':       if (ctrl)       resetTempo();          /* Ctrl+T: reset */
                    else            changeTempo(shift ? +1.0f : -1.0f);
                    break;                                  /* T down / Shift+T up */
    case 'P':       if (ctrl)       resetPitch();          /* Ctrl+P: reset */
                    else            changePitch(shift ? +1.0f : -1.0f);
                    break;                                  /* P down / Shift+P up (semitone) */
    case 'Q':       if (ctrl)       resetFreq();           /* Ctrl+Q: back to native */
                    else            changeFreq(shift ? +100.0f : -100.0f);
                    break;                                  /* Q down / Shift+Q up (100 Hz) */
    case 'V':       if (ctrl)       resetVol();
                    else if (shift)  changeVol(+0.01f);    /* Shift+V: up   */
                    else             changeVol(-0.01f);    /* V: down       */
                    break;
    case 'B':       toggleReverse(); break;  /* play backwards on/off */
    case VK_F11:    cueStart(-1); break;     /* hold = fast rewind  (tape style) */
    case VK_F12:    cueStart(+1); break;     /* hold = fast forward (tape style) */
    case 'R':       startEncode(hwnd); break;
    case 'E':       stopEncode(); break;
    case VK_BACK:   resetTempo(); resetPitch(); resetFreq(); break;  /* reset tempo/pitch/freq */
    default:        used = FALSE; break;   /* arrow up/down etc. -> list */
    }
    if (used) updateList();
    return used;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        buildList(hwnd);
        SetTimer(hwnd, ID_TIMER, 200, NULL);
        /* global Pause/Break hotkey: pauses even when we don't have focus */
        RegisterHotKey(hwnd, ID_HOTKEY_PAUSE, 0, VK_PAUSE);
        return 0;
    case WM_SIZE:
        MoveWindow(g_hList, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_hList);   /* always keep focus on the list */
        return 0;
    case WM_TIMER:
        updateList();
        return 0;
    case WM_KEYDOWN:
        handleKey(hwnd, wp);
        return 0;
    case WM_KEYUP:
        if (wp == VK_F11 || wp == VK_F12) { cueStop(); updateList(); }
        return 0;
    case WM_HOTKEY:
        if (wp == ID_HOTKEY_PAUSE) { togglePlay(); updateList(); }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        UnregisterHotKey(hwnd, ID_HOTKEY_PAUSE);
        closeStream();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)cmd;

    /* check runtime version of BASS_FX */
    if (HIWORD(BASS_FX_GetVersion()) != BASSVERSION) {
        MessageBox(NULL, "Wrong bass_fx.dll version.", APP_TITLE, MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    if (!BASS_Init(-1, 44100, 0, NULL, NULL)) {
        MessageBox(NULL, "BASS_Init failed.", APP_TITLE, MB_ICONERROR);
        return 1;
    }
    /* smaller buffer = lower latency (default is 500 ms).
     * BASS_CONFIG_BUFFER must be set BEFORE streams are created.
     * Increase it if the sound stutters. */
    BASS_SetConfig(BASS_CONFIG_BUFFER, 100);       /* playback buffer in ms */
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 10);  /* refill every 10 ms */
    loadPlugins();

    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "BassPlAIerWnd";
    RegisterClass(&wc);

    HWND hwnd = CreateWindow("BassPlAIerWnd", APP_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 430,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    SetFocus(hwnd);   /* the main window receives keys, not the list */

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    BASS_PluginFree(0);
    BASS_Free();
    return (int)m.wParam;
}
