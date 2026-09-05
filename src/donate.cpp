/* ============================================================================
 *  HDDHealth Monitor - Donate module implementation
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  Author  : Ari Sohandri Putra
 *  Company : ARImetic Inc.
 *  Sponsor : https://github.com/sponsors/arisohandriputra/
 *  License : MIT
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "donate.h"
#include "lang.h"
#include "mainwnd.h"   /* for g_hInst, g_nDpi */

/* ------------------------------------------------------------------ */
/*  Public: open GitHub Sponsors page                                 */
/* ------------------------------------------------------------------ */

BOOL Donate_OpenSponsorsPage(HWND hParent)
{
    HINSTANCE hResult = ShellExecuteW(
        hParent,
        L"open",
        L"https://github.com/sponsors/arisohandriputra/",
        NULL, NULL,
        SW_SHOWNORMAL);

    if ((INT_PTR)hResult <= 32) {
        wchar_t szMsg[512];
        _snwprintf(szMsg, 512, LStrW(STR_DONATE_FAIL_MSG), L"https://github.com/sponsors/arisohandriputra/");
        MessageBoxW(hParent, szMsg, LStrW(STR_DONATE_FAIL_TITLE), MB_ICONWARNING | MB_OK);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Donate dialog window procedure                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK DonateDlgProc(HWND hDlg, UINT uMsg,
                                       WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            int dpi = g_nDpi > 0 ? g_nDpi : 96;

            RECT rcClient;
            GetClientRect(hDlg, &rcClient);
            int cx = rcClient.right - rcClient.left;
            if (cx <= 0) cx = MulDiv(460, dpi, 96);

            int padX = MulDiv(20, dpi, 96);
            int wText = cx - padX * 2;

            /* Bold title font */
            HFONT hFontBold = CreateFontW(MulDiv(-15, dpi, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            HFONT hFontNorm = CreateFontW(MulDiv(-12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            HFONT hFontLink = CreateFontW(MulDiv(-12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            SetPropW(hDlg, L"f_bold", (HANDLE)hFontBold);
            SetPropW(hDlg, L"f_norm", (HANDLE)hFontNorm);
            SetPropW(hDlg, L"f_link", (HANDLE)hFontLink);

            int curY = MulDiv(16, dpi, 96);

            /* ---- Header ------------------------------------------------- */
            HWND hTitle = CreateWindowExW(0, L"STATIC",
                LStrW(STR_DONATE_HEADER),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(22, dpi, 96), hDlg, NULL, g_hInst, NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            curY += MulDiv(28, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Explanation text -------------------------------------- */
            curY += MulDiv(10, dpi, 96);
            int explainH = MulDiv(90, dpi, 96);
            HWND hExplain = CreateWindowExW(0, L"STATIC",
                LStrW(STR_DONATE_EXPLAIN),
                WS_CHILD | WS_VISIBLE,
                padX, curY, wText, explainH, hDlg, NULL, g_hInst, NULL);
            SendMessageW(hExplain, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += explainH + MulDiv(10, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Author / sponsor attribution -------------------------- */
            curY += MulDiv(10, dpi, 96);
            wchar_t authorLine[128];
            _snwprintf(authorLine, 128,
                LStrW(STR_DONATE_AUTHOR_LINE), L"Ari Sohandri Putra", L"ARImetic Inc.");
            HWND hAuthor = CreateWindowExW(0, L"STATIC", authorLine,
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(20, dpi, 96), hDlg, NULL, g_hInst, NULL);
            SendMessageW(hAuthor, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += MulDiv(22, dpi, 96);
            HWND hLink = CreateWindowExW(0, L"STATIC", L"https://github.com/sponsors/arisohandriputra/",
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                padX, curY, wText, MulDiv(20, dpi, 96), hDlg,
                (HMENU)IDC_DONATE_LINK_STATIC, g_hInst, NULL);
            SendMessageW(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            curY += MulDiv(26, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Buttons ------------------------------------------------ */
            curY += MulDiv(14, dpi, 96);
            int btnW = MulDiv(110, dpi, 96);
            int btnH = MulDiv(30, dpi, 96);
            int btnGap = MulDiv(20, dpi, 96);
            int totalBtnW = btnW * 2 + btnGap;
            int btnStartX = (cx - totalBtnW) / 2;

            HWND hDonate = CreateWindowExW(0, L"BUTTON", LStrW(STR_BTN_DONATE),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                btnStartX, curY, btnW, btnH, hDlg,
                (HMENU)IDC_DONATE_OPEN_BTN, g_hInst, NULL);
            SendMessageW(hDonate, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            HWND hClose = CreateWindowExW(0, L"BUTTON", LStrW(STR_BTN_CLOSE),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                btnStartX + btnW + btnGap, curY, btnW, btnH, hDlg,
                (HMENU)IDC_DONATE_CLOSE_BTN, g_hInst, NULL);
            SendMessageW(hClose, WM_SETFONT, (WPARAM)hFontNorm, TRUE);
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND hCtrl = (HWND)lParam;
            int  nID   = GetDlgCtrlID(hCtrl);
            HDC  hdcSt = (HDC)wParam;
            SetBkMode(hdcSt, TRANSPARENT);
            if (nID == IDC_DONATE_LINK_STATIC) {
                SetTextColor(hdcSt, RGB(0, 102, 204));
            }
            return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
        }

    case WM_COMMAND:
        {
            int nCtrl = LOWORD(wParam);

            if (nCtrl == IDC_DONATE_OPEN_BTN) {
                Donate_OpenSponsorsPage(hDlg);
                return 0;
            }

            if (nCtrl == IDC_DONATE_CLOSE_BTN || nCtrl == IDCANCEL) {
                DestroyWindow(hDlg);
                return 0;
            }

            if (nCtrl == IDC_DONATE_LINK_STATIC &&
                HIWORD(wParam) == STN_CLICKED) {
                Donate_OpenSponsorsPage(hDlg);
                return 0;
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hDlg);
        }
        return 0;

    case WM_DESTROY:
        {
            HFONT f1 = (HFONT)GetPropW(hDlg, L"f_bold");
            HFONT f2 = (HFONT)GetPropW(hDlg, L"f_norm");
            HFONT f3 = (HFONT)GetPropW(hDlg, L"f_link");
            if (f1) DeleteObject(f1);
            if (f2) DeleteObject(f2);
            if (f3) DeleteObject(f3);
            RemovePropW(hDlg, L"f_bold");
            RemovePropW(hDlg, L"f_norm");
            RemovePropW(hDlg, L"f_link");
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;
    }

    return DefWindowProcW(hDlg, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Public: show the Donate dialog                                    */
/* ------------------------------------------------------------------ */

void Donate_ShowDialog(HWND hParent)
{
    static BOOL bRegistered = FALSE;
    if (!bRegistered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = DonateDlgProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"LLHDDonateDlg";
        RegisterClassExW(&wc);
        bRegistered = TRUE;
    }

    HWND hExist = FindWindowW(L"LLHDDonateDlg", NULL);
    if (hExist) {
        SetForegroundWindow(hExist);
        return;
    }

    int dpi = g_nDpi > 0 ? g_nDpi : 96;
    int dlgW = MulDiv(460, dpi, 96);
    int dlgH = MulDiv(330, dpi, 96);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int x, y;
    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        x = rcP.left + ((rcP.right  - rcP.left) - dlgW) / 2;
        y = rcP.top  + ((rcP.bottom - rcP.top ) - dlgH) / 2;
    } else {
        x = (scrW - dlgW) / 2;
        y = (scrH - dlgH) / 2;
    }

    RECT rcAdj = {0, 0, dlgW, dlgH};
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    int winW = rcAdj.right - rcAdj.left;
    int winH = rcAdj.bottom - rcAdj.top;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"LLHDDonateDlg",
        LStrW(STR_DONATE_TITLE),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, winW, winH,
        hParent, NULL, g_hInst, NULL);

    if (!hDlg) return;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetFocus(hDlg);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public: no-op startup hook                                        */
/* ------------------------------------------------------------------ */

BOOL Donate_Startup(HWND hParent)
{
    (void)hParent;
    return TRUE;
}
