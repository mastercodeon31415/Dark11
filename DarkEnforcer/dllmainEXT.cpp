#include "pch.h"
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <vector>
#include <map>
#include <iostream>
#include <string>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// --- Undocumented Dark Mode Definitions ---
using fnSetPreferredAppMode = DWORD(WINAPI*)(int AppMode);
using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND hWnd, BOOL allow);
using fnSetWindowTheme = HRESULT(WINAPI*)(HWND hwnd, LPCWSTR pszSubAppName, LPCWSTR pszSubIdList);

#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define DWMWA_WINDOW_CORNER_PREFERENCE 33

// --- Global State ---
HBRUSH hDarkBrush = NULL;
HMODULE hInst = NULL;
std::map<DWORD, HHOOK> g_threadHooks;
CRITICAL_SECTION g_cs;

// Dark Grey RGB(25, 25, 25)
const COLORREF DARK_COLOR = RGB(25, 25, 25);
const COLORREF TEXT_COLOR = RGB(255, 255, 255);

void Log(const wchar_t* msg) {
    if (GetConsoleWindow()) std::wcout << L"[DLL] " << msg << std::endl;
}

// ---------------------------------------------------------
// SUBCLASS PROCEDURE (For Legacy GDI #32770 Only)
// ---------------------------------------------------------
LRESULT CALLBACK DarkSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, hDarkBrush);
        return 1;
    }
    // Handle Tabs (Properties Window)
    case WM_PAINT:
    case WM_PRINTCLIENT:
    {
        // Let default paint happen, but we might need cleanup for tabs in future iterations
        break;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, TEXT_COLOR);
        SetBkColor(hdc, DARK_COLOR);
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
    wchar_t className[256];
    GetClassNameW(hwnd, className, 256);

    // 1. Force Theme on everything
    SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);

    // 2. Special handling for Tabs (Properties Window)
    if (wcscmp(className, L"SysTabControl32") == 0) {
        // Tab controls need "Explorer" to pick up the dark theme correctly in some contexts
        SetWindowTheme(hwnd, L"Explorer", NULL);
    }

    // 3. Subclass standard controls for painting
    // We do NOT subclass DirectUI elements or unknown complex controls to avoid breaking them
    if (wcscmp(className, L"Button") == 0 ||
        wcscmp(className, L"Static") == 0 ||
        wcscmp(className, L"Edit") == 0 ||
        wcscmp(className, L"ComboBox") == 0 ||
        wcscmp(className, L"ListBox") == 0)
    {
        if (GetPropW(hwnd, L"DarkEnforced") == NULL) {
            SetPropW(hwnd, L"DarkEnforced", (HANDLE)1);
            SetWindowSubclass(hwnd, DarkSubclassProc, 1, 0);
        }
    }

    // Refresh controls
    SendMessage(hwnd, WM_THEMECHANGED, 0, 0);
    InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

// ---------------------------------------------------------
// APPLY THEME LOGIC
// ---------------------------------------------------------
void ApplyDarkTheme(HWND hwnd, bool isDirectUI)
{
    if (GetPropW(hwnd, L"DarkEnforced")) return;
    SetPropW(hwnd, L"DarkEnforced", (HANDLE)1);

    // 1. DWM Title Bar (Common to all)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // 2. Allow Dark Mode (Undocumented API)
    HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
    if (hUxtheme) {
        auto _AllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133));
        if (_AllowDarkModeForWindow) _AllowDarkModeForWindow(hwnd, TRUE);
    }

    if (isDirectUI) {
        // --- DIRECT UI STRATEGY (File Copy / Move) ---
        Log(L"Target: DirectUI/OperationStatus (Copy Dialog)");
        // DirectUI handles its own painting. We just need to tell it "We are Dark Explorer".
        // Do NOT subclass WndProc for painting, or you will break the progress graph.
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
    }
    else {
        // --- GDI STRATEGY (Run / Properties / Delete) ---
        Log(L"Target: Standard Dialog (Run/Prop/Delete)");

        // Apply "DarkMode_Explorer" to get dark scrollbars
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);

        // Subclass the main window to paint the background Grey
        SetWindowSubclass(hwnd, DarkSubclassProc, 1, 0);

        // Iterate children to fix buttons/text/tabs
        EnumChildWindows(hwnd, EnumChildProc, 0);
    }

    // Force Refresh
    SendMessage(hwnd, WM_THEMECHANGED, 0, 0);
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
            // 1. Standard Dialogs (Run, Properties, Delete Confirmation)
            if (wcscmp(className, L"#32770") == 0)
            {
                ApplyDarkTheme(hwnd, false); // false = Not DirectUI (Use GDI Injection)
            }
            // 2. File Copy / Operation Status (Modern Windows 10/11 Copy Dialog)
            else if (wcscmp(className, L"OperationStatusWindow") == 0)
            {
                ApplyDarkTheme(hwnd, true); // true = DirectUI (Theme only, no painting hooks)
            }
            // 3. Fallback for wrapped Dialogs
            else if (wcscmp(className, L"DirectUIHWND") == 0)
            {
                // Check parent. If parent is Desktop or Explorer, it might be a standalone window.
                // This is risky, but required for some modern confirm dialogs.
                // For safety, we treat it as DirectUI.
                ApplyDarkTheme(hwnd, true);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------
// HOOK MANAGEMENT (Unchanged)
// ---------------------------------------------------------
void InstallHookOnThread(DWORD tid) {
    EnterCriticalSection(&g_cs);
    if (g_threadHooks.find(tid) == g_threadHooks.end()) {
        HHOOK h = SetWindowsHookEx(WH_CBT, CbtHookProc, hInst, tid);
        if (h) g_threadHooks[tid] = h;
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
        hDarkBrush = CreateSolidBrush(DARK_COLOR);

        AllocConsole();
        { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr); }
        Log(L"DarkEnforcer v2 Loaded. Targets: Run, Properties, Copy, Delete.");

        // Force Process Dark Mode (SetPreferredAppMode)
        {
            HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
            if (hUxtheme) {
                auto _SetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
                if (_SetPreferredAppMode) _SetPreferredAppMode(2); // Force Dark
            }
        }

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