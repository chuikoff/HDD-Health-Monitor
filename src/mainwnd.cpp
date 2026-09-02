/* ============================================================================
 *  HDDHealth Monitor - Main window implementation
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  Author  : Ari Sohandri Putra
 *  Company : ARImetic Inc.
 *  Sponsor : https://github.com/sponsors/arisohandriputra/
 *  License : MIT
 *
 *  This translation unit implements the entire main-window experience:
 *    - Drive-selection buttons (custom owner-drawn buttons)
 *    - Health / Performance custom progress bars
 *    - S.M.A.R.T. attribute list view
 *    - Tray icon management with per-drive sub-icons
 *    - Device arrival / removal (hot-plug) handling
 *    - Temperature / health / failure critical alerts
 *    - About dialog
 *    - Save-screenshot feature (PNG via GDI+)
 *
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dbt.h>
/* MSVC-specific linker pragma.  MinGW / GCC ignores this with a warning,
   so we wrap it in _MSC_VER; the Makefile already passes -lmsimg32 in
   LDFLAGS for MinGW builds. */
#ifdef _MSC_VER
#pragma comment(lib, "Msimg32.lib")
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define GDIPVER 0x0110
#include <objbase.h>
#include <shlobj.h>
#include <gdiplus.h>
using namespace Gdiplus;

#include "mainwnd.h"
#include "smart.h"
#include "smart_history.h"
#include "donate.h"    
#include "lang.h"

static void FormatSmartValueW(BYTE bID, BYTE* pRaw,
                              BYTE bVal, BYTE bWorst, BYTE bThresh,
                              wchar_t* szBuf, int nBufLen);


static unsigned __int64 NVMeRead128Lo(const BYTE* p)
{
    unsigned __int64 lo = 0;
    int i;
    for (i = 7; i >= 0; i--) lo = (lo << 8) | p[i];
    return lo;
}

static WORD ReadLE16(const BYTE* p)
{
    return (WORD)p[0] | ((WORD)p[1] << 8);
}

DRIVE_INFO  g_Drives[MAX_DRIVES];
int         g_nDriveCount    = 0;
int         g_nSelectedDrive = 0;
HINSTANCE   g_hInst          = NULL;
HWND        g_hMainWnd       = NULL;
HWND        g_hHealthBar     = NULL;
HWND        g_hPerfBar       = NULL;
HWND        g_hDriveBtn[MAX_DRIVES];

static NOTIFYICONDATAA g_nid[MAX_DRIVES];
static BOOL            g_bTraySlot[MAX_DRIVES];
static int             g_nTrayCount    = 0;
static BOOL            g_bMinToTray    = FALSE;

static HDEVNOTIFY      g_hDevNotify    = NULL;
static DRIVE_INFO      g_PrevDrives[MAX_DRIVES];
static int             g_nPrevCount    = 0;
#define HOTPLUG_DELAY_MS  1200

#define ALERT_TEMP_WARN_C       55
#define ALERT_TEMP_CRITICAL_C   65
#define ALERT_HEALTH_WARN       40
#define ALERT_HEALTH_CRITICAL   20

typedef struct {
    BOOL bTempWarnSent;
    BOOL bTempCritSent;
    BOOL bHealthWarnSent;
    BOOL bHealthCritSent;
    BOOL bFailurePredSent;
    BOOL bNVMeCritWarnSent;
    BOOL bReallocSent;
    BOOL bUncorrectSent;
} DRIVE_ALERT_STATE;

static DRIVE_ALERT_STATE g_AlertState[MAX_DRIVES];
int g_nDpi = 96;
static BOOL g_bAlertStateInit = FALSE;

static void UpdateWindowTitle(HWND hWnd)
{
    SetWindowTextW(hWnd, LStrW(STR_APP_TITLE));
}

HBRUSH  g_hbrBG     = NULL;
HBRUSH  g_hbrPanel  = NULL;
HBRUSH  g_hbrGreen  = NULL;
HBRUSH  g_hbrYellow = NULL;
HBRUSH  g_hbrRed    = NULL;
HFONT   g_hFontTitle  = NULL;
HFONT   g_hFontNormal = NULL;
HFONT   g_hFontSmall  = NULL;
HFONT   g_hFontBig    = NULL;

static WNDPROC g_wpOrigBtn = NULL;

void DestroyGDIObjects(void)
{
    if (g_hbrBG)     { DeleteObject(g_hbrBG);     g_hbrBG = NULL; }
    if (g_hbrPanel)  { DeleteObject(g_hbrPanel);  g_hbrPanel = NULL; }
    if (g_hbrGreen)  { DeleteObject(g_hbrGreen);  g_hbrGreen = NULL; }
    if (g_hbrYellow) { DeleteObject(g_hbrYellow); g_hbrYellow = NULL; }
    if (g_hbrRed)    { DeleteObject(g_hbrRed);    g_hbrRed = NULL; }
    if (g_hFontTitle)  { DeleteObject(g_hFontTitle);  g_hFontTitle = NULL; }
    if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = NULL; }
    if (g_hFontSmall)  { DeleteObject(g_hFontSmall);  g_hFontSmall = NULL; }
    if (g_hFontBig)    { DeleteObject(g_hFontBig);    g_hFontBig = NULL; }
}

void CreateGDIObjects(void)
{
    DestroyGDIObjects();

    g_hbrBG     = CreateSolidBrush(CLR_BG);
    g_hbrPanel  = CreateSolidBrush(CLR_PANEL);
    g_hbrGreen  = CreateSolidBrush(CLR_GREEN);
    g_hbrYellow = CreateSolidBrush(CLR_YELLOW);
    g_hbrRed    = CreateSolidBrush(CLR_RED);

    const wchar_t* fontName = L"Microsoft YaHei UI";

    g_hFontTitle  = CreateFontW(MulDiv(-13, g_nDpi, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, fontName);
    g_hFontNormal = CreateFontW(MulDiv(-12, g_nDpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, fontName);
    g_hFontSmall  = CreateFontW(MulDiv(-11, g_nDpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, fontName);
    g_hFontBig    = CreateFontW(MulDiv(-32, g_nDpi, 96), 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, fontName);
}

static HICON BuildHealthIcon(int nHealth)
{
    const int SZ = 16;
    COLORREF clrBg;
    if      (nHealth < 0)   clrBg = RGB(30, 100, 210);
    else if (nHealth >= 70) clrBg = RGB(30, 150,  60);
    else if (nHealth >= 40) clrBg = RGB(180,120,   0);
    else                    clrBg = RGB(190,  30,  30);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcColor  = CreateCompatibleDC(hdcScreen);
    HDC hdcMask   = CreateCompatibleDC(hdcScreen);

    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, SZ, SZ);
    HBITMAP hbmMask  = CreateBitmap(SZ, SZ, 1, 1, NULL);

    HBITMAP hbmOldColor = (HBITMAP)SelectObject(hdcColor, hbmColor);
    HBITMAP hbmOldMask  = (HBITMAP)SelectObject(hdcMask,  hbmMask);

    HBRUSH hbrBg = CreateSolidBrush(clrBg);
    RECT rc = { 0, 0, SZ, SZ };
    FillRect(hdcColor, &rc, hbrBg);
    DeleteObject(hbrBg);

    char szText[8];
    if (nHealth < 0)
        lstrcpyA(szText, "--");
    else
        _snprintf(szText, sizeof(szText), "%d", nHealth);

    HFONT hFont = CreateFontA(-9, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HFONT hOldFont = (HFONT)SelectObject(hdcColor, hFont);
    SetBkMode(hdcColor, TRANSPARENT);
    SetTextColor(hdcColor, RGB(255, 255, 255));

    int len = (int)strlen(szText);
    SIZE sz;
    GetTextExtentPoint32A(hdcColor, szText, len, &sz);
    int x = (SZ - sz.cx) / 2;
    int y = (SZ - sz.cy) / 2;
    TextOutA(hdcColor, x, y, szText, len);

    SelectObject(hdcColor, hOldFont);
    DeleteObject(hFont);

    PatBlt(hdcMask, 0, 0, SZ, SZ, BLACKNESS);

    SelectObject(hdcColor, hbmOldColor);
    SelectObject(hdcMask,  hbmOldMask);

    DeleteDC(hdcColor);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdcScreen);

    ICONINFO ii;
    ii.fIcon    = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmColor = hbmColor;
    ii.hbmMask  = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return hIcon;
}

static void TrayBalloon(const char* szTitle, const char* szMsg, DWORD niif)
{
    if (!g_bTraySlot[0]) return;

    NOTIFYICONDATAA nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize      = sizeof(NOTIFYICONDATAA);
    nid.hWnd        = g_nid[0].hWnd;
    nid.uID         = g_nid[0].uID;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = niif;
    nid.uTimeout    = 4000;
    lstrcpynA(nid.szInfoTitle, szTitle, sizeof(nid.szInfoTitle) - 1);
    lstrcpynA(nid.szInfo,      szMsg,   sizeof(nid.szInfo)      - 1);
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

static ULONG_PTR g_gdiplusToken = 0;

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT nEncoders = 0, nSize = 0;
    GetImageEncodersSize(&nEncoders, &nSize);
    if (nSize == 0) return -1;

    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(nSize);
    if (!pInfo) return -1;
    GetImageEncoders(nEncoders, nSize, pInfo);

    for (UINT i = 0; i < nEncoders; i++) {
        if (wcscmp(pInfo[i].MimeType, format) == 0) {
            *pClsid = pInfo[i].Clsid;
            free(pInfo);
            return (int)i;
        }
    }
    free(pInfo);
    return -1;
}

static BOOL SaveScreenshotPNG(HWND hWnd, char* szPathOut, int nPathMax,
                               char* szErrOut, int nErrMax)
{

    char szDocDir[MAX_PATH] = "";
    if (!SHGetSpecialFolderPathA(NULL, szDocDir, CSIDL_PERSONAL, TRUE)) {

        GetModuleFileNameA(NULL, szDocDir, MAX_PATH);
        char* p = strrchr(szDocDir, '\\');
        if (p) *p = '\0';
    }
    char szOutDir[MAX_PATH];
    _snprintf(szOutDir, sizeof(szOutDir), "%s\\HDDH_Screenshots", szDocDir);
    CreateDirectoryA(szOutDir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char szFile[MAX_PATH];
    _snprintf(szFile, sizeof(szFile),
        "%s\\HDDH_%04d%02d%02d_%02d%02d%02d.png",
        szOutDir,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    if (szPathOut) lstrcpynA(szPathOut, szFile, nPathMax);

    RECT rc;
    GetClientRect(hWnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        if (szErrOut) lstrcpynA(szErrOut, "Window has zero size.", nErrMax);
        return FALSE;
    }

    HDC hdcWin = GetDC(hWnd);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWin, w, h);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    if (!PrintWindow(hWnd, hdcMem, PW_CLIENTONLY)) {

        POINT pt = {rc.left, rc.top};
        ClientToScreen(hWnd, &pt);
        HDC hdcScreen = GetDC(NULL);
        BitBlt(hdcMem, 0, 0, w, h, hdcScreen, pt.x, pt.y, SRCCOPY);
        ReleaseDC(NULL, hdcScreen);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    ReleaseDC(hWnd, hdcWin);

    BOOL bOK = FALSE;
    Bitmap* pBmp = Bitmap::FromHBITMAP(hbm, NULL);
    if (pBmp && pBmp->GetLastStatus() == Ok) {
        CLSID clsidPng;
        if (GetEncoderClsid(L"image/png", &clsidPng) >= 0) {

            WCHAR wszFile[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, szFile, -1, wszFile, MAX_PATH);
            Status st2 = pBmp->Save(wszFile, &clsidPng, NULL);
            bOK = (st2 == Ok);
            if (!bOK && szErrOut)
                _snprintf(szErrOut, nErrMax,
                    "GDI+ Save failed (status %d).", (int)st2);
        } else {
            if (szErrOut) lstrcpynA(szErrOut, "PNG encoder not found.", nErrMax);
        }
    } else {
        if (szErrOut) lstrcpynA(szErrOut, "GDI+ Bitmap creation failed.", nErrMax);
    }
    delete pBmp;
    DeleteObject(hbm);
    return bOK;
}

static void DoSaveScreenshot(HWND hWnd)
{
    char szPath[MAX_PATH] = "";
    char szErr[256]       = "";
    if (SaveScreenshotPNG(hWnd, szPath, MAX_PATH, szErr, sizeof(szErr))) {
        char szMsg[MAX_PATH + 128];
        _snprintf(szMsg, sizeof(szMsg),
            LStr(STR_SS_SUCCESS_MSG), szPath);
        int nRet = MessageBoxA(hWnd, szMsg, LStr(STR_SS_SUCCESS_TITLE),
                               MB_YESNO | MB_ICONINFORMATION);
        if (nRet == IDYES) {
            char szCmd[MAX_PATH + 32];
            _snprintf(szCmd, sizeof(szCmd), "/select,\"%s\"", szPath);
            ShellExecuteA(NULL, "open", "explorer.exe", szCmd, NULL, SW_SHOWNORMAL);
        }
    } else {
        char szMsg[320];
        _snprintf(szMsg, sizeof(szMsg), LStr(STR_SS_FAIL_MSG), szErr);
        MessageBoxA(hWnd, szMsg, LStr(STR_SS_FAIL_TITLE), MB_OK | MB_ICONERROR);
    }
}

static void ExportToTextFile(HWND hWnd)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t szDefName[MAX_PATH];
    _snwprintf(szDefName, MAX_PATH, L"HDD_Health_Report_%04d%02d%02d_%02d%02d%02d.txt",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    wchar_t szFile[MAX_PATH];
    wcsncpy(szFile, szDefName, MAX_PATH);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hWnd;
    ofn.lpstrFilter = (Lang_GetCurrent() == LANG_ZH_CN)
                      ? L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0"
                      : L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn))
        return;

    FILE* fp = _wfopen(szFile, L"wb");
    if (!fp) {
        wchar_t szErr[512];
        _snwprintf(szErr, 512, LStrW(STR_REPORT_FAIL_MSG), szFile);
        MessageBoxW(hWnd, szErr, LStrW(STR_REPORT_FAIL_TITLE), MB_OK | MB_ICONERROR);
        return;
    }

    /* Write UTF-8 BOM */
    unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, fp);

    BOOL bZH = (Lang_GetCurrent() == LANG_ZH_CN);

    /* Write Report Header */
    fprintf(fp, "================================================================================\r\n");
    if (bZH) {
        fprintf(fp, "  HDDHealth Monitor 1.1 - 硬盘健康与 S.M.A.R.T. 综合检测报告\r\n");
        fprintf(fp, "  生成时间: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(fp, "  检测硬盘总数: %d\r\n", g_nDriveCount);
    } else {
        fprintf(fp, "  HDDHealth Monitor 1.1 - Drive Health & S.M.A.R.T. Report\r\n");
        fprintf(fp, "  Generated: %04d-%02d-%02d %02d:%02d:%02d\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(fp, "  Total Drives Detected: %d\r\n", g_nDriveCount);
    }
    fprintf(fp, "================================================================================\r\n\r\n");

    int i;
    for (i = 0; i < g_nDriveCount; i++) {
        DRIVE_INFO* pInfo = &g_Drives[i];
        char szCap[32];
        FormatSize(pInfo->dwCapacityMB, szCap, sizeof(szCap));

        fprintf(fp, "--------------------------------------------------------------------------------\r\n");
        if (bZH) {
            fprintf(fp, "[磁盘 %d] %s (%s)\r\n",
                    pInfo->nDriveIndex, pInfo->szModel, Lang_GetDriveTypeName(pInfo->eType));
            fprintf(fp, "--------------------------------------------------------------------------------\r\n");
            fprintf(fp, "  型号 (Model):         %s\r\n", pInfo->szModel[0] ? pInfo->szModel : "-");
            fprintf(fp, "  序列号 (Serial):      %s\r\n", pInfo->szSerial[0] ? pInfo->szSerial : "-");
            fprintf(fp, "  固件版本 (Firmware):  %s\r\n", pInfo->szFirmware[0] ? pInfo->szFirmware : "-");
            fprintf(fp, "  存储容量 (Capacity):  %s\r\n", szCap);
            fprintf(fp, "  接口类型 (Type):      %s\r\n", Lang_GetDriveTypeName(pInfo->eType));
            if (pInfo->nTemperatureC > 0)
                fprintf(fp, "  当前温度 (Temp):      %d °C (%d °F)\r\n", pInfo->nTemperatureC, pInfo->nTemperatureC * 9 / 5 + 32);
            else
                fprintf(fp, "  当前温度 (Temp):      未知 / 不支持\r\n");

            if (pInfo->nHealthPercent >= 0)
                fprintf(fp, "  健康状态 (Health):    %d%% (%s)\r\n", pInfo->nHealthPercent, Lang_GetHealthStatusName(pInfo->eHealthStatus));
            else
                fprintf(fp, "  健康状态 (Health):    未知\r\n");

            if (pInfo->nPerformancePercent >= 0)
                fprintf(fp, "  性能评级 (Perf):      %d%%\r\n", pInfo->nPerformancePercent);

            if (pInfo->nReadSpeedMBs > 0)
                fprintf(fp, "  连续读取速度:         %d MB/s\r\n", pInfo->nReadSpeedMBs);

            fprintf(fp, "\r\n  【S.M.A.R.T. / NVMe 健康数据明细】\r\n");
            fprintf(fp, "  %-6s  %-36s  %-36s  %-8s\r\n", "ID", "属性名称", "当前值 / 详情", "状态");
            fprintf(fp, "  --------------------------------------------------------------------------------------\r\n");
        } else {
            fprintf(fp, "[Drive %d] %s (%s)\r\n",
                    pInfo->nDriveIndex, pInfo->szModel, Lang_GetDriveTypeName(pInfo->eType));
            fprintf(fp, "--------------------------------------------------------------------------------\r\n");
            fprintf(fp, "  Model:                %s\r\n", pInfo->szModel[0] ? pInfo->szModel : "-");
            fprintf(fp, "  Serial No.:           %s\r\n", pInfo->szSerial[0] ? pInfo->szSerial : "-");
            fprintf(fp, "  Firmware:             %s\r\n", pInfo->szFirmware[0] ? pInfo->szFirmware : "-");
            fprintf(fp, "  Capacity:             %s\r\n", szCap);
            fprintf(fp, "  Type:                 %s\r\n", Lang_GetDriveTypeName(pInfo->eType));
            if (pInfo->nTemperatureC > 0)
                fprintf(fp, "  Temperature:          %d °C (%d °F)\r\n", pInfo->nTemperatureC, pInfo->nTemperatureC * 9 / 5 + 32);
            else
                fprintf(fp, "  Temperature:          N/A\r\n");

            if (pInfo->nHealthPercent >= 0)
                fprintf(fp, "  Health Status:        %d%% (%s)\r\n", pInfo->nHealthPercent, Lang_GetHealthStatusName(pInfo->eHealthStatus));
            else
                fprintf(fp, "  Health Status:        N/A\r\n");

            if (pInfo->nPerformancePercent >= 0)
                fprintf(fp, "  Performance:          %d%%\r\n", pInfo->nPerformancePercent);

            if (pInfo->nReadSpeedMBs > 0)
                fprintf(fp, "  Read Speed:           %d MB/s\r\n", pInfo->nReadSpeedMBs);

            fprintf(fp, "\r\n  【S.M.A.R.T. / NVMe Health Attributes】\r\n");
            fprintf(fp, "  %-6s  %-36s  %-36s  %-8s\r\n", "ID", "Attribute", "Value / Info", "Status");
            fprintf(fp, "  --------------------------------------------------------------------------------------\r\n");
        }

        if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
            NVME_HEALTH_INFO_LOG* pLog = &pInfo->nvmeHealth;
            unsigned __int64 qwDataRead    = NVMeRead128Lo(pLog->DataUnitsRead);
            unsigned __int64 qwDataWritten = NVMeRead128Lo(pLog->DataUnitsWritten);
            unsigned __int64 qwPOH         = NVMeRead128Lo(pLog->PowerOnHours);
            unsigned __int64 qwPowerCycles = NVMeRead128Lo(pLog->PowerCycles);
            unsigned __int64 qwUnsafeSDs   = NVMeRead128Lo(pLog->UnsafeShutdowns);
            unsigned __int64 qwMediaErr    = NVMeRead128Lo(pLog->MediaErrors);
            unsigned __int64 qwErrLog      = NVMeRead128Lo(pLog->NumErrLogEntries);
            WORD wTempK = ReadLE16(pLog->CompositeTemperature);
            int  nTempC = (wTempK > 273) ? (int)wTempK - 273 : 0;

            #define DUMP_NVME(id_, name_, val_, stat_) \
                fprintf(fp, "  %-6s  %-36s  %-36s  %-8s\r\n", id_, name_, val_, stat_)

            char szCrit[64], szVBuf[64], szSpare[32], szSpTh[32], szPctU[32];
            char szDUR[64], szDUW[64], szPOH[32], szPC[32];
            char szUS[32], szME[32], szEL[32], szWCT[32], szCCT[32];

            if (pLog->CriticalWarning == 0) _snprintf(szCrit, sizeof(szCrit), bZH ? "0 (无)" : "0 (None)");
            else _snprintf(szCrit, sizeof(szCrit), "0x%02X (!)", pLog->CriticalWarning);
            DUMP_NVME("01h", bZH ? "严重警告" : "Critical Warning", szCrit, (pLog->CriticalWarning ? "Warning" : "OK"));

            _snprintf(szVBuf, sizeof(szVBuf), "%d °C (%d K)", nTempC, (int)wTempK);
            DUMP_NVME("02h", bZH ? "综合温度" : "Composite Temperature", szVBuf, (nTempC > 70 ? "Warning" : "OK"));

            _snprintf(szSpare, sizeof(szSpare), "%d %%", (int)pLog->AvailableSpare);
            DUMP_NVME("03h", bZH ? "可用预留空间" : "Available Spare", szSpare,
                      (pLog->AvailableSpare < pLog->AvailableSpareThreshold ? "Warning" : "OK"));

            _snprintf(szSpTh, sizeof(szSpTh), "%d %%", (int)pLog->AvailableSpareThreshold);
            DUMP_NVME("04h", bZH ? "预留空间阈值" : "Available Spare Threshold", szSpTh, "--");

            _snprintf(szPctU, sizeof(szPctU), "%d %%", (int)pLog->PercentageUsed);
            DUMP_NVME("05h", bZH ? "已用寿命百分比" : "Percentage Used (Endurance)", szPctU,
                      (pLog->PercentageUsed >= 100 ? "Warning" : "OK"));

            if (qwDataRead > 2048) _snprintf(szDUR, sizeof(szDUR), "%llu GB", (unsigned __int64)(qwDataRead / 2048));
            else _snprintf(szDUR, sizeof(szDUR), "%llu %s", qwDataRead, LStr(STR_UNIT_UNITS));
            DUMP_NVME("06h", bZH ? "总读取数据量" : "Data Units Read", szDUR, "OK");

            if (qwDataWritten > 2048) _snprintf(szDUW, sizeof(szDUW), "%llu GB", (unsigned __int64)(qwDataWritten / 2048));
            else _snprintf(szDUW, sizeof(szDUW), "%llu %s", qwDataWritten, LStr(STR_UNIT_UNITS));
            DUMP_NVME("07h", bZH ? "总写入数据量" : "Data Units Written", szDUW, "OK");

            _snprintf(szPOH, sizeof(szPOH), bZH ? "%llu 小时" : "%llu hours", qwPOH);
            DUMP_NVME("09h", bZH ? "通电时间" : "Power On Hours", szPOH, "OK");

            _snprintf(szPC, sizeof(szPC), "%llu", qwPowerCycles);
            DUMP_NVME("0Ch", bZH ? "通电次数" : "Power Cycles", szPC, "OK");

            _snprintf(szUS, sizeof(szUS), "%llu", qwUnsafeSDs);
            DUMP_NVME("10h", bZH ? "不安全关机计数" : "Unsafe Shutdowns", szUS, "OK");

            _snprintf(szME, sizeof(szME), "%llu", qwMediaErr);
            DUMP_NVME("11h", bZH ? "介质与数据完整性错误" : "Media and Data Integrity Errors", szME, (qwMediaErr > 0 ? "Warning" : "OK"));

            _snprintf(szEL, sizeof(szEL), "%llu", qwErrLog);
            DUMP_NVME("12h", bZH ? "错误日志条目数" : "Number of Error Log Entries", szEL, (qwErrLog > 0 ? "Warning" : "OK"));

            _snprintf(szWCT, sizeof(szWCT), bZH ? "%lu 分钟" : "%lu min", pLog->WarningCompTempTime);
            DUMP_NVME("13h", bZH ? "警告温度持续时间" : "Warning Composite Temp Time", szWCT, (pLog->WarningCompTempTime > 0 ? "Warning" : "OK"));

            _snprintf(szCCT, sizeof(szCCT), bZH ? "%lu 分钟" : "%lu min", pLog->CriticalCompTempTime);
            DUMP_NVME("14h", bZH ? "严重温度持续时间" : "Critical Composite Temp Time", szCCT, (pLog->CriticalCompTempTime > 0 ? "Warning" : "OK"));

            #undef DUMP_NVME
        }
        else if (pInfo->bSMART_Supported) {
            int j, k;
            for (j = 0; j < 30; j++) {
                SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[j];
                if (pAttr->bAttrID == 0) continue;

                BYTE bThresh = 0;
                for (k = 0; k < 30; k++) {
                    if (pInfo->threshData.stThresholds[k].bAttrID == pAttr->bAttrID) {
                        bThresh = pInfo->threshData.stThresholds[k].bThresholdValue;
                        break;
                    }
                }
                BOOL bFailed = (bThresh > 0 && pAttr->bAttrValue <= bThresh);
                wchar_t szValW[128];
                FormatSmartValueW(pAttr->bAttrID, pAttr->bRawValue,
                                 pAttr->bAttrValue, pAttr->bWorstValue, bThresh,
                                 szValW, 128);

                char szID[16];
                _snprintf(szID, sizeof(szID), "%02Xh", pAttr->bAttrID);

                const char* szStatus = bFailed ? (bZH ? "异常" : "FAILED") :
                                       (bThresh > 0 && pAttr->bAttrValue < bThresh + 10) ? (bZH ? "警告" : "Warning") : "OK";

                char szValUtf8[256];
                WideCharToMultiByte(CP_UTF8, 0, szValW, -1, szValUtf8, sizeof(szValUtf8), NULL, NULL);

                fprintf(fp, "  %-6s  %-36s  %-36s  %-8s\r\n",
                        szID, Lang_GetSmartAttrName(pAttr->bAttrID), szValUtf8, szStatus);
            }
        } else {
            fprintf(fp, "  %s\r\n", bZH ? "此硬盘未提供完整 S.M.A.R.T. 属性表。" : "SMART attributes table not exposed by this drive/bridge.");
        }

        fprintf(fp, "\r\n\r\n");
    }

    fclose(fp);

    /* Prompt user */
    wchar_t szPrompt[512];
    _snwprintf(szPrompt, 512, LStrW(STR_REPORT_SAVED_MSG), szFile);
    if (MessageBoxW(hWnd, szPrompt, LStrW(STR_REPORT_SAVED_TITLE), MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        ShellExecuteW(NULL, L"open", szFile, NULL, NULL, SW_SHOWNORMAL);
    }
}

static void Snapshot_Save(void)
{
    int i;
    g_nPrevCount = g_nDriveCount;
    for (i = 0; i < g_nDriveCount; i++)
        g_PrevDrives[i] = g_Drives[i];
}

static const char* DriveName(DRIVE_INFO* pD)
{
    return (strlen(pD->szModel) > 0) ? pD->szModel : "Drive";
}

static void CheckCriticalAlerts(void)
{
    int i;

    if (!g_bAlertStateInit) {
        ZeroMemory(g_AlertState, sizeof(g_AlertState));
        g_bAlertStateInit = TRUE;
    }

    for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
        DRIVE_INFO*       pD = &g_Drives[i];
        DRIVE_ALERT_STATE* pA = &g_AlertState[i];
        char szTitle[128], szMsg[512];

        if (i < g_nPrevCount) {
            BOOL bSameSerial = (strlen(pD->szSerial) > 0 &&
                                strcmp(pD->szSerial, g_PrevDrives[i].szSerial) == 0);
            BOOL bSameModel  = (strlen(pD->szModel) > 0 &&
                                strcmp(pD->szModel,  g_PrevDrives[i].szModel)  == 0);
            if (!bSameSerial && !bSameModel)
                ZeroMemory(pA, sizeof(*pA));
        }

        if (!pD->bSMART_Supported) continue;

        if (pD->nTemperatureC > 0) {
            if (pD->nTemperatureC >= ALERT_TEMP_CRITICAL_C && !pA->bTempCritSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_OVERHEAT_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_OVERHEAT_MSG),
                    DriveName(pD), pD->nTemperatureC, ALERT_TEMP_CRITICAL_C);
                TrayBalloon(szTitle, szMsg, NIIF_ERROR);
                pA->bTempCritSent = TRUE;
                pA->bTempWarnSent = TRUE;
            } else if (pD->nTemperatureC >= ALERT_TEMP_WARN_C && !pA->bTempWarnSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_TEMP_WARN_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_TEMP_WARN_MSG),
                    DriveName(pD), pD->nTemperatureC, ALERT_TEMP_WARN_C);
                TrayBalloon(szTitle, szMsg, NIIF_WARNING);
                pA->bTempWarnSent = TRUE;
            }

            if (pD->nTemperatureC < ALERT_TEMP_WARN_C - 5) {
                pA->bTempWarnSent = FALSE;
                pA->bTempCritSent = FALSE;
            }
        }

        if (pD->nHealthPercent >= 0) {
            if (pD->nHealthPercent < ALERT_HEALTH_CRITICAL && !pA->bHealthCritSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_HEALTH_CRIT_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_HEALTH_CRIT_MSG),
                    DriveName(pD), pD->nHealthPercent);
                TrayBalloon(szTitle, szMsg, NIIF_ERROR);
                pA->bHealthCritSent = TRUE;
                pA->bHealthWarnSent = TRUE;
            } else if (pD->nHealthPercent < ALERT_HEALTH_WARN && !pA->bHealthWarnSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_HEALTH_WARN_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_HEALTH_WARN_MSG),
                    DriveName(pD), pD->nHealthPercent);
                TrayBalloon(szTitle, szMsg, NIIF_WARNING);
                pA->bHealthWarnSent = TRUE;
            }
        }

        if (pD->bPredictFailure && !pA->bFailurePredSent) {
            lstrcpyA(szTitle, LStr(STR_ALERT_FAILURE_PRED_TITLE));
            _snprintf(szMsg, sizeof(szMsg),
                LStr(STR_ALERT_FAILURE_PRED_MSG),
                DriveName(pD));
            TrayBalloon(szTitle, szMsg, NIIF_ERROR);
            pA->bFailurePredSent = TRUE;
        }

        if (pD->bIsNVMe) {
            BYTE crit = pD->nvmeHealth.CriticalWarning;
            if (crit != 0 && !pA->bNVMeCritWarnSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_NVME_CRIT_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    "%s\n%s%s%s%s%s%s",
                    DriveName(pD),
                    LStr(STR_NVME_CRIT_HEADER),
                    (crit & NVME_CRIT_WARN_SPARE_BELOW_THRESH)   ? LStr(STR_NVME_CRIT_SPARE)       : "",
                    (crit & NVME_CRIT_WARN_TEMP_THRESHOLD)       ? LStr(STR_NVME_CRIT_TEMP)        : "",
                    (crit & NVME_CRIT_WARN_RELIABILITY_DEGRADED) ? LStr(STR_NVME_CRIT_RELIABILITY)   : "",
                    (crit & NVME_CRIT_WARN_READ_ONLY)            ? LStr(STR_NVME_CRIT_READ_ONLY)     : "",
                    (crit & NVME_CRIT_WARN_VOLATILE_MEM_BACKUP)  ? LStr(STR_NVME_CRIT_VOLATILE_MEM)  : "");
                TrayBalloon(szTitle, szMsg, NIIF_ERROR);
                pA->bNVMeCritWarnSent = TRUE;
            }
            if (crit == 0) pA->bNVMeCritWarnSent = FALSE;
        }

        if (!pD->bIsNVMe && !pD->bIsUSB) {
            int k;
            DWORD dwR05 = 0, dwRC6 = 0;
            for (k = 0; k < 30; k++) {
                BYTE  id = pD->attrData.stAttributes[k].bAttrID;
                DWORD dw = GetRawValue(pD->attrData.stAttributes[k].bRawValue);
                if (id == 0x05) dwR05 = dw;
                if (id == 0xC6) dwRC6 = dw;
            }
            if (dwR05 > 0 && !pA->bReallocSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_REALLOC_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_REALLOC_MSG),
                    DriveName(pD), (unsigned long)dwR05);
                TrayBalloon(szTitle, szMsg, NIIF_WARNING);
                pA->bReallocSent = TRUE;
            }
            if (dwRC6 > 0 && !pA->bUncorrectSent) {
                lstrcpyA(szTitle, LStr(STR_ALERT_UNCORRECT_TITLE));
                _snprintf(szMsg, sizeof(szMsg),
                    LStr(STR_ALERT_UNCORRECT_MSG),
                    DriveName(pD), (unsigned long)dwRC6);
                TrayBalloon(szTitle, szMsg, NIIF_ERROR);
                pA->bUncorrectSent = TRUE;
            }
        }
    }

    for (i = g_nDriveCount; i < MAX_DRIVES; i++)
        ZeroMemory(&g_AlertState[i], sizeof(DRIVE_ALERT_STATE));
}

static void Snapshot_Diff(void)
{

    (void)g_nPrevCount;
}

static void DeviceNotify_Register(HWND hWnd)
{
    static const GUID GUID_DEVINTERFACE_DISK =
    { 0x53F56307, 0xB6BF, 0x11D0,
      { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };

    DEV_BROADCAST_DEVICEINTERFACE_A dbi;
    ZeroMemory(&dbi, sizeof(dbi));
    dbi.dbcc_size       = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid  = GUID_DEVINTERFACE_DISK;

    g_hDevNotify = RegisterDeviceNotificationA(hWnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static void DeviceNotify_Unregister(void)
{
    if (g_hDevNotify) {
        UnregisterDeviceNotification(g_hDevNotify);
        g_hDevNotify = NULL;
    }
}

void TrayIcon_Add(HWND hWnd)
{
    int i;
    ZeroMemory(g_nid,      sizeof(g_nid));
    ZeroMemory(g_bTraySlot,sizeof(g_bTraySlot));
    g_nTrayCount = 0;

    g_nid[0].cbSize           = sizeof(NOTIFYICONDATAA);
    g_nid[0].hWnd             = hWnd;
    g_nid[0].uID              = IDI_TRAY + 0;
    g_nid[0].uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid[0].uCallbackMessage = WM_TRAYICON;
    g_nid[0].hIcon            = BuildHealthIcon(-1);
    lstrcpyA(g_nid[0].szTip,  LStr(STR_TRAY_SCANNING));
    Shell_NotifyIconA(NIM_ADD, &g_nid[0]);
    g_bTraySlot[0] = TRUE;
    g_nTrayCount   = 1;
    (void)i;
}

void TrayIcon_Remove(void)
{
    int i;
    for (i = 0; i < MAX_DRIVES; i++) {
        if (g_bTraySlot[i]) {
            if (g_nid[i].hIcon) { DestroyIcon(g_nid[i].hIcon); g_nid[i].hIcon = NULL; }
            Shell_NotifyIconA(NIM_DELETE, &g_nid[i]);
            g_bTraySlot[i] = FALSE;
        }
    }
    g_nTrayCount = 0;
}

void TrayIcon_Update(void)
{
    int i;
    HWND hWnd = g_hMainWnd;

    for (i = g_nDriveCount; i < MAX_DRIVES; i++) {
        if (g_bTraySlot[i]) {
            if (g_nid[i].hIcon) { DestroyIcon(g_nid[i].hIcon); g_nid[i].hIcon = NULL; }
            Shell_NotifyIconA(NIM_DELETE, &g_nid[i]);
            g_bTraySlot[i] = FALSE;
        }
    }

    for (i = 0; i < g_nDriveCount; i++) {
        int    h       = g_Drives[i].nHealthPercent;
        UINT   uID     = (UINT)(IDI_TRAY + i);
        char   szTip[128];
        HICON  hNewIcon = BuildHealthIcon(h);

        if (h >= 0)
            _snprintf(szTip, sizeof(szTip), LStr(STR_TRAY_HEALTH_FMT),
                      g_Drives[i].nDriveIndex, h,
                      (strlen(g_Drives[i].szModel) ? g_Drives[i].szModel : LStr(STR_APP_TITLE)));
        else
            _snprintf(szTip, sizeof(szTip), LStr(STR_TRAY_HEALTH_NA_FMT),
                      g_Drives[i].nDriveIndex,
                      (strlen(g_Drives[i].szModel) ? g_Drives[i].szModel : LStr(STR_APP_TITLE)));

        if (!g_bTraySlot[i]) {
            ZeroMemory(&g_nid[i], sizeof(NOTIFYICONDATAA));
            g_nid[i].cbSize           = sizeof(NOTIFYICONDATAA);
            g_nid[i].hWnd             = hWnd;
            g_nid[i].uID              = uID;
            g_nid[i].uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_nid[i].uCallbackMessage = WM_TRAYICON;
            g_nid[i].hIcon            = hNewIcon;
            lstrcpynA(g_nid[i].szTip, szTip, sizeof(g_nid[i].szTip) - 1);
            Shell_NotifyIconA(NIM_ADD, &g_nid[i]);
            g_bTraySlot[i] = TRUE;
        } else {
            if (g_nid[i].hIcon) DestroyIcon(g_nid[i].hIcon);
            g_nid[i].hIcon  = hNewIcon;
            g_nid[i].uFlags = NIF_ICON | NIF_TIP;
            lstrcpynA(g_nid[i].szTip, szTip, sizeof(g_nid[i].szTip) - 1);
            Shell_NotifyIconA(NIM_MODIFY, &g_nid[i]);
        }
    }

    g_nTrayCount = g_nDriveCount;
}

void TrayIcon_ShowContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuA(hMenu, MF_STRING, IDM_SHOW_WINDOW,  LStr(STR_MENU_SHOW_WINDOW));
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, IDM_EXIT, LStr(STR_MENU_EXIT));

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

COLORREF GetHealthColor(int nHealth)
{
    if (nHealth < 0)   return CLR_ACCENT;
    if (nHealth >= 70) return CLR_GREEN;
    if (nHealth >= 40) return CLR_YELLOW;
    return CLR_RED;
}

static void DrawGlassShine(HDC hdc, RECT* prc)
{
    int w = prc->right  - prc->left;
    int h = (prc->bottom - prc->top) / 2;
    if (w <= 0 || h <= 0) return;

    HDC     hdcMem  = CreateCompatibleDC(hdc);
    HBITMAP hbm     = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP hbmOld  = (HBITMAP)SelectObject(hdcMem, hbm);

    int y;
    for (y = 0; y < h; y++) {
        int alpha = 110 - (int)((110 - 18) * y / (h > 1 ? h - 1 : 1));
        int r = (255 * alpha) / 255;
        int g = (255 * alpha) / 255;
        int b = (255 * alpha) / 255;
        HBRUSH hbrLine = CreateSolidBrush(RGB(r, g, b));
        RECT rcLine = { 0, y, w, y + 1 };
        FillRect(hdcMem, &rcLine, hbrLine);
        DeleteObject(hbrLine);
    }

    for (y = 0; y < h; y++) {
        int alpha = 110 - (int)((110 - 18) * y / (h > 1 ? h - 1 : 1));
        BLENDFUNCTION bfRow = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };
        AlphaBlend(hdc, prc->left, prc->top + y, w, 1,
                   hdcMem, 0, y, w, 1, bfRow);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
}

static void DrawRoundRect(HDC hdc, RECT* prc, int rx, HBRUSH hbr, HPEN hpen)
{
    HPEN   hOldPen = (HPEN)SelectObject(hdc, hpen);
    HBRUSH hOldBr  = (HBRUSH)SelectObject(hdc, hbr);
    RoundRect(hdc, prc->left, prc->top, prc->right, prc->bottom, rx * 2, rx * 2);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBr);
}

LRESULT CALLBACK PerfBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HealthBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            HDC     hdc    = CreateCompatibleDC(hdcReal);
            HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
            HBITMAP hbmOldBuf = (HBITMAP)SelectObject(hdc, hbmBuf);

            int nHealth = -1;
            if (g_nDriveCount > 0 && g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount)
                nHealth = g_Drives[g_nSelectedDrive].nHealthPercent;

            int cxBar = rc.right  - rc.left;
            int cyBar = rc.bottom - rc.top;
            int rx    = 5;

            {
                COLORREF clrTop, clrBot;
                if (nHealth < 0 || nHealth == 100) {
                    clrTop = RGB( 50, 200,  80);
                    clrBot = RGB( 20, 140,  50);
                } else if (nHealth >= 70) {
                    clrTop = RGB( 90, 210,  60);
                    clrBot = RGB( 50, 155,  30);
                } else if (nHealth >= 40) {
                    clrTop = RGB(240, 160,  20);
                    clrBot = RGB(190, 110,   5);
                } else {
                    clrTop = RGB(220,  50,  40);
                    clrBot = RGB(160,  20,  15);
                }
                int y;
                for (y = 0; y < cyBar; y++) {
                    float t  = (cyBar > 1) ? (float)y / (float)(cyBar - 1) : 0.0f;
                    int cr = (int)(GetRValue(clrTop) + t * (int)(GetRValue(clrBot) - GetRValue(clrTop)));
                    int cg = (int)(GetGValue(clrTop) + t * (int)(GetGValue(clrBot) - GetGValue(clrTop)));
                    int cb = (int)(GetBValue(clrTop) + t * (int)(GetBValue(clrBot) - GetBValue(clrTop)));
                    HPEN hpCol = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
                    HPEN hpOld = (HPEN)SelectObject(hdc, hpCol);
                    MoveToEx(hdc, 0,     y, NULL);
                    LineTo  (hdc, cxBar, y);
                    SelectObject(hdc, hpOld);
                    DeleteObject(hpCol);
                }
            }

            int nFillPct = (nHealth < 0) ? 100 : nHealth;
            int nFillW   = (cxBar * nFillPct / 100);
            if (nFillW < cxBar) {
                int emptyW = cxBar - nFillW;
                HDC hdcMem2 = CreateCompatibleDC(hdc);
                HBITMAP hbm2 = CreateCompatibleBitmap(hdc, emptyW, cyBar);
                HBITMAP hbmOld2 = (HBITMAP)SelectObject(hdcMem2, hbm2);
                HBRUSH hbrDark = CreateSolidBrush(RGB(20, 20, 20));
                RECT rcFill2 = { 0, 0, emptyW, cyBar };
                FillRect(hdcMem2, &rcFill2, hbrDark);
                DeleteObject(hbrDark);
                BLENDFUNCTION bf2 = { AC_SRC_OVER, 0, 155, 0 };
                AlphaBlend(hdc, nFillW, 0, emptyW, cyBar,
                           hdcMem2, 0, 0, emptyW, cyBar, bf2);
                SelectObject(hdcMem2, hbmOld2);
                DeleteObject(hbm2);
                DeleteDC(hdcMem2);
            }

            { RECT rcShine = { 0, 0, cxBar, cyBar }; DrawGlassShine(hdc, &rcShine); }

            {
                HDC hdcS = CreateCompatibleDC(hdc);
                HBITMAP hbmS = CreateCompatibleBitmap(hdc, cxBar, 3);
                HBITMAP hbmSOld = (HBITMAP)SelectObject(hdcS, hbmS);
                HBRUSH hbrS = CreateSolidBrush(RGB(0,0,0));
                RECT rcS0 = {0,0,cxBar,3}; FillRect(hdcS, &rcS0, hbrS);
                DeleteObject(hbrS);
                BLENDFUNCTION bfS = { AC_SRC_OVER, 0, 45, 0 };
                AlphaBlend(hdc, 0, cyBar - 3, cxBar, 3, hdcS, 0, 0, cxBar, 3, bfS);
                SelectObject(hdcS, hbmSOld);
                DeleteObject(hbmS);
                DeleteDC(hdcS);
            }

            {
                HPEN   hpBorder = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
                HBRUSH hbrNull2 = (HBRUSH)GetStockObject(NULL_BRUSH);
                HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                HBRUSH hbOld    = (HBRUSH)SelectObject(hdc, hbrNull2);
                RoundRect(hdc, 0, 0, cxBar, cyBar, rx*2, rx*2);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBorder);
            }

            {
                char szPct[16];
                if (nHealth < 0)
                    _snprintf(szPct, sizeof(szPct), "N/A");
                else
                    _snprintf(szPct, sizeof(szPct), "%d%%", nHealth);

                HFONT hUseFont = g_hFontBig ? g_hFontBig : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                HFONT hOldFont = (HFONT)SelectObject(hdc, hUseFont);
                SetBkMode(hdc, TRANSPARENT);

                RECT rcBufText = { 0, 0, cxBar, cyBar };
                RECT rcBufSh   = rcBufText; rcBufSh.left++; rcBufSh.top++;
                SetTextColor(hdc, RGB(0, 0, 0));
                DrawTextA(hdc, szPct, -1, &rcBufSh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextA(hdc, szPct, -1, &rcBufText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, hOldFont);
            }

            BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, hbmOldBuf);
            DeleteObject(hbmBuf);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK PerfBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            HDC     hdc    = CreateCompatibleDC(hdcReal);
            HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
            HBITMAP hbmOldBuf = (HBITMAP)SelectObject(hdc, hbmBuf);

            int nPerf = -1;
            if (g_nDriveCount > 0 && g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount)
                nPerf = g_Drives[g_nSelectedDrive].nPerformancePercent;

            int cxBar = rc.right  - rc.left;
            int cyBar = rc.bottom - rc.top;
            int rx    = 5;

            {
                int y;
                for (y = 0; y < cyBar; y++) {
                    float t  = (cyBar > 1) ? (float)y / (float)(cyBar - 1) : 0.0f;
                    int cr = 204 - (int)(t * 30);
                    int cg = 232 - (int)(t * 40);
                    int cb = 248 - (int)(t * 32);
                    HPEN hpR = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
                    HPEN hpO = (HPEN)SelectObject(hdc, hpR);
                    MoveToEx(hdc, 0,      y, NULL);
                    LineTo  (hdc, cxBar,  y);
                    SelectObject(hdc, hpO);
                    DeleteObject(hpR);
                }
            }

            int nFillPct = (nPerf < 0) ? 100 : nPerf;
            int nFillW   = (cxBar * nFillPct / 100);
            if (nFillW < 2) nFillW = 2;
            if (nFillW > 0) {
                COLORREF clrTop, clrBot;
                if (nPerf < 0 || nPerf >= 70) {
                    clrTop = RGB( 20, 160, 220);
                    clrBot = RGB(  5, 110, 170);
                } else if (nPerf >= 40) {
                    clrTop = RGB(220, 160,  10);
                    clrBot = RGB(160, 100,   0);
                } else {
                    clrTop = RGB(220,  70,  50);
                    clrBot = RGB(160,  20,  15);
                }

                int y;
                for (y = 0; y < cyBar; y++) {
                    float t  = (cyBar > 1) ? (float)y / (float)(cyBar - 1) : 0.0f;
                    int cr = (int)(GetRValue(clrTop) + t * (GetRValue(clrBot) - GetRValue(clrTop)));
                    int cg = (int)(GetGValue(clrTop) + t * (GetGValue(clrBot) - GetGValue(clrTop)));
                    int cb = (int)(GetBValue(clrTop) + t * (GetBValue(clrBot) - GetBValue(clrTop)));
                    HPEN hpR = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
                    HPEN hpO = (HPEN)SelectObject(hdc, hpR);
                    MoveToEx(hdc, 0,          y, NULL);
                    LineTo  (hdc, nFillW,     y);
                    SelectObject(hdc, hpO);
                    DeleteObject(hpR);
                }

                HPEN hpEdge = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                HPEN hpEdgeOld = (HPEN)SelectObject(hdc, hpEdge);
                HDC hdcEdge = CreateCompatibleDC(hdc);
                HBITMAP hbmEdge = CreateCompatibleBitmap(hdc, 1, cyBar);
                HBITMAP hbmEdgeOld = (HBITMAP)SelectObject(hdcEdge, hbmEdge);
                HBRUSH hbrW = CreateSolidBrush(RGB(255,255,255));
                RECT rcEdgeFill = {0,0,1,cyBar};
                FillRect(hdcEdge, &rcEdgeFill, hbrW);
                DeleteObject(hbrW);
                BLENDFUNCTION bfEdge = { AC_SRC_OVER, 0, 60, 0 };
                AlphaBlend(hdc, nFillW - 1, 0, 1, cyBar,
                           hdcEdge, 0, 0, 1, cyBar, bfEdge);
                SelectObject(hdcEdge, hbmEdgeOld);
                DeleteObject(hbmEdge);
                DeleteDC(hdcEdge);
                SelectObject(hdc, hpEdgeOld);
                DeleteObject(hpEdge);
            }

            { RECT rcShine = { 0, 0, cxBar, cyBar }; DrawGlassShine(hdc, &rcShine); }

            {
                HDC hdcS = CreateCompatibleDC(hdc);
                HBITMAP hbmS = CreateCompatibleBitmap(hdc, cxBar, 3);
                HBITMAP hbmSOld = (HBITMAP)SelectObject(hdcS, hbmS);
                HBRUSH hbrS = CreateSolidBrush(RGB(0,0,0));
                RECT rcS0 = {0,0,cxBar,3}; FillRect(hdcS, &rcS0, hbrS);
                DeleteObject(hbrS);
                BLENDFUNCTION bfS = { AC_SRC_OVER, 0, 45, 0 };
                AlphaBlend(hdc, 0, cyBar - 3, cxBar, 3,
                           hdcS, 0, 0, cxBar, 3, bfS);
                SelectObject(hdcS, hbmSOld);
                DeleteObject(hbmS);
                DeleteDC(hdcS);
            }

            {
                HPEN   hpBorder = CreatePen(PS_SOLID, 1, RGB(100, 160, 210));
                HBRUSH hbrNull2 = (HBRUSH)GetStockObject(NULL_BRUSH);
                HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                HBRUSH hbOld    = (HBRUSH)SelectObject(hdc, hbrNull2);
                RoundRect(hdc, 0, 0, cxBar, cyBar, rx*2, rx*2);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBorder);
            }

            {
                char szPct[16];
                if (nPerf < 0)
                    _snprintf(szPct, sizeof(szPct), "N/A");
                else
                    _snprintf(szPct, sizeof(szPct), "%d%%", nPerf);

                HFONT hUseFont = g_hFontBig ? g_hFontBig : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                HFONT hOldFont = (HFONT)SelectObject(hdc, hUseFont);
                SetBkMode(hdc, TRANSPARENT);

                RECT rcBufText = { 0, 0, cxBar, cyBar };
                RECT rcBufSh   = rcBufText; rcBufSh.left++; rcBufSh.top++;
                SetTextColor(hdc, RGB(0, 0, 0));
                DrawTextA(hdc, szPct, -1, &rcBufSh, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SetTextColor(hdc, RGB(255, 255, 255));
                DrawTextA(hdc, szPct, -1, &rcBufText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, hOldFont);
            }

            BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, hbmOldBuf);
            DeleteObject(hbmBuf);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK DriveBtnWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
        {
            int nIdx = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            int w = rc.right  - rc.left;
            int h = rc.bottom - rc.top;

            HDC     hdc    = CreateCompatibleDC(hdcReal);
            HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
            HBITMAP hbmOldBuf = (HBITMAP)SelectObject(hdc, hbmBuf);

            RECT rcBuf = { 0, 0, w, h };

            BOOL bSelected = (nIdx == g_nSelectedDrive);
            BOOL bHover    = (GetPropA(hWnd, "hover") != NULL);

            COLORREF clrBorder, clrText;
            if (bSelected) {
                clrBorder = RGB(20,  70, 170);
                clrText   = RGB(255, 255, 255);
            } else if (bHover) {
                clrBorder = RGB(140, 165, 220);
                clrText   = RGB(30,  35,  50);
            } else {
                clrBorder = RGB(200, 205, 220);
                clrText   = RGB(30,  35,  50);
            }

            int rx = SCALE_DPI(6);

            {
                HBRUSH hbrWinBg = CreateSolidBrush(CLR_BG);
                FillRect(hdc, &rcBuf, hbrWinBg);
                DeleteObject(hbrWinBg);
            }

            HRGN hClipRgn = CreateRoundRectRgn(rcBuf.left, rcBuf.top, rcBuf.right+1, rcBuf.bottom+1, rx*2, rx*2);
            SelectClipRgn(hdc, hClipRgn);

            {
                int y;
                COLORREF clrTop, clrBot;
                if (bSelected) {
                    clrTop = RGB( 60, 130, 240);
                    clrBot = RGB( 20,  80, 190);
                } else if (bHover) {
                    clrTop = RGB(235, 242, 255);
                    clrBot = RGB(205, 218, 248);
                } else {
                    clrTop = RGB(252, 254, 255);
                    clrBot = RGB(228, 233, 245);
                }
                for (y = 0; y < h; y++) {
                    float t  = (h > 1) ? (float)y / (float)(h - 1) : 0.0f;
                    int cr = (int)(GetRValue(clrTop) + t * (int)(GetRValue(clrBot) - GetRValue(clrTop)));
                    int cg = (int)(GetGValue(clrTop) + t * (int)(GetGValue(clrBot) - GetGValue(clrTop)));
                    int cb = (int)(GetBValue(clrTop) + t * (int)(GetBValue(clrBot) - GetBValue(clrTop)));
                    HPEN hpR = CreatePen(PS_SOLID, 1, RGB(cr, cg, cb));
                    HPEN hpO = (HPEN)SelectObject(hdc, hpR);
                    MoveToEx(hdc, 0,  y, NULL);
                    LineTo  (hdc, w,  y);
                    SelectObject(hdc, hpO);
                    DeleteObject(hpR);
                }
            }

            { RECT rcShineBtn = { 0, 0, w, h }; DrawGlassShine(hdc, &rcShineBtn); }

            {
                HPEN   hpBord = CreatePen(PS_SOLID, 1, clrBorder);
                HPEN   hpOld  = (HPEN)SelectObject(hdc, hpBord);
                HBRUSH hbOld  = (HBRUSH)SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
                RoundRect(hdc, rcBuf.left, rcBuf.top, rcBuf.right, rcBuf.bottom, rx*2, rx*2);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBord);
            }

            SelectClipRgn(hdc, NULL);
            DeleteObject(hClipRgn);

            SetBkMode(hdc, TRANSPARENT);

            if (nIdx >= 0 && nIdx < g_nDriveCount) {
                DRIVE_INFO* pD = &g_Drives[nIdx];
                BOOL bZH = (Lang_GetCurrent() == LANG_ZH_CN);

                wchar_t szName[64];
                if (strlen(pD->szModel) > 0) {
                    _snwprintf(szName, 64, L"%s", Utf8ToW(pD->szModel));
                    if (wcslen(szName) > 26) { szName[24] = L'.'; szName[25] = L'.'; szName[26] = L'\0'; }
                } else {
                    _snwprintf(szName, 64, LStrW(STR_DRIVE_FMT), pD->nDriveIndex);
                }

                wchar_t szType[32];
                const wchar_t* szTW = Lang_GetDriveTypeNameW(pD->eType);
                _snwprintf(szType, 32, L"[%s]", szTW ? szTW : L"?");

                wchar_t szPerf[32];
                if (pD->nPerformancePercent >= 0)
                    _snwprintf(szPerf, 32, bZH ? L"性能: %d%%" : L"Perf: %d%%", pD->nPerformancePercent);
                else
                    _snwprintf(szPerf, 32, LStrW(STR_PERF_NA));

                wchar_t szHealth[32];
                if (pD->nHealthPercent >= 0)
                    _snwprintf(szHealth, 32, bZH ? L"健康: %d%%" : L"Health: %d%%", pD->nHealthPercent);
                else
                    _snwprintf(szHealth, 32, LStrW(STR_HEALTH_NA));

                char szCapA[32];
                FormatSize(pD->dwCapacityMB, szCapA, sizeof(szCapA));
                wchar_t szCap[32];
                _snwprintf(szCap, 32, L"%s", Utf8ToW(szCapA));

                wchar_t szTempStr[32];
                if (pD->nTemperatureC > 0)
                    _snwprintf(szTempStr, 32, L"%d °C", pD->nTemperatureC);
                else
                    szTempStr[0] = L'\0';

                int nH = (int)(pD->nHealthPercent);
                COLORREF clrH;
                if      (nH < 0)   clrH = bSelected ? RGB(180,210,255) : CLR_ACCENT;
                else if (nH >= 70) clrH = bSelected ? RGB(180,255,200) : CLR_GREEN;
                else if (nH >= 40) clrH = bSelected ? RGB(255,240,160) : CLR_YELLOW;
                else               clrH = bSelected ? RGB(255,180,180) : CLR_RED;

                COLORREF clrTemp;
                if      (pD->nTemperatureC <= 0)  clrTemp = bSelected ? RGB(180,210,255) : CLR_TEXT_DIM;
                else if (pD->nTemperatureC < 50)  clrTemp = bSelected ? RGB(180,255,200) : CLR_GREEN;
                else if (pD->nTemperatureC < 60)  clrTemp = bSelected ? RGB(255,240,160) : CLR_YELLOW;
                else                              clrTemp = bSelected ? RGB(255,180,180) : CLR_RED;

                HFONT hOldFont;

                hOldFont = (HFONT)SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, clrText);
                RECT rcName = { rcBuf.left + SCALE_DPI(8), rcBuf.top + SCALE_DPI(4), rcBuf.right - SCALE_DPI(8), rcBuf.top + SCALE_DPI(20) };
                DrawTextW(hdc, szName, -1, &rcName, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                SelectObject(hdc, g_hFontSmall);
                RECT rcType = { rcBuf.left + SCALE_DPI(8), rcBuf.top + SCALE_DPI(20), rcBuf.right - SCALE_DPI(8), rcBuf.top + SCALE_DPI(34) };
                SetTextColor(hdc, bSelected ? RGB(200, 220, 255) : CLR_TEXT_DIM);
                DrawTextW(hdc, szType, -1, &rcType, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                RECT rcPerf = { rcBuf.left + SCALE_DPI(8), rcBuf.top + SCALE_DPI(34), (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - SCALE_DPI(16) };
                SetTextColor(hdc, clrText);
                DrawTextW(hdc, szPerf, -1, &rcPerf, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                RECT rcHealth = { (rcBuf.left + rcBuf.right) / 2, rcBuf.top + SCALE_DPI(34), rcBuf.right - SCALE_DPI(8), rcBuf.bottom - SCALE_DPI(16) };
                SetTextColor(hdc, clrH);
                DrawTextW(hdc, szHealth, -1, &rcHealth, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

                {
                    RECT rcCap  = { rcBuf.left + SCALE_DPI(8), rcBuf.bottom - SCALE_DPI(18), (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - SCALE_DPI(2) };
                    RECT rcTmp  = { (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - SCALE_DPI(18), rcBuf.right - SCALE_DPI(8), rcBuf.bottom - SCALE_DPI(2) };
                    SetTextColor(hdc, clrText);
                    DrawTextW(hdc, szCap, -1, &rcCap, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                    if (szTempStr[0]) {
                        SetTextColor(hdc, clrTemp);
                        DrawTextW(hdc, szTempStr, -1, &rcTmp, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                    }
                }
                SelectObject(hdc, hOldFont);
            }

            BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);

            SelectObject(hdc, hbmOldBuf);
            DeleteObject(hbmBuf);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        if (!GetPropA(hWnd, "hover")) {
            SetPropA(hWnd, "hover", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;

    case WM_MOUSELEAVE:
        RemovePropA(hWnd, "hover");
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_LBUTTONUP:
        {
            int nIdx = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
            if (nIdx >= 0 && nIdx < g_nDriveCount) {
                HWND hParent = GetParent(hWnd);
                g_nSelectedDrive = nIdx;
                int i;
                for (i = 0; i < g_nDriveCount; i++)
                    if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
                UpdateDriveInfo(hParent, nIdx);
                UpdateAttrList(hParent, nIdx);
                InvalidateRect(hParent, NULL, FALSE);
                UpdateWindow(hParent);
            }
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, (LPCTSTR)IDC_HAND));
        return TRUE;
    }

    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

void RegisterHealthBarClass(HINSTANCE hInst)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HealthBarWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "LLHDHealthBar";
    RegisterClassA(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PerfBarWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "LLHDPerfBar";
    RegisterClassA(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = DriveBtnWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = "LLHDDriveBtn";
    RegisterClassA(&wc);
}

void RepaintHealthBar(void)
{
    if (g_hHealthBar) { InvalidateRect(g_hHealthBar, NULL, TRUE); UpdateWindow(g_hHealthBar); }
    if (g_hPerfBar)   { InvalidateRect(g_hPerfBar,   NULL, TRUE); UpdateWindow(g_hPerfBar);   }
}

void UpdateDriveButtons(HWND hWnd)
{
    int i;
    int nBtnW   = DRIVE_BTN_PANEL_W - SCALE_DPI(12);
    int nBtnH   = DRIVE_BTN_H;
    int nStartY = SCALE_DPI(36);

    BOOL bNeedRebuild = FALSE;

    if (g_hDriveBtn[0]) {
        char szClass[32] = "";
        GetClassNameA(g_hDriveBtn[0], szClass, sizeof(szClass));
        BOOL bIsStatic   = (strcmp(szClass, "Static") == 0 || strcmp(szClass, "static") == 0);
        BOOL bNeedStatic = (g_nDriveCount == 0);
        if (bIsStatic != bNeedStatic) bNeedRebuild = TRUE;
    }

    {
        int nExisting = 0;
        for (i = 0; i < MAX_DRIVES; i++)
            if (g_hDriveBtn[i]) nExisting++;
        if (nExisting != (g_nDriveCount == 0 ? 1 : g_nDriveCount))
            bNeedRebuild = TRUE;
    }

    if (bNeedRebuild) {
        for (i = 0; i < MAX_DRIVES; i++) {
            if (g_hDriveBtn[i]) {
                DestroyWindow(g_hDriveBtn[i]);
                g_hDriveBtn[i] = NULL;
            }
        }

        if (g_nDriveCount == 0) {
            HWND hPlaceholder = CreateWindowExW(0, L"STATIC", LStrW(STR_NO_DRIVES_FOUND),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                SCALE_DPI(6), nStartY, nBtnW, nBtnH,
                hWnd, (HMENU)(IDC_DRIVE_BTN_BASE), g_hInst, NULL);
            SendMessageW(hPlaceholder, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            g_hDriveBtn[0] = hPlaceholder;
            return;
        }

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            int nY = nStartY + i * (nBtnH + DRIVE_BTN_GAP);
            g_hDriveBtn[i] = CreateWindowExA(
                0, "LLHDDriveBtn", "",
                WS_CHILD | WS_VISIBLE,
                SCALE_DPI(6), nY, nBtnW, nBtnH,
                hWnd, (HMENU)(UINT_PTR)(IDC_DRIVE_BTN_BASE + i), g_hInst, NULL
            );
            SetWindowLongPtrA(g_hDriveBtn[i], GWLP_USERDATA, (LONG_PTR)i);
        }
    } else {
        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            if (g_hDriveBtn[i]) {
                int nY = nStartY + i * (nBtnH + DRIVE_BTN_GAP);
                SetWindowPos(g_hDriveBtn[i], NULL, SCALE_DPI(6), nY, nBtnW, nBtnH, SWP_NOZORDER);
                InvalidateRect(g_hDriveBtn[i], NULL, FALSE);
            }
        }
    }
}

void UpdateDriveInfo(HWND hWnd, int nDriveIdx)
{
    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SetDlgItemTextW(hWnd, IDC_MODEL_STATIC,       L"-");
        SetDlgItemTextW(hWnd, IDC_SERIAL_STATIC,      L"-");
        SetDlgItemTextW(hWnd, IDC_FIRMWARE_STATIC,    L"-");
        SetDlgItemTextW(hWnd, IDC_SIZE_STATIC,        L"-");
        SetDlgItemTextW(hWnd, IDC_TEMP_STATIC,        L"-");
        SetDlgItemTextW(hWnd, IDC_STATUS_STATIC,      LStrW(STR_NOT_AVAILABLE));
        SetDlgItemTextW(hWnd, IDC_READ_SPEED_STATIC,  L"-");
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC,     L"");
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];
    wchar_t szBufW[512];

    SetDlgItemTextW(hWnd, IDC_MODEL_STATIC,
                    strlen(pInfo->szModel) ? Utf8ToW(pInfo->szModel) : L"-");

    SetDlgItemTextW(hWnd, IDC_SERIAL_STATIC,
                    strlen(pInfo->szSerial) ? Utf8ToW(pInfo->szSerial) : L"-");

    SetDlgItemTextW(hWnd, IDC_FIRMWARE_STATIC,
                    strlen(pInfo->szFirmware) ? Utf8ToW(pInfo->szFirmware) : L"-");

    {
        char szSize[32];
        FormatSize(pInfo->dwCapacityMB, szSize, sizeof(szSize));
        _snwprintf(szBufW, 512, L"%s   %s%s",
                   Utf8ToW(szSize), LStrW(STR_TYPE_PREFIX), Lang_GetDriveTypeNameW(pInfo->eType));
        SetDlgItemTextW(hWnd, IDC_SIZE_STATIC, szBufW);
    }

    if (pInfo->nTemperatureC > 0)
        _snwprintf(szBufW, 512, L"%d °C", pInfo->nTemperatureC);
    else
        _snwprintf(szBufW, 512, L"-");
    SetDlgItemTextW(hWnd, IDC_TEMP_STATIC, szBufW);

    if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        if (Lang_GetCurrent() == LANG_ZH_CN) {
            _snwprintf(szBufW, 512, L"已用寿命 %d%%", (int)pInfo->nvmeHealth.PercentageUsed);
        } else {
            _snwprintf(szBufW, 512, L"NVMe Health Log   Spare: %d%%   Used: %d%%",
                       (int)pInfo->nvmeHealth.AvailableSpare,
                       (int)pInfo->nvmeHealth.PercentageUsed);
        }
    } else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        _snwprintf(szBufW, 512, L"%s", LStrW(STR_NVME_NOT_READABLE));
    } else if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        _snwprintf(szBufW, 512, L"%s", LStrW(STR_USB_NOT_SUPPORTED));
    } else if (pInfo->bIsUSB && pInfo->bSMART_Supported) {
        if (pInfo->eAccessMethod == SMART_ACCESS_SAT16 &&
            pInfo->attrData.stAttributes[0].bAttrID == 0 &&
            pInfo->attrData.wRevisionNumber == 0) {
            _snwprintf(szBufW, 512, L"%s", LStrW(STR_USB_LIMITED_SCSI));
        } else {
            _snwprintf(szBufW, 512, L"USB SAT - %s",
                       pInfo->bSMART_Enabled ? LStrW(STR_ENABLED) : LStrW(STR_DETECTED));
        }
    } else if (pInfo->bSMART_Supported) {
        _snwprintf(szBufW, 512, L"%s",
                   pInfo->bSMART_Enabled ? LStrW(STR_ENABLED) : LStrW(STR_DISABLED));
    } else {
        _snwprintf(szBufW, 512, L"%s", LStrW(STR_NOT_SUPPORTED));
    }
    SetDlgItemTextW(hWnd, IDC_STATUS_STATIC, szBufW);

    if (pInfo->nReadSpeedMBs > 0)
        _snwprintf(szBufW, 512, L"%d MB/s", pInfo->nReadSpeedMBs);
    else
        _snwprintf(szBufW, 512, L"-");
    SetDlgItemTextW(hWnd, IDC_READ_SPEED_STATIC, szBufW);

    wchar_t szReason[512];
    szReason[0] = L'\0';
    if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        BYTE crit = pInfo->nvmeHealth.CriticalWarning;
        if (crit != 0) {
            _snwprintf(szReason, 512, L"%s%s%s%s%s%s",
                LStrW(STR_NVME_CRIT_HEADER),
                (crit & NVME_CRIT_WARN_SPARE_BELOW_THRESH)   ? LStrW(STR_NVME_CRIT_SPARE)       : L"",
                (crit & NVME_CRIT_WARN_TEMP_THRESHOLD)       ? LStrW(STR_NVME_CRIT_TEMP)        : L"",
                (crit & NVME_CRIT_WARN_RELIABILITY_DEGRADED) ? LStrW(STR_NVME_CRIT_RELIABILITY)   : L"",
                (crit & NVME_CRIT_WARN_READ_ONLY)            ? LStrW(STR_NVME_CRIT_READ_ONLY)     : L"",
                (crit & NVME_CRIT_WARN_VOLATILE_MEM_BACKUP)  ? LStrW(STR_NVME_CRIT_VOLATILE_MEM)  : L"");
        }
    } else if (pInfo->bSMART_Supported && !pInfo->bIsUSB) {
        int k;
        DWORD dwR05=0, dwRC4=0, dwRC5=0, dwRC6=0, dwRBB=0;
        for (k = 0; k < 30; k++) {
            BYTE id = pInfo->attrData.stAttributes[k].bAttrID;
            DWORD dw = GetRawValue(pInfo->attrData.stAttributes[k].bRawValue);
            if      (id == 0x05) dwR05 = dw;
            else if (id == 0xC4) dwRC4 = dw;
            else if (id == 0xC5) dwRC5 = dw;
            else if (id == 0xC6) dwRC6 = dw;
            else if (id == 0xBB) dwRBB = dw;
        }
        if (Lang_GetCurrent() == LANG_ZH_CN) {
            if (dwRC6 > 0)
                _snwprintf(szReason, 512,
                    L"无法纠正错误: %u   重新分配扇区: %u   待映射: %u   重映射事件: %u",
                    (unsigned)dwRC6, (unsigned)dwR05, (unsigned)dwRC5, (unsigned)dwRC4);
            else if (dwRC5 > 0 || dwR05 > 0)
                _snwprintf(szReason, 512,
                    L"重新分配扇区: %u   待映射: %u   重映射事件: %u   ECC: %u",
                    (unsigned)dwR05, (unsigned)dwRC5, (unsigned)dwRC4, (unsigned)dwRBB);
        } else {
            if (dwRC6 > 0)
                _snwprintf(szReason, 512,
                    L"Uncorrectable: %u   Reallocated: %u   Pending: %u   Events: %u",
                    (unsigned)dwRC6, (unsigned)dwR05, (unsigned)dwRC5, (unsigned)dwRC4);
            else if (dwRC5 > 0 || dwR05 > 0)
                _snwprintf(szReason, 512,
                    L"Reallocated: %u   Pending: %u   Events: %u   ECC: %u",
                    (unsigned)dwR05, (unsigned)dwRC5, (unsigned)dwRC4, (unsigned)dwRBB);
        }
    }

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        wchar_t szBridge[128];
        if (pInfo->szBridgeVendor[0] || pInfo->szBridgeProduct[0]) {
            _snwprintf(szBridge, 128, L" [Bridge: %s %s]",
                Utf8ToW(pInfo->szBridgeVendor), Utf8ToW(pInfo->szBridgeProduct));
        } else {
            szBridge[0] = L'\0';
        }

        DWORD dwErr = pInfo->dwErrSat16;
        if (dwErr == 0) dwErr = pInfo->dwErrSat12;
        if (dwErr == 0) dwErr = pInfo->dwErrStorageProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrNvmeProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrNvmePassthrough;
        if (dwErr == 0) dwErr = pInfo->dwErrLogSense;

        const wchar_t* pszMeaning = L"";
        if (Lang_GetCurrent() == LANG_ZH_CN) {
            if (dwErr == ERROR_INVALID_FUNCTION)      pszMeaning = L" (桥接芯片不支持此指令)";
            else if (dwErr == ERROR_NOT_SUPPORTED)    pszMeaning = L" (桥接芯片明确拒绝此指令)";
            else if (dwErr == ERROR_IO_DEVICE)        pszMeaning = L" (USB驱动或桥接芯片I/O错误)";
            else if (dwErr == ERROR_INVALID_DATA)     pszMeaning = L" (桥接芯片返回空数据)";
            else if (dwErr >= 0x10000)                pszMeaning = L" (SCSI指令被拒绝)";

            if (dwErr != 0) {
                _snwprintf(szBufW, 512,
                    L"检测到 USB/移动硬盘。SMART 无法读取%s。错误码: %lu%s。",
                    szBridge, (unsigned long)dwErr, pszMeaning);
            } else {
                _snwprintf(szBufW, 512,
                    L"检测到 USB/移动硬盘%s。SMART 无法读取(桥接芯片不支持 SAT 直通)。", szBridge);
            }
        } else {
            if (dwErr == ERROR_INVALID_FUNCTION)      pszMeaning = L" (bridge does not implement this command)";
            else if (dwErr == ERROR_NOT_SUPPORTED)    pszMeaning = L" (bridge explicitly rejected the command)";
            else if (dwErr == ERROR_IO_DEVICE)        pszMeaning = L" (USB driver/bridge I/O error)";
            else if (dwErr == ERROR_INVALID_DATA)     pszMeaning = L" (bridge returned empty/zeroed data)";
            else if (dwErr >= 0x10000)                pszMeaning = L" (SCSI command rejected with non-zero status)";

            if (dwErr != 0) {
                _snwprintf(szBufW, 512,
                    L"USB/External drive detected. SMART not accessible%s. "
                    L"Last error: %lu%s.",
                    szBridge, (unsigned long)dwErr, pszMeaning);
            } else {
                _snwprintf(szBufW, 512,
                    L"USB/External drive detected%s. SMART not accessible "
                    L"(bridge does not support SAT passthrough).", szBridge);
            }
        }
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC, szBufW);
    } else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC,
            Lang_GetCurrent() == LANG_ZH_CN ?
            L"NVMe SMART 数据无法读取。请以管理员权限运行并点击刷新。部分主控需要 Windows 10 v1903+ 或更新驱动。" :
            L"NVMe SMART data could not be read. Run as Administrator and click Refresh. "
            L"Some controllers require Windows 10 v1903+ drivers.");
    } else if (pInfo->nHealthPercent < 0) {
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC,
            Lang_GetCurrent() == LANG_ZH_CN ?
            L"SMART 数据无法读取。请以管理员权限运行并点击刷新。" :
            L"SMART data could not be read. Run as Administrator and click Refresh.");
    } else if (pInfo->bPredictFailure) {
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC,
            Lang_GetCurrent() == LANG_ZH_CN ?
            L"!! 硬盘固件报告预测即将发生故障 !!" :
            L"!! DRIVE FAILURE PREDICTED BY DRIVE !!");
    } else if (pInfo->nHealthPercent < 40) {
        wchar_t szMsgW[512];
        _snwprintf(szMsgW, 512,
            Lang_GetCurrent() == LANG_ZH_CN ? L"!! 健康状况极差 - 请立即备份数据!  %s" : L"!! Poor Health - Back up data immediately!  %s",
            szReason);
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC, szMsgW);
    } else if (pInfo->nHealthPercent < 70) {
        wchar_t szMsgW[512];
        _snwprintf(szMsgW, 512,
            Lang_GetCurrent() == LANG_ZH_CN ? L"警告 ! 请密切关注硬盘状况。  %s" : L"Caution ! Monitor drive closely.  %s",
            szReason);
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC, szMsgW);
    } else if (pInfo->nHealthPercent < 100) {
        wchar_t szMsgW[512];
        if (Lang_GetCurrent() == LANG_ZH_CN) {
            _snwprintf(szMsgW, 512, L"良好。 %s",
                (szReason[0] ? szReason : L"所有关键属性均处于正常状态。"));
        } else {
            _snwprintf(szMsgW, 512, L"Good.  %s",
                (szReason[0] ? szReason : L"All critical attributes normal."));
        }
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC, szMsgW);
    } else {
        SetDlgItemTextW(hWnd, IDC_PREDICT_STATIC,
            Lang_GetCurrent() == LANG_ZH_CN ? L"优秀 - 硬盘处于完美状态。" : L"Excellent - Drive is in perfect condition.");
    }

    RepaintHealthBar();
}

static void ListViewSetCellIfChanged(HWND hList, int iItem, int iSubItem, const wchar_t* pszNew)
{
    wchar_t szOld[128] = L"";
    LVITEMW lvi;
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.mask       = LVIF_TEXT;
    lvi.iItem      = iItem;
    lvi.iSubItem   = iSubItem;
    lvi.pszText    = szOld;
    lvi.cchTextMax = 128;
    SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi);
    if (wcscmp(szOld, pszNew) != 0) {
        lvi.pszText = (LPWSTR)pszNew;
        SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
    }
}

typedef struct {
    wchar_t col[4][128];
} ATTR_ROW;

#define MAX_ATTR_ROWS 32

static void FormatSmartValueW(BYTE bID, BYTE* pRaw,
                              BYTE bVal, BYTE bWorst, BYTE bThresh,
                              wchar_t* szBuf, int nBufLen)
{
    DWORD dw32 = ((DWORD)pRaw[3] << 24) | ((DWORD)pRaw[2] << 16) |
                 ((DWORD)pRaw[1] <<  8) |  (DWORD)pRaw[0];
    WORD  w16  = ((WORD)pRaw[1] << 8) | (WORD)pRaw[0];
    unsigned __int64 qw48 =
        ((unsigned __int64)pRaw[5] << 40) |
        ((unsigned __int64)pRaw[4] << 32) |
        ((unsigned __int64)pRaw[3] << 24) |
        ((unsigned __int64)pRaw[2] << 16) |
        ((unsigned __int64)pRaw[1] <<  8) |
         (unsigned __int64)pRaw[0];

    wchar_t szSuffix[48] = L"";
    if (bThresh > 0)
        _snwprintf(szSuffix, 48, L"  [%d=%d>%d]", bVal, bWorst, bThresh);
    else if (bVal > 0 || bWorst > 0)
        _snwprintf(szSuffix, 48, L"  [%d=%d>0]", bVal, bWorst);

    wchar_t szMain[80] = L"";
    BOOL bZH = (Lang_GetCurrent() == LANG_ZH_CN);

    switch (bID)
    {
    case 0xBE:
    {
        int nC = (int)pRaw[0];
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax < 100)
            _snwprintf(szMain, 80, L"%d °C (%d °F)  min:%d max:%d", nC, nF, nMin, nMax);
        else
            _snwprintf(szMain, 80, L"%d °C (%d °F)", nC, nF);
        break;
    }
    case 0xC2:
    {
        int nC = (int)w16;
        if (nC <= 0 || nC > 100) nC = (int)bVal;
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax < 100)
            _snwprintf(szMain, 80, L"%d °C (%d °F)  min:%d max:%d", nC, nF, nMin, nMax);
        else
            _snwprintf(szMain, 80, L"%d °C (%d °F)", nC, nF);
        break;
    }
    case 0xE7:
        if (bVal > 0 && bVal <= 100) {
            int nF = (int)bVal * 9 / 5 + 32;
            _snwprintf(szMain, 80, L"%d °C (%d °F)", (int)bVal, nF);
        } else {
            _snwprintf(szMain, 80, L"%lu", (unsigned long)dw32);
        }
        break;

    case 0x09:
    {
        unsigned __int64 nHours = qw48;
        if (nHours > 200000) nHours = (unsigned __int64)w16;
        unsigned long nDays  = (unsigned long)(nHours / 24);
        double fYears = (double)nHours / 8760.0;
        if (bZH) {
            if (fYears >= 1.0)
                _snwprintf(szMain, 80, L"%llu 小时 (%.1f 年)", nHours, fYears);
            else
                _snwprintf(szMain, 80, L"%llu 小时 (%lu 天)", nHours, nDays);
        } else {
            if (fYears >= 1.0)
                _snwprintf(szMain, 80, L"%llu hours (%.1f years)", nHours, fYears);
            else
                _snwprintf(szMain, 80, L"%llu hours (%lu days)", nHours, nDays);
        }
        break;
    }

    case 0x04:
        _snwprintf(szMain, 80, bZH ? L"%lu 次" : L"%lu times", (unsigned long)dw32);
        break;

    case 0x0C:
        _snwprintf(szMain, 80, bZH ? L"%lu 次" : L"%lu cycles", (unsigned long)dw32);
        break;

    case 0x03:
    {
        WORD wMs = w16;
        if (wMs > 0 && wMs < 30000)
            _snwprintf(szMain, 80, L"%u ms", wMs);
        else
            _snwprintf(szMain, 80, L"%lu", (unsigned long)dw32);
        break;
    }

    case 0x0A:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 次重试  (!)" : L"%lu retries  (!)", (unsigned long)dw32);
        break;

    case 0x05:
    {
        DWORD dwSec = dw32 & 0xFFFF;
        if (dwSec == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 扇区  (!)" : L"%lu sectors  (!)", (unsigned long)dwSec);
        break;
    }

    case 0xC4:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 次事件  (!)" : L"%lu events  (!)", (unsigned long)dw32);
        break;

    case 0xC5:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 待处理  (!)" : L"%lu pending  (!)", (unsigned long)dw32);
        break;

    case 0xC6:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 无法纠正  (!)" : L"%lu uncorrectable  (!)", (unsigned long)dw32);
        break;

    case 0xC7:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu CRC 错误  (!)" : L"%lu CRC errors  (!)", (unsigned long)dw32);
        break;

    case 0xBB:
    case 0xC3:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu ECC 错误" : L"%lu ECC errors", (unsigned long)dw32);
        break;

    case 0xBC:
    {
        WORD wTotal  = (WORD)pRaw[0] | ((WORD)pRaw[1] << 8);
        WORD wLatest = (WORD)pRaw[2] | ((WORD)pRaw[3] << 8);
        if (wTotal == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%u 次超时 (最近: %u)" : L"%u timeouts  (latest: %u)", wTotal, wLatest);
        break;
    }

    case 0xC1:
        _snwprintf(szMain, 80, bZH ? L"%lu 次加载循环" : L"%lu load/unload cycles", (unsigned long)dw32);
        break;

    case 0xC0:
        _snwprintf(szMain, 80, bZH ? L"%lu 次紧急退出" : L"%lu emergency retracts", (unsigned long)dw32);
        break;

    case 0xB7:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 次降级  (!)" : L"%lu downshifts  (!)", (unsigned long)dw32);
        break;

    case 0xB8:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, bZH ? L"%lu 次错误  (!)" : L"%lu errors  (!)", (unsigned long)dw32);
        break;

    case 0xBF:
        _snwprintf(szMain, 80, bZH ? L"%lu 次冲击事件" : L"%lu shock events", (unsigned long)dw32);
        break;

    case 0x01:
    {
        DWORD dwErrHi = (((DWORD)pRaw[5] << 8) | (DWORD)pRaw[4]);
        if (dwErrHi > 0 && dw32 > 0 && dw32 < 0xFFFFFFFF)
            _snwprintf(szMain, 80, L"%lu / %lu  (err/total)", (unsigned long)dwErrHi, (unsigned long)dw32);
        else if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, L"%lu", (unsigned long)dw32);
        break;
    }

    case 0x07:
    {
        DWORD dwErrHi = (((DWORD)pRaw[5] << 8) | (DWORD)pRaw[4]);
        if (dwErrHi > 0 && dw32 > 0)
            _snwprintf(szMain, 80, L"%lu / %lu  (err/seeks)", (unsigned long)dwErrHi, (unsigned long)dw32);
        else if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, L"%lu", (unsigned long)dw32);
        break;
    }

    case 0xF1:
    case 0xF2:
    case 0xF3:
    case 0xF4:
    {
        unsigned __int64 nLBA = qw48;
        unsigned __int64 nGB  = nLBA / (1024ULL * 1024ULL * 2ULL);
        if (nGB >= 1024)
            _snwprintf(szMain, 80, L"%llu LBA  (~%llu TB)", nLBA, nGB / 1024ULL);
        else if (nGB > 0)
            _snwprintf(szMain, 80, L"%llu LBA  (~%llu GB)", nLBA, nGB);
        else
            _snwprintf(szMain, 80, L"%llu LBA", nLBA);
        break;
    }

    case 0xF9:
    case 0xE9:
    {
        unsigned __int64 nGiB = qw48;
        if (nGiB > 0 && nGiB < 1000000ULL)
            _snwprintf(szMain, 80, L"%llu GiB %s", nGiB, LStrW(STR_UNIT_WRITTEN));
        else
            _snwprintf(szMain, 80, L"%llu", nGiB);
        break;
    }

    case 0xA9:
    {
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        _snwprintf(szMain, 80, L"%lu%%  %s", (unsigned long)pct, LStrW(STR_UNIT_LIFE_REMAINING));
        break;
    }

    case 0xAD:
        _snwprintf(szMain, 80, L"%lu %s", (unsigned long)dw32, LStrW(STR_UNIT_WEAR_CYCLES));
        break;

    case 0xAB:
    case 0xAC:
    case 0xB5:
    case 0xB6:
    case 0xB0:
        if (dw32 == 0)
            _snwprintf(szMain, 80, L"0  (OK)");
        else
            _snwprintf(szMain, 80, L"%lu %s  (!)", (unsigned long)dw32, LStrW(STR_UNIT_FAILURES));
        break;

    case 0xAA:
    case 0xE8:
    {
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        _snwprintf(szMain, 80, L"%lu%%  %s", (unsigned long)pct, LStrW(STR_UNIT_RESERVED_SPACE));
        break;
    }

    case 0xF0:
        _snwprintf(szMain, 80, L"%lu %s", (unsigned long)(DWORD)w16, LStrW(STR_UNIT_FLYING_HOURS));
        break;

    default:
        if (qw48 > 0xFFFFFFFFULL)
            _snwprintf(szMain, 80, L"%llu", qw48);
        else if (dw32 == 0)
            _snwprintf(szMain, 80, L"0");
        else
            _snwprintf(szMain, 80, L"%lu", (unsigned long)dw32);
        break;
    }

    _snwprintf(szBuf, nBufLen, L"%s%s", szMain, szSuffix);
}

void UpdateAttrList(HWND hWnd, int nDriveIdx)
{
    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);

    ATTR_ROW rows[MAX_ATTR_ROWS];
    int      nDesired = 0;

    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SendMessageW(hList, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hList, LVM_DELETEALLITEMS, 0, 0);
        SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, NULL, TRUE);
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];
    BOOL bZH = (Lang_GetCurrent() == LANG_ZH_CN);

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        ATTR_ROW* r = &rows[0];
        _snwprintf(r->col[0], 128, L"--");
        _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_USB_NO_SMART));
        _snwprintf(r->col[2], 128, L""); _snwprintf(r->col[3], 128, L"");
        nDesired = 1;
    }
    else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        ATTR_ROW* r = &rows[0];
        _snwprintf(r->col[0], 128, L"--");
        _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_NVME_RUN_ADMIN));
        _snwprintf(r->col[2], 128, L""); _snwprintf(r->col[3], 128, L"");
        r = &rows[1];
        _snwprintf(r->col[0], 128, L"--");
        _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_NVME_WIN10_VER));
        _snwprintf(r->col[2], 128, L""); _snwprintf(r->col[3], 128, L"");
        nDesired = 2;
    }
    else if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        NVME_HEALTH_INFO_LOG* pLog = &pInfo->nvmeHealth;
        unsigned __int64 qwDataRead    = NVMeRead128Lo(pLog->DataUnitsRead);
        unsigned __int64 qwDataWritten = NVMeRead128Lo(pLog->DataUnitsWritten);
        unsigned __int64 qwPOH         = NVMeRead128Lo(pLog->PowerOnHours);
        unsigned __int64 qwPowerCycles = NVMeRead128Lo(pLog->PowerCycles);
        unsigned __int64 qwUnsafeSDs   = NVMeRead128Lo(pLog->UnsafeShutdowns);
        unsigned __int64 qwMediaErr    = NVMeRead128Lo(pLog->MediaErrors);
        unsigned __int64 qwErrLog      = NVMeRead128Lo(pLog->NumErrLogEntries);
        WORD wTempK = ReadLE16(pLog->CompositeTemperature);
        int  nTempC = (wTempK > 273) ? (int)wTempK - 273 : 0;

        #define NVME_ROW(id_, name_, val_, stat_) \
        { \
            ATTR_ROW* r = &rows[nDesired++]; \
            _snwprintf(r->col[0], 128, L"%s", id_); \
            _snwprintf(r->col[1], 128, L"%s", name_); \
            _snwprintf(r->col[2], 128, L"%s", val_); \
            _snwprintf(r->col[3], 128, L"%s", stat_); \
        }

        wchar_t szCrit[64], szVBuf[64], szSpare[32], szSpTh[32], szPctU[32];
        wchar_t szDUR[64], szDUW[64], szPOH[32], szPC[32];
        wchar_t szUS[32], szME[32], szEL[32], szWCT[32], szCCT[32];

        if (pLog->CriticalWarning == 0) _snwprintf(szCrit, 64, bZH ? L"0 (无)" : L"0 (None)");
        else _snwprintf(szCrit, 64, L"0x%02X (!)", pLog->CriticalWarning);
        NVME_ROW(L"01h", bZH ? L"严重警告" : L"Critical Warning", szCrit, (pLog->CriticalWarning ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szVBuf, 64, L"%d °C (%d K)", nTempC, (int)wTempK);
        NVME_ROW(L"02h", bZH ? L"综合温度" : L"Composite Temperature", szVBuf, (nTempC>70 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szSpare, 32, L"%d %%", (int)pLog->AvailableSpare);
        NVME_ROW(L"03h", bZH ? L"可用预留空间" : L"Available Spare", szSpare,
            (pLog->AvailableSpare<pLog->AvailableSpareThreshold ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szSpTh, 32, L"%d %%", (int)pLog->AvailableSpareThreshold);
        NVME_ROW(L"04h", bZH ? L"预留空间阈值" : L"Available Spare Threshold", szSpTh, LStrW(STR_STATUS_NA));

        _snwprintf(szPctU, 32, L"%d %%", (int)pLog->PercentageUsed);
        NVME_ROW(L"05h", bZH ? L"已用寿命百分比" : L"Percentage Used (Endurance)", szPctU,
            (pLog->PercentageUsed>=100 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        if (qwDataRead>2048) _snwprintf(szDUR, 64, L"%llu GB", (unsigned __int64)(qwDataRead/2048));
        else                  _snwprintf(szDUR, 64, L"%llu %s", qwDataRead, LStrW(STR_UNIT_UNITS));
        NVME_ROW(L"06h", bZH ? L"总读取数据量" : L"Data Units Read", szDUR, LStrW(STR_STATUS_OK));

        if (qwDataWritten>2048) _snwprintf(szDUW, 64, L"%llu GB", (unsigned __int64)(qwDataWritten/2048));
        else                     _snwprintf(szDUW, 64, L"%llu %s", qwDataWritten, LStrW(STR_UNIT_UNITS));
        NVME_ROW(L"07h", bZH ? L"总写入数据量" : L"Data Units Written", szDUW, LStrW(STR_STATUS_OK));

        _snwprintf(szPOH, 32, bZH ? L"%llu 小时" : L"%llu hours", qwPOH);
        NVME_ROW(L"09h", bZH ? L"通电时间" : L"Power On Hours", szPOH, LStrW(STR_STATUS_OK));

        _snwprintf(szPC, 32, L"%llu", qwPowerCycles);
        NVME_ROW(L"0Ch", bZH ? L"通电次数" : L"Power Cycles", szPC, LStrW(STR_STATUS_OK));

        _snwprintf(szUS, 32, L"%llu", qwUnsafeSDs);
        NVME_ROW(L"10h", bZH ? L"不安全关机计数" : L"Unsafe Shutdowns", szUS, LStrW(STR_STATUS_OK));

        _snwprintf(szME, 32, L"%llu", qwMediaErr);
        NVME_ROW(L"11h", bZH ? L"介质与数据完整性错误" : L"Media and Data Integrity Errors", szME, (qwMediaErr>0 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szEL, 32, L"%llu", qwErrLog);
        NVME_ROW(L"12h", bZH ? L"错误日志条目数" : L"Number of Error Log Entries", szEL, (qwErrLog>0 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szWCT, 32, bZH ? L"%lu 分钟" : L"%lu min", pLog->WarningCompTempTime);
        NVME_ROW(L"13h", bZH ? L"警告温度持续时间" : L"Warning Composite Temp Time", szWCT, (pLog->WarningCompTempTime>0 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        _snwprintf(szCCT, 32, bZH ? L"%lu 分钟" : L"%lu min", pLog->CriticalCompTempTime);
        NVME_ROW(L"14h", bZH ? L"严重温度持续时间" : L"Critical Composite Temp Time", szCCT, (pLog->CriticalCompTempTime>0 ? LStrW(STR_STATUS_WARNING) : LStrW(STR_STATUS_OK)));

        #undef NVME_ROW
    }
    else if (pInfo->bSMART_Supported) {
        int i, j;
        for (i = 0; i < 30 && nDesired < MAX_ATTR_ROWS; i++) {
            SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[i];
            if (pAttr->bAttrID == 0) continue;

            BYTE bThresh = 0;
            for (j = 0; j < 30; j++) {
                if (pInfo->threshData.stThresholds[j].bAttrID == pAttr->bAttrID) {
                    bThresh = pInfo->threshData.stThresholds[j].bThresholdValue;
                    break;
                }
            }
            BOOL bFailed = (bThresh > 0 && pAttr->bAttrValue <= bThresh);

            ATTR_ROW* r = &rows[nDesired++];
            _snwprintf(r->col[0], 128, L"%02Xh",   pAttr->bAttrID);
            _snwprintf(r->col[1], 128, L"%s",     Lang_GetSmartAttrNameW(pAttr->bAttrID));
            FormatSmartValueW(pAttr->bAttrID, pAttr->bRawValue,
                              pAttr->bAttrValue, pAttr->bWorstValue, bThresh,
                              r->col[2], 128);
            if (bFailed)
                _snwprintf(r->col[3], 128, L"%s", LStrW(STR_STATUS_FAILED));
            else if (bThresh > 0 && pAttr->bAttrValue < bThresh + 10)
                _snwprintf(r->col[3], 128, L"%s", LStrW(STR_STATUS_WARNING));
            else
                _snwprintf(r->col[3], 128, L"%s", LStrW(STR_STATUS_OK));
        }

        if (nDesired == 0) {
            /* Bridge gave us only SCSI LOG SENSE data (no ATA attribute
             * table). Show what was retrieved instead of an empty list. */
            ATTR_ROW* r = &rows[nDesired++];
            _snwprintf(r->col[0], 128, L"--");
            _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_SCSI_PREDICT));
            _snwprintf(r->col[2], 128, L"%s", pInfo->bPredictFailure ? LStrW(STR_SCSI_PRED_FAIL) : LStrW(STR_SCSI_NO_PRED_FAIL));
            _snwprintf(r->col[3], 128, L"%s", pInfo->bPredictFailure ? LStrW(STR_STATUS_FAILED) : LStrW(STR_STATUS_OK));

            if (nDesired < MAX_ATTR_ROWS) {
                r = &rows[nDesired++];
                _snwprintf(r->col[0], 128, L"--");
                _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_SCSI_TEMP));
                if (pInfo->nTemperatureC > 0)
                    _snwprintf(r->col[2], 128, L"%d °C", pInfo->nTemperatureC);
                else
                    _snwprintf(r->col[2], 128, L"%s", LStrW(STR_UNAVAILABLE));
                _snwprintf(r->col[3], 128, L"%s", LStrW(STR_STATUS_NA));
            }

            if (nDesired < MAX_ATTR_ROWS) {
                r = &rows[nDesired++];
                _snwprintf(r->col[0], 128, L"--");
                _snwprintf(r->col[1], 128, L"%s", LStrW(STR_ROW_SCSI_NOTE));
                _snwprintf(r->col[2], 128, L"");
                _snwprintf(r->col[3], 128, L"");
            }
        }
    }

    SendMessageW(hList, WM_SETREDRAW, FALSE, 0);

    int nCurrent = ListView_GetItemCount(hList);

    int row;
    for (row = 0; row < nDesired; row++) {
        if (row >= nCurrent) {
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask     = LVIF_TEXT;
            lvi.iItem    = row;
            lvi.iSubItem = 0;
            lvi.pszText  = rows[row].col[0];
            SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
        }
        int col;
        for (col = 0; col < 4; col++)
            ListViewSetCellIfChanged(hList, row, col, rows[row].col[col]);
    }

    while (ListView_GetItemCount(hList) > nDesired)
        SendMessageW(hList, LVM_DELETEITEM, ListView_GetItemCount(hList) - 1, 0);

    SendMessageW(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, FALSE);
    UpdateWindow(hList);
}

static volatile LONG g_bScanBusy = 0;

static DWORD WINAPI RefreshThreadProc(LPVOID lpParam)
{
    HWND hWnd = (HWND)lpParam;
    Snapshot_Save();
    g_nDriveCount = ScanDrives(g_Drives, MAX_DRIVES);
    History_Record(g_Drives, g_nDriveCount);
    Snapshot_Diff();
    CheckCriticalAlerts();
    PostMessage(hWnd, WM_APP_REFRESH_DONE, 0, 0);
    return 0;
}

void RefreshData(HWND hWnd)
{
    if (InterlockedCompareExchange(&g_bScanBusy, 1, 0) != 0)
        return;

    HANDLE hThread = CreateThread(NULL, 0, RefreshThreadProc, hWnd, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
    else {
        g_nDriveCount = ScanDrives(g_Drives, MAX_DRIVES);
        PostMessage(hWnd, WM_APP_REFRESH_DONE, 0, 0);
    }
}

#define ABOUT_W  440
#define ABOUT_H  360

/* Control identifiers used inside the About dialog. */
#define IDC_ABOUT_LINK       3500
#define IDC_ABOUT_LIC_STATUS 3501   
#define IDC_ABOUT_ACTIVATE   3502   
#define ABOUT_URL       "https://github.com/sponsors/arisohandriputra/"

static HCURSOR s_hCursorHand = NULL;

static LRESULT CALLBACK AboutDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            int dpi = g_nDpi > 0 ? g_nDpi : 96;

            RECT rcClient;
            GetClientRect(hDlg, &rcClient);
            int cx = rcClient.right - rcClient.left;
            if (cx <= 0) cx = MulDiv(390, dpi, 96);

            int icoSize = MulDiv(32, dpi, 96);
            int padX    = MulDiv(20, dpi, 96);
            int wText   = cx - padX * 2;

            HWND hIco = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                (cx - icoSize) / 2, MulDiv(14, dpi, 96), icoSize, icoSize,
                hDlg, (HMENU)0, g_hInst, NULL);
            HICON hIc = (HICON)LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                IMAGE_ICON, icoSize, icoSize, LR_DEFAULTCOLOR);
            if (hIc) SendMessageW(hIco, STM_SETICON, (WPARAM)hIc, 0);

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

            int curY = MulDiv(52, dpi, 96);
            HWND hName = CreateWindowExW(0, L"STATIC", L"HDDHealth Monitor",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(22, dpi, 96),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageW(hName, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            curY += MulDiv(26, dpi, 96);
            HWND hDesc = CreateWindowExW(0, L"STATIC",
                LStrW(STR_ABOUT_DESC),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(20, dpi, 96),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageW(hDesc, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += MulDiv(26, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            curY += MulDiv(10, dpi, 96);
            HWND hCopy = CreateWindowExW(0, L"STATIC",
                LStrW(STR_ABOUT_COPYRIGHT),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(20, dpi, 96),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageW(hCopy, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += MulDiv(22, dpi, 96);
            HWND hDrv = CreateWindowExW(0, L"STATIC",
                LStrW(STR_ABOUT_AUTHOR),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(20, dpi, 96),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageW(hDrv, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += MulDiv(26, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            curY += MulDiv(10, dpi, 96);
            HWND hLicStatus = CreateWindowExW(0, L"STATIC",
                LStrW(STR_ABOUT_FOSS),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                padX, curY, wText, MulDiv(20, dpi, 96),
                hDlg, (HMENU)IDC_ABOUT_LIC_STATUS, g_hInst, NULL);
            SendMessageW(hLicStatus, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += MulDiv(26, dpi, 96);
            int btnDonateW = MulDiv(130, dpi, 96);
            int btnDonateH = MulDiv(28, dpi, 96);
            HWND hActivate = CreateWindowExW(0, L"BUTTON", LStrW(STR_BTN_DONATE),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                (cx - btnDonateW) / 2, curY, btnDonateW, btnDonateH,
                hDlg, (HMENU)IDC_ABOUT_ACTIVATE, g_hInst, NULL);
            SendMessageW(hActivate, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            curY += btnDonateH + MulDiv(10, dpi, 96);
            CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                padX, curY, wText, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            curY += MulDiv(10, dpi, 96);
            HWND hLink = CreateWindowExW(0, L"STATIC",
                LStrW(STR_ABOUT_SPONSOR_LINK),
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                padX, curY, wText, MulDiv(20, dpi, 96),
                hDlg, (HMENU)IDC_ABOUT_LINK, g_hInst, NULL);
            SendMessageW(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            curY += MulDiv(28, dpi, 96);
            int btnOkW = MulDiv(90, dpi, 96);
            int btnOkH = MulDiv(28, dpi, 96);
            HWND hBtn = CreateWindowExW(0, L"BUTTON", LStrW(STR_BTN_OK),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                (cx - btnOkW) / 2, curY, btnOkW, btnOkH,
                hDlg, (HMENU)IDOK, g_hInst, NULL);
            SendMessageW(hBtn, WM_SETFONT, (WPARAM)hFontNorm, TRUE);

            if (!s_hCursorHand)
                s_hCursorHand = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            DestroyWindow(hDlg);
        }
        else if (LOWORD(wParam) == IDC_ABOUT_ACTIVATE) {
            Donate_OpenSponsorsPage(hDlg);
        }
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND hCtrl = (HWND)lParam;
            int  nID   = GetDlgCtrlID(hCtrl);
            HDC  hdcSt = (HDC)wParam;
            SetBkMode(hdcSt, TRANSPARENT);
            if (nID == IDC_ABOUT_LINK) {
                SetTextColor(hdcSt, RGB(0, 102, 204));
            } else if (nID == IDC_ABOUT_LIC_STATUS) {
                SetTextColor(hdcSt, RGB(0, 140, 0));
            }
            return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
        }

    case WM_SETCURSOR:
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hDlg, &pt);
            HWND hOver = ChildWindowFromPoint(hDlg, pt);
            if (hOver && GetDlgCtrlID(hOver) == IDC_ABOUT_LINK && s_hCursorHand) {
                SetCursor(s_hCursorHand);
                return TRUE;
            }
            break;
        }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
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

    if (uMsg == WM_COMMAND &&
        LOWORD(wParam) == IDC_ABOUT_LINK &&
        HIWORD(wParam) == STN_CLICKED) {
        Donate_OpenSponsorsPage(hDlg);
        return 0;
    }

    return DefWindowProcW(hDlg, uMsg, wParam, lParam);
}

void ShowAboutDialog(HWND hWndParent)
{
    static BOOL bRegistered = FALSE;
    if (!bRegistered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = AboutDlgProc;
        wc.hInstance     = g_hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"LLHDAboutDlg";
        RegisterClassExW(&wc);
        bRegistered = TRUE;
    }

    HWND hExist = FindWindowW(L"LLHDAboutDlg", NULL);
    if (hExist) { SetForegroundWindow(hExist); return; }

    int dpi = g_nDpi > 0 ? g_nDpi : 96;
    int dlgW = MulDiv(390, dpi, 96);
    int dlgH = MulDiv(370, dpi, 96);

    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);

    int nX, nY;
    if (hWndParent) {
        RECT rcP;
        GetWindowRect(hWndParent, &rcP);
        nX = rcP.left + (rcP.right  - rcP.left - dlgW) / 2;
        nY = rcP.top  + (rcP.bottom - rcP.top  - dlgH) / 2;
    } else {
        nX = (nScrW - dlgW) / 2;
        nY = (nScrH - dlgH) / 2;
    }

    RECT rcAdj = {0, 0, dlgW, dlgH};
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    int nWinW = rcAdj.right  - rcAdj.left;
    int nWinH = rcAdj.bottom - rcAdj.top;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"LLHDAboutDlg",
        LStrW(STR_ABOUT_TITLE),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hWndParent, NULL, g_hInst, NULL
    );
    if (!hDlg) return;

    HICON hIco = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hIco);
    SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIco);

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void CreateMenuBar(HWND hWnd)
{
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, IDM_SCREENSHOT, LStrW(STR_MENU_SCREENSHOT));
    AppendMenuW(hFile, MF_STRING, IDM_SAVETEXT,   LStrW(STR_MENU_SAVETEXT));
    AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hFile, MF_STRING, IDM_EXIT,       LStrW(STR_MENU_EXIT));
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hFile, LStrW(STR_MENU_FILE));

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, IDM_HISTORY, LStrW(STR_MENU_HISTORY));
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hView, LStrW(STR_MENU_VIEW));

    HMENU hLang = CreatePopupMenu();
    AppendMenuW(hLang, MF_STRING | (Lang_GetCurrent() == LANG_ZH_CN ? MF_CHECKED : MF_UNCHECKED),
                IDM_LANG_ZH, LStrW(STR_LANG_NAME_ZH));
    AppendMenuW(hLang, MF_STRING | (Lang_GetCurrent() == LANG_EN ? MF_CHECKED : MF_UNCHECKED),
                IDM_LANG_EN, LStrW(STR_LANG_NAME_EN));
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hLang, LStrW(STR_MENU_LANG));

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, IDM_DONATE, LStrW(STR_MENU_DONATE));
    AppendMenuW(hHelp, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hHelp, MF_STRING, IDM_ABOUT, LStrW(STR_MENU_ABOUT));
    AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, LStrW(STR_MENU_HELP));

    HMENU hOld = GetMenu(hWnd);
    SetMenu(hWnd, hMenuBar);
    if (hOld) DestroyMenu(hOld);
    DrawMenuBar(hWnd);
}

void RelayoutControls(HWND hWnd)
{
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;
    if (clientW < 100 || clientH < 100) return;

    int nLeftW   = DRIVE_BTN_PANEL_W;
    int nRightX  = nLeftW + SCALE_DPI(10);
    int nBarsW   = SCALE_DPI(190);
    int nInfoX   = nRightX + nBarsW + SCALE_DPI(10);
    int nInfoY   = SCALE_DPI(36);
    int nInfoH   = SCALE_DPI(18);
    int nInfoGap = SCALE_DPI(4);
    int nLblW    = SCALE_DPI(90);
    int nValX    = nInfoX + nLblW + SCALE_DPI(4);
    int nValW    = clientW - nValX - SCALE_DPI(8);
    if (nValW < SCALE_DPI(40)) nValW = SCALE_DPI(40);

    HWND hDriveLabel = GetDlgItem(hWnd, IDC_DRIVE_LIST);
    if (hDriveLabel) {
        SetWindowPos(hDriveLabel, NULL, SCALE_DPI(6), SCALE_DPI(8), nLeftW - SCALE_DPI(12), SCALE_DPI(16), SWP_NOZORDER);
        SendMessageW(hDriveLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    HWND hPerfLabel = GetDlgItem(hWnd, IDC_PERF_LABEL);
    if (hPerfLabel) {
        SetWindowPos(hPerfLabel, NULL, nRightX, SCALE_DPI(40), nBarsW, SCALE_DPI(14), SWP_NOZORDER);
        SendMessageW(hPerfLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    if (g_hPerfBar) {
        SetWindowPos(g_hPerfBar, NULL, nRightX, SCALE_DPI(56), nBarsW, SCALE_DPI(40), SWP_NOZORDER);
    }

    HWND hHealthLabel = GetDlgItem(hWnd, IDC_HEALTH_LABEL);
    if (hHealthLabel) {
        SetWindowPos(hHealthLabel, NULL, nRightX, SCALE_DPI(102), nBarsW, SCALE_DPI(14), SWP_NOZORDER);
        SendMessageW(hHealthLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    if (g_hHealthBar) {
        SetWindowPos(g_hHealthBar, NULL, nRightX, SCALE_DPI(118), nBarsW, SCALE_DPI(40), SWP_NOZORDER);
    }

    HWND hHistBtn = GetDlgItem(hWnd, IDC_HISTORY_BTN_MAIN);
    if (hHistBtn) {
        SetWindowPos(hHistBtn, NULL, nRightX, SCALE_DPI(164), SCALE_DPI(130), SCALE_DPI(26), SWP_NOZORDER);
        SendMessageW(hHistBtn, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    int lblIDs[7] = { IDC_MODEL_LABEL, IDC_SERIAL_LABEL, IDC_FIRMWARE_LABEL, IDC_SIZE_LABEL, IDC_TEMP_LABEL, IDC_STATUS_LABEL, IDC_READ_SPEED_LABEL };
    int valIDs[7] = { IDC_MODEL_STATIC, IDC_SERIAL_STATIC, IDC_FIRMWARE_STATIC, IDC_SIZE_STATIC, IDC_TEMP_STATIC, IDC_STATUS_STATIC, IDC_READ_SPEED_STATIC };

    int i;
    for (i = 0; i < 7; i++) {
        HWND hL = GetDlgItem(hWnd, lblIDs[i]);
        if (hL) {
            SetWindowPos(hL, NULL, nInfoX, nInfoY + (nInfoH + nInfoGap) * i, nLblW, nInfoH, SWP_NOZORDER);
            SendMessageW(hL, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
        }
        HWND hV = GetDlgItem(hWnd, valIDs[i]);
        if (hV) {
            SetWindowPos(hV, NULL, nValX, nInfoY + (nInfoH + nInfoGap) * i, nValW, nInfoH, SWP_NOZORDER);
            SendMessageW(hV, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
    }

    HWND hPred = GetDlgItem(hWnd, IDC_PREDICT_STATIC);
    if (hPred) {
        SetWindowPos(hPred, NULL, nRightX, SCALE_DPI(236), clientW - nRightX - SCALE_DPI(8), SCALE_DPI(20), SWP_NOZORDER);
        SendMessageW(hPred, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    }

    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        int listY = SCALE_DPI(260);
        int listW = clientW - nRightX - SCALE_DPI(8);
        int listH = clientH - listY - SCALE_DPI(8);
        if (listH < SCALE_DPI(50)) listH = SCALE_DPI(50);
        SetWindowPos(hList, NULL, nRightX, listY, listW, listH, SWP_NOZORDER);
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        ListView_SetColumnWidth(hList, 0, SCALE_DPI(48));
        ListView_SetColumnWidth(hList, 1, SCALE_DPI(170));
        ListView_SetColumnWidth(hList, 2, SCALE_DPI(260));
        ListView_SetColumnWidth(hList, 3, SCALE_DPI(80));
    }

    UpdateDriveButtons(hWnd);
}

void CreateControls(HWND hWnd)
{
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    int i;
    for (i = 0; i < MAX_DRIVES; i++) g_hDriveBtn[i] = NULL;

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_DRIVES),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)(IDC_DRIVE_LIST), g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_PERF),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_PERF_LABEL, g_hInst, NULL);

    g_hPerfBar = CreateWindowExW(WS_EX_CLIENTEDGE, L"LLHDPerfBar", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_PERF_BAR_FRAME, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_HEALTH),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_HEALTH_LABEL, g_hInst, NULL);

    g_hHealthBar = CreateWindowExW(WS_EX_CLIENTEDGE, L"LLHDHealthBar", L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_HEALTH_BAR_FRAME, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_MODEL),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_MODEL_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_MODEL_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_SERIAL),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_SERIAL_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_SERIAL_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_FIRMWARE),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_FIRMWARE_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_FIRMWARE_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_CAPACITY),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_SIZE_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_SIZE_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_TEMPERATURE),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_TEMP_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_TEMP_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_SMART),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_STATUS_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_STATUS_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", LStrW(STR_LBL_READ_SPEED),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_READ_SPEED_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"-",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_READ_SPEED_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_PREDICT_STATIC, g_hInst, NULL);

    CreateWindowExW(0, L"BUTTON", LStrW(STR_BTN_HISTORY_GRAPH),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_HISTORY_BTN_MAIN, g_hInst, NULL);

    HWND hList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        0, 0, 10, 10,
        hWnd, (HMENU)IDC_ATTR_LIST, g_hInst, NULL
    );
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    ListView_SetBkColor(hList, CLR_PANEL);
    ListView_SetTextBkColor(hList, CLR_ROW1);
    ListView_SetTextColor(hList, CLR_TEXT);

    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt  = LVCFMT_LEFT;

    col.cx = SCALE_DPI(48);  col.pszText = (LPWSTR)LStrW(STR_COL_ID);        SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = SCALE_DPI(170); col.pszText = (LPWSTR)LStrW(STR_COL_ATTRIBUTE); SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = SCALE_DPI(260); col.pszText = (LPWSTR)LStrW(STR_COL_VALUE);     SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = SCALE_DPI(80);  col.pszText = (LPWSTR)LStrW(STR_COL_STATUS);    SendMessageW(hList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);

    RelayoutControls(hWnd);
}

void UpdateUILanguage(HWND hWnd)
{
    UpdateWindowTitle(hWnd);
    CreateMenuBar(hWnd);

    /* Update static labels and buttons */
    SetDlgItemTextW(hWnd, IDC_DRIVE_LIST, LStrW(STR_LBL_DRIVES));
    SetDlgItemTextW(hWnd, IDC_PERF_LABEL, LStrW(STR_LBL_PERF));
    SetDlgItemTextW(hWnd, IDC_HEALTH_LABEL, LStrW(STR_LBL_HEALTH));
    SetDlgItemTextW(hWnd, IDC_MODEL_LABEL, LStrW(STR_LBL_MODEL));
    SetDlgItemTextW(hWnd, IDC_SERIAL_LABEL, LStrW(STR_LBL_SERIAL));
    SetDlgItemTextW(hWnd, IDC_FIRMWARE_LABEL, LStrW(STR_LBL_FIRMWARE));
    SetDlgItemTextW(hWnd, IDC_SIZE_LABEL, LStrW(STR_LBL_CAPACITY));
    SetDlgItemTextW(hWnd, IDC_TEMP_LABEL, LStrW(STR_LBL_TEMPERATURE));
    SetDlgItemTextW(hWnd, IDC_STATUS_LABEL, LStrW(STR_LBL_SMART));
    SetDlgItemTextW(hWnd, IDC_READ_SPEED_LABEL, LStrW(STR_LBL_READ_SPEED));
    SetDlgItemTextW(hWnd, IDC_HISTORY_BTN_MAIN, LStrW(STR_BTN_HISTORY_GRAPH));

    /* Update ListView column headers */
    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT;

        col.pszText = (LPWSTR)LStrW(STR_COL_ID);
        SendMessageW(hList, LVM_SETCOLUMNW, 0, (LPARAM)&col);

        col.pszText = (LPWSTR)LStrW(STR_COL_ATTRIBUTE);
        SendMessageW(hList, LVM_SETCOLUMNW, 1, (LPARAM)&col);

        col.pszText = (LPWSTR)LStrW(STR_COL_VALUE);
        SendMessageW(hList, LVM_SETCOLUMNW, 2, (LPARAM)&col);

        col.pszText = (LPWSTR)LStrW(STR_COL_STATUS);
        SendMessageW(hList, LVM_SETCOLUMNW, 3, (LPARAM)&col);
    }

    UpdateDriveButtons(hWnd);
    UpdateDriveInfo(hWnd, g_nSelectedDrive);
    UpdateAttrList(hWnd, g_nSelectedDrive);
    TrayIcon_Update();
    Graph_Repaint();
    InvalidateRect(hWnd, NULL, TRUE);
}

static LRESULT HandleCtlColor(HWND hWnd, WPARAM wParam)
{
    HDC  hdc     = (HDC)wParam;

    HWND hSender = WindowFromDC(hdc);
    if (hSender) {
        int id = GetDlgCtrlID(hSender);
        if (id == IDC_MODEL_LABEL    || id == IDC_SERIAL_LABEL   ||
            id == IDC_FIRMWARE_LABEL || id == IDC_SIZE_LABEL      ||
            id == IDC_TEMP_LABEL     || id == IDC_STATUS_LABEL    ||
            id == IDC_READ_SPEED_LABEL) {
            SetTextColor(hdc, CLR_TEXT_DIM);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
    }
    SetTextColor(hdc, CLR_TEXT);
    SetBkColor(hdc, CLR_BG);
    return (LRESULT)g_hbrBG;
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        g_hMainWnd = hWnd;
        RegisterHealthBarClass(g_hInst);
        Graph_RegisterClass(g_hInst);
        History_Init();
        History_Load();
        CreateGDIObjects();
        CreateMenuBar(hWnd);
        CreateControls(hWnd);
        TrayIcon_Add(hWnd);
        DeviceNotify_Register(hWnd);
        SetTimer(hWnd, IDT_REFRESH, REFRESH_INTERVAL_MS, NULL);
        SetTimer(hWnd, IDT_TITLE_UPDATE, 1000, NULL);

        UpdateWindowTitle(hWnd);
        RefreshData(hWnd);
        {
            GdiplusStartupInput gdipInput;
            GdiplusStartup(&g_gdiplusToken, &gdipInput, NULL);
        }
        return 0;

    case WM_DPICHANGED:
        {
            g_nDpi = HIWORD(wParam);
            RECT* prcNew = (RECT*)lParam;
            SetWindowPos(hWnd, NULL, prcNew->left, prcNew->top,
                         prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            CreateGDIObjects();
            RelayoutControls(hWnd);
            UpdateUILanguage(hWnd);
        }
        return 0;

    case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, g_hbrBG);
        }
        return 1;

    case WM_CTLCOLORSTATIC:
        return HandleCtlColor(hWnd, wParam);

    case WM_CTLCOLORBTN:
        return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, CLR_TEXT);
            SetBkColor(hdc, CLR_PANEL);
            return (LRESULT)g_hbrPanel;
        }

    case WM_NOTIFY:
        {
            NMHDR* pHdr = (NMHDR*)lParam;
            if (pHdr->idFrom == IDC_ATTR_LIST && pHdr->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW* pCD = (NMLVCUSTOMDRAW*)lParam;

                switch (pCD->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;

                case CDDS_ITEMPREPAINT:
                    pCD->clrTextBk = (pCD->nmcd.dwItemSpec % 2 == 0) ? CLR_ROW1 : CLR_ROW2;
                    pCD->clrText   = CLR_TEXT;
                    return CDRF_NOTIFYSUBITEMDRAW;

                case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
                    {
                        COLORREF clrRowBk = (pCD->nmcd.dwItemSpec % 2 == 0)
                                            ? CLR_ROW1 : CLR_ROW2;
                        pCD->clrTextBk = clrRowBk;
                        pCD->clrText   = CLR_TEXT;

                        if (pCD->iSubItem == 2) {
                            wchar_t szVal[128] = L"", szStat[32] = L"";
                            LVITEMW lvi;
                            ZeroMemory(&lvi, sizeof(lvi));
                            lvi.mask       = LVIF_TEXT;
                            lvi.iItem      = (int)pCD->nmcd.dwItemSpec;
                            lvi.iSubItem   = 2;
                            lvi.pszText    = szVal;
                            lvi.cchTextMax = 128;
                            SendMessageW(pCD->nmcd.hdr.hwndFrom, LVM_GETITEMW, 0, (LPARAM)&lvi);

                            lvi.iSubItem   = 3;
                            lvi.pszText    = szStat;
                            lvi.cchTextMax = 32;
                            SendMessageW(pCD->nmcd.hdr.hwndFrom, LVM_GETITEMW, 0, (LPARAM)&lvi);

                            BOOL bIsFailed  = (wcscmp(szStat, L"FAILED") == 0 || wcscmp(szStat, L"异常") == 0);
                            BOOL bIsWarning = (_wcsicmp(szStat, L"Warning") == 0 || wcscmp(szStat, L"WARNING") == 0 || wcscmp(szStat, L"警告") == 0);
                            BOOL bIsOK      = (wcscmp(szStat, L"OK") == 0 || wcscmp(szStat, L"正常") == 0);

                            COLORREF clrVal;
                            BOOL     bBold      = FALSE;
                            BOOL     bHasAlert  = (wcsstr(szVal, L"(!)") != NULL);

                            if (bIsFailed) {
                                clrVal = RGB(220, 38,  38);
                                bBold  = TRUE;
                            } else if (bIsWarning || bHasAlert) {
                                clrVal = RGB(194, 100,  0);
                                bBold  = TRUE;
                            } else if (bIsOK) {
                                clrVal = CLR_TEXT;
                            } else {
                                clrVal = CLR_TEXT_DIM;
                            }

                            HDC  hdc    = pCD->nmcd.hdc;
                            RECT rcCell = pCD->nmcd.rc;

                            HBRUSH hbrRow2 = CreateSolidBrush(clrRowBk);
                            FillRect(hdc, &rcCell, hbrRow2);
                            DeleteObject(hbrRow2);

                            if (szVal[0] == L'\0') return CDRF_SKIPDEFAULT;

                            SetBkMode(hdc, TRANSPARENT);

                            int nFull = lstrlenW(szVal);

                            wchar_t* pBracket = wcschr(szVal, L'[');
                            wchar_t* pAlert   = wcsstr(szVal, L"(!)");
                            wchar_t* pOK      = wcsstr(szVal, L"(OK)");
                            wchar_t* pNone    = wcsstr(szVal, L"(None)");
                            wchar_t* pNoneZH  = wcsstr(szVal, L"(无)");

                            HFONT hCurFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
                            LOGFONTW lf;
                            GetObjectW(hCurFont, sizeof(lf), &lf);
                            HFONT hBoldFont = NULL, hDimFont = NULL;
                            if (bBold) {
                                LOGFONTW lfBold = lf;
                                lfBold.lfWeight = FW_SEMIBOLD;
                                hBoldFont = CreateFontIndirectW(&lfBold);
                            }
                            {
                                LOGFONTW lfDim = lf;
                                lfDim.lfWeight = FW_NORMAL;
                                hDimFont = CreateFontIndirectW(&lfDim);
                            }

                            RECT rcDraw = rcCell;
                            rcDraw.left += SCALE_DPI(4);

                            if (pBracket && (pAlert == NULL)) {
                                int nMain  = (int)(pBracket - szVal);
                                while (nMain > 0 && szVal[nMain-1] == L' ') nMain--;

                                if (bBold && hBoldFont) SelectObject(hdc, hBoldFont);
                                SetTextColor(hdc, clrVal);
                                DrawTextW(hdc, szVal, nMain, &rcDraw,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                                int nMainW = rcDraw.right - rcDraw.left;
                                rcDraw.right = rcCell.right;
                                DrawTextW(hdc, szVal, nMain, &rcDraw,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                                RECT rcBrk = rcDraw;
                                rcBrk.left += nMainW + SCALE_DPI(4);
                                SelectObject(hdc, hDimFont);
                                SetTextColor(hdc, RGB(148, 163, 184));
                                DrawTextW(hdc, pBracket, -1, &rcBrk,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                            } else {
                                int iSplit = -1;
                                {
                                    int k;
                                    for (k = 0; k < nFull; k++) {
                                        if (szVal[k] == L' ' && k > 0) { iSplit = k; break; }
                                    }
                                }

                                COLORREF clrUnit = CLR_TEXT_DIM;

                                if (pAlert) {
                                    if (bBold && hBoldFont) SelectObject(hdc, hBoldFont);
                                    SetTextColor(hdc, clrVal);
                                    DrawTextW(hdc, szVal, -1, &rcDraw,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                } else if (pOK) {
                                    int nPre = (int)(pOK - szVal);
                                    while (nPre > 0 && szVal[nPre-1] == L' ') nPre--;
                                    SelectObject(hdc, bBold && hBoldFont ? hBoldFont : hDimFont);
                                    SetTextColor(hdc, clrVal);
                                    if (nPre > 0) {
                                        DrawTextW(hdc, szVal, nPre, &rcDraw,
                                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                                        int nW = rcDraw.right - rcDraw.left;
                                        rcDraw.right = rcCell.right;
                                        DrawTextW(hdc, szVal, nPre, &rcDraw,
                                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                        RECT rcOK2 = rcDraw; rcOK2.left += nW + SCALE_DPI(4);
                                        SelectObject(hdc, hDimFont);
                                        SetTextColor(hdc, RGB(22, 163, 74));
                                        DrawTextW(hdc, L"(OK)", -1, &rcOK2,
                                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                    } else {
                                        SetTextColor(hdc, RGB(22, 163, 74));
                                        DrawTextW(hdc, szVal, -1, &rcDraw,
                                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                    }
                                } else if (pNone || pNoneZH) {
                                    SelectObject(hdc, hDimFont);
                                    SetTextColor(hdc, CLR_TEXT_DIM);
                                    DrawTextW(hdc, szVal, -1, &rcDraw,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                } else if (iSplit > 0 && iSplit < nFull - 1) {
                                    if (bBold && hBoldFont) SelectObject(hdc, hBoldFont);
                                    SetTextColor(hdc, clrVal);
                                    DrawTextW(hdc, szVal, iSplit, &rcDraw,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                                    int nNumW = rcDraw.right - rcDraw.left;
                                    rcDraw.right = rcCell.right;
                                    DrawTextW(hdc, szVal, iSplit, &rcDraw,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                                    RECT rcUnit = rcDraw;
                                    rcUnit.left += nNumW + SCALE_DPI(3);
                                    SelectObject(hdc, hDimFont);
                                    SetTextColor(hdc, clrUnit);
                                    DrawTextW(hdc, szVal + iSplit + 1, -1, &rcUnit,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                } else {
                                    if (bBold && hBoldFont) SelectObject(hdc, hBoldFont);
                                    SetTextColor(hdc, clrVal);
                                    DrawTextW(hdc, szVal, -1, &rcDraw,
                                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                                }
                            }

                            SelectObject(hdc, hCurFont);
                            if (hBoldFont) DeleteObject(hBoldFont);
                            if (hDimFont)  DeleteObject(hDimFont);
                            return CDRF_SKIPDEFAULT;
                        }
                        else if (pCD->iSubItem == 3) {
                            wchar_t szStatus[32] = L"";
                            LVITEMW lvi;
                            ZeroMemory(&lvi, sizeof(lvi));
                            lvi.mask       = LVIF_TEXT;
                            lvi.iItem      = (int)pCD->nmcd.dwItemSpec;
                            lvi.iSubItem   = 3;
                            lvi.pszText    = szStatus;
                            lvi.cchTextMax = 32;
                            SendMessageW(pCD->nmcd.hdr.hwndFrom, LVM_GETITEMW, 0, (LPARAM)&lvi);

                            BOOL bIsFailed  = (wcscmp(szStatus, L"FAILED") == 0 || wcscmp(szStatus, L"异常") == 0);
                            BOOL bIsWarning = (_wcsicmp(szStatus, L"Warning") == 0 || wcscmp(szStatus, L"WARNING") == 0 || wcscmp(szStatus, L"警告") == 0);
                            BOOL bIsOK      = (wcscmp(szStatus, L"OK") == 0 || wcscmp(szStatus, L"正常") == 0);
                            BOOL bIsNA      = (wcscmp(szStatus, L"--") == 0);

                            COLORREF clrBadgeBg, clrBadgeBorder, clrBadgeText;
                            if (bIsFailed) {
                                clrBadgeBg     = RGB(220, 38,  38);
                                clrBadgeBorder = RGB(185, 28,  28);
                                clrBadgeText   = RGB(255, 255, 255);
                            } else if (bIsWarning) {
                                clrBadgeBg     = RGB(217, 119,  6);
                                clrBadgeBorder = RGB(180,  83,  9);
                                clrBadgeText   = RGB(255, 255, 255);
                            } else if (bIsOK) {
                                clrBadgeBg     = RGB(22,  163, 74);
                                clrBadgeBorder = RGB(21,  128, 61);
                                clrBadgeText   = RGB(255, 255, 255);
                            } else if (bIsNA) {
                                clrBadgeBg     = RGB(148, 163, 184);
                                clrBadgeBorder = RGB(100, 116, 139);
                                clrBadgeText   = RGB(255, 255, 255);
                            } else {
                                return CDRF_NEWFONT;
                            }

                            HDC hdc = pCD->nmcd.hdc;
                            RECT rcCell = pCD->nmcd.rc;
                            SetBkColor(hdc, clrRowBk);
                            HBRUSH hbrRow = CreateSolidBrush(clrRowBk);
                            FillRect(hdc, &rcCell, hbrRow);
                            DeleteObject(hbrRow);

                            int nLen = lstrlenW(szStatus);
                            SIZE sz;
                            GetTextExtentPoint32W(hdc, szStatus, nLen, &sz);

                            int badgeH  = sz.cy + SCALE_DPI(6);
                            int badgeW  = sz.cx + SCALE_DPI(16);
                            int cellCX  = rcCell.right  - rcCell.left;
                            int cellCY  = rcCell.bottom - rcCell.top;
                            int bx      = rcCell.left + (cellCX - badgeW) / 2;
                            int by      = rcCell.top  + (cellCY - badgeH) / 2;
                            RECT rcBadge = { bx, by, bx + badgeW, by + badgeH };

                            HBRUSH hbrBadge  = CreateSolidBrush(clrBadgeBg);
                            HPEN   hpBorder  = CreatePen(PS_SOLID, 1, clrBadgeBorder);
                            HBRUSH hbrOld    = (HBRUSH)SelectObject(hdc, hbrBadge);
                            HPEN   hpOld     = (HPEN)SelectObject(hdc, hpBorder);

                            RoundRect(hdc, rcBadge.left, rcBadge.top,
                                           rcBadge.right, rcBadge.bottom, SCALE_DPI(8), SCALE_DPI(8));
                            SelectObject(hdc, hbrOld);
                            SelectObject(hdc, hpOld);
                            DeleteObject(hbrBadge);
                            DeleteObject(hpBorder);

                            SetBkMode(hdc, TRANSPARENT);
                            SetTextColor(hdc, clrBadgeText);
                            DrawTextW(hdc, szStatus, nLen, &rcBadge,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                            return CDRF_SKIPDEFAULT;
                        }
                        else if (pCD->iSubItem == 0) {
                            wchar_t szID[16] = L"";
                            LVITEMW lvi;
                            ZeroMemory(&lvi, sizeof(lvi));
                            lvi.mask       = LVIF_TEXT;
                            lvi.iItem      = (int)pCD->nmcd.dwItemSpec;
                            lvi.iSubItem   = 0;
                            lvi.pszText    = szID;
                            lvi.cchTextMax = 16;
                            SendMessageW(pCD->nmcd.hdr.hwndFrom, LVM_GETITEMW, 0, (LPARAM)&lvi);
                            BYTE bID = (BYTE)wcstol(szID, NULL, 16);
                            if (IsAttrCritical(bID))
                                pCD->clrText = CLR_ACCENT;
                        }
                        return CDRF_NEWFONT;
                    }

                default:
                    return CDRF_DODEFAULT;
                }
            }
        }
        return 0;

    case WM_TRAYICON:
        {
            int nClickedDrive = (int)(wParam - IDI_TRAY);
            if (nClickedDrive < 0 || nClickedDrive >= MAX_DRIVES)
                nClickedDrive = 0;

            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                g_bMinToTray = FALSE;
                if (nClickedDrive < g_nDriveCount) {
                    g_nSelectedDrive = nClickedDrive;
                    int i;
                    for (i = 0; i < g_nDriveCount; i++)
                        if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
                    UpdateDriveInfo(hWnd, nClickedDrive);
                    UpdateAttrList(hWnd, nClickedDrive);
                    InvalidateRect(hWnd, NULL, FALSE);
                    UpdateWindow(hWnd);
                }
            } else if (lParam == WM_RBUTTONUP) {
                TrayIcon_ShowContextMenu(hWnd);
            }
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            KillTimer(hWnd, IDT_HOTPLUG);
            SetTimer(hWnd, IDT_HOTPLUG, HOTPLUG_DELAY_MS, NULL);
        }
        return TRUE;

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wParam == 'S') DoSaveScreenshot(hWnd);
            else if (wParam == 'T') ExportToTextFile(hWnd);
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        g_bMinToTray = TRUE;
        return 0;

    case WM_COMMAND:
        {
            int nCtrl = LOWORD(wParam);

            if (nCtrl >= IDC_DRIVE_BTN_BASE && nCtrl < IDC_DRIVE_BTN_BASE + MAX_DRIVES) {
                int nSel = nCtrl - IDC_DRIVE_BTN_BASE;
                if (nSel >= 0 && nSel < g_nDriveCount) {
                    g_nSelectedDrive = nSel;
                    int i;
                    for (i = 0; i < g_nDriveCount; i++)
                        if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
                    UpdateDriveInfo(hWnd, nSel);
                    UpdateAttrList(hWnd, nSel);
                    InvalidateRect(hWnd, NULL, FALSE);
                    UpdateWindow(hWnd);
                }
            }
            else if (nCtrl == IDM_SCREENSHOT) {
                DoSaveScreenshot(hWnd);
            }
            else if (nCtrl == IDM_SAVETEXT) {
                ExportToTextFile(hWnd);
            }
            else if (nCtrl == IDM_SHOW_WINDOW) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
                g_bMinToTray = FALSE;
            }
            else if (nCtrl == IDC_HISTORY_BTN_MAIN || nCtrl == IDM_HISTORY) {
                Graph_ShowWindow(hWnd, g_hInst, g_nSelectedDrive);
            }
            else if (nCtrl == IDM_LANG_ZH) {
                Lang_SetCurrent(LANG_ZH_CN);
                UpdateUILanguage(hWnd);
            }
            else if (nCtrl == IDM_LANG_EN) {
                Lang_SetCurrent(LANG_EN);
                UpdateUILanguage(hWnd);
            }
            else if (nCtrl == IDM_ABOUT) {
                ShowAboutDialog(hWnd);
            }
            else if (nCtrl == IDM_DONATE) {
                Donate_ShowDialog(hWnd);
            }
            else if (nCtrl == IDM_EXIT) {
                TrayIcon_Remove();
                DestroyWindow(hWnd);
            }
        }
        return 0;

    case WM_APP_REFRESH_DONE:
        InterlockedExchange(&g_bScanBusy, 0);
        {
            UpdateDriveButtons(hWnd);

            if (g_nSelectedDrive >= g_nDriveCount) g_nSelectedDrive = 0;

            UpdateDriveInfo(hWnd, g_nSelectedDrive);
            UpdateAttrList(hWnd, g_nSelectedDrive);
            RepaintHealthBar();
            TrayIcon_Update();
            Graph_Repaint();

            int i;
            for (i = 0; i < g_nDriveCount; i++)
                if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);

            InvalidateRect(hWnd, NULL, FALSE);
            UpdateWindow(hWnd);
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_REFRESH) {
            RefreshData(hWnd);
        } else if (wParam == IDT_TITLE_UPDATE) {
            UpdateWindowTitle(hWnd);
        } else if (wParam == IDT_HOTPLUG) {
            KillTimer(hWnd, IDT_HOTPLUG);
            RefreshData(hWnd);
        }
        return 0;

    case WM_SIZE:
        RelayoutControls(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_REFRESH);
        KillTimer(hWnd, IDT_TITLE_UPDATE);
        KillTimer(hWnd, IDT_HOTPLUG);
        DeviceNotify_Unregister();
        TrayIcon_Remove();
        History_Save();
        Graph_DestroyAll();
        DestroyGDIObjects();
        if (g_gdiplusToken) { GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}
