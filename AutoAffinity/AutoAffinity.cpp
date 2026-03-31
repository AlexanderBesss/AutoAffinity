#include <windows.h>
#include <shellapi.h>
#include "resource.h"
#include <tlhelp32.h>

#pragma function(memset)
extern "C" void* memset(void* dest, int c, size_t count) {
    unsigned char* d = static_cast<unsigned char*>(dest);
    while (count--) *d++ = static_cast<unsigned char>(c);
    return dest;
}

#define WM_TRAYICON  (WM_APP + 1)

#define IDM_SHOW     1001
#define IDM_EXIT     1002
#define IDM_STARTUP  1003
#define IDM_ABOUT    1004

#define ID_EDIT_LOG   100
#define ID_STATUSBAR  101
#define ID_TIMER_POLL 102
#define TRAY_ICON_ID  1

static HINSTANCE       g_hInst        = nullptr;
static HWND            g_hwnd         = nullptr;
static HWND            g_hEdit        = nullptr;
static HWND            g_hStatus      = nullptr;
static NOTIFYICONDATAW g_nid          = {};
static DWORD_PTR       g_affinityMask = 0;
static HFONT           g_hFont        = nullptr;

static const wchar_t* const k_targets[] = { L"cs2.exe", L"dota2.exe", L"FPSAimTrainer-Win64-Shipping.exe", L"AimLab_tb.exe" };
static constexpr int   k_numTargets = ARRAYSIZE(k_targets);
static const UINT      k_pollMs     = 20000;
static constexpr int   k_maxPids    = 8; // max tracked PIDs per target

struct TargetState {
    DWORD pids[k_maxPids];
    int   count;
};
static TargetState g_known[k_numTargets] = {};

// ---------------------------------------------------------------------------
// Affinity mask
// ---------------------------------------------------------------------------

static DWORD_PTR BuildAffinityMask() {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    return sys & ~(DWORD_PTR)0x3; // exclude logical CPUs 0 and 1
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static void AppendLog(const wchar_t* line) {
    // Keep at most 1000 lines - trim the first 200 when exceeded
    if (SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0) > 1000) {
        LRESULT trimTo = SendMessageW(g_hEdit, EM_LINEINDEX, 200, 0);
        SendMessageW(g_hEdit, EM_SETSEL, 0, trimTo);
        SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }

    LRESULT len = SendMessageW(g_hEdit, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g_hEdit, EM_SETSEL, len, len);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
}

static void Log(const wchar_t* level, const wchar_t* msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[512];
    wsprintfW(buf, L"[%02u:%02u:%02u] %s %s",
        st.wHour, st.wMinute, st.wSecond, level, msg);
    AppendLog(buf);
}

// ---------------------------------------------------------------------------
// Process management
// ---------------------------------------------------------------------------

static void CheckAndApply(DWORD pid, const wchar_t* name, bool isNew) {
    HANDLE h = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        wchar_t s[256];
        wsprintfW(s, L"%s PID %lu - OpenProcess failed (%lu)", name, pid, GetLastError());
        Log(L"[!]", s);
        return;
    }

    bool priorityDrifted = GetPriorityClass(h) != HIGH_PRIORITY_CLASS;
    DWORD_PTR curAff = 0, sysAff = 0;
    GetProcessAffinityMask(h, &curAff, &sysAff);
    bool affinityDrifted = curAff != g_affinityMask;

    if (!isNew && !priorityDrifted && !affinityDrifted) {
        CloseHandle(h);
        return;
    }

    {
        wchar_t s[256];
        wsprintfW(s, L"%s (PID %lu)%s", name, pid,
            isNew ? L" - new process" : L" - drift detected, reapplying");
        Log(L"[*]", s);
    }

    if (priorityDrifted || isNew) {
        if (SetPriorityClass(h, HIGH_PRIORITY_CLASS))
            Log(L"[+]", L"    Priority  -> High");
        else {
            wchar_t e[128];
            wsprintfW(e, L"    SetPriorityClass failed (%lu)", GetLastError());
            Log(L"[!]", e);
        }
    }

    if (affinityDrifted || isNew) {
        if (SetProcessAffinityMask(h, g_affinityMask)) {
            wchar_t a[128];
            wsprintfW(a, L"    Affinity  -> 0x%IX (CPUs 0+1 excluded)",
                g_affinityMask);
            Log(L"[+]", a);
        } else {
            wchar_t e[128];
            wsprintfW(e, L"    SetProcessAffinityMask failed (%lu)", GetLastError());
            Log(L"[!]", e);
        }
    }

    CloseHandle(h);
}

static bool ContainsPid(const TargetState& ts, DWORD pid) {
    for (int i = 0; i < ts.count; ++i)
        if (ts.pids[i] == pid) return true;
    return false;
}

static void WatcherTick() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    // Collect current PIDs per target
    TargetState current[k_numTargets] = {};

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
        do {
            for (int i = 0; i < k_numTargets; ++i)
                if (lstrcmpiW(pe.szExeFile, k_targets[i]) == 0
                    && current[i].count < k_maxPids)
                    current[i].pids[current[i].count++] = pe.th32ProcessID;
        } while (Process32NextW(hSnap, &pe));

    CloseHandle(hSnap);

    for (int i = 0; i < k_numTargets; ++i) {
        // Check new/existing PIDs
        for (int j = 0; j < current[i].count; ++j) {
            DWORD pid = current[i].pids[j];
            bool isNew = !ContainsPid(g_known[i], pid);
            if (isNew && g_known[i].count < k_maxPids)
                g_known[i].pids[g_known[i].count++] = pid;
            CheckAndApply(pid, k_targets[i], isNew);
        }

        // Remove exited PIDs
        for (int j = g_known[i].count - 1; j >= 0; --j) {
            if (!ContainsPid(current[i], g_known[i].pids[j])) {
                wchar_t s[256];
                wsprintfW(s, L"%s (PID %lu) exited.", k_targets[i], g_known[i].pids[j]);
                Log(L"[-]", s);
                g_known[i].pids[j] = g_known[i].pids[--g_known[i].count];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Startup registration
// ---------------------------------------------------------------------------

static bool IsStartupRegistered() {
    HKEY hKey;
    bool registered = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        if (RegQueryValueExW(hKey, L"AutoAffinity", nullptr, nullptr,
            reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS)
        {
            registered = true;
        }
        RegCloseKey(hKey);
    }
    return registered;
}

static void UpdateStartupRegistration(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (enable) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            RegSetValueExW(hKey, L"AutoAffinity", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(path),
                static_cast<DWORD>((lstrlenW(path) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"AutoAffinity");
        }
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------
// GUI helpers
// ---------------------------------------------------------------------------

static void ShowMainWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static constexpr int k_statusH = 22;

static void LayoutChildren(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    SetWindowPos(g_hEdit, nullptr, 0, 0,
        rc.right, rc.bottom - k_statusH, SWP_NOZORDER);
    SetWindowPos(g_hStatus, nullptr, 0, rc.bottom - k_statusH,
        rc.right, k_statusH, SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_EDIT_LOG, g_hInst, nullptr);

        HDC hdc = GetDC(nullptr);
        g_hFont = CreateFontW(
            -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        ReleaseDC(nullptr, hdc);
        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        wchar_t statusText[512] = L"Watching: ";
        for (int i = 0; i < k_numTargets; ++i) {
            if (i > 0) lstrcatW(statusText, L", ");
            lstrcatW(statusText, k_targets[i]);
        }
        lstrcatW(statusText, L"  |  Poll: 20s");

        g_hStatus = CreateWindowExW(
            WS_EX_STATICEDGE, L"STATIC",
            statusText,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_STATUSBAR, g_hInst, nullptr);
        SendMessageW(g_hStatus, WM_SETFONT,
            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        // Create main menu
        HMENU hMenuBar = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        AppendMenuW(hFileMenu, MF_STRING, IDM_EXIT, L"E&xit");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, L"&File");

        HMENU hSettingsMenu = CreatePopupMenu();
        AppendMenuW(hSettingsMenu, MF_STRING, IDM_STARTUP, L"&Run on startup");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hSettingsMenu, L"&Settings");

        HMENU hHelpMenu = CreatePopupMenu();
        AppendMenuW(hHelpMenu, MF_STRING, IDM_ABOUT, L"&About");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelpMenu, L"&Help");

        SetMenu(hwnd, hMenuBar);

        SetTimer(hwnd, ID_TIMER_POLL, k_pollMs, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wp == ID_TIMER_POLL)
            WatcherTick();
        return 0;

    case WM_INITMENUPOPUP: {
        HMENU hMenu = (HMENU)wp;
        CheckMenuItem(hMenu, IDM_STARTUP, MF_BYCOMMAND | (IsStartupRegistered() ? MF_CHECKED : MF_UNCHECKED));
        return 0;
    }

    case WM_SIZE:
        if (g_hEdit && g_hStatus)
            LayoutChildren(hwnd);
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowMainWindow();
        } else if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            if (hMenu) {
                bool startupOn = IsStartupRegistered();
                AppendMenuW(hMenu, MF_STRING, IDM_SHOW,
                    IsWindowVisible(hwnd) ? L"Hide window" : L"Show window");
                AppendMenuW(hMenu, MF_STRING | (startupOn ? MF_CHECKED : MF_UNCHECKED),
                    IDM_STARTUP, L"Run on startup");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                               pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(hMenu);
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_SHOW:
            if (IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
            else
                ShowMainWindow();
            break;

        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;

        case IDM_STARTUP: {
            bool current = IsStartupRegistered();
            UpdateStartupRegistration(!current);
            break;
        }

        case IDM_ABOUT:
            MessageBoxW(hwnd,
                L"AutoAffinity v1.0\n\n"
                L"Automatically sets CPU affinity for specific games to improve performance on hybrid architectures.\n"
                L"Excludes CPUs 0 and 1 by default.\n\n"
                L"Author: Besss\n"
                L"https://github.com/AlexanderBesss/AutoAffinity",
                L"About AutoAffinity", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_POLL);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd) {
    g_hInst = hInstance;
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"AutoAffinityMutex");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    g_affinityMask = BuildAffinityMask();
    if (g_affinityMask == 0) {
        MessageBoxW(nullptr,
            L"Affinity mask is zero - fewer than 3 logical CPUs.",
            L"AutoAffinity", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = g_hInst;
    wc.hIcon         = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
                           IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm       = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
                           IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AutoAffinityWnd";
    if (!RegisterClassExW(&wc)) { CloseHandle(hMutex); return 1; }

    g_hwnd = CreateWindowExW(
        0, L"AutoAffinityWnd", L"AutoAffinity",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 440,
        nullptr, nullptr, g_hInst, nullptr);
    if (!g_hwnd) { CloseHandle(hMutex); return 1; }

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = TRAY_ICON_ID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = wc.hIconSm;
    lstrcpynW(g_nid.szTip, L"AutoAffinity - watching", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    {
        wchar_t buf[256];
        wsprintfW(buf, L"AutoAffinity started  |  affinity mask 0x%IX  |  CPUs 0+1 excluded  |  poll 20s",
            g_affinityMask);
        AppendLog(buf);
        AppendLog(L"Double-click the tray icon to open this window.");
    }

    WatcherTick(); // run immediately on start, then every k_pollMs via timer

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(hMutex);
    return 0;
}

extern "C" void __stdcall Entry() {
    ExitProcess((UINT)wWinMain(GetModuleHandleW(nullptr), nullptr, nullptr, 0));
}
