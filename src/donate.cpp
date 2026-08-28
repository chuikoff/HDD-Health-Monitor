/* ============================================================================
 *  HDDHealth Monitor - Donate module implementation
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  License : MIT
 * ============================================================================
 */

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

#include "donate.h"
#include "mainwnd.h"   /* for g_hInst, g_hFontSmall, g_hFontNormal */
#include "utf8ui.h"
#include "safestr.h"

#define PROP_FONT_BOLD  "dmFntB"
#define PROP_FONT_LINK  "dmFntL"

static HCURSOR s_hDonateHand = NULL;

/* ------------------------------------------------------------------ */
/*  Public: open Boosty page                                          */
/* ------------------------------------------------------------------ */

BOOL Donate_OpenSponsorsPage(HWND hParent)
{
    /* ShellExecuteA with verb "open" launches the URL in the user's
       default browser.  SW_SHOWNORMAL is the most portable choice. */
    HINSTANCE hResult = ShellExecuteA(
        hParent,
        "open",
        DONATE_URL,
        NULL, NULL,
        SW_SHOWNORMAL);

    /* A result handle <= 32 indicates failure (per Win32 docs). */
    if ((INT_PTR)hResult <= 32) {
        MessageBoxU8(hParent,
            "Unable to open the donations page in your default browser.\n"
            "Please visit the following URL manually:\n\n"
            DONATE_URL,
            "Open Browser Failed",
            MB_ICONWARNING | MB_OK);
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
            int cx = DONATE_DLG_W;

            /* Bold title font used only inside this dialog. */
            HFONT hFontBold = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SetPropA(hDlg, PROP_FONT_BOLD, (HANDLE)hFontBold);

            /* ---- Header ------------------------------------------------- */
            HWND hTitle = CreateWindowExU8(0, "STATIC",
                "Поддержать DriveMonitor",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 16, cx - 40, 22, hDlg, NULL, g_hInst, NULL);
            SendMessageA(hTitle, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 46, cx - 40, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Explanation text -------------------------------------- */
            HWND hExplain = CreateWindowExU8(0, "STATIC",
                "DriveMonitor — свободное ПО с открытым исходным кодом.\n"
                "Нет ключа лицензии, нет пробного периода и нет активации.\n\n"
                "Если программа полезна, вы можете поддержать автора\n"
                "chuikoff на Boosty.",
                WS_CHILD | WS_VISIBLE,
                20, 56, cx - 40, 96, hDlg, NULL, g_hInst, NULL);
            SendMessageA(hExplain, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 158, cx - 40, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Author attribution ------------------------------------ */
            char authorLine[128];
            safe_snprintf(authorLine,
                "Автор: %s", DONATE_AUTHOR);
            HWND hAuthor = CreateWindowExU8(0, "STATIC", authorLine,
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 168, cx - 40, 18, hDlg, NULL, g_hInst, NULL);
            SendMessageA(hAuthor, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

            /* The link control displays the Boosty URL so the user
               can copy it even if they choose not to click. */
            HWND hLink = CreateWindowExU8(0, "STATIC", DONATE_URL,
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                20, 190, cx - 40, 20, hDlg,
                (HMENU)IDC_DONATE_LINK_STATIC, g_hInst, NULL);

            /* Underlined "link-style" font for the URL static. */
            HFONT hFontLink = CreateFontA(-12, 0, 0, 0, FW_NORMAL,
                FALSE, TRUE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SetPropA(hDlg, PROP_FONT_LINK, (HANDLE)hFontLink);
            SendMessageA(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 220, cx - 40, 2, hDlg, NULL, g_hInst, NULL);

            /* ---- Buttons ------------------------------------------------ */
            HWND hDonate = CreateWindowExU8(0, "BUTTON", "Поддержать",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                (cx - 240) / 2, 236, 110, 30, hDlg,
                (HMENU)IDC_DONATE_OPEN_BTN, g_hInst, NULL);
            SendMessageA(hDonate, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND hClose = CreateWindowExU8(0, "BUTTON", "Закрыть",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                (cx - 240) / 2 + 130, 236, 110, 30, hDlg,
                (HMENU)IDC_DONATE_CLOSE_BTN, g_hInst, NULL);
            SendMessageA(hClose, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            if (!s_hDonateHand)
                s_hDonateHand = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
        }
        return 0;

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

            /* Clicking the URL static also opens the browser. */
            if (nCtrl == IDC_DONATE_LINK_STATIC &&
                HIWORD(wParam) == STN_CLICKED) {
                Donate_OpenSponsorsPage(hDlg);
                return 0;
            }
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND hCtrl = (HWND)lParam;
            int  nID   = GetDlgCtrlID(hCtrl);
            HDC  hdcSt = (HDC)wParam;
            SetBkMode(hdcSt, TRANSPARENT);
            if (nID == IDC_DONATE_LINK_STATIC)
                SetTextColor(hdcSt, RGB(0, 102, 204));
            return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
        }

    case WM_SETCURSOR:
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hDlg, &pt);
            HWND hOver = ChildWindowFromPoint(hDlg, pt);
            if (hOver && GetDlgCtrlID(hOver) == IDC_DONATE_LINK_STATIC && s_hDonateHand) {
                SetCursor(s_hDonateHand);
                return TRUE;
            }
            break;
        }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hDlg);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        {
            HFONT hBold = (HFONT)GetPropA(hDlg, PROP_FONT_BOLD);
            HFONT hLink = (HFONT)GetPropA(hDlg, PROP_FONT_LINK);
            if (hBold) { DeleteObject(hBold); RemovePropA(hDlg, PROP_FONT_BOLD); }
            if (hLink) { DeleteObject(hLink); RemovePropA(hDlg, PROP_FONT_LINK); }
        }
        return 0;
    }

    return DefWindowProc(hDlg, uMsg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/*  Public: show the Donate dialog                                    */
/* ------------------------------------------------------------------ */

void Donate_ShowDialog(HWND hParent)
{
    /* Register the dialog window class exactly once per process. */
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

    /* If a donate dialog is already open, just bring it forward. */
    HWND hExist = FindWindowA("LLHDDonateDlg", NULL);
    if (hExist) {
        SetForegroundWindow(hExist);
        return;
    }

    /* Center on the parent window (or on screen if no parent). */
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int x, y;
    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        x = rcP.left + ((rcP.right  - rcP.left) - DONATE_DLG_W) / 2;
        y = rcP.top  + ((rcP.bottom - rcP.top ) - DONATE_DLG_H) / 2;
    } else {
        x = (scrW - DONATE_DLG_W) / 2;
        y = (scrH - DONATE_DLG_H) / 2;
    }

    HWND hDlg = CreateWindowExU8(
        WS_EX_DLGMODALFRAME,
        "LLHDDonateDlg",
        "DriveMonitor — поддержка",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, DONATE_DLG_W, DONATE_DLG_H,
        hParent, NULL, g_hInst, NULL);

    if (!hDlg) return;

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetFocus(hDlg);

    /* Modal-style message loop - runs until the dialog is closed. */
    MSG msg;
    while (IsWindow(hDlg) && GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public: no-op startup hook                                        */
/* ------------------------------------------------------------------ */

BOOL Donate_Startup(HWND hParent)
{
    /* Intentionally empty.  The program is unconditionally free, so
       there is no registration, no trial timer, and no install-date
       tracking to perform.  Kept only so the existing main.cpp call
       site does not require a separate patch. */
    (void)hParent;
    return TRUE;
}
