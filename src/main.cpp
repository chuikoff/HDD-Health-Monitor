/* ============================================================================
 *  HDDHealth Monitor - Application entry point
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  This file implements the WinMain entry point.  It performs:
 *    - Single-instance enforcement via a named global mutex.
 *    - Common-controls initialization.
 *    - GDI object construction.
 *    - Main window class registration and creation.
 *    - The standard Win32 message loop.
 *
 *  Author  : Ari Sohandri Putra
 *  Company : ARImetic Inc.
 *  Sponsor : https://github.com/sponsors/arisohandriputra/
 *  License : MIT
 * ============================================================================
 */

/* The two #pragma comment directives below are MSVC-specific.  They tell
   the MSVC linker to pull in comctl32.lib and the Common-Controls v6 SxS
   manifest.  MinGW / GCC ignores them (and emits a -Wunknown-pragmas
   warning), so we wrap them in _MSC_VER to keep MinGW builds clean.  The
   Makefile already passes -lcomctl32 in LDFLAGS, so MinGW links correctly
   without these pragmas. */
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include "mainwnd.h"
#include "smart.h"
#include "resource.h"
#include "donate.h"    

/* A globally-named mutex prevents the user from accidentally launching
   multiple instances of the monitor.  The "Global\" prefix makes the
   mutex visible across all terminal-server sessions. */
#define MUTEX_NAME  "Global\\HDDHealth_SingleInstance_v1"

/* ------------------------------------------------------------------ */
/*  Single-instance helpers                                           */
/* ------------------------------------------------------------------ */

/* Create the named global mutex with a NULL DACL so that any user
   session can interact with it.  This is important because the app
   requests elevation (requireAdministrator) and we still want a
   per-machine singleton. */
static HANDLE CreateWorldMutex(void)
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle       = FALSE;

    return CreateMutexA(&sa, TRUE, MUTEX_NAME);
}

/* If another instance is already running, this helper locates its
   main window and brings it to the foreground instead of starting
   a duplicate process. */
static void BringExistingWindowToFront(void)
{
    HWND hExist = FindWindowW(L"LLHDMonitorMainWnd", NULL);
    if (!hExist) return;

    if (!IsWindowVisible(hExist))
        ShowWindow(hExist, SW_SHOW);

    if (IsIconic(hExist))
        ShowWindow(hExist, SW_RESTORE);

    DWORD dwPid = 0;
    GetWindowThreadProcessId(hExist, &dwPid);
    AllowSetForegroundWindow(dwPid);
    SetForegroundWindow(hExist);
    BringWindowToTop(hExist);
}

/* ------------------------------------------------------------------ */
/*  WinMain - program entry point                                     */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    /* ---- Single-instance guard ------------------------------------ */
    HANDLE hMutex = CreateWorldMutex();
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        BringExistingWindowToFront();
        CloseHandle(hMutex);
        return 0;
    }

    g_hInst = hInstance;

    /* ---- Common controls init (ListViews, etc.) ------------------- */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    /* ---- Multi-language initialization --------------------------- */
    Lang_Init();

    /* ---- Query System DPI & Initialize GDI objects --------------- */
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
        g_nDpi = GetDeviceCaps(hdcScreen, LOGPIXELSY);
        ReleaseDC(NULL, hdcScreen);
    }
    if (g_nDpi <= 0) g_nDpi = 96;

    CreateGDIObjects();
   
    Donate_Startup(NULL);

    /* ---- Register the main window class --------------------------- */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LLHDMonitorMainWnd";
    wc.hIcon         = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APPICON),
                                          IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"RegisterClassEx failed!", L"Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    /* ---- Center the main window on the primary monitor ------------ */
    RECT rcWin = { 0, 0, WINDOW_W, WINDOW_H };
    AdjustWindowRectEx(&rcWin, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    int realWinW = rcWin.right - rcWin.left;
    int realWinH = rcWin.bottom - rcWin.top;

    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);
    int nX    = (nScrW - realWinW) / 2;
    int nY    = (nScrH - realWinH) / 2;
    if (nX < 0) nX = 0;
    if (nY < 0) nY = 0;

    HWND hWnd = CreateWindowExW(
        0,
        L"LLHDMonitorMainWnd",
        LStrW(STR_APP_TITLE),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        nX, nY, realWinW, realWinH,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        MessageBoxW(NULL, L"CreateWindow failed!", L"Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    /* Allow silent startup (e.g. launched from the autostart folder)
       via the /minimized command-line switch. */
    ShowWindow(hWnd, (lpCmdLine && strstr(lpCmdLine, "/minimized")) ? SW_HIDE : nCmdShow);
    UpdateWindow(hWnd);

    /* ---- Standard Win32 message loop ------------------------------ */
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return (int)msg.wParam;
}
