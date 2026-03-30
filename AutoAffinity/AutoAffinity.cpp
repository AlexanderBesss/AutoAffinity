#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#include <windows.h>
#include <shellapi.h>
#include "resource.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <thread>

#define WM_TRAYICON  (WM_APP + 1)
#define WM_LOG_MSG   (WM_APP + 2)

#define IDM_SHOW     1001
#define IDM_EXIT     1002
#define IDM_STARTUP  1003
#define IDM_ABOUT    1004

#define ID_EDIT_LOG  100
#define ID_STATUSBAR 101
#define TRAY_ICON_ID 1

static HINSTANCE         g_hInst        = nullptr;
static HWND              g_hwnd         = nullptr;
static HWND              g_hEdit        = nullptr;
static HWND              g_hStatus      = nullptr;
static NOTIFYICONDATAW   g_nid          = {};
static std::atomic<bool> g_running      { true };
static DWORD_PTR         g_affinityMask = 0;
static HFONT             g_hFont        = nullptr;

static const std::vector<std::wstring> k_targets = { L"cs2.exe", L"dota2.exe" };
static const DWORD k_pollMs = 20000;

// ---------------------------------------------------------------------------
// Affinity mask
// ---------------------------------------------------------------------------

static DWORD_PTR BuildAffinityMask() {
    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    return sys & ~(DWORD_PTR)0x3; // exclude logical CPUs 0 and 1
}

// ---------------------------------------------------------------------------
// Thread-safe logging
// ---------------------------------------------------------------------------

static void Log(const wchar_t* level, const std::wstring& msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wostringstream ss;
    ss << L"[" << std::setfill(L'0')
       << std::setw(2) << st.wHour   << L":"
       << std::setw(2) << st.wMinute << L":"
       << std::setw(2) << st.wSecond << L"] "
       << level << L" " << msg;
    PostMessageW(g_hwnd, WM_LOG_MSG, 0,
        reinterpret_cast<LPARAM>(new std::wstring(ss.str())));
}

// ---------------------------------------------------------------------------
// Process management
// ---------------------------------------------------------------------------

static void CheckAndApply(DWORD pid, const wchar_t* name, bool isNew) {
    HANDLE h = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) {
        std::wostringstream s;
        s << name << L" PID " << pid << L" - OpenProcess failed (" << GetLastError() << L")";
        Log(L"[!]", s.str());
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
        std::wostringstream s;
        s << name << L" (PID " << pid << L")"
          << (isNew ? L" - new process" : L" - drift detected, reapplying");
        Log(L"[*]", s.str());
    }

    if (priorityDrifted || isNew) {
        if (SetPriorityClass(h, HIGH_PRIORITY_CLASS))
            Log(L"[+]", L"    Priority  -> High");
        else {
            std::wostringstream e;
            e << L"    SetPriorityClass failed (" << GetLastError() << L")";
            Log(L"[!]", e.str());
        }
    }

    if (affinityDrifted || isNew) {
        if (SetProcessAffinityMask(h, g_affinityMask)) {
            std::wostringstream a;
            a << L"    Affinity  -> 0x" << std::hex << g_affinityMask
              << L" (CPUs 0+1 excluded)";
            Log(L"[+]", a.str());
        } else {
            std::wostringstream e;
            e << L"    SetProcessAffinityMask failed (" << GetLastError() << L")";
            Log(L"[!]", e.str());
        }
    }

    CloseHandle(h);
}

static std::map<std::wstring, std::set<DWORD>> SnapshotTargets(
    const std::vector<std::wstring>& targets)
{
    std::map<std::wstring, std::set<DWORD>> result;
    for (const auto& t : targets) result[t];

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
        do {
            for (auto& [name, pids] : result)
                if (_wcsicmp(pe.szExeFile, name.c_str()) == 0)
                    pids.insert(pe.th32ProcessID);
        } while (Process32NextW(hSnap, &pe));

    CloseHandle(hSnap);
    return result;
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
                static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"AutoAffinity");
        }
        RegCloseKey(hKey);
    }
}

static void RegisterStartup() {
    // This is called on first run or every run to ensure it's there
    // For now we keep it simple - we can make it toggleable
    if (!IsStartupRegistered()) {
        UpdateStartupRegistration(true);
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void WatcherThread() {
    std::map<std::wstring, std::set<DWORD>> known;
    for (const auto& t : k_targets) known[t];

    while (g_running) {
        auto current = SnapshotTargets(k_targets);

        for (auto& [name, pids] : current) {
            auto& knownPids = known[name];

            for (DWORD pid : pids) {
                bool isNew = !knownPids.count(pid);
                if (isNew) knownPids.insert(pid);
                CheckAndApply(pid, name.c_str(), isNew);
            }

            for (auto it = knownPids.begin(); it != knownPids.end(); )
                if (!pids.count(*it)) {
                    std::wostringstream s;
                    s << name << L" (PID " << *it << L") exited.";
                    Log(L"[-]", s.str());
                    it = knownPids.erase(it);
                } else {
                    ++it;
                }
        }

        for (DWORD i = 0; i < k_pollMs / 200 && g_running; ++i)
            Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// GUI helpers
// ---------------------------------------------------------------------------

static void AppendLog(const std::wstring& line) {
    // Keep at most 1000 lines - trim the first 200 when exceeded
    if (SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0) > 1000) {
        LRESULT trimTo = SendMessageW(g_hEdit, EM_LINEINDEX, 200, 0);
        SendMessageW(g_hEdit, EM_SETSEL, 0, trimTo);
        SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }

    LRESULT len = SendMessageW(g_hEdit, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g_hEdit, EM_SETSEL, len, len);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hEdit, EM_SCROLLCARET, 0, 0);
}

static void ShowMainWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
}

static void LayoutChildren(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT sr;
    GetWindowRect(g_hStatus, &sr);
    int statusH = sr.bottom - sr.top;
    SetWindowPos(g_hEdit, nullptr, 0, 0,
        rc.right, rc.bottom - statusH, SWP_NOZORDER);
    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
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

        g_hStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_STATUSBAR, g_hInst, nullptr);
        SendMessageW(g_hStatus, SB_SETTEXTW, 0,
            (LPARAM)L"Watching: cs2.exe, dota2.exe  |  Poll: 20s");

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
        return 0;
    }

    case WM_INITMENUPOPUP: {
        HMENU hMenu = (HMENU)wp;
        // Check "Run on startup" if it's registered
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
                AppendMenuW(hMenu, MF_STRING, IDM_SHOW,
                    IsWindowVisible(hwnd) ? L"Hide window" : L"Show window");

                // Add startup toggle to tray as well
                AppendMenuW(hMenu, MF_STRING | (IsStartupRegistered() ? MF_CHECKED : MF_UNCHECKED),
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
            if (IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                ShowMainWindow();
            }
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
                L"Author: Besss", 
                L"About AutoAffinity", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_LOG_MSG: {
        auto* s = reinterpret_cast<std::wstring*>(lp);
        AppendLog(*s);
        delete s;
        return 0;
    }

    case WM_DESTROY:
        g_running = false;
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

int main() {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"AutoAffinityMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    g_affinityMask = BuildAffinityMask();
    if (g_affinityMask == 0) {
        MessageBoxW(nullptr,
            L"Affinity mask is zero - fewer than 3 logical CPUs.",
            L"AutoAffinity", MB_ICONERROR);
        CloseHandle(hMutex);
        return 1;
    }

    RegisterStartup();

    g_hInst = GetModuleHandleW(nullptr);

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
    wcsncpy_s(g_nid.szTip, L"AutoAffinity - watching", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    {
        std::wostringstream ss;
        ss << L"AutoAffinity started  |  affinity mask 0x"
           << std::hex << g_affinityMask
           << L"  |  CPUs 0+1 excluded  |  poll 20s";
        AppendLog(ss.str());
        AppendLog(L"Double-click the tray icon to open this window.");
    }

    std::thread watcher(WatcherThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    if (watcher.joinable()) watcher.join();
    CloseHandle(hMutex);
    return 0;
}
