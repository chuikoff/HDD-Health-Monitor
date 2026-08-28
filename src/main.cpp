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
#include <stdio.h>
#include <string.h>

#include "mainwnd.h"
#include "smart.h"
#include "resource.h"
#include "donate.h"
#include "utf8ui.h"

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
    HWND hExist = FindWindowA("LLHDMonitorMainWnd", NULL);
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

/* Tiny crash log: no MessageBox (can deadlock in a crash). Writes
   %TEMP%\HDDHealthMonitor_crash.txt, else next to the exe. */
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    char path[MAX_PATH + 64];
    FILE* f = NULL;
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n && n < MAX_PATH) {
        _snprintf(path, sizeof(path), "%sHDDHealthMonitor_crash.txt", dir);
        f = fopen(path, "w");
    }
    if (!f && GetModuleFileNameA(NULL, dir, MAX_PATH)) {
        char* slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = '\0';
            _snprintf(path, sizeof(path), "%sHDDHealthMonitor_crash.txt", dir);
            f = fopen(path, "w");
        }
    }
    if (f) {
        DWORD code = 0;
        void* addr = NULL;
        if (ep && ep->ExceptionRecord) {
            code = ep->ExceptionRecord->ExceptionCode;
            addr = ep->ExceptionRecord->ExceptionAddress;
        }
        fprintf(f, "exception=0x%08lX\naddress=%p\nthread=%lu\n",
                (unsigned long)code, addr, (unsigned long)GetCurrentThreadId());
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    SetUnhandledExceptionFilter(CrashFilter);

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

    /* ---- GDI objects (brushes, fonts) ----------------------------- */
    CreateGDIObjects();

   
    Donate_Startup(NULL);

    /* ---- Register the main window class --------------------------- */
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "LLHDMonitorMainWnd";
    wc.hIcon         = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APPICON),
                                          IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "RegisterClassEx failed!", "Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    /* ---- Center the main window on the primary monitor ------------ */
    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);
    int nX    = (nScrW - WINDOW_W) / 2;
    int nY    = (nScrH - WINDOW_H) / 2;

    HMENU hMenuBar  = CreateMenu();
    HMENU hMenuFile = CreatePopupMenu();
    HMENU hMenuHelp = CreatePopupMenu();
    AppendMenuU8(hMenuFile, MF_STRING,    IDM_SHOW_WINDOW, "&Показать окно");
    AppendMenuU8(hMenuFile, MF_SEPARATOR, 0,               NULL);
    AppendMenuU8(hMenuFile, MF_STRING,    IDM_EXIT,        "В&ыход");
    AppendMenuU8(hMenuHelp, MF_STRING,    IDM_DONATE,      "&Поддержать...");
    AppendMenuU8(hMenuHelp, MF_SEPARATOR, 0,               NULL);
    AppendMenuU8(hMenuHelp, MF_STRING,    IDM_ABOUT,       "&О программе...");
    AppendMenuU8(hMenuBar,  MF_POPUP, (UINT_PTR)hMenuFile, "&Файл");
    AppendMenuU8(hMenuBar,  MF_POPUP, (UINT_PTR)hMenuHelp, "&Справка");

    HWND hWnd = CreateWindowExA(
        0,
        "LLHDMonitorMainWnd",
        "HDDHealth Monitor 1.1",
        WS_OVERLAPPEDWINDOW,
        nX, nY, WINDOW_W, WINDOW_H,
        NULL, hMenuBar, hInstance, NULL
    );

    if (!hWnd) {
        MessageBoxA(NULL, "CreateWindow failed!", "Error", MB_ICONERROR);
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
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return (int)msg.wParam;
}
