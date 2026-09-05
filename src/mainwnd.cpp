/* DriveMonitor - main window. Fork of HDDHealth Monitor, MIT: see LICENSE. */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dbt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define GDIPVER 0x0110
#include <objbase.h>
#include <shlobj.h>
#include <gdiplus.h>
using namespace Gdiplus;

#include "mainwnd.h"
#include "smart.h"
#include "utf8ui.h"
#include "safestr.h"

#define DONATE_URL "https://boosty.to/chuikoff"

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

/* NVMe Health Log Data Units: 1 unit = 1000 * 512 = 512000 bytes (NVMe spec).
 * CrystalDiskInfo TBW = units * 512000 / 1e12. */
static void FormatNvmeHostBytes(unsigned __int64 units, char* szBuf, int nBufLen)
{
    double bytes = (double)units * 512000.0;
    double tb = bytes / 1e12;
    if (tb >= 1.0)
        safe_snprintf_n(szBuf, nBufLen, "%.1f ТБ", tb);
    else
        safe_snprintf_n(szBuf, nBufLen, "%.1f ГБ", bytes / 1e9);
}

/* USB enclosure/chip name from SCSI INQUIRY; type name / VID:PID fallback.
 * Names come from DetectUsbBridgeType comments — no extra VID table. */

/* ATA SSD: nSSDTotalWritesGB is vendor RAW, typically GiB (attr E9/F9). */
static void FormatAtaWrites(int nGiB, char* szBuf, int nBufLen)
{
    if (nGiB < 0) { szBuf[0] = '\0'; return; }
    if (nGiB >= 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f ТБ", (double)nGiB / 1024.0);
    else
        safe_snprintf_n(szBuf, nBufLen, "%d ГБ", nGiB);
}

/* Unified SMART headline for every drive type:
 *   Запас …  ·  Износ …  ·  Записано …
 * Missing values are an em dash; fields are never omitted. */
static void FormatSmartHeadline(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    char szSpare[32], szWear[32], szWritten[32];
    if (!pInfo || nBufLen <= 0) return;
    szBuf[0] = '\0';
    lstrcpynA(szSpare, "н/д", sizeof(szSpare));
    lstrcpynA(szWear, "н/д", sizeof(szWear));
    lstrcpynA(szWritten, "н/д", sizeof(szWritten));

    if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        safe_snprintf(szSpare, "%d%%",
                  (int)pInfo->nvmeHealth.AvailableSpare);
        safe_snprintf(szWear, "%d%%",
                  (int)pInfo->nvmeHealth.PercentageUsed);
        FormatNvmeHostBytes(NVMeRead128Lo(pInfo->nvmeHealth.DataUnitsWritten),
                            szWritten, sizeof(szWritten));
    } else if (pInfo->bSMART_Supported) {
        /* ATA SSD: no spare-like field is extracted, so Запас stays н/д. */
        if (pInfo->nSSDLifeLeft >= 0 && pInfo->nSSDLifeLeft <= 100)
            safe_snprintf(szWear, "%d%%", 100 - pInfo->nSSDLifeLeft);
        if (pInfo->nSSDTotalWritesGB >= 0)
            FormatAtaWrites(pInfo->nSSDTotalWritesGB, szWritten, sizeof(szWritten));
    }

    safe_snprintf_n(szBuf, nBufLen, "Запас %s  ·  Износ %s  ·  Записано %s",
              szSpare, szWear, szWritten);
}

static void FormatUsbAdapterName(const DRIVE_INFO* p, char* szBuf, int nBufLen)
{
    const char* chip = NULL;
    if (!p || nBufLen <= 0) return;
    szBuf[0] = '\0';

    /* SCSI INQUIRY on SAT often returns the *disk* (Kingston A400), not the
     * USB chip. Skip it when it matches the drive model. Prefer USB VID. */
    {
        BOOL inqIsDisk = FALSE;
        if (p->szBridgeProduct[0] && p->szModel[0] &&
            (strstr(p->szModel, p->szBridgeProduct) ||
             strstr(p->szBridgeProduct, p->szModel)))
            inqIsDisk = TRUE;
        if (!inqIsDisk && p->szBridgeProduct[0]) {
            const char* pr = p->szBridgeProduct;
            if (strstr(pr, "RTL") || strstr(pr, "ASM") || strstr(pr, "JMS") ||
                strstr(pr, "Realtek") || strstr(pr, "ASMedia") ||
                strstr(pr, "JMicron")) {
                if (p->szBridgeVendor[0])
                    safe_snprintf_n(szBuf, nBufLen, "%s %s", p->szBridgeVendor, pr);
                else
                    safe_snprintf_n(szBuf, nBufLen, "%s", pr);
                return;
            }
        }
    }

    if (p->wUsbVid == 0x0BDA) {
        if (p->wUsbPid == 0x9210 || p->wUsbPid == 0x9211)
            safe_snprintf_n(szBuf, nBufLen, "Realtek RTL9210 (%04X:%04X)",
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        else if (p->wUsbPid)
            safe_snprintf_n(szBuf, nBufLen, "Realtek (%04X:%04X)",
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        else
            safe_snprintf_n(szBuf, nBufLen, "Realtek");
        return;
    }
    if (p->wUsbVid == 0x152D) {
        safe_snprintf_n(szBuf, nBufLen, "JMicron (%04X:%04X)",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        return;
    }
    if (p->wUsbVid == 0x174C) {
        safe_snprintf_n(szBuf, nBufLen, "ASMedia (%04X:%04X)",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        return;
    }

    switch (p->eUsbBridgeType) {
    case USB_BRIDGE_NVME_JMICRON: chip = "JMicron JMS583"; break;
    case USB_BRIDGE_NVME_ASMEDIA: chip = "ASMedia ASM2362"; break;
    case USB_BRIDGE_NVME_REALTEK: chip = "Realtek RTL9210"; break;
    case USB_BRIDGE_NVME_VLI:     chip = "VLI VL716"; break;
    case USB_BRIDGE_NVME_FMA:     chip = "FMA NL6221"; break;
    case USB_BRIDGE_ASM1352R:     chip = "ASMedia ASM1352R"; break;
    case USB_BRIDGE_JMICRON:      chip = "JMicron"; break;
    case USB_BRIDGE_SUNPLUS:      chip = "Sunplus"; break;
    case USB_BRIDGE_CYPRESS:      chip = "Cypress"; break;
    case USB_BRIDGE_IO_DATA:      chip = "I-O Data"; break;
    case USB_BRIDGE_LOGITEC:      chip = "Logitec"; break;
    case USB_BRIDGE_PROLIFIC:     chip = "Prolific"; break;
    case USB_BRIDGE_SAT:          chip = "SAT"; break;
    default: break;
    }

    if (chip && p->wUsbVid && p->wUsbPid)
        safe_snprintf_n(szBuf, nBufLen, "%s (%04X:%04X)", chip,
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
    else if (chip)
        safe_snprintf_n(szBuf, nBufLen, "%s", chip);
    else if (p->szBridgeVendor[0])
        safe_snprintf_n(szBuf, nBufLen, "%s", p->szBridgeVendor);
    else if (p->wUsbVid && p->wUsbPid)
        safe_snprintf_n(szBuf, nBufLen, "VID:%04X PID:%04X",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
    else
        safe_snprintf_n(szBuf, nBufLen, "—");
}

DRIVE_INFO  g_Drives[MAX_DRIVES];
int         g_nDriveCount    = 0;
int         g_nSelectedDrive = 0;
HINSTANCE   g_hInst          = NULL;
HWND        g_hMainWnd       = NULL;
HWND        g_hHealthBar     = NULL;
HWND        g_hDriveBtn[MAX_DRIVES];

static HDEVNOTIFY      g_hDevNotify    = NULL;
static DRIVE_INFO      g_PrevDrives[MAX_DRIVES];
static int             g_nPrevCount    = 0;
#define HOTPLUG_DELAY_MS  1200

/* Note: the previous WinRAR-style nag timer state variables
   (g_nagSecondsLeft, g_bNagPending) have been removed because the
   program is 100% free and open source
   reminder to show anymore. */

static void UpdateWindowTitle(HWND hWnd)
{
    SetWindowTextU8(hWnd, "DriveMonitor");
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

void CreateGDIObjects(void)
{
    g_hbrBG     = CreateSolidBrush(CLR_BG);
    g_hbrPanel  = CreateSolidBrush(CLR_PANEL);
    g_hbrGreen  = CreateSolidBrush(CLR_GREEN);
    g_hbrYellow = CreateSolidBrush(CLR_YELLOW);
    g_hbrRed    = CreateSolidBrush(CLR_RED);

    g_hFontTitle  = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hFontNormal = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hFontSmall  = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_hFontBig    = CreateFontA(-32, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
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
    safe_snprintf(szOutDir, "%s\\HDDH_Screenshots", szDocDir);
    CreateDirectoryA(szOutDir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char szFile[MAX_PATH];
    safe_snprintf(szFile,
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
                safe_snprintf_n(szErrOut, nErrMax,
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
        safe_snprintf(szMsg,
            "Screenshot saved successfully!\n\n%s\n\nOpen folder now?", szPath);
        int nRet = MessageBoxU8(hWnd, szMsg, "DriveMonitor - Screenshot Saved",
                               MB_YESNO | MB_ICONINFORMATION);
        if (nRet == IDYES) {

            char szCmd[MAX_PATH + 32];
            safe_snprintf(szCmd, "/select,\"%s\"", szPath);
            ShellExecuteA(NULL, "open", "explorer.exe", szCmd, NULL, SW_SHOWNORMAL);
        }
    } else {
        char szMsg[320];
        safe_snprintf(szMsg, "Screenshot failed:\n%s", szErr);
        MessageBoxU8(hWnd, szMsg, "DriveMonitor - Screenshot Error", MB_OK | MB_ICONERROR);
    }
}

/* Append UTF-8 bytes to a heap report buffer. Always NUL-terminates. */
static void ReportCat(char* buf, size_t cap, size_t* pLen, const char* s)
{
    size_t n, room;
    if (!buf || !pLen || cap == 0)
        return;
    if (*pLen >= cap) {
        buf[cap - 1] = '\0';
        return;
    }
    if (!s)
        return;
    room = cap - *pLen - 1;
    n = strlen(s);
    if (n > room)
        n = room;
    if (n > 0)
        memcpy(buf + *pLen, s, n);
    *pLen += n;
    buf[*pLen] = '\0';
}

static const char* ReportDash(const char* s)
{
    return (s && s[0]) ? s : "—";
}

/* Replace \ / : * ? " < > | and control chars with _; keep UTF-8 payload. */
static void SanitizeModelForFilename(const char* src, char* dst, int nDst)
{
    int i, o;
    if (!dst || nDst <= 0)
        return;
    dst[0] = '\0';
    if (nDst == 1)
        return;
    if (!src)
        src = "";
    o = 0;
    for (i = 0; src[i] && o < nDst - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 32 || c == 127 || c == '\\' || c == '/' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            dst[o++] = '_';
        else
            dst[o++] = (char)c;
    }
    dst[o] = '\0';
    if (dst[0] == '\0')
        lstrcpynA(dst, "disk", nDst);
}

static void Utf8TruncateBytes(char* s, int nMax)
{
    int i = 0;
    if (!s)
        return;
    if (nMax <= 0) {
        s[0] = '\0';
        return;
    }
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        int seq = 1;
        if ((c & 0xE0) == 0xC0)
            seq = 2;
        else if ((c & 0xF0) == 0xE0)
            seq = 3;
        else if ((c & 0xF8) == 0xF0)
            seq = 4;
        if (i + seq > nMax) {
            s[i] = '\0';
            return;
        }
        i += seq;
    }
}

static void BuildSaveReportFilter(WCHAR* dst, int nDst)
{
    static const char* parts[] = {
        "Текстовый отчёт (*.txt)",
        "*.txt",
        "Все файлы (*.*)",
        "*.*",
        ""
    };
    int pos = 0;
    int i;
    if (!dst || nDst <= 0)
        return;
    dst[0] = 0;
    for (i = 0; i < 5; i++) {
        WCHAR tmp[96];
        int n = U8ToW(parts[i], tmp, 96);
        if (n <= 0)
            n = 1;
        if (pos + n > nDst) {
            if (pos < nDst)
                dst[pos] = 0;
            break;
        }
        memcpy(dst + pos, tmp, (size_t)n * sizeof(WCHAR));
        pos += n;
    }
    if (pos < nDst)
        dst[pos] = 0; /* extra NUL if the last copy did not land one */
}

static void DoSaveDriveReport(HWND hWnd)
{
    const size_t kCap = 65536;
    DRIVE_INFO* pInfo;
    char* buf;
    size_t len = 0;
    SYSTEMTIME st;
    char szDocDir[MAX_PATH];
    char szModel[64];
    char szFileName[MAX_PATH];
    char szLine[512];
    WCHAR wzFile[MAX_PATH];
    WCHAR wzDir[MAX_PATH];
    WCHAR wzFilter[192];
    WCHAR wzTitle[64];
    OPENFILENAMEW ofn;
    HANDLE hFile;
    HWND hList;
    int nItems;
    int nMaxModel;
    int nDirLen;

    if (g_nSelectedDrive < 0 || g_nSelectedDrive >= g_nDriveCount) {
        MessageBoxU8(hWnd, "Нет выбранного диска", "DriveMonitor",
                     MB_OK | MB_ICONWARNING);
        return;
    }
    pInfo = &g_Drives[g_nSelectedDrive];

    buf = (char*)malloc(kCap);
    if (!buf) {
        MessageBoxU8(hWnd, "Недостаточно памяти для отчёта.", "DriveMonitor",
                     MB_OK | MB_ICONERROR);
        return;
    }
    buf[0] = '\0';

    GetLocalTime(&st);

    ReportCat(buf, kCap, &len, "DriveMonitor — отчёт по диску\r\n");
    safe_snprintf(szLine, "Дата: %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    ReportCat(buf, kCap, &len, szLine);

    ReportCat(buf, kCap, &len, "Диск\r\n");

    safe_snprintf(szLine, "Модель: %s\r\n", ReportDash(pInfo->szModel));
    ReportCat(buf, kCap, &len, szLine);

    {
        const char* brand = GetVendorName(pInfo->eVendor);
        if (pInfo->eVendor == VENDOR_UNKNOWN || pInfo->eVendor == VENDOR_OTHER)
            brand = "—";
        safe_snprintf(szLine, "Бренд: %s\r\n", brand);
        ReportCat(buf, kCap, &len, szLine);
    }

    safe_snprintf(szLine, "Контроллер: %s\r\n", GetControllerName(pInfo->eController));
    ReportCat(buf, kCap, &len, szLine);

    safe_snprintf(szLine, "NAND: %s\r\n", GetNandName(pInfo->eNand));
    ReportCat(buf, kCap, &len, szLine);

    safe_snprintf(szLine, "Серийный номер: %s\r\n", ReportDash(pInfo->szSerial));
    ReportCat(buf, kCap, &len, szLine);

    safe_snprintf(szLine, "Прошивка: %s\r\n", ReportDash(pInfo->szFirmware));
    ReportCat(buf, kCap, &len, szLine);

    {
        char szSize[32];
        FormatSize(pInfo->dwCapacityMB, szSize, (int)sizeof(szSize));
        safe_snprintf(szLine, "Объём: %s   Тип: %s\r\n",
                      szSize, GetDriveTypeName(pInfo->eType));
        ReportCat(buf, kCap, &len, szLine);
    }

    if (pInfo->nTemperatureC > 0) {
        if (pInfo->eTempBand != TEMP_BAND_UNKNOWN)
            safe_snprintf(szLine, "Температура: %d\xC2\xB0""C (%s)\r\n",
                          pInfo->nTemperatureC,
                          GetTempBandName(pInfo->eTempBand, TRUE));
        else
            safe_snprintf(szLine, "Температура: %d\xC2\xB0""C\r\n", pInfo->nTemperatureC);
    } else {
        safe_snprintf(szLine, "Температура: —\r\n");
    }
    ReportCat(buf, kCap, &len, szLine);

    {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        safe_snprintf(szLine, "Наработка: %s\r\n", szPoh);
        ReportCat(buf, kCap, &len, szLine);
    }
    if (pInfo->dwPowerCycleCount > 0)
        safe_snprintf(szLine, "Циклы включения: %lu\r\n",
                      (unsigned long)pInfo->dwPowerCycleCount);
    else
        safe_snprintf(szLine, "Циклы включения: нет данных\r\n");
    ReportCat(buf, kCap, &len, szLine);

    safe_snprintf(szLine, "Протокол: %s\r\n", ReportDash(pInfo->szProtocol));
    ReportCat(buf, kCap, &len, szLine);

    if (pInfo->bIsUSB) {
        char szAdapter[64];
        FormatUsbAdapterName(pInfo, szAdapter, (int)sizeof(szAdapter));
        safe_snprintf(szLine, "Переходник/мост: %s\r\n", ReportDash(szAdapter));
        ReportCat(buf, kCap, &len, szLine);
    }

    ReportCat(buf, kCap, &len, "\r\nОценка\r\n");
    if (len < kCap - 1) {
        FormatHealthLecturePlain(pInfo, buf + len, (int)(kCap - len));
        len = strlen(buf);
    }
    if (len == 0 || buf[len - 1] != '\n')
        ReportCat(buf, kCap, &len, "\r\n");
    ReportCat(buf, kCap, &len, "\r\n── Экспертный отчёт ──\r\n\r\n");
    if (len < kCap - 1) {
        FormatHealthLectureExpert(pInfo, buf + len, (int)(kCap - len));
        len = strlen(buf);
    }
    if (len == 0 || buf[len - 1] != '\n')
        ReportCat(buf, kCap, &len, "\r\n");

    ReportCat(buf, kCap, &len, "\r\nSMART / NVMe таблица\r\n");

    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    nItems = hList ? (int)ListView_GetItemCount(hList) : 0;
    if (nItems <= 0) {
        ReportCat(buf, kCap, &len, "Таблица SMART пуста.\r\n");
    } else {
        int r, c;
        static const char* headers[7] = {
            "ID", "Параметр", "Значение", "Худший", "Порог", "RAW", "Статус"
        };
        for (c = 0; c < 7; c++) {
            ReportCat(buf, kCap, &len, headers[c]);
            ReportCat(buf, kCap, &len, (c < 6) ? "\t" : "\r\n");
        }
        for (r = 0; r < nItems; r++) {
            for (c = 0; c < 7; c++) {
                WCHAR wcell[256];
                char ucell[512];
                wcell[0] = 0;
                ListView_GetItemText(hList, r, c, wcell, 256);
                WToU8(wcell, ucell, 512);
                ReportCat(buf, kCap, &len, ucell);
                ReportCat(buf, kCap, &len, (c < 6) ? "\t" : "\r\n");
            }
        }
    }

    szDocDir[0] = '\0';
    if (!SHGetSpecialFolderPathA(NULL, szDocDir, CSIDL_PERSONAL, TRUE)) {
        GetModuleFileNameA(NULL, szDocDir, MAX_PATH);
        {
            char* slash = strrchr(szDocDir, '\\');
            if (slash) *slash = '\0';
        }
    }

    SanitizeModelForFilename(pInfo->szModel, szModel, (int)sizeof(szModel));
    nDirLen = (int)strlen(szDocDir);
    /* dir + '\' + DriveMonitor_ + model + _YYYYMMDD_HHMMSS.txt + NUL */
    nMaxModel = MAX_PATH - nDirLen - 1 - 13 - 20 - 1;
    if (nMaxModel < 1)
        nMaxModel = 1;
    if (nMaxModel > (int)sizeof(szModel) - 1)
        nMaxModel = (int)sizeof(szModel) - 1;
    Utf8TruncateBytes(szModel, nMaxModel);
    if (szModel[0] == '\0')
        lstrcpynA(szModel, "disk", (int)sizeof(szModel));

    safe_snprintf(szFileName, "DriveMonitor_%s_%04d%02d%02d_%02d%02d%02d.txt",
                  szModel,
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);

    U8ToW(szFileName, wzFile, MAX_PATH);
    U8ToW(szDocDir, wzDir, MAX_PATH);
    BuildSaveReportFilter(wzFilter, 192);
    U8ToW("Сохранить отчёт", wzTitle, 64);

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = hWnd;
    ofn.lpstrFilter     = wzFilter;
    ofn.nFilterIndex    = 1;
    ofn.lpstrFile       = wzFile;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrInitialDir = wzDir;
    ofn.lpstrTitle      = wzTitle;
    ofn.Flags           = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                          OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    ofn.lpstrDefExt     = L"txt";

    if (!GetSaveFileNameW(&ofn)) {
        free(buf);
        return;
    }

    hFile = CreateFileW(ofn.lpstrFile, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxU8(hWnd, "Не удалось сохранить отчёт.", "DriveMonitor",
                     MB_OK | MB_ICONERROR);
        free(buf);
        return;
    }
    {
        static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, bom, 3, &written, NULL);
        if (ok && len > 0)
            ok = WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
        free(buf);
        buf = NULL;
        if (!ok) {
            MessageBoxU8(hWnd, "Не удалось записать отчёт.", "DriveMonitor",
                         MB_OK | MB_ICONERROR);
            return;
        }
    }

    {
        char szPathU8[MAX_PATH * 3];
        char szMsg[MAX_PATH * 3 + 128];
        WToU8(ofn.lpstrFile, szPathU8, (int)sizeof(szPathU8));
        safe_snprintf(szMsg, "Отчёт сохранён\n\n%s\n\nОткрыть папку?", szPathU8);
        if (MessageBoxU8(hWnd, szMsg, "DriveMonitor",
                         MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            WCHAR wzCmd[MAX_PATH + 32];
            _snwprintf(wzCmd, MAX_PATH + 32, L"/select,\"%s\"", ofn.lpstrFile);
            wzCmd[MAX_PATH + 31] = 0;
            ShellExecuteW(NULL, L"open", L"explorer.exe", wzCmd, NULL, SW_SHOWNORMAL);
        }
    }
}

static void Snapshot_Save(void)
{
    int i;
    g_nPrevCount = g_nDriveCount;
    for (i = 0; i < g_nDriveCount; i++)
        g_PrevDrives[i] = g_Drives[i];
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

void DestroyGDIObjects(void){
    if (g_hbrBG)     DeleteObject(g_hbrBG);
    if (g_hbrPanel)  DeleteObject(g_hbrPanel);
    if (g_hbrGreen)  DeleteObject(g_hbrGreen);
    if (g_hbrYellow) DeleteObject(g_hbrYellow);
    if (g_hbrRed)    DeleteObject(g_hbrRed);
    if (g_hFontTitle)  DeleteObject(g_hFontTitle);
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontSmall)  DeleteObject(g_hFontSmall);
    if (g_hFontBig)    DeleteObject(g_hFontBig);
}

COLORREF GetHealthStatusColor(DRIVE_HEALTH_STATUS eStatus)
{
    switch (eStatus) {
    case HEALTH_STATUS_GOOD:     return CLR_GREEN;
    case HEALTH_STATUS_OBSERVE:  return CLR_YELLOW;
    case HEALTH_STATUS_CAUTION:  return CLR_ORANGE;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:  return CLR_RED;
    case HEALTH_STATUS_CRITICAL: return CLR_CRITICAL;
    default:                     return CLR_ACCENT;
    }
}

static const char* GetAxisStatusName(DRIVE_HEALTH_STATUS eStatus, BOOL bTemp)
{
    if (bTemp && eStatus == HEALTH_STATUS_GOOD)
        return "НОРМА";
    if (bTemp && (eStatus == HEALTH_STATUS_OBSERVE || eStatus == HEALTH_STATUS_CAUTION))
        return "ПОВЫШЕНА";
    if (bTemp && (eStatus == HEALTH_STATUS_BAD || eStatus == HEALTH_STATUS_WARNING ||
                  eStatus == HEALTH_STATUS_CRITICAL))
        return "КРИТИЧЕСКАЯ";
    return GetHealthStatusName(eStatus);
}

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

            DRIVE_HEALTH_STATUS eSt = HEALTH_STATUS_UNKNOWN;
            BOOL bHaveDrive = (g_nDriveCount > 0 && g_nSelectedDrive >= 0 &&
                               g_nSelectedDrive < g_nDriveCount);
            if (bHaveDrive)
                eSt = g_Drives[g_nSelectedDrive].eHealthStatus;

            {
                HBRUSH hbrFill = CreateSolidBrush(GetHealthStatusColor(eSt));
                FillRect(hdc, &rc, hbrFill);
                DeleteObject(hbrFill);
            }

            {
                HPEN   hpBorder = CreatePen(PS_SOLID, 1, CLR_BORDER);
                HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                HBRUSH hbOld    = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, 0, 0, w, h);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBorder);
            }

            {
                const char* szName = GetHealthStatusName(eSt);
                HFONT hUseFont = g_hFontBig ? g_hFontBig :
                    (g_hFontTitle ? g_hFontTitle : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
                HFONT hOldFont = (HFONT)SelectObject(hdc, hUseFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                RECT rcText = { 0, 0, w, h };
                DrawTextU8(hdc, szName, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, hOldFont);
                if (GetFocus() == hWnd)
                    DrawFocusRect(hdc, &rc);
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

    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        return 0;

    case WM_LBUTTONUP:
        {
            HWND hParent = GetParent(hWnd);
            if (hParent)
                ShowHealthLectureDialog(hParent);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            HWND hParent = GetParent(hWnd);
            if (hParent)
                ShowHealthLectureDialog(hParent);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_BUTTON | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, (LPCTSTR)IDC_HAND));
        return TRUE;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
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

            COLORREF clrFill, clrBorder, clrText;
            if (bSelected) {
                clrFill   = CLR_HEADER;
                clrBorder = CLR_ACCENT;
                clrText   = CLR_TEXT;
            } else if (bHover) {
                clrFill   = CLR_ROW2;
                clrBorder = RGB(150, 180, 220);
                clrText   = CLR_TEXT;
            } else {
                clrFill   = CLR_PANEL;
                clrBorder = CLR_BORDER;
                clrText   = CLR_TEXT;
            }

            {
                HBRUSH hbrFill = CreateSolidBrush(clrFill);
                FillRect(hdc, &rcBuf, hbrFill);
                DeleteObject(hbrFill);
            }

            {
                HPEN   hpBord = CreatePen(PS_SOLID, 1, clrBorder);
                HPEN   hpOld  = (HPEN)SelectObject(hdc, hpBord);
                HBRUSH hbOld  = (HBRUSH)SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rcBuf.left, rcBuf.top, rcBuf.right, rcBuf.bottom);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBord);
            }

            SetBkMode(hdc, TRANSPARENT);

            if (nIdx >= 0 && nIdx < g_nDriveCount) {
                DRIVE_INFO* pD = &g_Drives[nIdx];

                char szName[64];
                if (strlen(pD->szModel) > 0) {
                    safe_snprintf(szName, "%s", pD->szModel);
                    if (strlen(szName) > 26) { szName[24] = '.'; szName[25] = '.'; szName[26] = '\0'; }
                } else {
                    safe_snprintf(szName, "Диск %d", pD->nDriveIndex);
                }

                char szType[16];
                const char* szT = GetDriveTypeName(pD->eType);
                safe_snprintf(szType, "[%s]", szT ? szT : "?");

                char szHealth[48];
                if (pD->eHealthStatus == HEALTH_STATUS_UNKNOWN)
                    safe_snprintf(szHealth, "—");
                else
                    safe_snprintf(szHealth, "%s", GetHealthStatusName(pD->eHealthStatus));

                char szCap[24];
                FormatSize(pD->dwCapacityMB, szCap, sizeof(szCap));

                char szTempStr[24];
                if (pD->nTemperatureC > 0)
                    safe_snprintf(szTempStr, "%d\xC2\xB0""C", pD->nTemperatureC);
                else
                    szTempStr[0] = '\0';

                COLORREF clrH = GetHealthStatusColor(pD->eHealthStatus);

                COLORREF clrTemp;
                if      (pD->nTemperatureC <= 0)  clrTemp = CLR_TEXT_DIM;
                else if (pD->nTemperatureC < 50)  clrTemp = CLR_GREEN;
                else if (pD->nTemperatureC < 60)  clrTemp = CLR_YELLOW;
                else                              clrTemp = CLR_RED;

                HFONT hOldFont;

                hOldFont = (HFONT)SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, clrText);
                RECT rcName = { rcBuf.left + 8, rcBuf.top + 4, rcBuf.right - 8, rcBuf.top + 20 };
                DrawTextU8(hdc, szName, &rcName, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                SelectObject(hdc, g_hFontSmall);
                RECT rcType = { rcBuf.left + 8, rcBuf.top + 20, (rcBuf.left + rcBuf.right) / 2, rcBuf.top + 36 };
                SetTextColor(hdc, CLR_TEXT_DIM);
                DrawTextU8(hdc, szType, &rcType, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                RECT rcHealth = { (rcBuf.left + rcBuf.right) / 2, rcBuf.top + 20, rcBuf.right - 8, rcBuf.top + 36 };
                SetTextColor(hdc, clrH);
                DrawTextU8(hdc, szHealth, &rcHealth, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

                {
                    RECT rcCap  = { rcBuf.left + 8, rcBuf.bottom - 17, (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - 4 };
                    RECT rcTmp  = { (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - 17, rcBuf.right - 8, rcBuf.bottom - 4 };
                    SetTextColor(hdc, clrText);
                    DrawTextU8(hdc, szCap, &rcCap, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                    if (szTempStr[0]) {
                        SetTextColor(hdc, clrTemp);
                        DrawTextU8(hdc, szTempStr, &rcTmp, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
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

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void RegisterHealthBarClass(HINSTANCE hInst)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HealthBarWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"LLHDHealthBar";
    RegisterClassW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = DriveBtnWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"LLHDDriveBtn";
    RegisterClassW(&wc);
}

void RepaintHealthBar(void)
{
    if (g_hHealthBar) { InvalidateRect(g_hHealthBar, NULL, TRUE); UpdateWindow(g_hHealthBar); }
}

void UpdateDriveButtons(HWND hWnd)
{
    int i;
    int nBtnW  = DRIVE_BTN_PANEL_W - 12;
    int nBtnH  = DRIVE_BTN_H;
    int nStartY = 40;

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
            HWND hPlaceholder = CreateWindowExU8(0, "STATIC", "Диски не найдены",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                6, nStartY, nBtnW, nBtnH,
                hWnd, (HMENU)(IDC_DRIVE_BTN_BASE), g_hInst, NULL);
            SendMessage(hPlaceholder, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            g_hDriveBtn[0] = hPlaceholder;
            return;
        }

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            int nY = nStartY + i * (nBtnH + DRIVE_BTN_GAP);
            g_hDriveBtn[i] = CreateWindowExU8(
                0, "LLHDDriveBtn", "",
                WS_CHILD | WS_VISIBLE,
                6, nY, nBtnW, nBtnH,
                hWnd, (HMENU)(UINT_PTR)(IDC_DRIVE_BTN_BASE + i), g_hInst, NULL
            );
            SetWindowLongPtrA(g_hDriveBtn[i], GWLP_USERDATA, (LONG_PTR)i);
        }
    } else {

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            if (g_hDriveBtn[i])
                InvalidateRect(g_hDriveBtn[i], NULL, FALSE);
        }
    }
}


void UpdateDriveInfo(HWND hWnd, int nDriveIdx)
{
    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SetDlgItemTextU8(hWnd, IDC_MODEL_STATIC,       "-");
        SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC,       "—");
        SetDlgItemTextU8(hWnd, IDC_CONTROLLER_STATIC,  "—");
        SetDlgItemTextU8(hWnd, IDC_NAND_STATIC,        "—");
        SetDlgItemTextU8(hWnd, IDC_SERIAL_STATIC,      "-");
        SetDlgItemTextU8(hWnd, IDC_FIRMWARE_STATIC,    "-");
        SetDlgItemTextU8(hWnd, IDC_SIZE_STATIC,        "-");
        SetDlgItemTextU8(hWnd, IDC_TEMP_STATIC,        "-");
        SetDlgItemTextU8(hWnd, IDC_POH_STATIC,         "-");
        SetDlgItemTextU8(hWnd, IDC_STATUS_STATIC,      "Нет данных");
        SetDlgItemTextU8(hWnd, IDC_PROTOCOL_STATIC,  "-");  /* protocol */
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC,     "—");

        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC,     "");
        SetDlgItemTextU8(hWnd, IDC_AXIS_STATIC,        "");
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];
    char szBuf[256];

    SetDlgItemTextU8(hWnd, IDC_MODEL_STATIC,
                    strlen(pInfo->szModel) ? pInfo->szModel : "-");

    {
        const char* brand = GetVendorName(pInfo->eVendor);
        if (pInfo->eVendor == VENDOR_UNKNOWN || pInfo->eVendor == VENDOR_OTHER)
            brand = "—";
        SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC, brand);
    }
    SetDlgItemTextU8(hWnd, IDC_CONTROLLER_STATIC, GetControllerName(pInfo->eController));
    SetDlgItemTextU8(hWnd, IDC_NAND_STATIC, GetNandName(pInfo->eNand));

    SetDlgItemTextU8(hWnd, IDC_SERIAL_STATIC,
                    strlen(pInfo->szSerial) ? pInfo->szSerial : "-");

    SetDlgItemTextU8(hWnd, IDC_FIRMWARE_STATIC,
                    strlen(pInfo->szFirmware) ? pInfo->szFirmware : "-");

    {
        char szSize[32];
        FormatSize(pInfo->dwCapacityMB, szSize, sizeof(szSize));
        safe_snprintf(szBuf, "%s   Тип: %s",
                  szSize, GetDriveTypeName(pInfo->eType));
        SetDlgItemTextU8(hWnd, IDC_SIZE_STATIC, szBuf);
    }

    if (pInfo->nTemperatureC > 0) {
        if (pInfo->eTempBand != TEMP_BAND_UNKNOWN)
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s",
                          pInfo->nTemperatureC,
                          GetTempBandName(pInfo->eTempBand, TRUE));
        else
            safe_snprintf(szBuf, "%d\xC2\xB0""C", pInfo->nTemperatureC);
    } else {
        safe_snprintf(szBuf, "-");
    }
    SetDlgItemTextU8(hWnd, IDC_TEMP_STATIC, szBuf);

    {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        SetDlgItemTextU8(hWnd, IDC_POH_STATIC, szPoh);
    }

    if (!pInfo->bSMART_Supported) {
        if (pInfo->bIsNVMe)
            safe_snprintf(szBuf, "NVMe Health Log ошибка (err %lu)",
                (unsigned long)pInfo->dwErrNvmeProtocol);
        else if (pInfo->bIsUSB) {
            if (IsLikelyUsbFlashDrive(pInfo))
                safe_snprintf(szBuf, "SMART недоступен (USB-флешка)");
            else
                safe_snprintf(szBuf, "SMART недоступен (USB-мост)");
        } else {
            safe_snprintf(szBuf, "Не поддерживается");
        }
    } else {
        FormatSmartHeadline(pInfo, szBuf, sizeof(szBuf));
    }
    SetDlgItemTextU8(hWnd, IDC_STATUS_STATIC, szBuf);

    SetDlgItemTextU8(hWnd, IDC_PROTOCOL_STATIC,
                    pInfo->szProtocol[0] ? pInfo->szProtocol : "-");

    if (pInfo->bIsUSB) {
        char szAdapter[64];
        FormatUsbAdapterName(pInfo, szAdapter, sizeof(szAdapter));
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC, szAdapter);
    } else {
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC, "—");
    }

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        char szBridge[80], szName[64];
        FormatUsbAdapterName(pInfo, szName, sizeof(szName));
        if (szName[0] && strcmp(szName, "—") != 0)
            safe_snprintf(szBridge, " [%s]", szName);
        else
            szBridge[0] = '\0';

        DWORD dwErr = pInfo->dwErrSat16;
        if (dwErr == 0) dwErr = pInfo->dwErrSat12;
        if (dwErr == 0) dwErr = pInfo->dwErrStorageProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrNvmeProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrLogSense;

        if (IsLikelyUsbFlashDrive(pInfo)) {
            if (dwErr != 0)
                safe_snprintf(szBuf,
                    "У этой USB-флешки SMART не отдаётся — так у большинства флешек. "
                    "Код ошибки: %lu.",
                    (unsigned long)dwErr);
            else
                safe_snprintf(szBuf,
                    "У этой USB-флешки SMART не отдаётся — так у большинства флешек.");
        } else {
            if (dwErr != 0)
                safe_snprintf(szBuf,
                    "USB-корпус/мост не отдаёт SMART%s. Код ошибки: %lu.",
                    szBridge, (unsigned long)dwErr);
            else
                safe_snprintf(szBuf,
                    "USB-корпус/мост не отдаёт SMART%s "
                    "(SAT/vendor passthrough недоступен).",
                    szBridge);
        }
        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szBuf);
    } else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        {
            char szErr[320];
            safe_snprintf(szErr,
                "NVMe Health Log IOCTL не удался (Win32 %lu). 5=нет прав, 1/50=не поддерживается, 87=плохой запрос.",
                (unsigned long)pInfo->dwErrNvmeProtocol);
            SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szErr);
        }
    } else if (!pInfo->bSMART_Supported) {
        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC,
            "Не удалось прочитать SMART. Запустите от имени администратора и нажмите «Обновить».");
    } else {
        /* SMART present: prompt to open the lecture. No NVMe field dump, no %. */
        {
            const char* szPred = "Нажмите на состояние, чтобы узнать подробности.";
            switch (pInfo->eHealthStatus) {
            case HEALTH_STATUS_GOOD:
                szPred = "Критических проблем не обнаружено.";
                break;
            case HEALTH_STATUS_OBSERVE:
                if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe)
                    szPred = "Повышенный механический риск. Повреждение поверхности не подтверждено.";
                else
                    szPred = "Есть факторы риска. Нажмите на состояние.";
                break;
            case HEALTH_STATUS_CAUTION:
                szPred = "Есть признаки деградации. Нажмите на состояние.";
                break;
            case HEALTH_STATUS_BAD:
            case HEALTH_STATUS_WARNING:
                szPred = "Обнаружены признаки деградации носителя. Сделайте резервную копию.";
                break;
            case HEALTH_STATUS_CRITICAL:
                szPred = "Высокий риск отказа. Немедленно копируйте данные.";
                break;
            default:
                szPred = "Недостаточно данных для достоверной оценки.";
                break;
            }
            SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szPred);
        }
    }

    {
        char szAxis[256];
        char szRow4Buf[48];
        const char* szTempAx;
        const char* szRow4Lbl;
        const char* szRow4Val;
        if (pInfo->nTemperatureC <= 0)
            szTempAx = "нет данных";
        else
            szTempAx = GetAxisStatusName(pInfo->eTempStatus, TRUE);
        if (pInfo->eType == DRIVE_TYPE_HDD || pInfo->nGSenseEvents >= 0) {
            szRow4Lbl = "Механика    ";
            if (pInfo->eMechanics == HEALTH_STATUS_GOOD)
                szRow4Val = "НОРМА";
            else if (pInfo->eMechanics == HEALTH_STATUS_UNKNOWN)
                szRow4Val = "нет данных";
            else
                szRow4Val = GetHealthStatusName(pInfo->eMechanics);
        } else {
            szRow4Lbl = "Ресурс      ";
            if (pInfo->eWear != HEALTH_STATUS_UNKNOWN && pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%s (%d%%)",
                    GetHealthStatusName(pInfo->eWear), pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else if (pInfo->eWear != HEALTH_STATUS_UNKNOWN) {
                szRow4Val = GetHealthStatusName(pInfo->eWear);
            } else if (pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%d%%", pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else {
                szRow4Val = "нет данных";
            }
        }
        safe_snprintf(szAxis,
            "Носитель     %s" "\r\n"
            "Интерфейс    %s" "\r\n"
            "Температура  %s" "\r\n"
            "%s%s",
            GetAxisStatusName(pInfo->eReliability, FALSE),
            GetAxisStatusName(pInfo->eInterface, FALSE),
            szTempAx, szRow4Lbl, szRow4Val);
        SetDlgItemTextU8(hWnd, IDC_AXIS_STATIC, szAxis);
    }

    RepaintHealthBar();
}

static void ListViewSetCellIfChanged(HWND hList, int iItem, int iSubItem, const char* pszNew)
{
    WCHAR wzOld[128];
    WCHAR wzNew[128];
    LVITEMW lvi;
    wzOld[0] = 0;
    U8ToW(pszNew ? pszNew : "", wzNew, 128);
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.mask       = LVIF_TEXT;
    lvi.iItem      = iItem;
    lvi.iSubItem   = iSubItem;
    lvi.pszText    = wzOld;
    lvi.cchTextMax = 128;
    SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi);
    if (wcscmp(wzOld, wzNew) != 0) {
        lvi.pszText = wzNew;
        SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
    }
}

typedef struct {
    char col[7][128];
} ATTR_ROW;

#define MAX_ATTR_ROWS 48

static void AttrRowSet(ATTR_ROW* r,
                       const char* id, const char* name,
                       const char* val, const char* worst, const char* thresh,
                       const char* raw, const char* stat)
{
    safe_snprintf(r->col[0], "%s", id     ? id     : "");
    safe_snprintf(r->col[1], "%s", name   ? name   : "");
    safe_snprintf(r->col[2], "%s", val    ? val    : "");
    safe_snprintf(r->col[3], "%s", worst  ? worst  : "");
    safe_snprintf(r->col[4], "%s", thresh ? thresh : "");
    safe_snprintf(r->col[5], "%s", raw    ? raw    : "");
    safe_snprintf(r->col[6], "%s", stat   ? stat   : "");
}

/* Status badge code stored in LVITEM.lParam. Low 8 bits = ATTRST_*.
 * Bit 8 (0x100) selects the alternate label: ПЛОХО vs СБОЙ, Жарко vs Внимание. */
enum {
    ATTRST_NONE = 0,
    ATTRST_OK,
    ATTRST_WARN,
    ATTRST_BAD,
    ATTRST_DIM,
    ATTRST_SKIP,
    ATTRST_INFO,
    ATTRST_RISK
};

static LPARAM AttrStatusParam(const char* s)
{
    if (!s || !s[0])
        return (LPARAM)ATTRST_NONE;
    if (strcmp(s, "СБОЙ") == 0)
        return (LPARAM)ATTRST_BAD;
    if (strcmp(s, "ПЛОХО") == 0)
        return (LPARAM)(ATTRST_BAD | 0x100);
    if (strcmp(s, "Внимание") == 0)
        return (LPARAM)ATTRST_WARN;
    if (strcmp(s, "Жарко") == 0)
        return (LPARAM)(ATTRST_WARN | 0x100);
    if (strcmp(s, "ОК") == 0)
        return (LPARAM)ATTRST_OK;
    if (strcmp(s, "--") == 0)
        return (LPARAM)ATTRST_DIM;
    if (strcmp(s, "Не оценивается") == 0)
        return (LPARAM)ATTRST_SKIP;
    if (strcmp(s, "INFO") == 0)
        return (LPARAM)ATTRST_INFO;
    if (strcmp(s, "Риск") == 0)
        return (LPARAM)ATTRST_RISK;
    return (LPARAM)ATTRST_NONE;
}

static BOOL IsAtaSsdType(DRIVE_TYPE t)
{
    return t == DRIVE_TYPE_SSD_SATA || t == DRIVE_TYPE_M2_SATA;
}

static BOOL VendorUsesE7AsLife(DRIVE_CONTROLLER c, DRIVE_TYPE t)
{
    (void)t;
    /* Only Phison is known-sure: E7 is SSD life, not temperature.
     * SMI is left alone — E7 meaning varies by SM225/SM226 firmware. */
    return c == CONTROLLER_PHISON;
}

static void FormatSmartValue(BYTE bID, BYTE* pRaw,
                              BYTE bVal, BYTE bWorst, BYTE bThresh,
                              const DRIVE_INFO* pDrv,
                              char* szBuf, int nBufLen)
{
    DRIVE_VENDOR eVendor = pDrv ? pDrv->eVendor : VENDOR_UNKNOWN;
    DRIVE_TYPE eType = pDrv ? pDrv->eType : DRIVE_TYPE_UNKNOWN;
    DRIVE_CONTROLLER eCtl = pDrv ? pDrv->eController : CONTROLLER_UNKNOWN;

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

    (void)bWorst;
    (void)bThresh;
    char szMain[80] = "";

    {
        ATTR_DECODE dec;
        GetAttrDecode(bID, pDrv, &dec);
        if (dec.eState == ATTR_DECODE_UNKNOWN) {
            if (pDrv && pDrv->eType == DRIVE_TYPE_HDD &&
                (bID == 0xB5 || bID == 0xB6 || bID == 0xAB || bID == 0xAC)) {
                unsigned hi = (unsigned)((dw32 >> 16) & 0xFFFFu);
                unsigned lo = (unsigned)(dw32 & 0xFFFFu);
                safe_snprintf(szMain, "%u / %u", hi, lo);
            } else if (qw48 > 0xFFFFFFFFULL)
                safe_snprintf(szMain, "%llu  (vendor-specific)",
                              (unsigned long long)qw48);
            else
                safe_snprintf(szMain, "%lu  (vendor-specific)",
                              (unsigned long)dw32);
            safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
            return;
        }
    }

    switch (bID)
    {

    case 0xBE:
    {
        int nC = (int)pRaw[0];
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax < 100)
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)  мин:%d макс:%d", nC, nF, nMin, nMax);
        else
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", nC, nF);
        break;
    }
    case 0xC2:
    {
        int nC = (int)w16;
        if (nC <= 0 || nC > 100) nC = (int)bVal;
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax < 100)
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)  мин:%d макс:%d", nC, nF, nMin, nMax);
        else
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", nC, nF);
        break;
    }
    case 0xE7:
    {
        int nLife = -1;
        if (bVal <= 100)
            nLife = (int)bVal;
        else if (dw32 <= 100)
            nLife = (int)dw32;
        else if (w16 <= 100)
            nLife = (int)w16;

        if (VendorUsesE7AsLife(eCtl, eType)) {
            if (nLife >= 0)
                safe_snprintf(szMain, "%d%% остаток ресурса", nLife);
            else
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            break;
        }
        if (eCtl == CONTROLLER_UNKNOWN &&
            IsAtaSsdType(eType)) {
            if (nLife >= 0 && nLife <= 100)
                safe_snprintf(szMain, "%d%% остаток ресурса", nLife);
            else if (bVal > 0 && bVal <= 100) {
                int nF = (int)bVal * 9 / 5 + 32;
                safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", (int)bVal, nF);
            } else {
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            }
            break;
        }
        if (bVal > 0 && bVal <= 100) {
            int nF = (int)bVal * 9 / 5 + 32;
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", (int)bVal, nF);
        } else {
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        }
        break;
    }

    case 0x09:
    {
        unsigned __int64 nHours = qw48;
        if (nHours > 200000) nHours = (unsigned __int64)w16;
        if (nHours > 0xFFFFFFFFULL) nHours = 0xFFFFFFFFULL;
        FormatPowerOnHours((DWORD)nHours, szMain, (int)sizeof(szMain));
        break;
    }

    case 0x04:
        safe_snprintf(szMain, "%lu раз", (unsigned long)dw32);
        break;

    case 0x0C:
        safe_snprintf(szMain, "%lu циклов", (unsigned long)dw32);
        break;

    case 0x03:
    {
        WORD wMs = w16;
        if (wMs > 0 && wMs < 30000)
            safe_snprintf(szMain, "%u ms", wMs);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0x0A:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu повторов  (!)", (unsigned long)dw32);
        break;

    case 0x05:
    {
        DWORD dwSec = dw32 & 0xFFFF;
        if (dwSec == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu секторов  (!)", (unsigned long)dwSec);
        break;
    }

    case 0xC4:
        if (dw32 == 0)
            safe_snprintf(szMain, "0 событий  (ОК)");
        else
            safe_snprintf(szMain, "%lu событий  (!)", (unsigned long)dw32);
        break;

    case 0xC5:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu нестабильных  (!)", (unsigned long)dw32);
        break;

    case 0xC6:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu неисправимых  (!)", (unsigned long)dw32);
        break;

    case 0xC7:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu ошибок CRC  (!)", (unsigned long)dw32);
        break;

    case 0xBB:
    case 0xC3:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu ошибок ECC", (unsigned long)dw32);
        break;

    case 0xBC:
    {
        WORD wTotal  = (WORD)pRaw[0] | ((WORD)pRaw[1] << 8);
        WORD wLatest = (WORD)pRaw[2] | ((WORD)pRaw[3] << 8);
        if (wTotal == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%u таймаутов  (посл.: %u)", wTotal, wLatest);
        break;
    }

    case 0xC1:
        safe_snprintf(szMain, "%lu циклов парковки", (unsigned long)dw32);
        break;

    case 0xC0:
        if (DriveTreatsC0AsPowerLoss(pDrv))
            safe_snprintf(szMain, "%lu событий", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu аварийных парковок", (unsigned long)dw32);
        break;

    case 0xB7:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu понижений  (!)", (unsigned long)dw32);
        break;

    case 0xB8:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu ошибок  (!)", (unsigned long)dw32);
        break;

    case 0x0E:
    case 0xBF:
    case 0xDD:
        if (dw32 == 0)
            safe_snprintf(szMain, "0 событий");
        else
            safe_snprintf(szMain, "%lu событий", (unsigned long)dw32);
        break;

    case 0xC8:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu  (значение важнее RAW)", (unsigned long)dw32);
        break;

    case 0x01:
    {
        if (eVendor == VENDOR_SEAGATE) {
            safe_snprintf(szMain, "%02X%02X%02X%02X%02X%02X  регистр частоты, мл. 16 бит: %u",
                          pRaw[5], pRaw[4], pRaw[3], pRaw[2], pRaw[1], pRaw[0],
                          (unsigned)w16);
            break;
        }
        DWORD dwErrHi = (((DWORD)pRaw[5] << 8) | (DWORD)pRaw[4]);
        if (dwErrHi > 0 && dw32 > 0 && dw32 < 0xFFFFFFFF)
            safe_snprintf(szMain, "%lu / %lu  (ош./всего)", (unsigned long)dwErrHi, (unsigned long)dw32);
        else if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0x07:
    {
        if (eVendor == VENDOR_SEAGATE) {
            safe_snprintf(szMain, "%02X%02X%02X%02X%02X%02X  регистр частоты, мл. 16 бит: %u",
                          pRaw[5], pRaw[4], pRaw[3], pRaw[2], pRaw[1], pRaw[0],
                          (unsigned)w16);
            break;
        }
        DWORD dwErrHi = (((DWORD)pRaw[5] << 8) | (DWORD)pRaw[4]);
        if (dwErrHi > 0 && dw32 > 0)
            safe_snprintf(szMain, "%lu / %lu  (ош./позиц.)", (unsigned long)dwErrHi, (unsigned long)dw32);
        else if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
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
            safe_snprintf(szMain, "%llu LBA  (~%llu TB)", (unsigned long long)nLBA, (unsigned long long)(nGB / 1024ULL));
        else if (nGB > 0)
            safe_snprintf(szMain, "%llu LBA  (~%llu GB)", (unsigned long long)nLBA, (unsigned long long)nGB);
        else
            safe_snprintf(szMain, "%llu LBA", (unsigned long long)nLBA);
        break;
    }

    case 0xF9:
    {
        unsigned __int64 nGiB = qw48;
        if (nGiB > 0 && nGiB < 1000000ULL)
            safe_snprintf(szMain, "%llu ГиБ записано", (unsigned long long)nGiB);
        else
            safe_snprintf(szMain, "%llu", (unsigned long long)nGiB);
        break;
    }

    case 0xE9:
    {
        if (eCtl == CONTROLLER_INTEL && bVal <= 100) {
            safe_snprintf(szMain, "%u%%  индикатор износа", (unsigned)bVal);
            break;
        }
        unsigned __int64 nGiB = qw48;
        if (nGiB > 0 && nGiB < 1000000ULL)
            safe_snprintf(szMain, "%llu ГиБ записано", (unsigned long long)nGiB);
        else
            safe_snprintf(szMain, "%llu", (unsigned long long)nGiB);
        break;
    }

    case 0xA9:
    {
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        safe_snprintf(szMain, "%lu%%  заявленный остаток ресурса", (unsigned long)pct);
        break;
    }

    case 0xB1:
        if (eCtl == CONTROLLER_SAMSUNG && bVal <= 100) {
            safe_snprintf(szMain, "износ/остаток %u%%", (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xCA:
        if (eCtl == CONTROLLER_MICRON && bVal <= 100) {
            safe_snprintf(szMain, "%u%% ресурса", (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xAD:
        if (eCtl == CONTROLLER_MICRON)
            safe_snprintf(szMain, "%lu среднее стираний", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu циклов выравнивания", (unsigned long)dw32);
        break;

    case 0xA1:
    case 0xB2:
        safe_snprintf(szMain, "%lu блоков", (unsigned long)dw32);
        break;

    case 0xAB:
    case 0xAC:
    case 0xB5:
    case 0xB6:
        if (pDrv && pDrv->eType == DRIVE_TYPE_HDD) {
            unsigned hi = (unsigned)((dw32 >> 16) & 0xFFFFu);
            unsigned lo = (unsigned)(dw32 & 0xFFFFu);
            safe_snprintf(szMain, "%u / %u", hi, lo);
        } else if (dw32 == 0)
            safe_snprintf(szMain, "0 сбоев  (ОК)");
        else
            safe_snprintf(szMain, "%lu сбоев  (!)", (unsigned long)dw32);
        break;

    case 0xAA:
    {
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        if (eCtl == CONTROLLER_INTEL && bVal <= 100)
            pct = (DWORD)bVal;
        safe_snprintf(szMain, "%lu%%  резервное пространство", (unsigned long)pct);
        break;
    }

    case 0xE8:
    {
        if (eCtl == CONTROLLER_INTEL && bVal <= 100) {
            safe_snprintf(szMain, "%u%%  резервное пространство", (unsigned)bVal);
            break;
        }
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        safe_snprintf(szMain, "%lu%%  резервное пространство", (unsigned long)pct);
        break;
    }

    case 0xF0:
        safe_snprintf(szMain, "%lu ч полёта головок", (unsigned long)(DWORD)w16);
        break;

    case 0xE1:
        if (eCtl == CONTROLLER_INTEL) {
            unsigned __int64 nGiB = ((unsigned __int64)dw32 * 32ULL) / 1024ULL;
            if (nGiB >= 1024ULL)
                safe_snprintf(szMain, "%lu единиц (32 MiB)  (~%llu TB)",
                              (unsigned long)dw32, (unsigned long long)(nGiB / 1024ULL));
            else
                safe_snprintf(szMain, "%lu единиц (32 MiB)  (~%llu GiB)",
                              (unsigned long)dw32, (unsigned long long)nGiB);
            break;
        }
        safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xE2:
        if (eCtl == CONTROLLER_INTEL)
            safe_snprintf(szMain, "%lu ч нагрузки", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xF6:
        if (eCtl == CONTROLLER_MICRON) {
            unsigned __int64 nLBA = qw48;
            unsigned __int64 nGB  = nLBA / (1024ULL * 1024ULL * 2ULL);
            if (nGB >= 1024)
                safe_snprintf(szMain, "%llu LBA  (~%llu TB)", (unsigned long long)nLBA, (unsigned long long)(nGB / 1024ULL));
            else if (nGB > 0)
                safe_snprintf(szMain, "%llu LBA  (~%llu GB)", (unsigned long long)nLBA, (unsigned long long)nGB);
            else
                safe_snprintf(szMain, "%llu LBA", (unsigned long long)nLBA);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    default:
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else if (dw32 == 0)
            safe_snprintf(szMain, "0");
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
}

static DWORD AttrRaw32(const SMART_ATTRIBUTE* p)
{
    return (DWORD)p->bRawValue[0]
         | ((DWORD)p->bRawValue[1] << 8)
         | ((DWORD)p->bRawValue[2] << 16)
         | ((DWORD)p->bRawValue[3] << 24);
}

/* Status from RAW counters and collapsed Value, not from Value==100. */
static const char* AtaRowStatus(const DRIVE_INFO* p, const SMART_ATTRIBUTE* a,
                                BYTE bThresh, BOOL bFailed)
{
    BYTE id = a->bAttrID;
    BYTE val = a->bAttrValue;
    BYTE worst = a->bWorstValue;
    BOOL ssd = p->bIsNVMe || p->eType == DRIVE_TYPE_SSD_SATA ||
               p->eType == DRIVE_TYPE_M2_SATA;
    int n = -1;
    DWORD raw;

    if (bFailed)
        return "СБОЙ";

    if (ssd && (id == 0xE7 || id == 0xA9)) {
        int nLeft = p->nEndurancePercent;
        if (nLeft >= 0 && nLeft <= 5) return "ПЛОХО";
        if (nLeft >= 0 && nLeft <= 10) return "Внимание";
        if (nLeft >= 0 && nLeft <= 20) return "Риск";
        return "ОК";
    }

    if (id == 0x05) n = p->nReallocated;
    else if (id == 0xC4) n = p->nRemapEvents;
    else if (id == 0xC5) n = p->nPendingSectors;
    else if (id == 0xC6) n = p->nUncorrectable;
    else if (id == 0xC7) n = p->nCrcErrors;
    if (n > 0) {
        if (id == 0xC6 || n >= 10 || (id == 0xC5 && n >= 4))
            return "ПЛОХО";
        return "Внимание";
    }

    if (id == 0xBB) {
        raw = AttrRaw32(a);
        if (val <= 1 && (raw > 0 || val == 1)) return "ПЛОХО";
        if (val <= 10 || raw > 0) return "Внимание";
        return "ОК";
    }

    if (id == 0xC3 && !ssd) {
        if (val <= 10) return "ПЛОХО";
        if (val < 70 || worst < 50) return "Внимание";
        return "ОК";
    }

    if (id == 0xC8 || id == 0x01 || id == 0x07) {
        if (val > 0 && val <= 1) return "СБОЙ";
        if (val > 0 && val <= 10) return "Внимание";
        if (worst > 0 && worst < 70) return "Внимание";
        return "ОК";
    }

    if (id == 0xC0 && !ssd && DriveTreatsC0AsPowerLoss(p))
        return "INFO";

    if (!ssd && IsShockSensorAttr(id)) {
        if (p->eMechanics == HEALTH_STATUS_CAUTION ||
            p->eMechanics == HEALTH_STATUS_BAD)
            return "Внимание";
        if (p->eMechanics == HEALTH_STATUS_OBSERVE)
            return "Риск";
        return "ОК";
    }

    if ((id == 0xC2 || id == 0xBE) && p->nTemperatureC > 70)
        return "Жарко";

    if (bThresh > 0 && val < bThresh + 10)
        return "Внимание";
    return "ОК";
}

static void FormatSmartRaw(BYTE bID, BYTE* pRaw,
                           BYTE bVal, BYTE bWorst, BYTE bThresh,
                           const DRIVE_INFO* pDrv,
                           char* szBuf, int nBufLen)
{
    char szDec[80];
    FormatSmartValue(bID, pRaw, bVal, bWorst, bThresh,
                     pDrv, szDec, sizeof(szDec));
    safe_snprintf_n(szBuf, nBufLen, "%02X%02X%02X%02X%02X%02X (%s)",
              pRaw[5], pRaw[4], pRaw[3], pRaw[2], pRaw[1], pRaw[0], szDec);
}

void UpdateAttrList(HWND hWnd, int nDriveIdx)
{
    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);

    ATTR_ROW rows[MAX_ATTR_ROWS];
    int      nDesired = 0;
    ZeroMemory(rows, sizeof(rows));

    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SendMessage(hList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(hList);
        SendMessage(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, NULL, TRUE);
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        const char* msg = IsLikelyUsbFlashDrive(pInfo)
            ? "У этой USB-флешки SMART не отдаётся — так у большинства флешек."
            : "USB-корпус/мост не отдаёт SMART";
        AttrRowSet(&rows[0], "--", msg, "", "—", "—", "", "");
        nDesired = 1;
    }
    else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        char szErr[64];
        safe_snprintf(szErr, "Win32 %lu", (unsigned long)pInfo->dwErrNvmeProtocol);
        AttrRowSet(&rows[0], "--", "NVMe Health Log IOCTL не удался",
                   szErr, "—", "—", "", "");
        AttrRowSet(&rows[1], "--", "5=нет прав  1/50=нет в драйвере  87=параметр",
                   "", "—", "—", "", "");
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
            if (nDesired < MAX_ATTR_ROWS) \
                AttrRowSet(&rows[nDesired++], (id_), (name_), (val_), "—", "—", "", (stat_)); \
        }

        char szCrit[64], szVBuf[64], szSpare[32], szSpTh[32], szPctU[32];
        char szDUR[64], szDUW[64], szPOH[64], szPC[32];
        char szUS[32], szME[32], szEL[32], szWCT[32], szCCT[32];

        if (pLog->CriticalWarning == 0) safe_snprintf(szCrit, "0 (нет)");
        else safe_snprintf(szCrit, "0x%02X (!)", pLog->CriticalWarning);
        NVME_ROW("01h","Критическое предупреждение",szCrit,(pLog->CriticalWarning?"ПЛОХО":"ОК"));

        safe_snprintf(szVBuf,"%d C (%d K)",nTempC,(int)wTempK);
        NVME_ROW("02h","Температура",szVBuf,(nTempC>70?"Жарко":"ОК"));

        safe_snprintf(szSpare,"%d %%",(int)pLog->AvailableSpare);
        NVME_ROW("03h","Запас блоков",szSpare,
            (pLog->AvailableSpare<pLog->AvailableSpareThreshold?"ПЛОХО":"ОК"));

        safe_snprintf(szSpTh,"%d %%",(int)pLog->AvailableSpareThreshold);
        NVME_ROW("04h","Порог запаса блоков",szSpTh,"ОК");

        safe_snprintf(szPctU,"%d %%",(int)pLog->PercentageUsed);
        {
            const char* szWearSt = "ОК";
            int nLeft = 100 - (int)pLog->PercentageUsed;
            if (nLeft < 0) nLeft = 0;
            if (pLog->PercentageUsed >= 100 || nLeft <= 5) szWearSt = "ПЛОХО";
            else if (nLeft <= 10) szWearSt = "Внимание";
            else if (nLeft <= 20) szWearSt = "Риск";
            NVME_ROW("05h","Износ",szPctU,szWearSt);
        }

        FormatNvmeHostBytes(qwDataRead, szDUR, sizeof(szDUR));
        NVME_ROW("06h","Прочитано (host)",szDUR,"ОК");

        FormatNvmeHostBytes(qwDataWritten, szDUW, sizeof(szDUW));
        NVME_ROW("07h","Записано (host)",szDUW,"ОК");

        {
            DWORD dwPoh = (qwPOH > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (DWORD)qwPOH;
            FormatPowerOnHours(dwPoh, szPOH, (int)sizeof(szPOH));
        }
        NVME_ROW("09h","Наработка",szPOH,"ОК");

        safe_snprintf(szPC,"%llu",(unsigned long long)qwPowerCycles);
        NVME_ROW("0Ch","Циклы включения",szPC,"ОК");

        safe_snprintf(szUS,"%llu",(unsigned long long)qwUnsafeSDs);
        NVME_ROW("10h","Аварийные выключения",szUS,"ОК");

        safe_snprintf(szME,"%llu",(unsigned long long)qwMediaErr);
        NVME_ROW("11h","Ошибки носителя",szME,(qwMediaErr>0?"ПЛОХО":"ОК"));

        safe_snprintf(szEL,"%llu",(unsigned long long)qwErrLog);
        NVME_ROW("12h","Записи в журнале ошибок",szEL,(qwErrLog>0?"Внимание":"ОК"));

        safe_snprintf(szWCT,"%lu мин",(unsigned long)pLog->WarningCompTempTime);
        NVME_ROW("13h","Время при высокой температуре",szWCT,(pLog->WarningCompTempTime>0?"Внимание":"ОК"));

        safe_snprintf(szCCT,"%lu мин",(unsigned long)pLog->CriticalCompTempTime);
        NVME_ROW("14h","Время при критической температуре",szCCT,(pLog->CriticalCompTempTime>0?"ПЛОХО":"ОК"));

        {
            char szHR[64], szHW[64], szBT[64];
            safe_snprintf(szHR, "%llu", (unsigned long long)pInfo->qwNVMeHostReads);
            NVME_ROW("--", "Команды чтения хоста", szHR, "ОК");
            safe_snprintf(szHW, "%llu", (unsigned long long)pInfo->qwNVMeHostWrites);
            NVME_ROW("--", "Команды записи хоста", szHW, "ОК");
            safe_snprintf(szBT, "%llu мин", (unsigned long long)pInfo->qwNVMeControllerBusyTime);
            NVME_ROW("--", "Занятость контроллера", szBT, "ОК");
        }
        {
            int ts;
            for (ts = 0; ts < 8; ts++) {
                if (pInfo->nTempSensor[ts] >= 0) {
                    char szId[8], szName[48], szVal[32];
                    safe_snprintf(szId, "T%d", ts + 1);
                    safe_snprintf(szName, "Датчик температуры %d", ts + 1);
                    safe_snprintf(szVal, "%d C", pInfo->nTempSensor[ts]);
                    NVME_ROW(szId, szName, szVal,
                             (pInfo->nTempSensor[ts] > 70 ? "Жарко" : "ОК"));
                }
            }
        }
        if (pLog->ThermalMgmtTemp1TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp1TransCnt);
            NVME_ROW("--", "Thermal Mgmt T1 переходов", sz, "ОК");
        }
        if (pLog->ThermalMgmtTemp2TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp2TransCnt);
            NVME_ROW("--", "Thermal Mgmt T2 переходов", sz, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp1) {
            char sz[32];
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp1);
            NVME_ROW("--", "Thermal Mgmt T1 время", sz, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp2) {
            char sz[32];
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp2);
            NVME_ROW("--", "Thermal Mgmt T2 время", sz, "ОК");
        }

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
            const char* szStat;
            char szVal[16], szWorst[16], szThresh[16], szRaw[128];

            safe_snprintf(szVal, "%u", (unsigned)pAttr->bAttrValue);
            safe_snprintf(szWorst, "%u", (unsigned)pAttr->bWorstValue);
            if (bThresh)
                safe_snprintf(szThresh, "%u", (unsigned)bThresh);
            else
                safe_snprintf(szThresh, "—");
            FormatSmartRaw(pAttr->bAttrID, pAttr->bRawValue,
                           pAttr->bAttrValue, pAttr->bWorstValue, bThresh,
                           pInfo,
                           szRaw, 128);
            szStat = AtaRowStatus(pInfo, pAttr, bThresh, bFailed);
            {
                char szId[8];
                safe_snprintf(szId, "%d", pAttr->bAttrID);
                AttrRowSet(&rows[nDesired++], szId, GetAttrNameEx(pAttr->bAttrID, pInfo),
                           szVal, szWorst, szThresh, szRaw, szStat);
            }
        }

        if (nDesired == 0) {
            /* Bridge gave us only SCSI LOG SENSE data (no ATA attribute
             * table). Show what was retrieved instead of an empty list. */
            AttrRowSet(&rows[nDesired++], "--", "Прогноз отказа (SCSI)",
                       pInfo->bPredictFailure ? "Отказ прогнозируется" : "Отказ не прогнозируется",
                       "—", "—", "",
                       pInfo->bPredictFailure ? "СБОЙ" : "ОК");

            if (nDesired < MAX_ATTR_ROWS) {
                char szT[32];
                if (pInfo->nTemperatureC > 0)
                    safe_snprintf(szT, "%d C", pInfo->nTemperatureC);
                else
                    safe_snprintf(szT, "Нет данных");
                AttrRowSet(&rows[nDesired++], "--", "Температура (SCSI Log Sense)",
                           szT, "—", "—", "",
                           (pInfo->nTemperatureC > 70 ? "Жарко" : "ОК"));
            }

            if (nDesired < MAX_ATTR_ROWS) {
                AttrRowSet(&rows[nDesired++], "--",
                           "Полная таблица SMART мостом не отдаётся",
                           "", "—", "—", "", "");
            }
        }

        /* Extra ATA info already collected in AcquireATASMART — display only. */
        if (nDesired < MAX_ATTR_ROWS && pInfo->wRotationRate >= 0x0401) {
            char sz[32];
            safe_snprintf(sz, "%u об/мин", (unsigned)pInfo->wRotationRate);
            AttrRowSet(&rows[nDesired++], "--", "Обороты", sz, "—", "—", "", "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotErrorLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nErrorLogCount);
            AttrRowSet(&rows[nDesired++], "--", "Журнал ошибок SMART", sz, "—", "—", "",
                       pInfo->nErrorLogCount > 0 ? "Внимание" : "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotSelfTestLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nSelfTestStatus);
            AttrRowSet(&rows[nDesired++], "--", "Журнал самопроверки", sz, "—", "—", "", "ОК");
        }
    }

    SendMessage(hList, WM_SETREDRAW, FALSE, 0);

    int nCurrent = ListView_GetItemCount(hList);

    int row;
    for (row = 0; row < nDesired; row++) {
        LPARAM lpStat = AttrStatusParam(rows[row].col[6]);
        if (row >= nCurrent) {
            WCHAR wz[128];
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            U8ToW(rows[row].col[0], wz, 128);
            lvi.mask     = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem    = row;
            lvi.iSubItem = 0;
            lvi.pszText  = wz;
            lvi.lParam   = lpStat;
            SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
        } else {
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask     = LVIF_PARAM;
            lvi.iItem    = row;
            lvi.iSubItem = 0;
            lvi.lParam   = lpStat;
            SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
        }
        int col;
        for (col = 0; col < 7; col++)
            ListViewSetCellIfChanged(hList, row, col, rows[row].col[col]);
    }

    while (ListView_GetItemCount(hList) > nDesired)
        ListView_DeleteItem(hList, ListView_GetItemCount(hList) - 1);

    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, FALSE);
    UpdateWindow(hList);
}

static volatile LONG g_bScanBusy = 0;

/* Private scan buffer: the worker thread writes ONLY these. The UI thread
 * copies into g_Drives / g_nDriveCount on WM_APP_REFRESH_DONE so paint
 * never races with ScanDrives' ZeroMemory of live slots. */
static DRIVE_INFO g_ScanBuf[MAX_DRIVES];
static int        g_nScanBufCount;

static void ScanDrivesToBuf(void)
{
    int n = ScanDrives(g_ScanBuf, MAX_DRIVES);
    g_nScanBufCount = n;
}

static DWORD WINAPI RefreshThreadProc(LPVOID lpParam)
{
    HWND hWnd = (HWND)lpParam;
    ScanDrivesToBuf();
    PostMessage(hWnd, WM_APP_REFRESH_DONE, 0, 0);
    return 0;
}

void RefreshData(HWND hWnd)
{
    HWND hReread = GetDlgItem(hWnd, IDC_REREAD_BTN);
    HWND hReport = GetDlgItem(hWnd, IDC_REPORT_BTN);
    if (InterlockedCompareExchange(&g_bScanBusy, 1, 0) != 0)
        return;
    if (hReread) EnableWindow(hReread, FALSE);
    if (hReport) EnableWindow(hReport, FALSE);

    HANDLE hThread = CreateThread(NULL, 0, RefreshThreadProc, hWnd, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
    else {
        /* CreateThread failed: scan on UI thread into the private buffer,
         * then the same merge path via WM_APP_REFRESH_DONE. */
        RefreshThreadProc(hWnd);
    }
}

#define ABOUT_W  440
#define ABOUT_H  360
#define LECTURE_W 640
#define LECTURE_H 640
#define IDC_LECTURE_TEXT      3600
#define PROP_LECTURE_FONT     "dmLecF"

/* Control identifiers used inside the About dialog. */
#define IDC_ABOUT_LINK       3500
#define IDC_ABOUT_LIC_STATUS 3501
#define IDC_ABOUT_ACTIVATE   3502

#define PROP_ABOUT_FONT_BOLD  "dmFntB"
#define PROP_ABOUT_FONT_LINK  "dmFntL"

static HCURSOR s_hCursorHand = NULL;

BOOL OpenDonatePage(HWND hParent)
{
    HINSTANCE hResult = ShellExecuteA(
        hParent,
        "open",
        DONATE_URL,
        NULL, NULL,
        SW_SHOWNORMAL);

    if ((INT_PTR)hResult <= 32) {
        MessageBoxU8(hParent,
            "Не удалось открыть браузер.\n" DONATE_URL,
            "DriveMonitor",
            MB_ICONWARNING | MB_OK);
        return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK AboutDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            int cx = ABOUT_W;

            HWND hIco = CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                (cx - 32) / 2, 18, 32, 32,
                hDlg, (HMENU)0, g_hInst, NULL);
            HICON hIc = (HICON)LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
            if (hIc) SendMessageA(hIco, STM_SETICON, (WPARAM)hIc, 0);

            HFONT hFontBold = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SetPropA(hDlg, PROP_ABOUT_FONT_BOLD, (HANDLE)hFontBold);
            HWND hName = CreateWindowExU8(0, "STATIC", "DriveMonitor 1.5.1-beta",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 58, cx - 40, 22,
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hName, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            HWND hDesc = CreateWindowExU8(0, "STATIC",
                "Мониторинг состояния дисков и S.M.A.R.T. на низком уровне.",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 82, cx - 40, 18,
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hDesc, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 108, cx - 40, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            HWND hCopy = CreateWindowExU8(0, "STATIC",
                "\xC2\xA9 2026 chuikoff",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 118, cx - 40, 18,
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND hDrv = CreateWindowExU8(0, "STATIC",
                "Автор: chuikoff — MIT License",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                20, 140, cx - 40, 18,
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hDrv, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 166, cx - 40, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            {
                HWND hLicStatus = CreateWindowExU8(0, "STATIC",
                    "Свободное ПО с открытым исходным кодом",
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    20, 176, cx - 40, 18,
                    hDlg, (HMENU)IDC_ABOUT_LIC_STATUS, g_hInst, NULL);
                SendMessageA(hLicStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

                HWND hActivate = CreateWindowExU8(0, "BUTTON", "Поддержать",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    (cx - 120) / 2, 200, 120, 26,
                    hDlg, (HMENU)IDC_ABOUT_ACTIVATE, g_hInst, NULL);
                SendMessageA(hActivate, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            }

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                20, 234, cx - 40, 2,
                hDlg, (HMENU)0, g_hInst, NULL);

            HFONT hFontLink = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SetPropA(hDlg, PROP_ABOUT_FONT_LINK, (HANDLE)hFontLink);
            HWND hLink = CreateWindowExU8(0, "STATIC",
                DONATE_URL,
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                20, 244, cx - 40, 18,
                hDlg, (HMENU)IDC_ABOUT_LINK, g_hInst, NULL);
            SendMessageA(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            HWND hBtn = CreateWindowExU8(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                (cx - 80) / 2, 274, 80, 26,
                hDlg, (HMENU)IDOK, g_hInst, NULL);
            SendMessageA(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            if (!s_hCursorHand)
                s_hCursorHand = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            DestroyWindow(hDlg);
        }
        else if (LOWORD(wParam) == IDC_ABOUT_ACTIVATE) {
            OpenDonatePage(hDlg);
        }
        else if (LOWORD(wParam) == IDC_ABOUT_LINK &&
                 HIWORD(wParam) == STN_CLICKED) {
            OpenDonatePage(hDlg);
        }
        return 0;

    case WM_NOTIFY:
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND hCtrl = (HWND)lParam;
            int  nID   = GetDlgCtrlID(hCtrl);
            HDC  hdcSt = (HDC)wParam;
            SetBkMode(hdcSt, TRANSPARENT);
            if (nID == IDC_ABOUT_LINK) {
                /* Boosty link - render in hyperlink blue. */
                SetTextColor(hdcSt, RGB(0, 102, 204));
            } else if (nID == IDC_ABOUT_LIC_STATUS) {
                /* FOSS banner - render in green to signal "free / good". */
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

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        {
            HFONT hBold = (HFONT)GetPropA(hDlg, PROP_ABOUT_FONT_BOLD);
            HFONT hLink = (HFONT)GetPropA(hDlg, PROP_ABOUT_FONT_LINK);
            if (hBold) { DeleteObject(hBold); RemovePropA(hDlg, PROP_ABOUT_FONT_BOLD); }
            if (hLink) { DeleteObject(hLink); RemovePropA(hDlg, PROP_ABOUT_FONT_LINK); }
        }
        return 0;
    }

    return DefWindowProc(hDlg, uMsg, wParam, lParam);
}

void ShowAboutDialog(HWND hWndParent)
{

    static BOOL bRegistered = FALSE;
    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = AboutDlgProc;
        wc.hInstance     = g_hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"LLHDAboutDlg";
        RegisterClassW(&wc);
        bRegistered = TRUE;
    }

    HWND hExist = FindWindowA("LLHDAboutDlg", NULL);
    if (hExist) { SetForegroundWindow(hExist); return; }

    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);

    int nX, nY;
    if (hWndParent) {
        RECT rcP;
        GetWindowRect(hWndParent, &rcP);
        nX = rcP.left + (rcP.right  - rcP.left - ABOUT_W) / 2;
        nY = rcP.top  + (rcP.bottom - rcP.top  - ABOUT_H) / 2;
    } else {
        nX = (nScrW - ABOUT_W) / 2;
        nY = (nScrH - ABOUT_H) / 2;
    }

    RECT rcAdj = {0, 0, ABOUT_W, ABOUT_H};
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    int nWinW = rcAdj.right  - rcAdj.left;
    int nWinH = rcAdj.bottom - rcAdj.top;

    HWND hDlg = CreateWindowExU8(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "LLHDAboutDlg",
        "О программе — DriveMonitor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hWndParent, NULL, g_hInst, NULL
    );
    if (!hDlg) return;

    HICON hIco = LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON));
    SendMessageA(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hIco);
    SendMessageA(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)
        LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static LRESULT CALLBACK HealthLectureDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            const DRIVE_INFO* pInfo = cs ? (const DRIVE_INFO*)cs->lpCreateParams : NULL;
            RECT rc;
            int cx, cy, btnW, btnH, margin, editH;
            HFONT hFont;
            HWND hEdit, hBtn;
            char szText[16384];
            WCHAR wz[16384];

            GetClientRect(hDlg, &rc);
            cx = rc.right - rc.left;
            cy = rc.bottom - rc.top;
            if (cx < 200) cx = LECTURE_W;
            if (cy < 200) cy = LECTURE_H;
            btnW = 88;
            btnH = 26;
            margin = 12;
            editH = cy - margin * 3 - btnH;
            if (editH < 80) editH = 80;

            hFont = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
            if (!hFont)
                hFont = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
            if (!hFont)
                hFont = g_hFontNormal;
            SetPropA(hDlg, PROP_LECTURE_FONT, (HANDLE)hFont);

            hEdit = CreateWindowExU8(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                margin, margin, cx - 2 * margin, editH,
                hDlg, (HMENU)IDC_LECTURE_TEXT, g_hInst, NULL);
            if (hFont)
                SendMessageA(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            FormatHealthLecture(pInfo, szText, (int)sizeof(szText));
            U8ToW(szText, wz, 16384);
            SetWindowTextW(hEdit, wz);

            hBtn = CreateWindowExU8(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                (cx - btnW) / 2, cy - margin - btnH, btnW, btnH,
                hDlg, (HMENU)IDOK, g_hInst, NULL);
            if (hFont)
                SendMessageA(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetFocus(hBtn);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            DestroyWindow(hDlg);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            DestroyWindow(hDlg);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        {
            HFONT hFont = (HFONT)GetPropA(hDlg, PROP_LECTURE_FONT);
            if (hFont && hFont != g_hFontNormal) {
                DeleteObject(hFont);
            }
            RemovePropA(hDlg, PROP_LECTURE_FONT);
        }
        return 0;
    }

    return DefWindowProc(hDlg, uMsg, wParam, lParam);
}

void ShowHealthLectureDialog(HWND hParent)
{
    static BOOL bRegistered = FALSE;
    const DRIVE_INFO* pInfo = NULL;
    int nX, nY, nWinW, nWinH;
    RECT rcAdj;
    HWND hDlg, hExist;

    if (g_nDriveCount <= 0 || g_nSelectedDrive < 0 ||
        g_nSelectedDrive >= g_nDriveCount) {
        MessageBoxU8(hParent, "Нет выбранного диска", "Почему такая оценка",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    pInfo = &g_Drives[g_nSelectedDrive];

    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = HealthLectureDlgProc;
        wc.hInstance     = g_hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"LLHDHealthLecture";
        RegisterClassW(&wc);
        bRegistered = TRUE;
    }

    hExist = FindWindowA("LLHDHealthLecture", NULL);
    if (hExist)
        DestroyWindow(hExist);

    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        nX = rcP.left + (rcP.right  - rcP.left - LECTURE_W) / 2;
        nY = rcP.top  + (rcP.bottom - rcP.top  - LECTURE_H) / 2;
    } else {
        nX = (GetSystemMetrics(SM_CXSCREEN) - LECTURE_W) / 2;
        nY = (GetSystemMetrics(SM_CYSCREEN) - LECTURE_H) / 2;
    }

    rcAdj.left = 0;
    rcAdj.top = 0;
    rcAdj.right = LECTURE_W;
    rcAdj.bottom = LECTURE_H;
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    nWinW = rcAdj.right  - rcAdj.left;
    nWinH = rcAdj.bottom - rcAdj.top;

    hDlg = CreateWindowExU8(
        WS_EX_DLGMODALFRAME,
        "LLHDHealthLecture",
        "Почему такая оценка",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hParent, NULL, g_hInst, (LPVOID)pInfo
    );
    if (!hDlg) return;

    SendMessageA(hDlg, WM_SETICON, ICON_BIG, (LPARAM)
        LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON)));
    SendMessageA(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)
        LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void CreateMenuBar(HWND hWnd)
{
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuU8(hFile, MF_STRING, IDM_REPORT,     "Сохранить отчёт");
    AppendMenuU8(hFile, MF_STRING, IDM_SCREENSHOT, "Сохранить снимок\tCtrl+S");
    AppendMenuU8(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(hFile, MF_STRING, IDM_EXIT,       "Выход");
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)hFile, "Файл");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuU8(hHelp, MF_STRING, IDM_DONATE, "Поддержать...");
    AppendMenuU8(hHelp, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(hHelp, MF_STRING, IDM_ABOUT, "О программе");
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "Справка");

    SetMenu(hWnd, hMenuBar);
}

void CreateControls(HWND hWnd)
{
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    int i;
    for (i = 0; i < MAX_DRIVES; i++) g_hDriveBtn[i] = NULL;

    HWND hDriveLabel = CreateWindowExU8(0, "STATIC", "ДИСКИ",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        6, 8, DRIVE_BTN_PANEL_W - 12, 16,
        hWnd, (HMENU)(IDC_DRIVE_LIST), g_hInst, NULL);
    SendMessage(hDriveLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    int nRightX = DRIVE_BTN_PANEL_W + 10;
    int nBarsW  = 190;

    HWND hLabel = CreateWindowExU8(0, "STATIC", "СОСТОЯНИЕ ДИСКА",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        nRightX, 40, nBarsW, 14,
        hWnd, (HMENU)IDC_HEALTH_LABEL, g_hInst, NULL);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    g_hHealthBar = CreateWindowExU8(WS_EX_CLIENTEDGE, "LLHDHealthBar", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        nRightX, 56, nBarsW, 40,
        hWnd, (HMENU)IDC_HEALTH_BAR_FRAME, g_hInst, NULL);

    { HWND h = CreateWindowExU8(0, "BUTTON", "Перечитать",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX, 102, 90, 24,
        hWnd, (HMENU)IDC_REREAD_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", "Отчёт",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX + 100, 102, 90, 24,
        hWnd, (HMENU)IDC_REPORT_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }

    {
        HWND hAxis = CreateWindowExU8(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            nRightX, 132, nBarsW, 72,
            hWnd, (HMENU)IDC_AXIS_STATIC, g_hInst, NULL);
        SendMessage(hAxis, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    int nInfoX   = nRightX + nBarsW + 10;
    int nInfoY   = 36;
    int nInfoH   = 16;   /* tighter so 12 rows still fit above the list */
    int nInfoGap = 2;
    int nLblW    = 100;
    int nValX    = nInfoX + nLblW + 4;
    int nValW    = WINDOW_W - nValX - 8;

    {
        static const struct { int idLbl; int idVal; const char* lbl; const char* val; } rows[] = {
            { IDC_MODEL_LABEL,      IDC_MODEL_STATIC,      "Модель",      "-" },
            { IDC_BRAND_LABEL,      IDC_BRAND_STATIC,      "Бренд",       "—" },
            { IDC_CONTROLLER_LABEL, IDC_CONTROLLER_STATIC, "Контроллер",  "—" },
            { IDC_NAND_LABEL,       IDC_NAND_STATIC,       "NAND",        "—" },
            { IDC_SERIAL_LABEL,     IDC_SERIAL_STATIC,     "Серийный №",  "-" },
            { IDC_FIRMWARE_LABEL,   IDC_FIRMWARE_STATIC,   "Прошивка",    "-" },
            { IDC_SIZE_LABEL,       IDC_SIZE_STATIC,       "Объём",       "-" },
            { IDC_TEMP_LABEL,       IDC_TEMP_STATIC,       "Температура", "-" },
            { IDC_POH_LABEL,        IDC_POH_STATIC,        "Наработка",   "-" },
            { IDC_STATUS_LABEL,     IDC_STATUS_STATIC,     "S.M.A.R.T.",  "-" },
            { IDC_PROTOCOL_LABEL, IDC_PROTOCOL_STATIC, "Протокол",    "-" },
            { IDC_ADAPTER_LABEL,    IDC_ADAPTER_STATIC,    "Переходник",  "—" },
        };
        int r;
        for (r = 0; r < 12; r++) {
            int y = nInfoY + (nInfoH + nInfoGap) * r;
            HWND hL = CreateWindowExU8(0, "STATIC", rows[r].lbl,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nInfoX, y, nLblW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idLbl, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", rows[r].val,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nValX, y, nValW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idVal, g_hInst, NULL);
            SendMessage(hL, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            SendMessage(hV, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
    }

    HWND hPred = CreateWindowExU8(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        nRightX, 258, 430, 17,
        hWnd, (HMENU)IDC_PREDICT_STATIC, g_hInst, NULL);

    SendMessage(hPred, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    HWND hList = CreateWindowExU8(
        WS_EX_CLIENTEDGE, "SysListView32", "",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        nRightX, 283, 540, 322,
        hWnd, (HMENU)IDC_ATTR_LIST, g_hInst, NULL
    );
    SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    ListView_SetBkColor(hList, CLR_PANEL);
    ListView_SetTextBkColor(hList, CLR_ROW1);
    ListView_SetTextColor(hList, CLR_TEXT);

    {
        LVCOLUMNW col;
        WCHAR w0[16], w1[64], w2[64], w3[64], w4[64], w5[64], w6[64];
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt  = LVCFMT_LEFT;

        U8ToW("ID", w0, 16);
        col.cx = 48;  col.pszText = w0;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        U8ToW("Параметр", w1, 64);
        col.cx = 200; col.pszText = w1;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        U8ToW("Значение", w2, 64);
        col.cx = 70;  col.pszText = w2;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
        U8ToW("Худший", w3, 64);
        col.cx = 50;  col.pszText = w3;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
        U8ToW("Порог", w4, 64);
        col.cx = 50;  col.pszText = w4;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);
        U8ToW("RAW", w5, 64);
        col.cx = 120; col.pszText = w5;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 5, (LPARAM)&col);
        U8ToW("Статус", w6, 64);
        col.cx = 124; col.pszText = w6;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 6, (LPARAM)&col);
    }
}

static LRESULT HandleCtlColor(HWND hWnd, WPARAM wParam)
{
    HDC  hdc     = (HDC)wParam;

    HWND hSender = WindowFromDC(hdc);
    if (hSender) {
        int id = GetDlgCtrlID(hSender);
        if (id == IDC_MODEL_LABEL    || id == IDC_SERIAL_LABEL   ||
            id == IDC_FIRMWARE_LABEL || id == IDC_SIZE_LABEL      ||
            id == IDC_TEMP_LABEL     || id == IDC_POH_LABEL       ||
            id == IDC_STATUS_LABEL    ||
            id == IDC_PROTOCOL_LABEL || id == IDC_ADAPTER_LABEL ||
            id == IDC_BRAND_LABEL    || id == IDC_CONTROLLER_LABEL ||
            id == IDC_NAND_LABEL     ||
            id == IDC_MODEL_STATIC   || id == IDC_SERIAL_STATIC   ||
            id == IDC_FIRMWARE_STATIC|| id == IDC_SIZE_STATIC     ||
            id == IDC_POH_STATIC     || id == IDC_STATUS_STATIC    ||
            id == IDC_PROTOCOL_STATIC || id == IDC_ADAPTER_STATIC ||
            id == IDC_BRAND_STATIC   || id == IDC_CONTROLLER_STATIC ||
            id == IDC_NAND_STATIC || id == IDC_AXIS_STATIC) {
            BOOL bLabel = (id == IDC_MODEL_LABEL || id == IDC_SERIAL_LABEL ||
                           id == IDC_FIRMWARE_LABEL || id == IDC_SIZE_LABEL ||
                           id == IDC_TEMP_LABEL || id == IDC_POH_LABEL ||
                           id == IDC_STATUS_LABEL ||
                           id == IDC_PROTOCOL_LABEL || id == IDC_ADAPTER_LABEL ||
                           id == IDC_BRAND_LABEL || id == IDC_CONTROLLER_LABEL ||
                           id == IDC_NAND_LABEL);
            SetTextColor(hdc, bLabel ? CLR_TEXT_DIM : CLR_TEXT);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
        if (id == IDC_TEMP_STATIC) {
            COLORREF clr = CLR_TEXT;
            if (g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount) {
                const DRIVE_INFO* pT = &g_Drives[g_nSelectedDrive];
                if (pT->eTempBand == TEMP_BAND_NORMAL)
                    clr = CLR_GREEN;
                else if (pT->eTempBand == TEMP_BAND_ELEVATED)
                    clr = CLR_YELLOW;
                else if (pT->eTempBand == TEMP_BAND_HIGH ||
                         pT->eTempBand == TEMP_BAND_CRITICAL)
                    clr = CLR_RED;
                else if (pT->nTemperatureC > 0) {
                    if (pT->nTemperatureC < 50)      clr = CLR_GREEN;
                    else if (pT->nTemperatureC < 60) clr = CLR_YELLOW;
                    else                             clr = CLR_RED;
                }
            }
            SetTextColor(hdc, clr);
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
        CreateGDIObjects();
        CreateMenuBar(hWnd);
        CreateControls(hWnd);
        /* No tray: close exits, no background monitoring. */
        DeviceNotify_Register(hWnd);
        /* SMART is read once at create and on hotplug — no periodic refresh. */

        UpdateWindowTitle(hWnd);
        RefreshData(hWnd);
        {
            GdiplusStartupInput gdipInput;
            GdiplusStartup(&g_gdiplusToken, &gdipInput, NULL);
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
            /* Status badge for col 6 only. Code comes from lItemlParam
             * (set at fill time). Never LVM_GETITEM / CreateFont here. */
            {
                HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
                HWND hHdr  = hList ? ListView_GetHeader(hList) : NULL;
                if (hHdr && pHdr->hwndFrom == hHdr && pHdr->code == NM_CUSTOMDRAW) {
                    NMCUSTOMDRAW* pCD = (NMCUSTOMDRAW*)lParam;
                    if (pCD->dwDrawStage == CDDS_PREPAINT)
                        return CDRF_NOTIFYITEMDRAW;
                    if (pCD->dwDrawStage == CDDS_ITEMPREPAINT) {
                        HDC hdc = pCD->hdc;
                        HBRUSH hbr = CreateSolidBrush(CLR_HEADER);
                        FillRect(hdc, &pCD->rc, hbr);
                        DeleteObject(hbr);
                        HPEN hp = CreatePen(PS_SOLID, 1, CLR_BORDER);
                        HPEN hpOld = (HPEN)SelectObject(hdc, hp);
                        MoveToEx(hdc, pCD->rc.left, pCD->rc.bottom - 1, NULL);
                        LineTo(hdc, pCD->rc.right, pCD->rc.bottom - 1);
                        SelectObject(hdc, hpOld);
                        DeleteObject(hp);
                        WCHAR wz[64];
                        HDITEMW hi;
                        ZeroMemory(&hi, sizeof(hi));
                        hi.mask = HDI_TEXT;
                        hi.pszText = wz;
                        hi.cchTextMax = 64;
                        SendMessageW(hHdr, HDM_GETITEMW, pCD->dwItemSpec, (LPARAM)&hi);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, CLR_TEXT);
                        RECT rcT = pCD->rc;
                        rcT.left += 6;
                        DrawTextW(hdc, wz, -1, &rcT,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
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
                        if (pCD->iSubItem != 6)
                            return CDRF_NEWFONT;

                        int nSt = (int)(pCD->nmcd.lItemlParam & 0xFF);
                        RECT rcCell = pCD->nmcd.rc;
                        if (nSt == ATTRST_NONE || (rcCell.right - rcCell.left) < 8)
                            return CDRF_DODEFAULT;

                        COLORREF clrBadgeBg;
                        const char* psz;
                        int nAlt = (int)(pCD->nmcd.lItemlParam & 0x100);
                        switch (nSt) {
                        case ATTRST_BAD:
                            clrBadgeBg = CLR_RED;
                            psz = nAlt ? "ПЛОХО" : "СБОЙ";
                            break;
                        case ATTRST_WARN:
                            clrBadgeBg = CLR_ORANGE;
                            psz = nAlt ? "Жарко" : "Внимание";
                            break;
                        case ATTRST_RISK:
                            clrBadgeBg = CLR_YELLOW;
                            psz = "Риск";
                            break;
                        case ATTRST_OK:
                            clrBadgeBg = CLR_GREEN;
                            psz = "ОК";
                            break;
                        case ATTRST_DIM:
                            clrBadgeBg = RGB(148, 163, 184);
                            psz = "--";
                            break;
                        case ATTRST_SKIP:
                            clrBadgeBg = RGB(148, 163, 184);
                            psz = "Не оценивается";
                            break;
                        case ATTRST_INFO:
                            clrBadgeBg = RGB(71, 99, 128);
                            psz = "INFO";
                            break;
                        default:
                            return CDRF_DODEFAULT;
                        }

                        HDC hdc = pCD->nmcd.hdc;
                        HBRUSH hbrRow = CreateSolidBrush(clrRowBk);
                        FillRect(hdc, &rcCell, hbrRow);
                        DeleteObject(hbrRow);

                        HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontSmall);
                        WCHAR wz[32];
                        U8ToW(psz, wz, 32);
                        SIZE sz;
                        GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);

                        int badgeH  = sz.cy + 6;
                        int badgeW  = sz.cx + ((nSt == ATTRST_SKIP) ? 20 : 16);
                        int cellCX  = rcCell.right  - rcCell.left;
                        int cellCY  = rcCell.bottom - rcCell.top;
                        int bx      = rcCell.left + (cellCX - badgeW) / 2;
                        int by      = rcCell.top  + (cellCY - badgeH) / 2;
                        RECT rcBadge = { bx, by, bx + badgeW, by + badgeH };

                        HBRUSH hbrBadge = CreateSolidBrush(clrBadgeBg);
                        HPEN   hpBorder = CreatePen(PS_SOLID, 1, clrBadgeBg);
                        HBRUSH hbrOld   = (HBRUSH)SelectObject(hdc, hbrBadge);
                        HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                        RoundRect(hdc, rcBadge.left, rcBadge.top,
                                       rcBadge.right, rcBadge.bottom, 8, 8);
                        SelectObject(hdc, hbrOld);
                        SelectObject(hdc, hpOld);
                        DeleteObject(hbrBadge);
                        DeleteObject(hpBorder);

                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, RGB(255, 255, 255));
                        DrawTextU8(hdc, psz, &rcBadge,
                                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                        SelectObject(hdc, hOldFont);
                        return CDRF_SKIPDEFAULT;
                    }
                default:
                    return CDRF_DODEFAULT;
                }
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
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000))
            DoSaveScreenshot(hWnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
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
            else if (nCtrl == IDC_REREAD_BTN) {
                RefreshData(hWnd);  /* full one-shot scan, incl. RTL9210 0xE4 */
            }
            else if (nCtrl == IDC_REPORT_BTN || nCtrl == IDM_REPORT) {
                DoSaveDriveReport(hWnd);
            }
            else if (nCtrl == IDM_SCREENSHOT) {
                DoSaveScreenshot(hWnd);
            }
            else if (nCtrl == IDM_ABOUT) {
                ShowAboutDialog(hWnd);
            }
            else if (nCtrl == IDM_DONATE) {
                OpenDonatePage(hWnd);
            }
            else if (nCtrl == IDM_EXIT) {
                DestroyWindow(hWnd);
            }
        }
        return 0;

    case WM_APP_REFRESH_DONE:
        {
            int i;

            /* Snapshot CURRENT g_Drives before replacing them. */
            Snapshot_Save();

            memcpy(g_Drives, g_ScanBuf, sizeof(DRIVE_INFO) * MAX_DRIVES);
            g_nDriveCount = g_nScanBufCount;

            Snapshot_Diff();

            InterlockedExchange(&g_bScanBusy, 0);
            {
                HWND hReread = GetDlgItem(hWnd, IDC_REREAD_BTN);
                HWND hReport = GetDlgItem(hWnd, IDC_REPORT_BTN);
                if (hReread) EnableWindow(hReread, TRUE);
                if (hReport) EnableWindow(hReport, TRUE);
            }

            UpdateDriveButtons(hWnd);

            if (g_nSelectedDrive >= g_nDriveCount) g_nSelectedDrive = 0;

            UpdateDriveInfo(hWnd, g_nSelectedDrive);
            UpdateAttrList(hWnd, g_nSelectedDrive);
            RepaintHealthBar();

            for (i = 0; i < g_nDriveCount; i++)
                if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);

            InvalidateRect(hWnd, NULL, FALSE);
            UpdateWindow(hWnd);
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_HOTPLUG) {
            KillTimer(hWnd, IDT_HOTPLUG);
            RefreshData(hWnd);    /* one-shot full scan on plug/unplug */
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* pmmi = (MINMAXINFO*)lParam;
            pmmi->ptMinTrackSize.x = WINDOW_W;
            pmmi->ptMinTrackSize.y = WINDOW_H;
        }
        return 0;

    case WM_SIZE:
        {
            int cxClient = LOWORD(lParam);
            int cyClient = HIWORD(lParam);
            if (cxClient < 100 || cyClient < 100) break;
            if (wParam == SIZE_MINIMIZED) break;

            int i;
            int nBtnW = DRIVE_BTN_PANEL_W - 12;
            int nStartY = 40;
            for (i = 0; i < MAX_DRIVES; i++) {
                if (g_hDriveBtn[i]) {
                    int nY = nStartY + i * (DRIVE_BTN_H + DRIVE_BTN_GAP);
                    SetWindowPos(g_hDriveBtn[i], NULL, 6, nY, nBtnW, DRIVE_BTN_H, SWP_NOZORDER);
                }
            }

            HWND hDriveLabel = GetDlgItem(hWnd, IDC_DRIVE_LIST);
            if (hDriveLabel)
                SetWindowPos(hDriveLabel, NULL, 6, 8, nBtnW, 16, SWP_NOZORDER);

            int nRightX = DRIVE_BTN_PANEL_W + 10;
            int nBarsW  = 190;
            int nInfoX  = nRightX + nBarsW + 10;

            HWND hHl = GetDlgItem(hWnd, IDC_HEALTH_LABEL);
            if (hHl) SetWindowPos(hHl, NULL, nRightX, 40, nBarsW, 14, SWP_NOZORDER);
            if (g_hHealthBar) SetWindowPos(g_hHealthBar, NULL, nRightX, 56, nBarsW, 40, SWP_NOZORDER);
            {
                HWND hReread = GetDlgItem(hWnd, IDC_REREAD_BTN);
                HWND hReport = GetDlgItem(hWnd, IDC_REPORT_BTN);
                if (hReread) SetWindowPos(hReread, NULL, nRightX, 102, 90, 24, SWP_NOZORDER);
                if (hReport) SetWindowPos(hReport, NULL, nRightX + 100, 102, 90, 24, SWP_NOZORDER);
                HWND hAxis = GetDlgItem(hWnd, IDC_AXIS_STATIC);
                if (hAxis) SetWindowPos(hAxis, NULL, nRightX, 132, nBarsW, 72, SWP_NOZORDER);
            }

            int nLblW2  = 100;
            int nValX2  = nInfoX + nLblW2 + 4;
            int nValW2  = cxClient - nValX2 - 8;
            if (nValW2 < 40) nValW2 = 40;
            int nInfoY2 = 36, nInfoH2 = 16, nInfoGap2 = 2;
            { int lblIds[] = { IDC_MODEL_LABEL, IDC_BRAND_LABEL, IDC_CONTROLLER_LABEL,
                               IDC_NAND_LABEL, IDC_SERIAL_LABEL, IDC_FIRMWARE_LABEL,
                               IDC_SIZE_LABEL, IDC_TEMP_LABEL, IDC_POH_LABEL, IDC_STATUS_LABEL,
                               IDC_PROTOCOL_LABEL, IDC_ADAPTER_LABEL };
              int k2;
              for (k2 = 0; k2 < 12; k2++) {
                  HWND hL = GetDlgItem(hWnd, lblIds[k2]);
                  if (hL) SetWindowPos(hL, NULL, nInfoX,
                      nInfoY2 + (nInfoH2 + nInfoGap2) * k2, nLblW2, nInfoH2, SWP_NOZORDER);
              }
            }

            { int valIds[] = { IDC_MODEL_STATIC, IDC_BRAND_STATIC, IDC_CONTROLLER_STATIC,
                               IDC_NAND_STATIC, IDC_SERIAL_STATIC, IDC_FIRMWARE_STATIC,
                               IDC_SIZE_STATIC, IDC_TEMP_STATIC, IDC_POH_STATIC, IDC_STATUS_STATIC,
                               IDC_PROTOCOL_STATIC, IDC_ADAPTER_STATIC };
              int k3;
              for (k3 = 0; k3 < 12; k3++) {
                  HWND hV = GetDlgItem(hWnd, valIds[k3]);
                  if (hV) SetWindowPos(hV, NULL, nValX2,
                      nInfoY2 + (nInfoH2 + nInfoGap2) * k3, nValW2, nInfoH2, SWP_NOZORDER);
              }
            }
            HWND hPred = GetDlgItem(hWnd, IDC_PREDICT_STATIC);
            if (hPred) SetWindowPos(hPred, NULL, nRightX, 258, cxClient - nRightX - 8, 17, SWP_NOZORDER);

            HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
            if (hList) {
                int nListTop = 283;
                int nListH   = cyClient - nListTop - 8;
                if (nListH < 50) nListH = 50;
                int nListW = cxClient - nRightX - 8;
                if (nListW < 200) nListW = 200;
                SetWindowPos(hList, NULL, nRightX, nListTop, nListW, nListH, SWP_NOZORDER);
                ListView_SetColumnWidth(hList, 0, 42);
                ListView_SetColumnWidth(hList, 1, 200);
                ListView_SetColumnWidth(hList, 2, 70);
                ListView_SetColumnWidth(hList, 3, 50);
                ListView_SetColumnWidth(hList, 4, 50);
                ListView_SetColumnWidth(hList, 6, 124);
                {
                    int nRaw = nListW - 42 - 200 - 70 - 50 - 50 - 124 - 24;
                    if (nRaw < 80) nRaw = 80;
                    ListView_SetColumnWidth(hList, 5, nRaw);
                }
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_HOTPLUG);
        DeviceNotify_Unregister();
        DestroyGDIObjects();
        if (g_gdiplusToken) { GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}
