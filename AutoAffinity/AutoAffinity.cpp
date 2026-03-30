#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#include <windows.h>
#include <shellapi.h>
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

// Custom messages
#define WM_TRAYICON  (WM_APP + 1)
#define WM_LOG_MSG   (WM_APP + 2)

// Menu IDs
#define IDM_SHOW     1001
#define IDM_EXIT     1002

// Control IDs
#define ID_EDIT_LOG  100
#define ID_STATUSBAR 101

// Tray
#define TRAY_ICON_ID 1

static HWND              g_hwnd         = nullptr;
static HWND              g_hEdit        = nullptr;
static HWND              g_hStatus      = nullptr;
static NOTIFYICONDATAW   g_nid          = {};
static std::atomic<bool> g_running      { true };
static DWORD_PTR         g_affinityMask = 0;
static HFONT             g_hFont        = nullptr;

// ---------------------------------------------------------------------------
// Affinity mask
// ---------------------------------------------------------------------------

static DWORD_PTR BuildAffinityMask() {
    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    return sys & ~(DWORD_PTR)0x3; // exclude logical CPUs 0 and 1
}

// ---------------------------------------------------------------------------
// Thread-safe logging — posts a heap-allocated string to the main window
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
    const std::vector<const wchar_t*>& targets)
{
    std::map<std::wstring, std::set<DWORD>> result;
    for (auto t : targets) result[t];

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

static void RegisterStartup() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"AutoAffinity", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void WatcherThread() {
    const std::vector<const wchar_t*> targets = { L"cs2.exe", L"dota2.exe" };
    const DWORD pollMs = 20000;

    std::map<std::wstring, std::set<DWORD>> known;
    for (auto t : targets) known[t];

    while (g_running) {
        auto current = SnapshotTargets(targets);

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

        for (DWORD i = 0; i < pollMs / 200 && g_running; ++i)
            Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// GUI helpers
// ---------------------------------------------------------------------------

static void AppendLog(const std::wstring& line) {
    // Keep at most 1000 lines — trim the first 200 when exceeded
    int lineCount = (int)SendMessageW(g_hEdit, EM_GETLINECOUNT, 0, 0);
    if (lineCount > 1000) {
        LRESULT trimTo = SendMessageW(g_hEdit, EM_LINEINDEX, 200, 0);
        SendMessageW(g_hEdit, EM_SETSEL, 0, trimTo);
        SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }

    std::wstring text = line + L"\r\n";
    LRESULT len = SendMessageW(g_hEdit, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(g_hEdit, EM_SETSEL, len, len);
    SendMessageW(g_hEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
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
        // Multiline read-only edit for the log
        g_hEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_EDIT_LOG,
            GetModuleHandleW(nullptr), nullptr);

        // Consolas 9pt
        HDC hdc = GetDC(nullptr);
        int lpy = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(nullptr, hdc);
        g_hFont = CreateFontW(
            -MulDiv(9, lpy, 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        // Status bar
        g_hStatus = CreateWindowExW(
            0, STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_STATUSBAR,
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_hStatus, SB_SETTEXTW, 0,
            (LPARAM)L"Watching: cs2.exe, dota2.exe  |  Poll: 20s");
        return 0;
    }

    case WM_SIZE:
        if (g_hEdit && g_hStatus)
            LayoutChildren(hwnd);
        return 0;

    // Hide to tray when user clicks the minimize button
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    // Hide to tray when user clicks X
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
            bool visible = IsWindowVisible(hwnd) != 0;
            AppendMenuW(hMenu, MF_STRING, IDM_SHOW,
                        visible ? L"Hide window" : L"Show window");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                           pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == IDM_SHOW) {
            if (IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
            else
                ShowMainWindow();
        } else if (LOWORD(wp) == IDM_EXIT) {
            g_running = false;
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            DestroyWindow(hwnd);
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

    // Register window class
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AutoAffinityWnd";
    if (!RegisterClassExW(&wc)) return 1;

    // Create window (hidden — lives in tray until user opens it)
    g_hwnd = CreateWindowExW(
        0, L"AutoAffinityWnd", L"AutoAffinity",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 440,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return 1;

    // Tray icon
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = TRAY_ICON_ID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, L"AutoAffinity - watching", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Initial log line (window is created, controls exist)
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
