#include "pch.h"
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <vector>
#include <map>
#include <iostream>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// --- Undocumented Dark Mode Definitions ---
using fnSetPreferredAppMode = DWORD(WINAPI*)(int AppMode);
using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND hWnd, BOOL allow);

#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_WINDOW_CORNER_PREFERENCE 33

// --- Global State ---
// RGB(25, 25, 25) is the StartAllBack specific dark grey (0x191919)
HBRUSH hDarkBrush = NULL;
HMODULE hInst = NULL;

std::map<DWORD, HHOOK> g_threadHooks;
CRITICAL_SECTION g_cs;

void Log(const wchar_t* msg) {
    if (GetConsoleWindow()) std::wcout << L"[DLL] " << msg << std::endl;
}

// ---------------------------------------------------------
// SUBCLASS PROCEDURE
// ---------------------------------------------------------
LRESULT CALLBACK DarkSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_ERASEBKGND:
    {
        // 1. Force the background to RGB(25,25,25)
        // This overwrites the "Black" background Windows tries to apply via SetWindowTheme
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, hDarkBrush);
        return 1;
    }
    case WM_CTLCOLORDLG:
    {
        return (LRESULT)hDarkBrush;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));

        // 2. Force text background to RGB(25,25,25)
        SetBkColor(hdc, RGB(25, 25, 25));

        // 3. OPAQUE mode paints the background box.
        // Since (1) and (2) match perfectly, the "Square" is invisible.
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)hDarkBrush;
    }
    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(25, 25, 25));
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)hDarkBrush;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(25, 25, 25));
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)hDarkBrush;
    }
    case WM_DESTROY:
        RemoveWindowSubclass(hWnd, DarkSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------
// RECURSIVE CHILD STYLING
// ---------------------------------------------------------
BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    // Force dark scrollbars and buttons
    SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);

    // Subclass for text coloring
    if (GetPropW(hwnd, L"DarkEnforced") == NULL) {
        SetPropW(hwnd, L"DarkEnforced", (HANDLE)1);
        SetWindowSubclass(hwnd, DarkSubclassProc, 1, 0);
    }

    // Refresh controls
    SendMessage(hwnd, WM_THEMECHANGED, 0, 0);
    InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

// ---------------------------------------------------------
// APPLY THEME
// ---------------------------------------------------------
void ApplyDarkTheme(HWND hwnd)
{
    if (GetPropW(hwnd, L"DarkEnforced")) return;
    SetPropW(hwnd, L"DarkEnforced", (HANDLE)1);

    Log(L"!!! TARGET ACQUIRED: Run Box Detected !!!");

    // 1. DWM Attributes (Title Bar)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // 2. Allow Dark Mode
    HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
    if (hUxtheme) {
        auto _AllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133));
        if (_AllowDarkModeForWindow) _AllowDarkModeForWindow(hwnd, TRUE);
    }

    // 3. Set Theme (Crucial for Dark Mode context)
    // RESTORED: This makes the window "Dark Aware", fixing the Light Mode regression.
    SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);

    // 4. Subclass Background
    SetWindowSubclass(hwnd, DarkSubclassProc, 1, 0);

    // 5. Style Children
    EnumChildWindows(hwnd, EnumChildProc, 0);

    // 6. Force Refresh
    SendMessage(hwnd, WM_THEMECHANGED, 0, 0);

    // Force repaint to cover the default black background with our grey brush
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------
// CBT HOOK PROCEDURE
// ---------------------------------------------------------
LRESULT CALLBACK CbtHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HCBT_ACTIVATE)
    {
        HWND hwnd = (HWND)wParam;
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256))
        {
            if (wcscmp(className, L"#32770") == 0)
            {
                ApplyDarkTheme(hwnd);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------
// HOOK MANAGEMENT
// ---------------------------------------------------------
void InstallHookOnThread(DWORD tid) {
    EnterCriticalSection(&g_cs);
    if (g_threadHooks.find(tid) == g_threadHooks.end()) {
        HHOOK h = SetWindowsHookEx(WH_CBT, CbtHookProc, hInst, tid);
        if (h) {
            g_threadHooks[tid] = h;
        }
    }
    LeaveCriticalSection(&g_cs);
}

void RemoveHookOnThread(DWORD tid) {
    EnterCriticalSection(&g_cs);
    auto it = g_threadHooks.find(tid);
    if (it != g_threadHooks.end()) {
        UnhookWindowsHookEx(it->second);
        g_threadHooks.erase(it);
    }
    LeaveCriticalSection(&g_cs);
}

void InstallInitialHooks() {
    DWORD pid = GetCurrentProcessId();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);
        if (Thread32First(hSnap, &te32)) {
            do {
                if (te32.th32OwnerProcessID == pid) {
                    InstallHookOnThread(te32.th32ThreadID);
                }
            } while (Thread32Next(hSnap, &te32));
        }
        CloseHandle(hSnap);
    }
}

void UninstallAllHooks() {
    EnterCriticalSection(&g_cs);
    for (auto const& [tid, hook] : g_threadHooks) {
        UnhookWindowsHookEx(hook);
    }
    g_threadHooks.clear();
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------
// ENTRY POINT
// ---------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        hInst = hModule;
        InitializeCriticalSection(&g_cs);
        // MATCHED COLOR: RGB(25, 25, 25)
        hDarkBrush = CreateSolidBrush(RGB(25, 25, 25));

        AllocConsole();
        { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr); }
        Log(L"DarkEnforcer Loaded. Init Hooks...");

        // 1. Force Process Dark Mode
        {
            HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
            if (hUxtheme) {
                auto _SetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
                if (_SetPreferredAppMode) _SetPreferredAppMode(2); // Force Dark
            }
        }

        // 2. Hook existing threads
        InstallInitialHooks();
        break;

    case DLL_THREAD_ATTACH:
        InstallHookOnThread(GetCurrentThreadId());
        break;

    case DLL_THREAD_DETACH:
        RemoveHookOnThread(GetCurrentThreadId());
        break;

    case DLL_PROCESS_DETACH:
        Log(L"Unloading...");
        UninstallAllHooks();
        if (hDarkBrush) DeleteObject(hDarkBrush);
        DeleteCriticalSection(&g_cs);
        FreeConsole();
        break;
    }
    return TRUE;
}