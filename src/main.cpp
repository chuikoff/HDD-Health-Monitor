/* DriveMonitor - WinMain. Fork of HDDHealth Monitor, MIT: see LICENSE. */

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "mainwnd.h"
#include "smart.h"
#include "resource.h"
#include "utf8ui.h"
#include "safestr.h"

#define MUTEX_NAME  "Global\\DriveMonitor_SingleInstance"

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

static void BringExistingWindowToFront(void)
{
    HWND hExist = FindWindowW(DRIVEMONITOR_WNDCLASS, NULL);
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

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    char path[MAX_PATH + 64];
    FILE* f = NULL;
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n && n < MAX_PATH) {
        safe_snprintf(path, "%sDriveMonitor_crash.txt", dir);
        f = fopen(path, "w");
    }
    if (!f && GetModuleFileNameA(NULL, dir, MAX_PATH)) {
        char* slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = '\0';
            safe_snprintf(path, "%sDriveMonitor_crash.txt", dir);
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
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashFilter);

    HANDLE hMutex = CreateWorldMutex();
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        BringExistingWindowToFront();
        CloseHandle(hMutex);
        return 0;
    }

    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DRIVEMONITOR_WNDCLASS;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                          IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    if (!RegisterClassExW(&wc)) {
        MessageBoxU8(NULL, "RegisterClassEx failed!", "Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);
    int nX    = (nScrW - WINDOW_W) / 2;
    int nY    = (nScrH - WINDOW_H) / 2;

    HWND hWnd = CreateWindowExU8(
        0,
        "DriveMonitorMainWnd",
        "DriveMonitor",
        WS_OVERLAPPEDWINDOW,
        nX, nY, WINDOW_W, WINDOW_H,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        MessageBoxU8(NULL, "CreateWindow failed!", "Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return (int)msg.wParam;
}
