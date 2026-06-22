/*
 * Minimal BASS player with tempo (BASS_FX), recording (BASSenc),
 * plugin loading and status display in a SysListView32.
 *
 * Keyboard:
 *   O           Open file
 *   Space       Play / Pause
 *   S           Stop (rewind to start)
 *   Left/Right  Seek -5 / +5 sec
 *   Up/Down     Tempo +5 % / -5 %
 *   T           Reset tempo to 0 %
 *   R           Start recording what is playing (-> recording.wav)
 *   E           Stop recording
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

#define APP_TITLE   "BASS Player"
#define ID_LISTVIEW 1001
#define ID_TIMER    1
#define PLUGIN_DIR  "plugins"

/* ---- global state ---- */
static HWND      g_hList   = NULL;     /* SysListView32 */
static WNDPROC   g_listProc = NULL;    /* original listview procedure */
static DWORD     g_stream  = 0;        /* tempo stream (what we play) */
static float     g_tempo   = 0.0f;     /* tempo change in percent */
static BOOL      g_rec     = FALSE;    /* recording right now? */
static char      g_file[MAX_PATH] = "";
static char      g_recFile[MAX_PATH] = "";  /* current recording filename */

/* ---- ListView rows ---- */
enum {
    ROW_FILE = 0, ROW_STATUS, ROW_POS, ROW_LEN,
    ROW_TEMPO, ROW_VOL, ROW_REC, ROW_COUNT
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
    col.cx = 150; col.pszText = (LPSTR)"Property";
    ListView_InsertColumn(g_hList, 0, &col);
    col.cx = 320; col.pszText = (LPSTR)"Value";
    ListView_InsertColumn(g_hList, 1, &col);

    lvAddRow(ROW_FILE,   "File");
    lvAddRow(ROW_STATUS, "Status");
    lvAddRow(ROW_POS,    "Position");
    lvAddRow(ROW_LEN,    "Length");
    lvAddRow(ROW_TEMPO,  "Tempo");
    lvAddRow(ROW_VOL,    "Volume");
    lvAddRow(ROW_REC,    "Recording");

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

/* ---- helper: format seconds as m:ss ---- */
static void fmtTime(double secs, char *out, size_t n)
{
    if (secs < 0) secs = 0;
    int s = (int)(secs + 0.5);
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

static void stopPlay(void)
{
    if (!g_stream) return;
    BASS_ChannelStop(g_stream);
    BASS_ChannelSetPosition(g_stream, 0, BASS_POS_BYTE);
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
    /* BASS_ENCODE_PCM writes a WAV. The channel is float, so
     * BASS_ENCODE_FP_16BIT converts to 16-bit integer (not float). */
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
        fmtTime(BASS_ChannelBytes2Seconds(g_stream, pos), t, sizeof(t));
        lvSetText(ROW_POS, t);
        fmtTime(BASS_ChannelBytes2Seconds(g_stream, len), t, sizeof(t));
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

    switch (key) {
    case 'O':       openFile(hwnd); break;
    case VK_SPACE:  togglePlay(); break;
    case 'S':       stopPlay(); break;
    case VK_LEFT:   seekBy(ctrl ? -30.0 : -5.0); break;
    case VK_RIGHT:  seekBy(ctrl ? +30.0 : +5.0); break;
    case 'G':       gotoMinutes(hwnd); break;
    case 'I':       inputTempo(hwnd); break;
    case 'T':       if (ctrl)       resetTempo();
                    else if (shift)  changeTempo(+5.0f);   /* Shift+T: up   */
                    else             changeTempo(-5.0f);   /* T: down       */
                    break;
    case 'V':       if (ctrl)       resetVol();
                    else if (shift)  changeVol(+0.05f);    /* Shift+V: up   */
                    else             changeVol(-0.05f);    /* V: down       */
                    break;
    case 'R':       startEncode(hwnd); break;
    case 'E':       stopEncode(); break;
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
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
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
    wc.lpszClassName = "BassPlayerWnd";
    RegisterClass(&wc);

    HWND hwnd = CreateWindow("BassPlayerWnd", APP_TITLE,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 280,
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
