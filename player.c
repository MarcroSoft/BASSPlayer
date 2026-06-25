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
 *   T / Shift+T Tempo down / up (-5 % / +5 %)
 *   I           Enter exact tempo value
 *   Ctrl+T / Backspace  Reset tempo to 0 %
 *   R           Start recording what is playing (-> recording.wav)
 *   E           Stop recording
 *   1..9, 0          Cut EQ band 1 dB (1 = 80 Hz .. 0 = 14 kHz)  [10-band EQ]
 *   Shift+1..9, 0    Boost that EQ band 1 dB
 *   Ctrl+I           Reset all EQ bands to flat
 *   Esc         Quit
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
static float     g_tempo   = 0.0f;     /* tempo change in percent */
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
    ROW_FILE = 0, ROW_STATUS, ROW_POS, ROW_LEN,
    ROW_TEMPO, ROW_VOL, ROW_REC,
    ROW_EQ_LO,                     /* EQ bands 1-5 on one row, 6-10 on the next */
    ROW_EQ_HI,
    ROW_COUNT
};

static BOOL handleKey(HWND hwnd, WPARAM key);   /* forward */

/* Subclass: the list sends keys to our handler first.
 * Keys we don't use (e.g. arrow up/down) pass through to the list itself. */
static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (handleKey(GetParent(hwnd), wp))
            return 0;                 /* used -> swallow the key */
        /* otherwise: fall through to the list (navigation) */
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
    ListView_SetItemText(g_hList, row, 1, (LPSTR)txt);
}

static void lvAddRow(int row, const char *name)
{
    LVITEM it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.iSubItem = 0;
    it.pszText = (LPSTR)name;
    ListView_InsertItem(g_hList, &it);
    ListView_SetItemText(g_hList, row, 1, (LPSTR)"");
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

    lvAddRow(ROW_FILE,   "File");
    lvAddRow(ROW_STATUS, "Status");
    lvAddRow(ROW_POS,    "Position");
    lvAddRow(ROW_LEN,    "Length");
    lvAddRow(ROW_TEMPO,  "Tempo");
    lvAddRow(ROW_VOL,    "Volume");
    lvAddRow(ROW_REC,    "Recording");

    /* two EQ rows: bands 1-5 and bands 6-10, each holding 5 "freq:gain" cells */
    lvAddRow(ROW_EQ_LO, "EQ 80-900 Hz");
    lvAddRow(ROW_EQ_HI, "EQ 1.8-14 kHz");

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
        BASS_StreamFree(g_stream);   /* BASS_FX_FREESOURCE frees the source */
        g_stream = 0;
        g_eqFx = 0;                  /* effect is freed together with the stream */
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

/* highlight the row holding a band so it is clear which one just changed */
static void selectEqBand(int band)
{
    if (band < 0 || band >= EQ_BANDS) return;
    int row = ROW_EQ_LO + band / EQ_PER_ROW;
    ListView_SetItemState(g_hList, row,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hList, row, FALSE);
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

    /* decoder channel -> tempo wrapper. Float for best quality. */
    DWORD dec = BASS_StreamCreateFile(FALSE, path, 0, 0,
        BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (!dec) {
        MessageBox(hwnd, "Could not open the file.", APP_TITLE, MB_ICONERROR);
        return;
    }
    g_stream = BASS_FX_TempoCreate(dec, BASS_FX_FREESOURCE);
    if (!g_stream) {
        BASS_StreamFree(dec);
        MessageBox(hwnd, "Could not create tempo stream.", APP_TITLE, MB_ICONERROR);
        return;
    }

    lstrcpyn(g_file, path, MAX_PATH);
    g_tempo = 0.0f;
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, g_tempo);
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

static void changeTempo(float delta)
{
    if (!g_stream) return;
    g_tempo += delta;
    if (g_tempo < -90.0f) g_tempo = -90.0f;   /* BASS_FX limit */
    if (g_tempo > 5000.0f) g_tempo = 5000.0f;
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, g_tempo);
}

static void resetTempo(void)
{
    g_tempo = 0.0f;
    if (g_stream) BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, 0.0f);
}

static void changeVol(float delta)
{
    if (!g_stream) return;
    float v = 1.0f;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_VOL, &v);
    v += delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 3.0f) v = 3.0f;   /* up to 300 % */
    BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, v);
}

static void resetVol(void)
{
    if (g_stream) BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_VOL, 1.0f);
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

    if (g_stream) {
        const char *base = strrchr(g_file, '\\');
        lvSetText(ROW_FILE, base ? base + 1 : g_file);

        DWORD st = BASS_ChannelIsActive(g_stream);
        lvSetText(ROW_STATUS,
            st == BASS_ACTIVE_PLAYING ? "Playing" :
            st == BASS_ACTIVE_PAUSED  ? "Paused"  :
            st == BASS_ACTIVE_STALLED ? "Buffering" : "Stopped");

        char t[32];
        QWORD pos = BASS_ChannelGetPosition(g_stream, BASS_POS_BYTE);
        QWORD len = BASS_ChannelGetLength(g_stream, BASS_POS_BYTE);
        /* BASS reports time on the original timeline; scale it to real
         * playback time at the current tempo. +100 % = double speed
         * (half the time), -50 % = half speed (double the time). */
        double factor = (g_tempo + 100.0) / 100.0;
        if (factor < 0.01) factor = 0.01;        /* guard (tempo is clamped >= -90) */
        fmtTime(BASS_ChannelBytes2Seconds(g_stream, pos) / factor, t, sizeof(t));
        lvSetText(ROW_POS, t);
        fmtTime(BASS_ChannelBytes2Seconds(g_stream, len) / factor, t, sizeof(t));
        lvSetText(ROW_LEN, t);

        snprintf(buf, sizeof(buf), "%+.0f %%", g_tempo);
        lvSetText(ROW_TEMPO, buf);

        float vol = 1.0f;
        BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_VOL, &vol);
        snprintf(buf, sizeof(buf), "%.0f %%", vol * 100.0f);
        lvSetText(ROW_VOL, buf);
    } else {
        lvSetText(ROW_FILE, "(none)");
        lvSetText(ROW_STATUS, "Stopped");
        lvSetText(ROW_POS, "0:00");
        lvSetText(ROW_LEN, "0:00");
        lvSetText(ROW_TEMPO, "+0 %");
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
            if (i) strncat(line, "   ", sizeof(line) - strlen(line) - 1);
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
    HWND dlg = CreateWindowEx(WS_EX_DLGMODALFRAME, "InputBox", "Go to",
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

/* enter an exact tempo value in percent (e.g. -10 or 25) */
static void inputTempo(HWND hwnd)
{
    if (!g_stream) return;
    char buf[64];
    if (inputBox(hwnd, "Tempo in percent (e.g. -10 or 20):", buf, sizeof(buf))) {
        float v = (float)atof(buf);
        if (v < -90.0f) v = -90.0f;
        if (v > 5000.0f) v = 5000.0f;
        g_tempo = v;
        BASS_ChannelSetAttribute(g_stream, BASS_ATTRIB_TEMPO, g_tempo);
    }
}

/* jump to a specific time (minutes, decimals allowed: 3.5 = 3:30) */
static void gotoMinutes(HWND hwnd)
{
    if (!g_stream) return;
    char buf[64];
    if (inputBox(hwnd, "Go to (minutes, e.g. 3 or 3.5):", buf, sizeof(buf))) {
        double secs = atof(buf) * 60.0;
        if (secs < 0) secs = 0;
        BASS_ChannelSetPosition(g_stream,
            BASS_ChannelSeconds2Bytes(g_stream, secs), BASS_POS_BYTE);
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
     * number cuts its band, Shift+number boosts it. 1..9 -> bands 1..9,
     * 0 -> the last band (16 kHz). Top row and numpad both work. */
    if (!ctrl && ((key >= '0' && key <= '9') ||
                  (key >= VK_NUMPAD0 && key <= VK_NUMPAD9))) {
        int d = (key >= VK_NUMPAD0) ? (int)(key - VK_NUMPAD0)
                                    : (int)(key - '0');
        int band = (d == 0) ? EQ_BANDS - 1 : d - 1;
        changeEqBand(band, shift ? +1.0f : -1.0f);
        selectEqBand(band);          /* highlight the band that changed */
        updateList();
        return TRUE;
    }

    switch (key) {
    case 'O':       openFile(hwnd); break;
    case VK_SPACE:  togglePlay(); break;
    case VK_RETURN: playFromStart(); break;
    case VK_LEFT:   seekBy(ctrl ? -30.0 : -5.0); break;
    case VK_RIGHT:  seekBy(ctrl ? +30.0 : +5.0); break;
    case 'G':       gotoMinutes(hwnd); break;
    case 'I':       if (ctrl) resetEq();          /* Ctrl+I: flatten EQ */
                    else      inputTempo(hwnd);
                    break;
    case 'T':       if (ctrl)       resetTempo();          /* Ctrl+T: reset */
                    else            changeTempo(shift ? +5.0f : -5.0f);
                    break;                                  /* T down / Shift+T up */
    case 'V':       if (ctrl)       resetVol();
                    else if (shift)  changeVol(+0.05f);    /* Shift+V: up   */
                    else             changeVol(-0.05f);    /* V: down       */
                    break;
    case 'R':       startEncode(hwnd); break;
    case 'E':       stopEncode(); break;
    case VK_BACK:   resetTempo(); break;     /* reset tempo to 0 % */
//    case VK_ESCAPE: DestroyWindow(hwnd); break;
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
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 340,
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
