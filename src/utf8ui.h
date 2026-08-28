/* ============================================================================
 *  UTF-8 display helpers for Win32 Wide APIs
 *  Source is UTF-8; do not rely on ACP / -fexec-charset=CP1251.
 * ============================================================================ */
#pragma once
#ifndef UTF8UI_H
#define UTF8UI_H

#include <windows.h>

static inline int U8ToW(const char* u8, WCHAR* w, int n)
{
    if (n > 0) w[0] = 0;
    if (!u8)
        return 0;
    {
        int r = MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, n);
        if (r == 0 && n > 0) w[0] = 0;
        return r;
    }
}

static inline int WToU8(const WCHAR* w, char* u8, int n)
{
    if (!w) {
        if (n > 0) u8[0] = 0;
        return 0;
    }
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, n, NULL, NULL);
}

static inline void DrawTextU8(HDC hdc, const char* u8, RECT* rc, UINT fmt)
{
    WCHAR w[512];
    U8ToW(u8, w, 512);
    DrawTextW(hdc, w, -1, rc, fmt);
}

/* Draw a UTF-8 substring of exactly nBytes (not necessarily NUL-terminated). */
static inline void DrawTextU8N(HDC hdc, const char* u8, int nBytes, RECT* rc, UINT fmt)
{
    WCHAR w[512];
    int nw;
    if (!u8 || nBytes <= 0) {
        DrawTextW(hdc, L"", 0, rc, fmt);
        return;
    }
    if (nBytes > 480) nBytes = 480;
    nw = MultiByteToWideChar(CP_UTF8, 0, u8, nBytes, w, 511);
    if (nw < 0) nw = 0;
    w[nw] = 0;
    DrawTextW(hdc, w, nw, rc, fmt);
}

static inline void SetWindowTextU8(HWND h, const char* u8)
{
    WCHAR w[1024];
    U8ToW(u8 ? u8 : "", w, 1024);
    SetWindowTextW(h, w);
}

static inline void SetDlgItemTextU8(HWND h, int id, const char* u8)
{
    WCHAR w[1024];
    U8ToW(u8 ? u8 : "", w, 1024);
    SetDlgItemTextW(h, id, w);
}

static inline BOOL AppendMenuU8(HMENU hMenu, UINT flags, UINT_PTR idNew, const char* u8)
{
    WCHAR w[256];
    if (!u8)
        return AppendMenuW(hMenu, flags, idNew, NULL);
    U8ToW(u8, w, 256);
    return AppendMenuW(hMenu, flags, idNew, w);
}

static inline HWND CreateWindowExU8(
    DWORD dwExStyle, const char* lpClass, const char* lpWindow,
    DWORD dwStyle, int x, int y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    WCHAR wc[128], wt[1024];
    U8ToW(lpClass ? lpClass : "", wc, 128);
    U8ToW(lpWindow ? lpWindow : "", wt, 1024);
    return CreateWindowExW(dwExStyle, wc, wt, dwStyle, x, y, nWidth, nHeight,
                           hWndParent, hMenu, hInstance, lpParam);
}

#endif /* UTF8UI_H */
