#include "pch.h"
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <iostream>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using fnSetPreferredAppMode = DWORD(WINAPI*)(int AppMode);
using fnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND hWnd, BOOL allow);

#define DWMWA_USE_IMMERSIVE_DARK_MODE 20

// --- Config ---
const COLORREF DARK_BG = RGB(32, 32, 32);
const COLORREF DARK_TXT = RGB(255, 255, 255);

HBRUSH hDarkBrush = NULL;
HMODULE hInst = NULL;
std::map<DWORD, HHOOK> g_threadHooks;
std::set<HWND> g_processedWindows;
CRITICAL_SECTION g_cs;
DWORD g_currentPID = 0;

void Log(const wchar_t* msg) {
    if (GetConsoleWindow()) std::wcout << L"[DarkEnforcer] " << msg << std::endl;
}

// ---------------------------------------------------------
// COMPONENT: ListView Subclass (Legacy Mode Attempt)
// ---------------------------------------------------------
LRESULT CALLBACK ListViewSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_PAINT:
    {
        // Force Colors immediately before painting
        SendMessage(hWnd, LVM_SETBKCOLOR, 0, (LPARAM)DARK_BG);
        SendMessage(hWnd, LVM_SETTEXTBKCOLOR, 0, (LPARAM)DARK_BG);
        SendMessage(hWnd, LVM_SETTEXTCOLOR, 0, (LPARAM)DARK_TXT);
        break;
    }
    case WM_NCPAINT:
    {
        // Force Theme Strip to attempt to fix Scrollbars/Borders
        SetWindowTheme(hWnd, L"", L"");
        break;
    }
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, hDarkBrush);
        return 1;
    }
    case WM_DESTROY:
        RemoveWindowSubclass(hWnd, ListViewSubclass, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------
// COMPONENT: Tab Control Subclass
// ---------------------------------------------------------
LRESULT CALLBACK TabControlSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
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
    case WM_DESTROY:
        RemoveWindowSubclass(hWnd, TabControlSubclass, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------
// COMPONENT: Nuclear Painter (Text & Icons)
// ---------------------------------------------------------
LRESULT CALLBACK NuclearStaticSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_PAINT:
    {
        if (!IsWindow(hWnd)) return 0;

        LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);

        // Special handling for Icons (Preserve transparency)
        if (style & SS_ICON) {
            // Check if we are in the Run box (double check) to avoid artifacts
            // If we are here, we decided to paint manually, but for Icons, 
            // standard painting is often safer if the background is correct.
            // However, we will proceed with transparent draw.
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);

            // Fill background only if we aren't assuming transparency from parent
            FillRect(hdc, &rc, hDarkBrush);

            HICON hIcon = (HICON)SendMessage(hWnd, STM_GETICON, 0, 0);
            if (hIcon) {
                int iconW = GetSystemMetrics(SM_CXICON);
                int iconH = GetSystemMetrics(SM_CYICON);
                int x = (style & SS_CENTERIMAGE) ? (rc.right - iconW) / 2 : 0;
                int y = (style & SS_CENTERIMAGE) ? (rc.bottom - iconH) / 2 : 0;
                DrawIconEx(hdc, x, y, hIcon, 0, 0, 0, NULL, DI_NORMAL);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }

        // Skip OwnerDraw / Bitmaps / Metafiles
        if ((style & SS_OWNERDRAW) || (style & SS_BITMAP) || (style & SS_ENHMETAFILE) ||
            (style & SS_ETCHEDHORZ) || (style & SS_ETCHEDVERT) || (style & SS_ETCHEDFRAME)) {
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        // Manual Text Drawing
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        // We fill the rect to ensure dark background
        FillRect(hdc, &rc, hDarkBrush);

        int len = GetWindowTextLengthW(hWnd);
        if (len > 0) {
            std::vector<wchar_t> buf(len + 1);
            GetWindowTextW(hWnd, buf.data(), len + 1);
            HFONT hFont = (HFONT)SendMessage(hWnd, WM_GETFONT, 0, 0);
            HGDIOBJ hOldFont = SelectObject(hdc, hFont);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, DARK_TXT);

            UINT format = DT_VCENTER | DT_SINGLELINE;
            if (style & SS_CENTER) format = DT_CENTER | DT_WORDBREAK;
            else if (style & SS_RIGHT) format = DT_RIGHT | DT_WORDBREAK;
            else format = DT_LEFT;
            if ((style & SS_LEFTNOWORDWRAP) == 0) { format &= ~DT_SINGLELINE; format |= DT_WORDBREAK; }
            if (style & SS_NOPREFIX) format |= DT_NOPREFIX;

            DrawTextW(hdc, buf.data(), -1, &rc, format);
            SelectObject(hdc, hOldFont);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_ENABLE: InvalidateRect(hWnd, NULL, TRUE); break;
    case WM_DESTROY: RemoveWindowSubclass(hWnd, NuclearStaticSubclass, uIdSubclass); break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------
// COMPONENT: Parent Handler
// ---------------------------------------------------------
BOOL CALLBACK ScanChildren(HWND hwnd, LPARAM lParam);

LRESULT CALLBACK DarkParentSubclass(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
    case WM_SHOWWINDOW:
        EnumChildWindows(hWnd, ScanChildren, 0);
        break;

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->code == TCN_SELCHANGE || pnm->code == LVN_ITEMCHANGED) {
            SetTimer(hWnd, 888, 50, NULL);
            SetTimer(hWnd, 889, 200, NULL);
        }
        break;
    }
    case WM_TIMER:
        if (wParam == 888 || wParam == 889) {
            KillTimer(hWnd, wParam);
            EnumChildWindows(hWnd, ScanChildren, 0);
            RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        break;

    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_CREATE) EnumChildWindows(hWnd, ScanChildren, 0);
        break;

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, hDarkBrush);
        return 1;
    }
    // These handlers are CRITICAL for the Rename/Run box fixes.
    // They ensure that standard GDI controls (that have been Theme Stripped)
    // draw transparently with white text.
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, DARK_TXT);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)hDarkBrush;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, DARK_TXT);
        SetBkColor(hdc, DARK_BG);
        SetBkMode(hdc, OPAQUE);
        return (LRESULT)hDarkBrush;
    }
    case WM_DESTROY:
        RemoveWindowSubclass(hWnd, DarkParentSubclass, uIdSubclass);
        EnterCriticalSection(&g_cs); g_processedWindows.erase(hWnd); LeaveCriticalSection(&g_cs);
        break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------
// COMPONENT: Recursive Scanner
// ---------------------------------------------------------
BOOL CALLBACK ScanChildren(HWND hwnd, LPARAM lParam) {
    wchar_t className[256];
    if (!GetClassNameW(hwnd, className, 256)) return TRUE;

    // 1. TABS
    if (wcscmp(className, L"SysTabControl32") == 0) {
        SetWindowTheme(hwnd, L"Explorer", NULL);
        if (GetPropW(hwnd, L"DarkTabSub") == NULL) {
            SetPropW(hwnd, L"DarkTabSub", (HANDLE)1);
            SetWindowSubclass(hwnd, TabControlSubclass, 777, 0);
        }
        return TRUE;
    }

    // 2. LISTS
    if (wcscmp(className, L"SysListView32") == 0) {
        SetWindowTheme(hwnd, L"", L"");
        if (GetPropW(hwnd, L"DarkListSub") == NULL) {
            SetPropW(hwnd, L"DarkListSub", (HANDLE)1);
            SetWindowSubclass(hwnd, ListViewSubclass, 888, 0);
        }
        return TRUE;
    }

    // 3. TREES
    if (wcscmp(className, L"SysTreeView32") == 0) {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
        SendMessage(hwnd, TVM_SETTEXTCOLOR, 0, (LPARAM)DARK_TXT);
        SendMessage(hwnd, TVM_SETBKCOLOR, 0, (LPARAM)DARK_BG);
        InvalidateRect(hwnd, NULL, TRUE);
        return TRUE;
    }

    // 4. STATIC (Fixed for Rename & Run Dialogs)
    if (wcscmp(className, L"Static") == 0) {
        // [FIX ISSUE 1 & 2]: STRIP THEME from Statics.
        // This forces the Rename Dialog text (which usually ignores color) to obey WM_CTLCOLORSTATIC.
        // This also forces the Run Dialog text/icon to obey WM_CTLCOLORSTATIC (Transparent), removing artifacts.
        SetWindowTheme(hwnd, L"", L"");

        // Detect if this is the Run box or a standard Dialog where we prefer GDI over Nuclear
        // The "Run" box usually has the title "Run" on the parent.
        wchar_t parentText[256] = { 0 };
        GetWindowTextW(GetParent(hwnd), parentText, 256);

        bool isRunBox = (wcsstr(parentText, L"Run") != NULL);

        if (isRunBox) {
            // [FIX ISSUE 2]: Do NOT apply Nuclear Subclass to Run box items.
            // Just let Theme Stripping + Parent WM_CTLCOLORSTATIC handle it.
            // This prevents the "miscolored boxes" behind icons/text.
            InvalidateRect(hwnd, NULL, TRUE);
            return TRUE;
        }

        // For other windows, we apply Nuclear as a fallback, 
        // but Theme Stripping above might have already fixed the Black Text issue (Rename Box).
        // We keep Nuclear for stubborn custom controls.
        if (GetPropW(hwnd, L"DarkSub") == NULL) {
            SetPropW(hwnd, L"DarkSub", (HANDLE)1);
            SetWindowSubclass(hwnd, NuclearStaticSubclass, 9999, 0);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return TRUE;
    }

    // 5. DIRECTUI
    if (wcscmp(className, L"DirectUIHWND") == 0) {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
        HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
        if (hUxtheme) {
            auto _Allow = (fnAllowDarkModeForWindow)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133));
            if (_Allow) _Allow(hwnd, TRUE);
        }
        return TRUE;
    }

    // 6. INPUTS
    if (wcscmp(className, L"ComboBoxEx32") == 0 ||
        wcscmp(className, L"ComboBox") == 0 ||
        wcscmp(className, L"Edit") == 0 ||
        wcscmp(className, L"ListBox") == 0 ||
        wcscmp(className, L"Button") == 0)
    {
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
        InvalidateRect(hwnd, NULL, TRUE);
    }

    // 7. NESTED PAGES
    if (wcscmp(className, L"#32770") == 0) {
        if (GetPropW(hwnd, L"DarkRoot") == NULL) {
            SetPropW(hwnd, L"DarkRoot", (HANDLE)1);
            SetWindowSubclass(hwnd, DarkParentSubclass, 1, 0);
            EnumChildWindows(hwnd, ScanChildren, 0);
        }
    }

    return TRUE;
}

// ---------------------------------------------------------
// ENTRY POINT
// ---------------------------------------------------------
void ApplyDarkTheme(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != g_currentPID) return;
    wchar_t cls[256];
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, L"ConsoleWindowClass") == 0) return;

    EnterCriticalSection(&g_cs);
    if (g_processedWindows.count(hwnd)) { LeaveCriticalSection(&g_cs); return; }
    g_processedWindows.insert(hwnd);
    LeaveCriticalSection(&g_cs);

    Log(L"Theming Dialog...");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
    if (hUxtheme) {
        auto _Allow = (fnAllowDarkModeForWindow)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133));
        if (_Allow) _Allow(hwnd, TRUE);
    }
    SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
    SetWindowSubclass(hwnd, DarkParentSubclass, 1, 0);
    EnumChildWindows(hwnd, ScanChildren, 0);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

LRESULT CALLBACK CbtHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_ACTIVATE) {
        HWND hwnd = (HWND)wParam;
        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256)) {
            if (wcscmp(className, L"#32770") == 0 || wcscmp(className, L"OperationStatusWindow") == 0) ApplyDarkTheme(hwnd);
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void InstallHookOnThread(DWORD tid) {
    EnterCriticalSection(&g_cs);
    if (g_threadHooks.find(tid) == g_threadHooks.end()) {
        HHOOK h = SetWindowsHookEx(WH_CBT, CbtHookProc, hInst, tid);
        if (h) g_threadHooks[tid] = h;
    }
    LeaveCriticalSection(&g_cs);
}

DWORD WINAPI ScannerThread(LPVOID) {
    auto RetroScan = [](HWND hwnd, LPARAM) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != g_currentPID) return TRUE;
        wchar_t cn[256];
        GetClassNameW(hwnd, cn, 256);
        if (wcscmp(cn, L"#32770") == 0 || wcscmp(cn, L"OperationStatusWindow") == 0) ApplyDarkTheme(hwnd);
        return TRUE;
        };
    EnumWindows(RetroScan, 0);
    while (true) {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te32; te32.dwSize = sizeof(THREADENTRY32);
            if (Thread32First(hSnap, &te32)) {
                do { if (te32.th32OwnerProcessID == g_currentPID) InstallHookOnThread(te32.th32ThreadID); } while (Thread32Next(hSnap, &te32));
            }
            CloseHandle(hSnap);
        }
        Sleep(1000);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul, LPVOID) {
    switch (ul) {
    case DLL_PROCESS_ATTACH:
        hInst = hModule; g_currentPID = GetCurrentProcessId(); InitializeCriticalSection(&g_cs);
        hDarkBrush = CreateSolidBrush(DARK_BG);
        AllocConsole(); { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr); }
        Log(L"DarkEnforcer v16.1 - Run/Rename Fixes Applied.");
        {
            HMODULE hUxtheme = GetModuleHandle(L"uxtheme.dll");
            if (hUxtheme) {
                auto _SetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
                if (_SetPreferredAppMode) _SetPreferredAppMode(2);
            }
        }
        CreateThread(NULL, 0, ScannerThread, NULL, 0, NULL);
        break;
    case DLL_THREAD_ATTACH: InstallHookOnThread(GetCurrentThreadId()); break;
    case DLL_PROCESS_DETACH: if (hDarkBrush) DeleteObject(hDarkBrush); DeleteCriticalSection(&g_cs); FreeConsole(); break;
    }
    return TRUE;
}