/* ============================================================================
 *  HDDHealth Monitor - S.M.A.R.T. data acquisition core
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  Author  : Ari Sohandri Putra
 *  Company : ARImetic Inc.
 *  Sponsor : https://github.com/sponsors/arisohandriputra/
 *  License : MIT
 *
 *  This translation unit contains the low-level drive enumeration and
 *  S.M.A.R.T. attribute parsing logic.  It supports three transport
 *  families:
 *    1. ATA / SATA  - via IOCTL_ATA_PASS_THROUGH_DIRECT (IDENTIFY,
 *                     SMART_READ_DATA, SMART_READ_THRESHOLDS).
 *    2. USB bridge  - via IOCTL_SCSI_PASS_THROUGH_DIRECT using the
 *                     SAT (SCSI-ATA-Translation) protocol; tested with
 *                     JMicron, ASMedia, Realtek and Cypress bridges.
 *    3. NVMe        - via IOCTL_STORAGE_QUERY_PROPERTY on the native
 *                     Microsoft NVMe driver; reads Health Info Log 0x02
 *                     and translates the key SMART-equivalent fields.
 *
 *  The public entry point is ScanDrives() which fills a caller-allocated
 *  DRIVE_INFO[] array with model, serial, firmware, size, temperature,
 *  health %, performance metrics, and the normalized attribute table.
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <setupapi.h>
#include <devguid.h>
#include <regstr.h>
#include <cfgmgr32.h>
#include "smart.h"
#include "safestr.h"

/* #pragma comment(lib, "setupapi.lib") -- not supported by MinGW; linked via Makefile (-lsetupapi) */

#ifndef CR_SUCCESS
#define CR_SUCCESS 0
#endif

/* Forward declarations for USB VID/PID helpers.
 * These must be declared before IsNVMeDrive and GetBridgeIdentity
 * which call ParseVidPidFromHardwareId. */
static BOOL ParseVidPidFromHardwareId(const char* szHwId, WORD* pwVid, WORD* pwPid);
static BOOL GetDevicePathFromHandle(HANDLE hDrive, WORD* pwVid, WORD* pwPid);
static int FillSmartData(DRIVE_INFO* pInfo, const BYTE* pRawBuf);
static int FillSmartThreshold(DRIVE_INFO* pInfo, const BYTE* pRawBuf);

/* ============================================================
 * Internal definitions (compatibility shims)
 * ============================================================ */
#ifndef StorageDeviceProtocolSpecificProperty
#define StorageDeviceProtocolSpecificProperty ((STORAGE_PROPERTY_ID)49)
#endif
#ifndef StorageAdapterProtocolSpecificProperty
#define StorageAdapterProtocolSpecificProperty ((STORAGE_PROPERTY_ID)50)
#endif

#define MY_ProtocolTypeUnknown   0
#define MY_ProtocolTypeScsi      1
#define MY_ProtocolTypeAta       2
#define MY_ProtocolTypeNvme      3
#define MY_ProtocolTypeSd        4
#define MY_ProtocolTypeUfs       5

#define MY_NVMeDataTypeUnknown   0
#define MY_NVMeDataTypeIdentify  1
#define MY_NVMeDataTypeLogPage   2
#define MY_NVMeDataTypeFeature   3

#define MY_AtaDataTypeUnknown           0
#define MY_AtaDataTypeIdentify          1
#define MY_AtaDataTypeSmartData         2
#define MY_AtaDataTypeSmartThresholds   3

#define NVME_LOG_PAGE_ERROR_INFO         0x01
#define NVME_LOG_PAGE_HEALTH_INFO        0x02
#define NVME_LOG_PAGE_FIRMWARE_SLOT_INFO 0x03

/* ATA pass-through flags (Windows IDEREGS-derived) */
#ifndef ATA_FLAGS_DRDY_REQUIRED
#define ATA_FLAGS_DRDY_REQUIRED  (1 << 0)
#define ATA_FLAGS_DATA_IN        (1 << 1)
#define ATA_FLAGS_DATA_OUT       (1 << 2)
#define ATA_FLAGS_48BIT_COMMAND  (1 << 3)
#define ATA_FLAGS_USE_DMA        (1 << 4)
#define ATA_FLAGS_NO_MULTIPLE    (1 << 5)
#endif

#pragma pack(push, 1)

/* SAT pass-through buffer (SCSI A1h / 85h) */
typedef struct _SAT_PASSTHROUGH_BUF {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG    Filler;
    BYTE     SenseBuf[32];
} SAT_PASSTHROUGH_BUF;

#define SAT_SENSEBUF_OFFSET  (sizeof(SCSI_PASS_THROUGH_DIRECT) + sizeof(ULONG))

/* CDI-style SCSI_PASS_THROUGH (non-DIRECT) for USB bridges
 * IOCTL_SCSI_PASS_THROUGH (not DIRECT)
 * because many USB-SATA bridge chips require the buffer-based approach. */
typedef struct _CDI_SAT_PASSTHROUGH_BUF {
    SCSI_PASS_THROUGH spt;
    ULONG  Filler;
    BYTE   SenseBuf[32];
    BYTE   DataBuf[512];
} CDI_SAT_PASSTHROUGH_BUF;

/* Native Windows ATA pass-through */
typedef struct _MY_ATA_PASS_THROUGH_EX {
    USHORT    Length;
    USHORT    AtaFlags;
    UCHAR     PathId;
    UCHAR     TargetId;
    UCHAR     Lun;
    UCHAR     ReservedAsUchar;
    ULONG     DataTransferLength;
    ULONG     TimeOutValue;
    ULONG     ReservedAsUlong;
    ULONG_PTR DataBufferOffset;
    UCHAR     PreviousTaskFile[8];
    UCHAR     CurrentTaskFile[8];
} MY_ATA_PASS_THROUGH_EX;

typedef struct _MY_ATA_PASS_THROUGH_BUF {
    MY_ATA_PASS_THROUGH_EX apt;
    ULONG  Filler;
    BYTE   DataBuf[512];
} MY_ATA_PASS_THROUGH_BUF;

/* Storage protocol query (Win8+) */
typedef struct _MY_STORAGE_PROTOCOL_SPECIFIC_DATA {
    ULONG ProtocolType;
    ULONG DataType;
    ULONG ProtocolDataRequestValue;
    ULONG ProtocolDataRequestSubValue;
    ULONG ProtocolDataOffset;
    ULONG ProtocolDataLength;
    ULONG FixedProtocolReturnData;
    ULONG Reserved[3];
} MY_STORAGE_PROTOCOL_SPECIFIC_DATA;

typedef struct _MY_STORAGE_PROTOCOL_QUERY {
    ULONG PropertyId;
    ULONG QueryType;
    MY_STORAGE_PROTOCOL_SPECIFIC_DATA ProtocolSpecific;
} MY_STORAGE_PROTOCOL_QUERY;

#pragma pack(pop)

/* ============================================================
 * Utility helpers
 * ============================================================ */

/* Safe NVMe admin data via IOCTL_STORAGE_QUERY_PROPERTY only.
 * Never use IOCTL_STORAGE_PROTOCOL_COMMAND here: a hand-built NVMe
 * command can bugcheck Microsoft nvme.sys (IRQL_NOT_LESS_OR_EQUAL).
 * Tries device + adapter property and a few NSIDs; uses the offset
 * the driver returned, not a hardcoded one. */
static BOOL IsBufferAllZero(const BYTE* p, int nLen);
static BOOL NvmeMiniportAdmin(HANDLE hDrive, DWORD cdw0, DWORD nsid, DWORD cdw10, BYTE* pOut, DWORD dwOut);
static BOOL NVMeOverUSBTryAll(HANDLE hDrive, DRIVE_INFO* pInfo);
static BOOL NvmeIntelRstAdmin(HANDLE hDrive, DWORD opcode, DWORD nsid, DWORD cdw10, BYTE* pOut, DWORD dwOut);
static BOOL IsRealtekNvmeUsbBridge(const DRIVE_INFO* pInfo);
static void CopyNvmeIdentBuf(DRIVE_INFO* pInfo, const BYTE* pBuf, DWORD nAvail);

#pragma pack(push, 1)
typedef struct _CDI_NVME_QUERY_BUF {
    ULONG PropertyId;
    ULONG QueryType;
    MY_STORAGE_PROTOCOL_SPECIFIC_DATA ProtocolSpecific;
    BYTE  Buffer[4096];
} CDI_NVME_QUERY_BUF;
#pragma pack(pop)

static DWORD g_dwLastNvmeQueryErr;

/* NVMe Identify Controller byte offset 80 is VER (spec):
 * bits 31:16 major, 15:8 minor, 7:0 tertiary. Named fields of
 * NVME_IDENTIFY_CONTROLLER are misaligned — read the raw copy. */
static void FillNvmeProtocolFromIdent(DRIVE_INFO* pInfo)
{
    const BYTE* id;
    DWORD ver;
    unsigned maj, minr, ter;
    if (!pInfo) return;
    id = (const BYTE*)&pInfo->nvmeIdent;
    ver = (DWORD)id[80] | ((DWORD)id[81] << 8) |
          ((DWORD)id[82] << 16) | ((DWORD)id[83] << 24);
    if (ver == 0) {
        safe_snprintf(pInfo->szProtocol, "NVMe");
        return;
    }
    maj  = (ver >> 16) & 0xFFFF;
    minr = (ver >> 8) & 0xFF;
    ter  = ver & 0xFF;
    safe_snprintf(pInfo->szProtocol, "NVMe %u.%u.%u", maj, minr, ter);
}

static void FillAtaProtocolFromIdent(DRIVE_INFO* pInfo, const WORD* pIdent)
{
    WORD w76, w77;
    unsigned neg;
    if (!pInfo || !pIdent) return;
    w76 = pIdent[76];
    w77 = pIdent[77];
    /* Word 77 bits 3:1 = negotiated SATA speed (1=Gen1, 2=Gen2, 3=Gen3). */
    neg = (unsigned)((w77 >> 1) & 0x7);
    if (neg == 3)
        safe_snprintf(pInfo->szProtocol, "SATA 6 Гбит/с");
    else if (neg == 2)
        safe_snprintf(pInfo->szProtocol, "SATA 3 Гбит/с");
    else if (neg == 1)
        safe_snprintf(pInfo->szProtocol, "SATA 1.5 Гбит/с");
    else if (w76 != 0 && w76 != 0xFFFF) {
        /* Word 76: bit1=1.5, bit2=3.0, bit3=6.0 Gb/s supported. */
        if (w76 & 0x0008)
            safe_snprintf(pInfo->szProtocol, "SATA 6 Гбит/с");
        else if (w76 & 0x0004)
            safe_snprintf(pInfo->szProtocol, "SATA 3 Гбит/с");
        else if (w76 & 0x0002)
            safe_snprintf(pInfo->szProtocol, "SATA 1.5 Гбит/с");
        else
            safe_snprintf(pInfo->szProtocol, "SATA");
    } else {
        safe_snprintf(pInfo->szProtocol, "ATA");
    }
}

/* Final protocol string after type / SMART / NVMe ident are known. */
static void FillDriveProtocol(DRIVE_INFO* pInfo)
{
    char hay[384];
    if (!pInfo) return;

    if (pInfo->bIsNVMe) {
        if (pInfo->bGotNVMeIdent) {
            FillNvmeProtocolFromIdent(pInfo);
        } else if (pInfo->szProtocol[0] == '\0') {
            safe_snprintf(pInfo->szProtocol, "NVMe");
        }
        return;
    }

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        safe_snprintf(hay, "%s %s %s",
                  pInfo->szModel, pInfo->szBridgeVendor, pInfo->szBridgeProduct);
        if (IsRealtekNvmeUsbBridge(pInfo) ||
            pInfo->eUsbBridgeType == USB_BRIDGE_NVME_REALTEK ||
            strstr(hay, "RTL9210") || strstr(hay, "RTL921"))
            safe_snprintf(pInfo->szProtocol, "USB (RTL9210)");
        else
            safe_snprintf(pInfo->szProtocol, "USB");
        return;
    }

    if (pInfo->szProtocol[0] &&
        (strncmp(pInfo->szProtocol, "SATA", 4) == 0 ||
         strcmp(pInfo->szProtocol, "ATA") == 0))
        return;

    if (pInfo->eType == DRIVE_TYPE_HDD ||
        pInfo->eType == DRIVE_TYPE_SSD_SATA ||
        pInfo->eType == DRIVE_TYPE_M2_SATA)
        safe_snprintf(pInfo->szProtocol, "SATA");
    else if (pInfo->eType == DRIVE_TYPE_EMMC)
        safe_snprintf(pInfo->szProtocol, "eMMC");
    else if (pInfo->eType == DRIVE_TYPE_SD)
        safe_snprintf(pInfo->szProtocol, "SD");
    else if (pInfo->eType == DRIVE_TYPE_SCSI)
        safe_snprintf(pInfo->szProtocol, "SCSI");
    else if (pInfo->bIsUSB)
        safe_snprintf(pInfo->szProtocol, "USB");
    else if (pInfo->szProtocol[0] == '\0')
        safe_snprintf(pInfo->szProtocol, "ATA");
}

/* memcpy Identify Controller without overreading a 4096-byte page. */
static void CopyNvmeIdentBuf(DRIVE_INFO* pInfo, const BYTE* pBuf, DWORD nAvail)
{
    DWORD n;
    if (!pInfo || !pBuf || nAvail == 0) return;
    n = (DWORD)sizeof(NVME_IDENTIFY_CONTROLLER);
    if (n > nAvail) n = nAvail;
    if (n > 4096) n = 4096;
    memcpy(&pInfo->nvmeIdent, pBuf, n);
    if (n >= 84)
        FillNvmeProtocolFromIdent(pInfo);
    else
        safe_snprintf(pInfo->szProtocol, "NVMe");
}

/* Realtek RTL9210 USB dual-mode enclosure (NVMe via 0xE4, SATA via SAT).
 * Native NvmeMini IOCTL on this USB handle crashes the process.
 * ScanDrivesEx: SAT SMART first, then one-shot vendor 0xE4.
 * Never 0xE4 / NvmeMini from RefreshDriveSmart or NVMeOverUSBTryAll. */
static BOOL IsRealtekNvmeUsbBridge(const DRIVE_INFO* p)
{
    char hay[384];
    int i;
    if (!p) return FALSE;
    if (p->eUsbBridgeType == USB_BRIDGE_NVME_REALTEK)
        return TRUE;
    /* Any Realtek USB storage VID — SAT first, then 0xE4. */
    if (p->wUsbVid == 0x0BDA)
        return TRUE;
    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);
    for (i = 0; hay[i]; i++)
        hay[i] = (char)toupper((unsigned char)hay[i]);
    if (strstr(hay, "RTL9210") || strstr(hay, "RTL9211") ||
        strstr(hay, "RTL921"))
        return TRUE;
    if (strstr(hay, "REALTEK") && strstr(hay, "9210"))
        return TRUE;
    return FALSE;
}

static BOOL QueryNVMeProtocolOnHandle(HANDLE h, ULONG dataType, ULONG requestValue,
                                      BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied)
{
    const ULONG propertyIds[2] = {
        (ULONG)StorageAdapterProtocolSpecificProperty,
        (ULONG)StorageDeviceProtocolSpecificProperty
    };
    ULONG subValues[3];
    ULONG lengths[2];
    int nSub, nLen, ip, isv, il;

    if (dataType == MY_NVMeDataTypeLogPage) {
        subValues[0] = 0;
        subValues[1] = 0xFFFFFFFFu;
        subValues[2] = 1;
        nSub = 3;
        lengths[0] = 512;
        lengths[1] = 4096;
        nLen = 2;
    } else {
        subValues[0] = 0;
        subValues[1] = 1;
        nSub = 2;
        lengths[0] = 4096;
        nLen = 1;
    }

    for (ip = 0; ip < 2; ip++) {
        for (isv = 0; isv < nSub; isv++) {
            for (il = 0; il < nLen; il++) {
                CDI_NVME_QUERY_BUF* q = (CDI_NVME_QUERY_BUF*)HeapAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CDI_NVME_QUERY_BUF));
                DWORD dwBytes = 0;
                BYTE* pData;
                DWORD avail;
                ULONG off;

                if (!q) return FALSE;

                q->PropertyId = propertyIds[ip];
                q->QueryType  = 0;
                q->ProtocolSpecific.ProtocolType             = MY_ProtocolTypeNvme;
                q->ProtocolSpecific.DataType                 = dataType;
                q->ProtocolSpecific.ProtocolDataRequestValue = requestValue;
                q->ProtocolSpecific.ProtocolDataRequestSubValue = subValues[isv];
                q->ProtocolSpecific.ProtocolDataOffset       = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
                q->ProtocolSpecific.ProtocolDataLength       = lengths[il];

                if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                        q, sizeof(*q), q, sizeof(*q), &dwBytes, NULL)) {
                    g_dwLastNvmeQueryErr = GetLastError();
                    HeapFree(GetProcessHeap(), 0, q);
                    continue;
                }

                off = q->ProtocolSpecific.ProtocolDataOffset;
                if (off == 0) off = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
                pData = (BYTE*)&q->ProtocolSpecific + off;
                if (pData < (BYTE*)q || pData >= (BYTE*)q + sizeof(*q))
                    pData = q->Buffer;
                avail = (DWORD)(((BYTE*)q + sizeof(*q)) - pData);
                if (avail > dwOutLen) avail = dwOutLen;

                if (avail >= 8 && !IsBufferAllZero(pData, avail > 16 ? 16 : (int)avail)) {
                    memcpy(pOut, pData, avail);
                    if (pdwCopied) *pdwCopied = avail;
                    HeapFree(GetProcessHeap(), 0, q);
                    return TRUE;
                }
                if (!IsBufferAllZero(q->Buffer, 16)) {
                    avail = dwOutLen < 4096 ? dwOutLen : 4096;
                    memcpy(pOut, q->Buffer, avail);
                    if (pdwCopied) *pdwCopied = avail;
                    HeapFree(GetProcessHeap(), 0, q);
                    return TRUE;
                }
                HeapFree(GetProcessHeap(), 0, q);
            }
        }
    }
    return FALSE;
}

static BOOL QueryNVMeProtocol(HANDLE hDrive, ULONG dataType, ULONG requestValue,
                              BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied)
{
    SCSI_ADDRESS addr;
    DWORD dwBytes = 0;
    HANDLE hScsi;
    char szScsi[32];

    g_dwLastNvmeQueryErr = 0;
    if (QueryNVMeProtocolOnHandle(hDrive, dataType, requestValue, pOut, dwOutLen, pdwCopied))
        return TRUE;

    ZeroMemory(&addr, sizeof(addr));
    addr.Length = sizeof(addr);
    if (DeviceIoControl(hDrive, IOCTL_SCSI_GET_ADDRESS,
            NULL, 0, &addr, sizeof(addr), &dwBytes, NULL)) {
        safe_snprintf(szScsi, "\\\\.\\Scsi%d:", (int)addr.PortNumber);
        hScsi = CreateFileA(szScsi, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hScsi != INVALID_HANDLE_VALUE) {
            BOOL ok = QueryNVMeProtocolOnHandle(hScsi, dataType, requestValue,
                                                pOut, dwOutLen, pdwCopied);
            CloseHandle(hScsi);
            if (ok) return TRUE;
        } else if (g_dwLastNvmeQueryErr == 0) {
            g_dwLastNvmeQueryErr = GetLastError();
        }
    } else if (g_dwLastNvmeQueryErr == 0) {
        g_dwLastNvmeQueryErr = GetLastError();
    }
    return FALSE;
}

static void TrimStr(char* sz)
{
    if (!sz || !sz[0]) return;
    int len = (int)strlen(sz);
    while (len > 0 && (sz[len - 1] == ' ' || sz[len - 1] == '\t' ||
                       sz[len - 1] == '\r' || sz[len - 1] == '\n')) {
        sz[--len] = '\0';
    }
    int start = 0;
    while (sz[start] == ' ' || sz[start] == '\t') start++;
    if (start > 0) {
        int j;
        for (j = 0; sz[start + j] != '\0'; j++)
            sz[j] = sz[start + j];
        sz[j] = '\0';
    }
}

/* Bounded copy of a STORAGE_DEVICE_DESCRIPTOR string.
 * Never reads past dwBytes; always NUL-terminates dst. */
static void CopyDescStr(char* dst, size_t dstSize,
                        const BYTE* buf, DWORD dwBytes, DWORD offset)
{
    size_t nMax, i;
    const char* src;

    if (!dst || dstSize == 0)
        return;
    dst[0] = '\0';
    if (!buf || !offset || offset >= dwBytes)
        return;

    nMax = (size_t)(dwBytes - offset);
    if (nMax > dstSize - 1)
        nMax = dstSize - 1;

    src = (const char*)(buf + offset);
    for (i = 0; i < nMax && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    TrimStr(dst);
}

static void SwapATAString(char* szDst, const WORD* pSrc, int nWords)
{
    int i;
    for (i = 0; i < nWords; i++) {
        szDst[i * 2]     = (char)(pSrc[i] >> 8);
        szDst[i * 2 + 1] = (char)(pSrc[i] & 0xFF);
    }
    szDst[nWords * 2] = '\0';
    TrimStr(szDst);
}

static WORD ReadLE16(const BYTE* p)
{
    return (WORD)p[0] | ((WORD)p[1] << 8);
}

static DWORD ReadLE32(const BYTE* p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static unsigned __int64 ReadLE64(const BYTE* p)
{
    unsigned __int64 v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

DWORD GetRawValue(const BYTE* pRaw)
{
    return ((DWORD)pRaw[3] << 24) |
           ((DWORD)pRaw[2] << 16) |
           ((DWORD)pRaw[1] <<  8) |
            (DWORD)pRaw[0];
}

unsigned __int64 GetRawValue48(const BYTE* pRaw)
{
    return ((unsigned __int64)pRaw[5] << 40) |
           ((unsigned __int64)pRaw[4] << 32) |
           ((unsigned __int64)pRaw[3] << 24) |
           ((unsigned __int64)pRaw[2] << 16) |
           ((unsigned __int64)pRaw[1] <<  8) |
            (unsigned __int64)pRaw[0];
}

static WORD GetRawValue16Lo(BYTE* pRaw)
{
    return ((WORD)pRaw[1] << 8) | (WORD)pRaw[0];
}

/* Forward declaration — SATSendCommand is used by ReadSMARTLogSAT et al.
 * but defined later in the SAT section. */
static BOOL SATSendCommand(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen, SMART_ACCESS_METHOD* pMethod);

static BOOL IsBufferAllZero(const BYTE* p, int nLen)
{
    int i;
    for (i = 0; i < nLen; i++)
        if (p[i] != 0x00) return FALSE;
    return TRUE;
}

static BOOL IsBufferAllFF(const BYTE* p, int nLen)
{
    int i;
    for (i = 0; i < nLen; i++)
        if (p[i] != 0xFF) return FALSE;
    return TRUE;
}

/* ============================================================
 * SMART attribute name table
 * Comprehensive coverage of standard + vendor-specific attributes
 * ============================================================ */
static const char* VendorSpecificAttrName(BYTE bID);

const ATTR_NAME g_AttrNames[] = {
    /* ---- Standard ATA SMART attributes ---- */
    { 0x01, "Частота ошибок чтения",             ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x02, "Производительность",                ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x03, "Время раскрутки",                   ATTR_CRIT_NONE,      INTERP_DURATION     },
    { 0x04, "Циклы старт/стоп",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x05, "Переназначенные сектора",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0x06, "Запас канала чтения",               ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x07, "Частота ошибок позиционирования",   ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x08, "Скорость позиционирования",         ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x09, "Часы наработки",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0A, "Повторы раскрутки",                 ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0x0B, "Повторы калибровки",                ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0C, "Циклы включения",                   ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0D, "Частота программных ошибок чтения", ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x0E, "Частота ошибок G-сенсора",          ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0x0F, "Повторы парковки",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x10, "Часы полёта головок",               ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0x11, "Повторы калибровки (альт.)",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* ---- SATA/ATA additional standard ---- */
    { 0x16, "Уровень гелия",                     ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0x17, "Гелий (нижний порог)",              ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x18, "Гелий (верхний порог)",             ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x19, "Счётчик состояния гелия",           ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x1A, "Остаток ресурса",                   ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0x1B, "Остаток выносливости",              ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0x1C, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },

    /* ---- SSD vendor-specific attributes (multiple vendors) ---- */
    { 0xA0, "Внезапные выключения",              ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA1, "Атрибут A1h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA2, "Худший резервный блок",             ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xA3, "Начальные плохие блоки",            ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA4, "Всего стираний",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA5, "Макс. стираний",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA6, "Мин. стираний",                     ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA7, "Среднее стираний",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA8, "Макс. стираний по спецификации",    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA9, "Заявленный остаток ресурса",         ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0xAA, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xAB, "Ошибки программирования",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xAC, "Ошибки стирания",                   ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xAD, "Выравнивание износа",               ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xAE, "Внезапная потеря питания",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xAF, "Сбой защиты питания",               ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB0, "Атрибут B0h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB1, "Атрибут B1h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB2, "Атрибут B2h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB3, "Использовано резервных блоков (всего)", ATTR_CRIT_ADVISORY, INTERP_COUNTER32 },
    { 0xB4, "Свободные резервные блоки",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB5, "Ошибки программирования (всего)",   ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB6, "Ошибки стирания (всего)",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB7, "Ошибки понижения SATA",             ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xB8, "Ошибка End-to-End",                 ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB9, "Стабильность головок",              ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xBA, "Детектор вибрации",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xBB, "Неисправимые ошибки ECC",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xBC, "Таймауты команд",                   ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xBD, "Записи на большой высоте",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xBE, "Температура воздуха",               ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xBF, "Частота ошибок G-сенсора",          ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xC0, "Парковки при выключении",           ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC1, "Циклы парковки",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC2, "Температура",                       ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xC3, "Восстановлено ECC",                 ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC4, "События переназначения",            ATTR_CRIT_CRITICAL,  INTERP_EVENT_COUNT  },
    { 0xC5, "Нестабильные сектора",              ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xC6, "Неисправимые сектора",              ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xC7, "Ошибки CRC UltraDMA",               ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xC8, "Частота ошибок записи",             ATTR_CRIT_CRITICAL,  INTERP_RATE         },
    { 0xC9, "Частота программных ошибок чтения", ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xCA, "Ошибки DAM",                        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xCB, "Отмена Run-Out",                    ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xCC, "Программная коррекция ECC",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xCD, "Тепловые выбросы",                  ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xCE, "Высота полёта",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xCF, "Ток раскрутки",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD0, "Жужжание шпинделя",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD1, "Offline-позиционирование",          ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD2, "Вибрация при записи",               ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD3, "Вибрация при записи (альт.)",       ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD4, "Удар при записи",                   ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD5, "Защита от падения",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD6, "События свободного падения",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xDC, "Смещение диска",                    ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xDD, "Частота ошибок G-сенсора (альт.)",  ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xDE, "Часы под нагрузкой",                ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xDF, "Повторы парковки",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE0, "Трение парковки",                   ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE1, "Циклы парковки (альт.)",            ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE2, "Время парковки",                    ATTR_CRIT_NONE,      INTERP_DURATION     },
    { 0xE3, "Усиление крутящего момента",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE4, "Циклы парковки при выключении",     ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE5, "Амплитуда GMR",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE6, "Амплитуда GMR (альт.)",             ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE7, "Остаток ресурса SSD / температура", ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0xE8, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xE9, "Записи NAND (ГБ) / износ",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEA, "Среднее стираний / записи",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEB, "Хорошие блоки / ресурс NAND",       ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEC, "Ошибки записи",                     ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xED, "Ошибки CRC",                        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xEE, "Стабильность PMR",                  ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xEF, "Ошибки SATA PHY",                   ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },

    /* ---- Samsung-specific ---- */
    { 0xF0, "Часы полёта головок (альт.)",       ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF1, "Записано LBA",                      ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF2, "Прочитано LBA",                     ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF3, "Записано LBA (расш.)",              ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF4, "Прочитано LBA (расш.)",             ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF5, "Атрибут F5h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF6, "Атрибут F6h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF7, "Атрибут F7h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF8, "Атрибут F8h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF9, "Записи NAND (ГиБ)",                 ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xFA, "Повторы ошибок чтения",             ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0xFB, "Остаток запасных блоков",           ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xFC, "Новые плохие блоки NAND",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xFD, "Межповерхностные дефекты",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xFE, "Защита от падения",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xFF, "Атрибут производителя",             ATTR_CRIT_NONE,      INTERP_NORMAL       },

    /* ---- Intel SSD specific ---- */
    { 0xB8, "Обнаружение ошибок End-to-End",     ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- Micron/Crucial SSD specific ---- */
    { 0xBB, "Неисправимые ошибки",               ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- Additional vendor-specific SSD attributes ---- */
    { 0xA0, "Температура (производитель)",       ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xA9, "Заявленный остаток ресурса",         ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },

    /* ---- Kingston SSD specific ---- */
    { 0xB6, "Ошибки стирания (Kingston)",        ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- WDC/HGST specific ---- */
    { 0x18, "Уровень гелия (WDC)",               ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },

    /* ---- Seagate specific ---- */
    { 0x02, "Циклы старт/стоп (Seagate)",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* ---- SK Hynix specific ---- */
    { 0xB4, "Ошибки SATA PHY (SK Hynix)",        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32   },

    /* ---- ADATA specific ---- */
    { 0xAD, "Среднее стираний (ADATA)",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* Terminator */
    { 0x00, NULL,                                ATTR_CRIT_NONE,      INTERP_NORMAL       }
};

const char* GetAttrName(BYTE bID)
{
    int i = 0;
    while (g_AttrNames[i].szName != NULL) {
        if (g_AttrNames[i].bID == bID) return g_AttrNames[i].szName;
        i++;
    }
    return VendorSpecificAttrName(bID);
}

/* Vendor overlay: only IDs whose name differs from generic g_AttrNames.
 * Terminated by id 0. HGST/Hitachi share WD names (generic already matches).
 * Kioxia uses the Toshiba/Phison SATA-SSD overlay. Solidigm is VENDOR_INTEL. */
typedef struct _ATTR_OVERLAY {
    BYTE        id;
    const char* name;
} ATTR_OVERLAY;

static const ATTR_OVERLAY g_OvSeagateHdd[] = {
    { 0x01, "Частота ошибок чтения (RAW вендора)" },
    { 0xBB, "Неисправимые ошибки" },
    { 0xF0, "Часы полёта головок" },
    { 0x00, NULL }
};

static const ATTR_OVERLAY g_OvSamsungSsd[] = {
    { 0xB1, "Выравнивание износа" },
    { 0xB3, "Использовано резервных блоков" },
    { 0xB5, "Ошибки программирования" },
    { 0xB6, "Ошибки стирания" },
    { 0xB7, "Плохие блоки (runtime)" },
    { 0xBB, "Неисправимые ошибки" },
    { 0xEB, "Защита от потери питания" },
    { 0xF5, "Мин. стираний (Samsung)" },
    { 0xF6, "Макс. стираний (Samsung)" },
    { 0xF7, "Среднее стираний (Samsung)" },
    { 0xF8, "Выравнивание износа (Samsung)" },
    { 0x00, NULL }
};

/* Phison SATA SSD: E7 life remaining, E9 NAND writes */
static const ATTR_OVERLAY g_OvPhisonSsd[] = {
    { 0xE7, "Остаток ресурса SSD" },
    { 0xE9, "Записи NAND (ГиБ)" },
    { 0x00, NULL }
};

static const ATTR_OVERLAY g_OvToshibaHdd[] = {
    { 0xC7, "Ошибки CRC" },
    { 0x00, NULL }
};

static const ATTR_OVERLAY g_OvMicronSsd[] = {
    { 0xCA, "Остаток ресурса %" },
    { 0xF6, "Записано секторов хоста" },
    { 0xAD, "Среднее стираний" },
    { 0x00, NULL }
};

static const ATTR_OVERLAY g_OvIntelSsd[] = {
    { 0xAA, "Доступное резервное пространство" },
    { 0xE1, "Записи хоста" },
    { 0xE2, "Таймер нагрузки" },
    { 0xE8, "Доступное резервное пространство" },
    { 0xE9, "Индикатор износа" },
    { 0x00, NULL }
};

static const char* LookupOverlay(const ATTR_OVERLAY* tab, BYTE bID)
{
    int i;
    if (!tab) return NULL;
    for (i = 0; tab[i].id != 0; i++) {
        if (tab[i].id == bID) return tab[i].name;
    }
    return NULL;
}

static const ATTR_OVERLAY* OverlayFor(const DRIVE_INFO* p)
{
    DRIVE_TYPE t;
    DRIVE_CONTROLLER c;
    BOOL ssd, hdd;
    if (!p) return NULL;
    t = p->eType;
    c = p->eController;
    ssd = (t == DRIVE_TYPE_SSD_SATA || t == DRIVE_TYPE_M2_SATA);
    hdd = (t == DRIVE_TYPE_HDD);
    if (t == DRIVE_TYPE_NVME)
        return NULL;

    /* Overlays key off controller, not brand (Kingston A400 is Phison). */
    switch (c) {
    case CONTROLLER_PHISON:
        return g_OvPhisonSsd;
    case CONTROLLER_SAMSUNG:
        return hdd ? NULL : g_OvSamsungSsd;
    case CONTROLLER_INTEL:
        return g_OvIntelSsd;
    case CONTROLLER_MICRON:
        return g_OvMicronSsd;
    case CONTROLLER_SEAGATE:
        return ssd ? NULL : g_OvSeagateHdd;
    case CONTROLLER_TOSHIBA:
        return ssd ? NULL : g_OvToshibaHdd;
    default:
        break;
    }
    /* Seagate HDD brand + HDD type keeps the Seagate overlay even if the
     * MCU was left UNKNOWN. Toshiba HDD CRC overlay similarly. */
    if (hdd && p->eVendor == VENDOR_SEAGATE)
        return g_OvSeagateHdd;
    if (hdd && p->eVendor == VENDOR_TOSHIBA)
        return g_OvToshibaHdd;
    return NULL;
}

static char g_szVendorAttrName[256][48];
static int  g_bVendorAttrNameReady = 0;

static void InitVendorAttrNames(void)
{
    int i;
    if (g_bVendorAttrNameReady) return;
    for (i = 0; i < 256; i++) {
        (void)_snprintf(g_szVendorAttrName[i], 48,
                        "Атрибут %02Xh (vendor-specific)", i);
        g_szVendorAttrName[i][47] = '\0';
    }
    g_bVendorAttrNameReady = 1;
}

static const char* VendorSpecificAttrName(BYTE bID)
{
    InitVendorAttrNames();
    return g_szVendorAttrName[bID];
}

static BOOL IsPhisonFamily(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    if (p->eController == CONTROLLER_PHISON)
        return TRUE;
    if (p->szFirmware[0] && strncmp(p->szFirmware, "HPS", 3) == 0)
        return TRUE;
    return FALSE;
}

static BOOL IsAtaSsdTypeInfo(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    return p->eType == DRIVE_TYPE_SSD_SATA || p->eType == DRIVE_TYPE_M2_SATA;
}

BOOL DriveTreatsC0AsPowerLoss(const DRIVE_INFO* pInfo)
{
    if (!pInfo) return FALSE;
    if (IsPhisonFamily(pInfo)) return TRUE;
    if (IsAtaSsdTypeInfo(pInfo)) return TRUE;
    return FALSE;
}

/* Vendor-SSD IDs whose RAW packing is not ATA-standard. BE/BF and F1-F4 stay out. */
static BOOL IsVendorSpecificId(BYTE bID)
{
    if (bID == 0xBE || bID == 0xBF)
        return FALSE;
    if (bID >= 0xA0 && bID <= 0xBD)
        return TRUE;
    if (bID == 0xC3)
        return TRUE;
    if (bID >= 0xE7 && bID <= 0xF0)
        return TRUE;
    if (bID >= 0xF5 && bID <= 0xF8)
        return TRUE;
    return FALSE;
}

static RAW_ENC EncFromInterp(ATTR_INTERP e)
{
    switch (e) {
    case INTERP_LIFE_PCT:    return RAW_ENC_PERCENT;
    case INTERP_TEMPERATURE: return RAW_ENC_TEMP_C;
    case INTERP_COUNTER32:   return RAW_ENC_COUNTER32;
    case INTERP_COUNTER48:   return RAW_ENC_COUNTER48;
    case INTERP_RATE:        return RAW_ENC_EVENTS;
    case INTERP_DURATION:    return RAW_ENC_COUNTER32;
    case INTERP_EVENT_COUNT: return RAW_ENC_EVENTS;
    case INTERP_NORMAL:
    default:                 return RAW_ENC_COUNTER32;
    }
}

static void ApplyPhisonSsdDecode(BYTE bID, ATTR_DECODE* out)
{
    switch (bID) {
    case 0xA0:
        out->szName = "Внезапные выключения";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA1:
        out->szName = "Резервные блоки (осталось)";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xA3:
        out->szName = "Начальные плохие блоки";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA4:
        out->szName = "Всего стираний";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA5:
        out->szName = "Макс. стираний";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA6:
        out->szName = "Мин. стираний";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA7:
        out->szName = "Среднее стираний";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA8:
        out->szName = "Макс. стираний по спецификации";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 75;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA9:
        out->szName = "Заявленный остаток ресурса";
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xAF:
        out->szName = "Сбой защиты питания";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xC0:
        out->szName = "Аварийные отключения питания";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB0:
        out->szName = "Ошибки стирания (худший кристалл)";
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB1:
        out->szName = "Всего циклов выравнивания износа";
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB2:
        out->szName = "Резервные блоки (использовано)";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xB5:
        out->szName = "Ошибки программирования (всего)";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xB6:
        out->szName = "Ошибки стирания (всего)";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xC3:
        out->szName = "ECC (вендор)";
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 50;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xE7:
        out->szName = "Остаток ресурса SSD";
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xE8:
        out->szName = "Резервное пространство";
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xE9:
        out->szName = "Записи NAND (ГиБ)";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 75;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF1:
        out->szName = "Записано LBA";
        out->eEnc = RAW_ENC_LBA;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 90;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF2:
        out->szName = "Прочитано LBA";
        out->eEnc = RAW_ENC_LBA;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 90;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF5:
    case 0xF6:
    case 0xF7:
        out->szName = VendorSpecificAttrName(bID);
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 0;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    default:
        break;
    }
}

void GetAttrDecode(BYTE bID, const DRIVE_INFO* pInfo, ATTR_DECODE* out)
{
    int i;
    const char* ovl;
    BOOL bOverlayTrustsEnc;

    if (!out) return;
    InitVendorAttrNames();
    out->szName = VendorSpecificAttrName(bID);
    out->eEnc = RAW_ENC_UNKNOWN;
    out->eState = ATTR_DECODE_UNKNOWN;
    out->nSemanticConfidence = 0;
    out->eCrit = ATTR_CRIT_NONE;

    i = 0;
    while (g_AttrNames[i].szName != NULL) {
        if (g_AttrNames[i].bID == bID) {
            out->szName = g_AttrNames[i].szName;
            out->eCrit = g_AttrNames[i].eCritLevel;
            out->eEnc = EncFromInterp(g_AttrNames[i].eInterp);
            out->nSemanticConfidence = 60;
            break;
        }
        i++;
    }

    /* Well-known ATA encodings (name already from table). */
    switch (bID) {
    case 0x05: out->eEnc = RAW_ENC_SECTORS_LO16; break;
    case 0x09: out->eEnc = RAW_ENC_HOURS; break;
    case 0x04:
    case 0x0C: out->eEnc = RAW_ENC_CYCLES; break;
    case 0xC2:
    case 0xBE: out->eEnc = RAW_ENC_TEMP_C; break;
    case 0xC4: out->eEnc = RAW_ENC_EVENTS; break;
    case 0xF1:
    case 0xF2:
    case 0xF3:
    case 0xF4: out->eEnc = RAW_ENC_LBA; break;
    default: break;
    }

    ovl = LookupOverlay(OverlayFor(pInfo), bID);
    if (ovl)
        out->szName = ovl;

    /* Overlay on Samsung/Intel/Micron is a trusted profile for that ID.
     * Phison overlay is names-only for E7/E9; encoding comes from ApplyPhison. */
    bOverlayTrustsEnc = FALSE;
    if (ovl && pInfo) {
        switch (pInfo->eController) {
        case CONTROLLER_SAMSUNG:
        case CONTROLLER_INTEL:
        case CONTROLLER_MICRON:
            bOverlayTrustsEnc = TRUE;
            break;
        default:
            break;
        }
    }

    if (IsVendorSpecificId(bID) && !bOverlayTrustsEnc) {
        BOOL bSsdLike = TRUE; /* no pInfo: do not claim a universal encoding */
        if (pInfo) {
            bSsdLike = (pInfo->eType == DRIVE_TYPE_SSD_SATA ||
                        pInfo->eType == DRIVE_TYPE_M2_SATA ||
                        pInfo->eType == DRIVE_TYPE_NVME ||
                        IsPhisonFamily(pInfo));
        }
        if (bSsdLike) {
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            /* Keep the name. Profile (Phison) may restore KNOWN below. */
            if (!ovl && out->nSemanticConfidence > 30)
                out->nSemanticConfidence = 30;
        }
    }

    if (IsPhisonFamily(pInfo))
        ApplyPhisonSsdDecode(bID, out);

    /* SSD C0 is emergency/unsafe power-off count, not HDD head-park. */
    if (bID == 0xC0 && DriveTreatsC0AsPowerLoss(pInfo)) {
        out->szName = "Аварийные отключения питания";
        out->eEnc = RAW_ENC_COUNTER32;
        out->eCrit = ATTR_CRIT_NONE;
        if (out->nSemanticConfidence < 80)
            out->nSemanticConfidence = 80;
    }

    out->eState = (out->eEnc == RAW_ENC_UNKNOWN)
                  ? ATTR_DECODE_UNKNOWN
                  : ATTR_DECODE_KNOWN;
}

const char* GetAttrNameEx(BYTE bID, const DRIVE_INFO* pInfo)
{
    ATTR_DECODE d;
    GetAttrDecode(bID, pInfo, &d);
    return d.szName ? d.szName : VendorSpecificAttrName(bID);
}

ATTR_CRITICALITY GetAttrCriticality(BYTE bID)
{
    int i = 0;
    while (g_AttrNames[i].szName != NULL) {
        if (g_AttrNames[i].bID == bID) return g_AttrNames[i].eCritLevel;
        i++;
    }
    return ATTR_CRIT_NONE;
}

ATTR_INTERP GetAttrInterpretation(BYTE bID)
{
    int i = 0;
    while (g_AttrNames[i].szName != NULL) {
        if (g_AttrNames[i].bID == bID) return g_AttrNames[i].eInterp;
        i++;
    }
    return INTERP_NORMAL;
}

BOOL IsAttrCritical(BYTE bID)
{
    return (GetAttrCriticality(bID) >= ATTR_CRIT_CRITICAL);
}

const char* GetDriveTypeName(DRIVE_TYPE eType)
{
    switch (eType) {
    case DRIVE_TYPE_HDD:      return "HDD";
    case DRIVE_TYPE_SSD_SATA: return "SSD SATA";
    case DRIVE_TYPE_NVME:     return "NVMe SSD";
    case DRIVE_TYPE_USB:      return "USB";
    case DRIVE_TYPE_M2_SATA:  return "M.2 SATA";
    case DRIVE_TYPE_EMMC:     return "eMMC";
    case DRIVE_TYPE_SD:       return "SD";
    case DRIVE_TYPE_SCSI:     return "SAS";
    default:                  return "Неизвестно";
    }
}

const char* GetAccessMethodName(SMART_ACCESS_METHOD eMethod)
{
    switch (eMethod) {
    case SMART_ACCESS_LEGACY_IOCTL:     return "Legacy IOCTL";
    case SMART_ACCESS_ATA_PASSTHROUGH:  return "ATA Pass-Through";
    case SMART_ACCESS_SAT12:            return "SAT-12 (SCSI A1h)";
    case SMART_ACCESS_SAT16:            return "SAT-16 (SCSI 85h)";
    case SMART_ACCESS_STORAGE_QUERY:    return "Storage Query";
    case SMART_ACCESS_NVME_PROTOCOL:    return "NVMe Protocol Query";
    case SMART_ACCESS_NVME_PASSTHROUGH: return "NVMe Pass-Through";
    default:                            return "None";
    }
}

const char* GetHealthStatusName(DRIVE_HEALTH_STATUS eStatus)
{
    switch (eStatus) {
    case HEALTH_STATUS_GOOD:     return "Хорошо";
    case HEALTH_STATUS_CAUTION:  return "Внимание";
    case HEALTH_STATUS_BAD:      return "Плохо";
    case HEALTH_STATUS_WARNING:  return "Плохо"; /* predictive failure displays as BAD */
    default:                     return "Нет данных";
    }
}

const char* GetVendorName(DRIVE_VENDOR eVendor)
{
    switch (eVendor) {
    case VENDOR_SAMSUNG:       return "Samsung";
    case VENDOR_WDC:           return "Western Digital";
    case VENDOR_SEAGATE:       return "Seagate";
    case VENDOR_TOSHIBA:       return "Toshiba";
    case VENDOR_HITACHI:       return "HGST";
    case VENDOR_INTEL:         return "Intel";
    case VENDOR_MICRON:        return "Micron";
    case VENDOR_KINGSTON:      return "Kingston";
    case VENDOR_SANDISK:       return "SanDisk";
    case VENDOR_SKHYNIX:       return "SK Hynix";
    case VENDOR_KIOXIA:        return "Kioxia";
    case VENDOR_ADATA:         return "ADATA";
    case VENDOR_PNY:           return "PNY";
    case VENDOR_CORSAIR:       return "Corsair";
    case VENDOR_LEXAR:         return "Lexar";
    case VENDOR_SILICON_POWER: return "Silicon Power";
    case VENDOR_TEAMGROUP:     return "TeamGroup";
    case VENDOR_GOODRAM:       return "GOODRAM";
    case VENDOR_PLEXTOR:       return "Plextor";
    case VENDOR_OCZ:           return "OCZ";
    case VENDOR_OTHER:         return "Other";
    default:                   return "Unknown";
    }
}

const char* GetControllerName(DRIVE_CONTROLLER eController)
{
    switch (eController) {
    case CONTROLLER_PHISON:   return "Phison";
    case CONTROLLER_SMI:      return "Silicon Motion";
    case CONTROLLER_SAMSUNG:  return "Samsung";
    case CONTROLLER_MARVELL:  return "Marvell";
    case CONTROLLER_INTEL:    return "Intel";
    case CONTROLLER_MICRON:   return "Micron";
    case CONTROLLER_HYNIX:    return "SK Hynix";
    case CONTROLLER_KIOXIA:   return "Kioxia";
    case CONTROLLER_SANDISK:  return "SanDisk";
    case CONTROLLER_REALTEK:  return "Realtek";
    case CONTROLLER_SEAGATE:  return "Seagate";
    case CONTROLLER_WD:       return "Western Digital";
    case CONTROLLER_TOSHIBA:  return "Toshiba";
    case CONTROLLER_HITACHI:  return "HGST";
    case CONTROLLER_AMAZON:   return "Amazon";
    case CONTROLLER_MAXIO:    return "Maxio";
    case CONTROLLER_INNOGRIT: return "Innogrit";
    default:                  return "—";
    }
}

const char* GetNandName(NAND_VENDOR eNand)
{
    switch (eNand) {
    case NAND_SAMSUNG: return "Samsung";
    case NAND_MICRON:  return "Micron";
    case NAND_KIOXIA:  return "Kioxia";
    case NAND_HYNIX:   return "SK Hynix";
    case NAND_INTEL:   return "Intel";
    case NAND_SANDISK: return "SanDisk";
    default:           return "—";
    }
}

/* ============================================================
 * Drive vendor detection from model name
 * Identifies vendors for attribute interpretation
 * ============================================================ */
DRIVE_VENDOR DetectDriveVendor(const char* szModel)
{
    if (!szModel || !szModel[0]) return VENDOR_UNKNOWN;

    /* Convert to uppercase for case-insensitive matching */
    char szUpper[42];
    int i;
    for (i = 0; i < 41 && szModel[i]; i++)
        szUpper[i] = (char)toupper((unsigned char)szModel[i]);
    szUpper[i] = '\0';

    /* MZ-76E500, MZNLN128, MZVLB256 — OEM codes start with MZ + letter. */
    if (strstr(szUpper, "SAMSUNG") || strstr(szUpper, "MZ-") ||
        (szUpper[0] == 'M' && szUpper[1] == 'Z' &&
         szUpper[2] >= 'A' && szUpper[2] <= 'Z') ||
        strstr(szUpper, "PM9") || strstr(szUpper, "SM9") ||
        (strstr(szUpper, "SSD ") && strstr(szUpper, "EVO")) ||
        (strstr(szUpper, "SSD ") && strstr(szUpper, "PRO")))
        return VENDOR_SAMSUNG;

    if (strstr(szUpper, "WDC") || strstr(szUpper, "WD ") ||
        strstr(szUpper, "WESTERN") || strstr(szUpper, "BLUE") ||
        strstr(szUpper, "BLACK") || strstr(szUpper, "GREEN") ||
        strstr(szUpper, "RED ") || strstr(szUpper, "PURPLE") ||
        strstr(szUpper, "GOLD") || strstr(szUpper, "WD20") ||
        strstr(szUpper, "WD30") || strstr(szUpper, "WD40") ||
        strstr(szUpper, "WD50") || strstr(szUpper, "WD60") ||
        strstr(szUpper, "WD80") || strstr(szUpper, "WD10"))
        return VENDOR_WDC;

    if (strstr(szUpper, "ST") && (szUpper[2] >= '0' && szUpper[2] <= '9') &&
        (strstr(szUpper, "SSD") || strstr(szUpper, "DM") || strstr(szUpper, "AS") ||
         strstr(szUpper, "NM") || strstr(szUpper, "VN") || strstr(szUpper, "B8") ||
         strstr(szUpper, "LM") || strstr(szUpper, "BX")))
        return VENDOR_SEAGATE;
    if (strstr(szUpper, "SEAGATE"))
        return VENDOR_SEAGATE;

    if (strstr(szUpper, "TOSHIBA") || strstr(szUpper, "MK") ||
        strstr(szUpper, "DT01") || strstr(szUpper, "DT02") ||
        strstr(szUpper, "MQ01") || strstr(szUpper, "MQ02") ||
        strstr(szUpper, "MG"))
        return VENDOR_TOSHIBA;

    if (strstr(szUpper, "HITACHI") || strstr(szUpper, "HGST") ||
        strstr(szUpper, "HUA") || strstr(szUpper, "HDT") ||
        strstr(szUpper, "HDP") || strstr(szUpper, "HCS") ||
        strstr(szUpper, "IC") || strstr(szUpper, "HTS") ||
        strstr(szUpper, "HMS") || strstr(szUpper, "HUH"))
        return VENDOR_HITACHI;

    if (strstr(szUpper, "INTEL") || strstr(szUpper, "SSDSC") ||
        strstr(szUpper, "SSDPED") || strstr(szUpper, "SSDPE") ||
        strstr(szUpper, "SOLIDIGM"))
        return VENDOR_INTEL;

    if (strstr(szUpper, "MICRON") || strstr(szUpper, "CRUCIAL") ||
        strstr(szUpper, "CT") || strstr(szUpper, "MX") ||
        strstr(szUpper, "M4-") || strstr(szUpper, "MTF"))
        return VENDOR_MICRON;

    if (strstr(szUpper, "KINGSTON") || strstr(szUpper, "SA400") ||
        strstr(szUpper, "SA600") || strstr(szUpper, "SV300") ||
        strstr(szUpper, "SHFS") || strstr(szUpper, "SHSS") ||
        strstr(szUpper, "SKC"))
        return VENDOR_KINGSTON;

    if (strstr(szUpper, "SANDISK") || strstr(szUpper, "SDSS") ||
        strstr(szUpper, "SD8S"))
        return VENDOR_SANDISK;

    if (strstr(szUpper, "SKHYNIX") || strstr(szUpper, "HFS") ||
        strstr(szUpper, "HFM") || strstr(szUpper, "BC7") ||
        strstr(szUpper, "BC5") || strstr(szUpper, "PC7") ||
        strstr(szUpper, "PC5"))
        return VENDOR_SKHYNIX;

    if (strstr(szUpper, "KIOXIA") || strstr(szUpper, "EXCERIA") ||
        strstr(szUpper, "KXG") || strstr(szUpper, "BG"))
        return VENDOR_KIOXIA;

    if (strstr(szUpper, "ADATA") || strstr(szUpper, "SX8") ||
        strstr(szUpper, "SP9") || strstr(szUpper, "SU8") ||
        strstr(szUpper, "IM2P"))
        return VENDOR_ADATA;

    if (strstr(szUpper, "PNY") || strstr(szUpper, "CS"))
        return VENDOR_PNY;

    if (strstr(szUpper, "CORSAIR") || strstr(szUpper, "CSSD") ||
        strstr(szUpper, "FORCE"))
        return VENDOR_CORSAIR;

    if (strstr(szUpper, "LEXAR") || strstr(szUpper, "NM"))
        return VENDOR_LEXAR;

    if (strstr(szUpper, "SILICON POWER") || strstr(szUpper, "SPCC"))
        return VENDOR_SILICON_POWER;

    if (strstr(szUpper, "TEAMGROUP") || strstr(szUpper, "TEAM ") ||
        strstr(szUpper, "TM8") || strstr(szUpper, "T-FORCE") ||
        strstr(szUpper, "TFORCE"))
        return VENDOR_TEAMGROUP;

    if (strstr(szUpper, "GOODRAM") || strstr(szUpper, "IRDM"))
        return VENDOR_GOODRAM;

    if (strstr(szUpper, "PLEXTOR") || strstr(szUpper, "PX-"))
        return VENDOR_PLEXTOR;

    if (strstr(szUpper, "OCZ") || strstr(szUpper, "VERTEX") ||
        strstr(szUpper, "AGILITY"))
        return VENDOR_OCZ;

    /* If model has SSD keyword but vendor unknown */
    if (strstr(szUpper, "SSD") || strstr(szUpper, "NVME"))
        return VENDOR_OTHER;

    return VENDOR_UNKNOWN;
}

static void ToUpperCopy(char* dst, int nDst, const char* src)
{
    int i;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (i = 0; i < nDst - 1 && src[i]; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static BOOL HasSmartAttr(const DRIVE_INFO* p, BYTE id)
{
    int i;
    if (!p || id == 0) return FALSE;
    for (i = 0; i < 30; i++) {
        if (p->attrData.stAttributes[i].bAttrID == id)
            return TRUE;
    }
    return FALSE;
}

static int CountSmartAttrRange(const DRIVE_INFO* p, BYTE lo, BYTE hi)
{
    int n = 0;
    unsigned id;
    for (id = (unsigned)lo; id <= (unsigned)hi; id++) {
        if (HasSmartAttr(p, (BYTE)id)) n++;
    }
    return n;
}

/* NVMe Identify Controller bytes 0-1 are PCI VID (spec). Named fields of
 * NVME_IDENTIFY_CONTROLLER skip this; read the raw copy. Do not invent a VID. */
static USHORT NvmeIdentVid(const DRIVE_INFO* p)
{
    const BYTE* id;
    if (!p || !p->bGotNVMeIdent) return 0;
    id = (const BYTE*)&p->nvmeIdent;
    return (USHORT)id[0] | ((USHORT)id[1] << 8);
}

static DRIVE_CONTROLLER ControllerFromNvmeVid(USHORT vid)
{
    switch (vid) {
    case 0x144D: return CONTROLLER_SAMSUNG;
    case 0x1987:
    case 0x1D97:
    case 0x1C2C: return CONTROLLER_PHISON;
    case 0x126F: return CONTROLLER_SMI;
    case 0x1B4B: return CONTROLLER_MARVELL;   /* PCI SIG: Marvell */
    case 0x1C5C: return CONTROLLER_HYNIX;
    case 0x8086: return CONTROLLER_INTEL;
    case 0x025E: return CONTROLLER_INTEL;    /* Solidigm (ex-Intel NAND) */
    case 0x1E0F:
    case 0x1179: return CONTROLLER_KIOXIA;   /* 1179 = Toshiba/Kioxia PCI */
    case 0x15B7: return CONTROLLER_SANDISK;
    case 0x1B96: return CONTROLLER_WD;
    case 0x10EC: return CONTROLLER_REALTEK;
    case 0x1BB1: return CONTROLLER_SEAGATE;
    case 0x1D0F: return CONTROLLER_AMAZON;
    case 0x1344:
    case 0xC0A9: return CONTROLLER_MICRON;
    case 0x1E4B: return CONTROLLER_MAXIO;
    case 0x1DDC: return CONTROLLER_INNOGRIT;
    default:     return CONTROLLER_UNKNOWN;
    }
}

static DRIVE_CONTROLLER DetectDriveController(const DRIVE_INFO* p)
{
    USHORT vid;
    char szModelU[48], szFwU[16];
    int nPhison;
    BOOL hdd, ssd;

    if (!p) return CONTROLLER_UNKNOWN;

    hdd = (p->eType == DRIVE_TYPE_HDD);
    ssd = (p->eType == DRIVE_TYPE_SSD_SATA ||
           p->eType == DRIVE_TYPE_M2_SATA ||
           p->eType == DRIVE_TYPE_NVME ||
           p->bIsNVMe);

    ToUpperCopy(szFwU, sizeof(szFwU), p->szFirmware);
    ToUpperCopy(szModelU, sizeof(szModelU), p->szModel);

    /* 1. NVMe PCI/identify VID — highest confidence. */
    if (p->bIsNVMe || p->bGotNVMeIdent) {
        vid = NvmeIdentVid(p);
        if (vid) {
            DRIVE_CONTROLLER c = ControllerFromNvmeVid(vid);
            if (c != CONTROLLER_UNKNOWN)
                return c;
        }
    }

    /* 2. Firmware / model strings (work even before SMART). */
    if (strncmp(szFwU, "HPS", 3) == 0 ||
        strncmp(szFwU, "SBFK", 4) == 0 ||
        strncmp(szFwU, "SBFM", 4) == 0 ||
        strncmp(szFwU, "SAFM", 4) == 0 ||
        strncmp(szFwU, "SBFB", 4) == 0 ||
        strncmp(szFwU, "ECFM", 4) == 0 ||
        strncmp(szFwU, "E8FM", 4) == 0 ||
        strncmp(szFwU, "E7FM", 4) == 0)
        return CONTROLLER_PHISON;

    if (strstr(szModelU, "SM226") || strstr(szModelU, "SM225") ||
        strstr(szModelU, "SM232") || strstr(szModelU, "SM250") ||
        strstr(szFwU, "SM226") || strstr(szFwU, "SM225") ||
        strstr(szFwU, "SM232"))
        return CONTROLLER_SMI;

    if (strstr(szModelU, "MAP120") || strstr(szModelU, "MAP160") ||
        strstr(szModelU, "MAP100") || strstr(szFwU, "MAP16") ||
        strstr(szFwU, "MAP12"))
        return CONTROLLER_MAXIO;

    if (strstr(szModelU, "RTS576") || strstr(szModelU, "RTS577") ||
        strstr(szFwU, "RTS576") || strstr(szFwU, "RTL57"))
        return CONTROLLER_REALTEK;

    if (strstr(szModelU, "IG521") || strstr(szModelU, "IG523") ||
        strstr(szModelU, "INNOGRIT"))
        return CONTROLLER_INNOGRIT;

    /* 3. SATA SMART fingerprint. 0xCA is not Micron-only (Samsung has it). */
    nPhison = CountSmartAttrRange(p, 0xA0, 0xA9);
    if (HasSmartAttr(p, 0xA9) || nPhison >= 3)
        return CONTROLLER_PHISON;
    if (HasSmartAttr(p, 0xE7) && HasSmartAttr(p, 0xE9))
        return CONTROLLER_PHISON;

    if (p->eVendor == VENDOR_SAMSUNG && !hdd)
        return CONTROLLER_SAMSUNG;

    if (HasSmartAttr(p, 0xE1) &&
        (p->eVendor == VENDOR_INTEL || p->eVendor == VENDOR_UNKNOWN))
        return CONTROLLER_INTEL;

    if (HasSmartAttr(p, 0xCA) && p->eVendor == VENDOR_MICRON)
        return CONTROLLER_MICRON;

    /* Kingston A400 and similar: consumer SATA is Phison. */
    if (!hdd && p->eVendor == VENDOR_KINGSTON &&
        (HasSmartAttr(p, 0xE7) || HasSmartAttr(p, 0xE9) ||
         HasSmartAttr(p, 0xA9) || p->eType == DRIVE_TYPE_SSD_SATA ||
         p->eType == DRIVE_TYPE_M2_SATA))
        return CONTROLLER_PHISON;

    /* 4. HDD MCU follows brand. In-house SSD silicon too. */
    if (hdd) {
        switch (p->eVendor) {
        case VENDOR_SEAGATE: return CONTROLLER_SEAGATE;
        case VENDOR_WDC:     return CONTROLLER_WD;
        case VENDOR_TOSHIBA: return CONTROLLER_TOSHIBA;
        case VENDOR_HITACHI: return CONTROLLER_HITACHI;
        case VENDOR_SAMSUNG: return CONTROLLER_SAMSUNG;
        default: break;
        }
    } else if (ssd || !hdd) {
        /* Rebrands (Kingston/ADATA/PNY/…) stay UNKNOWN unless fingerprinted. */
        switch (p->eVendor) {
        case VENDOR_SAMSUNG: return CONTROLLER_SAMSUNG;
        case VENDOR_INTEL:   return CONTROLLER_INTEL;
        case VENDOR_MICRON:  return CONTROLLER_MICRON;
        case VENDOR_SKHYNIX: return CONTROLLER_HYNIX;
        case VENDOR_SANDISK: return CONTROLLER_SANDISK;
        case VENDOR_KIOXIA:  return CONTROLLER_KIOXIA;
        default: break;
        }
    }

    return CONTROLLER_UNKNOWN;
}

static NAND_VENDOR DetectNandVendor(const DRIVE_INFO* p)
{
    char szU[48];
    if (!p) return NAND_UNKNOWN;
    if (p->eType == DRIVE_TYPE_HDD)
        return NAND_UNKNOWN;

    ToUpperCopy(szU, sizeof(szU), p->szModel);

    /* Conservative: only well-known Samsung consumer NAND. */
    if (p->eVendor == VENDOR_SAMSUNG) {
        if (strstr(szU, "MZ-") ||
            (szU[0] == 'M' && szU[1] == 'Z' && szU[2] >= 'A' && szU[2] <= 'Z') ||
            strstr(szU, "870") || strstr(szU, "860") ||
            strstr(szU, "850") ||
            strstr(szU, "980") || strstr(szU, "990") ||
            ((strstr(szU, "EVO") || strstr(szU, "PRO")) &&
             (strstr(szU, "SSD") || strstr(szU, "NVME"))))
            return NAND_SAMSUNG;
    }

    /* Intel 6xxp is often Intel/Micron mix — leave UNKNOWN.
     * Never assume Kingston NAND=Kingston or ADATA NAND=ADATA. */
    return NAND_UNKNOWN;
}

void IdentifyDriveParts(DRIVE_INFO* pInfo)
{
    if (!pInfo) return;
    pInfo->eVendor     = DetectDriveVendor(pInfo->szModel);
    pInfo->eController = DetectDriveController(pInfo);
    pInfo->eNand       = DetectNandVendor(pInfo);
}

/* ============================================================
 * Device open
 * ============================================================ */
BOOL OpenDrive(int nDrive, HANDLE* phDrive)
{
    char szPath[32];
    safe_snprintf(szPath, "\\\\.\\PhysicalDrive%d", nDrive);

    *phDrive = CreateFileA(szPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (*phDrive == INVALID_HANDLE_VALUE) {
        *phDrive = CreateFileA(szPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }
    if (*phDrive == INVALID_HANDLE_VALUE) {
        *phDrive = CreateFileA(szPath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }
    return (*phDrive != INVALID_HANDLE_VALUE);
}

BOOL OpenDriveReadOnly(int nDrive, HANDLE* phDrive)
{
    char szPath[32];
    safe_snprintf(szPath, "\\\\.\\PhysicalDrive%d", nDrive);
    *phDrive = CreateFileA(szPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    return (*phDrive != INVALID_HANDLE_VALUE);
}

BYTE GetStorageBusType(HANDLE hDrive)
{
    STORAGE_PROPERTY_QUERY spq;
    BYTE outBuf[1024];
    DWORD dwBytes = 0;

    /* Device descriptor BusType is the drive itself (17 = NVMe).
     * The old code read byte 8 of STORAGE_ADAPTER_DESCRIPTOR, which is
     * MaximumTransferLength, not BusType (offset 24). That made native
     * NVMe look like SCSI/USB and skip the NVMe SMART path. */
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;
    ZeroMemory(outBuf, sizeof(outBuf));
    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                        &spq, sizeof(spq), outBuf, sizeof(outBuf),
                        &dwBytes, NULL) &&
        dwBytes >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(BYTE)) {
        STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;
        return (BYTE)pDesc->BusType;
    }

    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageAdapterProperty;
    spq.QueryType  = PropertyStandardQuery;
    ZeroMemory(outBuf, sizeof(outBuf));
    dwBytes = 0;
    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                        &spq, sizeof(spq), outBuf, sizeof(outBuf),
                        &dwBytes, NULL) && dwBytes >= 25)
        return outBuf[24];

    return 0;
}

BOOL IsUSBDrive(HANDLE hDrive)
{
    return (GetStorageBusType(hDrive) == 7);
}

BOOL IsEMMCDrive(HANDLE hDrive)
{
    BYTE bus = GetStorageBusType(hDrive);
    return (bus == 12 || bus == 13);
}

BOOL IsNVMeDrive(HANDLE hDrive)
{
    BYTE bBusType = GetStorageBusType(hDrive);
    if (bBusType == 17) return TRUE;

    /* Bus 8 = RAID (Intel RST/VMD often owns NVMe). Still try a safe
     * Health Info Log query — if it works, this is NVMe. */
    {
        BYTE health[sizeof(NVME_HEALTH_INFO_LOG)];
        DWORD nCopied = 0;
        ZeroMemory(health, sizeof(health));
        if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, NVME_LOG_PAGE_HEALTH_INFO,
                              health, sizeof(health), &nCopied))
            return TRUE;
    }
    {
        BYTE ident[256];
        DWORD nCopied = 0;
        ZeroMemory(ident, sizeof(ident));
        if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeIdentify, 1,
                              ident, sizeof(ident), &nCopied))
            return TRUE;
    }

    /* For USB drives (bus type 7), check if this might be NVMe-over-USB.
     * product name contains "NVMe", OR the bridge
     * chip is a known NVMe-over-USB bridge (JMicron JMS583/586, ASMedia
     * ASM2362, Realtek RTL9210, VLI VL716/VL717).
     *
     * Many USB-NVMe enclosures DON'T include "NVMe" in their product string,
     * so we also check the bridge chip VID/PID. */
    if (bBusType == 7) {
        /* First check product name for "NVMe" keyword */
        STORAGE_PROPERTY_QUERY spqDev;
        ZeroMemory(&spqDev, sizeof(spqDev));
        spqDev.PropertyId = StorageDeviceProperty;
        spqDev.QueryType  = PropertyStandardQuery;

        BYTE devBuf[1024];
        ZeroMemory(devBuf, sizeof(devBuf));
        DWORD dwDevBytes = 0;

        if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                            &spqDev, sizeof(spqDev), devBuf, sizeof(devBuf),
                            &dwDevBytes, NULL) && dwDevBytes > 32) {
            STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)devBuf;

            /* Check product name for NVMe keyword */
            if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pStr = (const char*)devBuf + pDesc->ProductIdOffset;
                if (strstr(pStr, "NVMe") || strstr(pStr, "NVME") ||
                    strstr(pStr, "nvme"))
                    return TRUE;
            }

            /* Check VID/PID for known NVMe-over-USB bridges.
             * This is critical: many USB-NVMe enclosures don't include
             * "NVMe" in their product string, so VID/PID matching is
             * the primary detection method. */
            WORD wVid = 0, wPid = 0;
            DRIVE_INFO tempInfo;
            ZeroMemory(&tempInfo, sizeof(tempInfo));

            /* Try extracting VID/PID from device descriptor strings */
            if (pDesc->VendorIdOffset && pDesc->VendorIdOffset < dwDevBytes) {
                const char* pVidStr = (const char*)devBuf + pDesc->VendorIdOffset;
                ParseVidPidFromHardwareId(pVidStr, &wVid, &wPid);
            }
            if (wVid == 0 && wPid == 0 &&
                pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pPidStr = (const char*)devBuf + pDesc->ProductIdOffset;
                ParseVidPidFromHardwareId(pPidStr, &wVid, &wPid);
            }

            /* If we got VID/PID from descriptor strings, check for NVMe bridges */
            if (wVid != 0 || wPid != 0) {
                /* JMicron NVMe: VID 152D, PID 0583/0586/058C/058F */
                if (wVid == 0x152D && (wPid == 0x0583 || wPid == 0x0586 ||
                                       wPid == 0x058C || wPid == 0x058F))
                    return TRUE;

                /* ASMedia NVMe: VID 174C, PID 2362/2364 */
                if (wVid == 0x174C && (wPid == 0x2362 || wPid == 0x2364))
                    return TRUE;

                /* Realtek NVMe: VID 0BDA, PID 9210/9220/9221 */
                if (wVid == 0x0BDA && (wPid == 0x9210 || wPid == 0x9220 || wPid == 0x9221))
                    return TRUE;

                /* VLI NVMe: VID 2109, PID 0900/0901/0902 */
                if (wVid == 0x2109 && (wPid == 0x0900 || wPid == 0x0901 || wPid == 0x0902))
                    return TRUE;
            }

            /* Also check product name for known NVMe bridge model numbers */
            if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pProd = (const char*)devBuf + pDesc->ProductIdOffset;
                char szUpper[65];
                int k;
                for (k = 0; k < 64 && pProd[k]; k++)
                    szUpper[k] = (char)toupper((unsigned char)pProd[k]);
                szUpper[k] = '\0';

                if (strstr(szUpper, "JMS583") || strstr(szUpper, "JMS586") ||
                    strstr(szUpper, "ASM2362") || strstr(szUpper, "RTL9210") ||
                    strstr(szUpper, "VL716") || strstr(szUpper, "VL717") ||
                    strstr(szUpper, "NL6221"))
                    return TRUE;
            }

            /* Vendor names like "JMicron" also appear on SATA bridges, so a
             * vendor-only match is not enough to claim NVMe. Product-id
             * checks above already cover the known NVMe bridge chips. */
        }
    }
    return FALSE;
}

/* ============================================================
 * Generic STORAGE_DEVICE_DESCRIPTOR reader
 * ============================================================ */
BOOL GetDeviceDescriptor(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    ZeroMemory(outBuf, sizeof(outBuf));
    DWORD dwBytes = 0;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    if (dwBytes < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    if (pInfo->szModel[0] == '\0')
        CopyDescStr(pInfo->szModel, sizeof(pInfo->szModel),
                    outBuf, dwBytes, pDesc->ProductIdOffset);
    if (pInfo->szSerial[0] == '\0')
        CopyDescStr(pInfo->szSerial, sizeof(pInfo->szSerial),
                    outBuf, dwBytes, pDesc->SerialNumberOffset);
    if (pInfo->szFirmware[0] == '\0')
        CopyDescStr(pInfo->szFirmware, sizeof(pInfo->szFirmware),
                    outBuf, dwBytes, pDesc->ProductRevisionOffset);
    return TRUE;
}

/* ============================================================
 * USB/SCSI bridge identification
 * ============================================================ */
BOOL GetBridgeIdentity(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    ZeroMemory(outBuf, sizeof(outBuf));
    DWORD dwBytes = 0;

    pInfo->szBridgeVendor[0]  = '\0';
    pInfo->szBridgeProduct[0] = '\0';
    pInfo->wUsbVid = 0;
    pInfo->wUsbPid = 0;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    if (dwBytes < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    CopyDescStr(pInfo->szBridgeVendor, sizeof(pInfo->szBridgeVendor),
                outBuf, dwBytes, pDesc->VendorIdOffset);
    CopyDescStr(pInfo->szBridgeProduct, sizeof(pInfo->szBridgeProduct),
                outBuf, dwBytes, pDesc->ProductIdOffset);

    /* Try to extract VID/PID from the (already bounded) descriptor strings.
     * Some USB bridges include VID/PID in the vendor or product strings. */
    if (pInfo->szBridgeVendor[0])
        ParseVidPidFromHardwareId(pInfo->szBridgeVendor, &pInfo->wUsbVid, &pInfo->wUsbPid);
    if (pInfo->wUsbVid == 0 && pInfo->wUsbPid == 0 && pInfo->szBridgeProduct[0])
        ParseVidPidFromHardwareId(pInfo->szBridgeProduct, &pInfo->wUsbVid, &pInfo->wUsbPid);

    /* Also try SCSI INQUIRY to get bridge vendor/product for more accurate detection.
     * SCSI INQUIRY for USB bridge identification. */
    {
        CDI_SAT_PASSTHROUGH_BUF sptwb;
        DWORD dwInqBytes = 0;
        ZeroMemory(&sptwb, sizeof(sptwb));

        sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
        sptwb.spt.PathId             = 0;
        sptwb.spt.TargetId           = 0;
        sptwb.spt.Lun                = 0;
        sptwb.spt.CdbLength          = 6;
        sptwb.spt.SenseInfoLength    = 32;
        sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = 96;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
        sptwb.spt.TimeOutValue       = 10;

        /* Standard SCSI INQUIRY */
        sptwb.spt.Cdb[0] = 0x12;   /* INQUIRY */
        sptwb.spt.Cdb[4] = 96;     /* Allocation length */

        DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf) + 96;

        if (DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
                &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwInqBytes, NULL) &&
            sptwb.spt.ScsiStatus == 0) {
            /* Parse INQUIRY data: Vendor at offset 8 (8 bytes), Product at offset 16 (16 bytes) */
            BYTE* pInq = sptwb.DataBuf;
            if (pInq[0] != 0xFF && pInq[4] >= 0x1F) {
                /* Valid INQUIRY response */
                if (pInfo->szBridgeVendor[0] == '\0') {
                    memcpy(pInfo->szBridgeVendor, pInq + 8, 8);
                    pInfo->szBridgeVendor[8] = '\0';
                    TrimStr(pInfo->szBridgeVendor);
                }
                if (pInfo->szBridgeProduct[0] == '\0') {
                    memcpy(pInfo->szBridgeProduct, pInq + 16, 16);
                    pInfo->szBridgeProduct[16] = '\0';
                    TrimStr(pInfo->szBridgeProduct);
                }
            }
        }
    }

    return (pInfo->szBridgeVendor[0] != '\0' || pInfo->szBridgeProduct[0] != '\0');
}

/* ============================================================
 * USB VID/PID retrieval via SetupDi API
 * The VID/PID is extracted from the device's hardware ID string
 * in the Windows registry (SPDRP_HARDWAREID), which contains
 * entries like "USB\\VID_152D&PID_0583".
 * This is the most reliable way to identify USB bridge chips.
 * ============================================================ */

/* Parse "VID_xxxx&PID_xxxx" from a hardware ID string
 * (Definition — forward declared at top of file for use in IsNVMeDrive/GetBridgeIdentity) */
static BOOL ParseVidPidFromHardwareId(const char* szHwId, WORD* pwVid, WORD* pwPid)
{
    const char* pVid = strstr(szHwId, "VID_");
    const char* pPid = strstr(szHwId, "PID_");
    if (!pVid || !pPid) return FALSE;

    /* Parse VID (4 hex digits after "VID_") */
    char szVid[5] = {0};
    int i;
    for (i = 0; i < 4; i++) {
        char c = pVid[4 + i];
        if (!c) return FALSE;
        szVid[i] = (char)toupper((unsigned char)c);
    }

    /* Parse PID (4 hex digits after "PID_") */
    char szPid[5] = {0};
    for (i = 0; i < 4; i++) {
        char c = pPid[4 + i];
        if (!c) return FALSE;
        szPid[i] = (char)toupper((unsigned char)c);
    }

    *pwVid = (WORD)strtol(szVid, NULL, 16);
    *pwPid = (WORD)strtol(szPid, NULL, 16);
    return (*pwVid != 0 || *pwPid != 0);
}

/* Get the device instance path for a PhysicalDrive handle.*/
static BOOL GetDevicePathFromHandle(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    /* Walk the device tree from a PhysicalDrive handle to find the
     * USB parent device and extract its VID/PID.  This is the same */
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    /* Enumerate all disk class devices and match by adapter/serial */
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_DISK,
                                              NULL, NULL,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);

    BOOL bFound = FALSE;
    DWORD dwIndex = 0;

    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL,
                                       &GUID_DEVINTERFACE_DISK, dwIndex, &did)) {
        dwIndex++;

        /* Get required buffer size */
        DWORD dwReqSize = 0;
        SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, NULL, 0, &dwReqSize, NULL);
        if (dwReqSize == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A* pDetail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, dwReqSize);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        SP_DEVINFO_DATA dd;
        dd.cbSize = sizeof(dd);

        if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, pDetail,
                                              dwReqSize, &dwReqSize, &dd)) {
            /* Check if this device path matches our PhysicalDrive */
            const char* szDetailPath = pDetail->DevicePath;

            /* Try to open this device path and compare with our handle */
            HANDLE hTest = CreateFileA(szDetailPath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, 0, NULL);

            if (hTest != INVALID_HANDLE_VALUE) {
                /* Compare by getting disk number */
                STORAGE_DEVICE_NUMBER sdn1, sdn2;
                DWORD dwRet1 = 0, dwRet2 = 0;
                BOOL bOk1 = DeviceIoControl(hTest, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &sdn1, sizeof(sdn1), &dwRet1, NULL);
                BOOL bOk2 = DeviceIoControl(hDrive, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &sdn2, sizeof(sdn2), &dwRet2, NULL);

                if (bOk1 && bOk2 && sdn1.DeviceNumber == sdn2.DeviceNumber) {
                    /* Match found - now walk up to find USB parent */
                    DEVINST devInst = dd.DevInst;
                    DEVINST parentInst = 0;
                    WORD wVid = 0, wPid = 0;

                    /* Walk up the device tree looking for USB\VID_ */
                    int maxWalk = 20;
                    while (maxWalk-- > 0) {
                        CHAR szHwId[512] = {0};
                        if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &dd,
                                SPDRP_HARDWAREID, NULL,
                                (BYTE*)szHwId, sizeof(szHwId), NULL)) {
                            if (ParseVidPidFromHardwareId(szHwId, &wVid, &wPid)) {
                                bFound = TRUE;
                                if (pwVid) *pwVid = wVid;
                                if (pwPid) *pwPid = wPid;
                                break;
                            }
                        }

                        /* Try parent device */
                        if (CM_Get_Parent(&parentInst, devInst, 0) != CR_SUCCESS)
                            break;

                        /* Get hardware ID of parent */
                        ULONG ulSize = 0;
                        CM_Get_Device_IDA(parentInst, NULL, 0, 0);
                        CM_Get_Device_ID_Size(&ulSize, parentInst, 0);
                        ulSize += 2;
                        char* szParentId = (char*)HeapAlloc(
                            GetProcessHeap(), HEAP_ZERO_MEMORY, ulSize);
                        if (szParentId) {
                            if (CM_Get_Device_IDA(parentInst, szParentId, ulSize, 0) == CR_SUCCESS) {
                                if (ParseVidPidFromHardwareId(szParentId, &wVid, &wPid)) {
                                    bFound = TRUE;
                                    if (pwVid) *pwVid = wVid;
                                    if (pwPid) *pwPid = wPid;
                                    HeapFree(GetProcessHeap(), 0, szParentId);
                                    break;
                                }
                            }
                            HeapFree(GetProcessHeap(), 0, szParentId);
                        }

                        devInst = parentInst;
                    }
                }
                CloseHandle(hTest);
            }
        }

        HeapFree(GetProcessHeap(), 0, pDetail);
        if (bFound) break;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return bFound;
}

/* Fallback: try reading VID/PID from the STORAGE_DEVICE_DESCRIPTOR's
 * VendorID offset, which may contain USB VID info for some bridge chips.
 * Also try parsing the hardware ID from the device's SCSI address. */
static BOOL GetUSBVidPidFallback(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    /* Try to get VID/PID from the device descriptor's raw properties.
     * Some USB bridge chips expose VID/PID via the STORAGE_DEVICE_DESCRIPTOR. */
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    /* Try to parse VID/PID from the VendorId string.
     * Some USB-SATA bridges put "VID_xxxx&PID_xxxx" in the vendor ID. */
    if (pDesc->VendorIdOffset && pDesc->VendorIdOffset < dwBytes) {
        const char* pStr = (const char*)outBuf + pDesc->VendorIdOffset;
        if (ParseVidPidFromHardwareId(pStr, pwVid, pwPid))
            return TRUE;
    }

    /* Try from ProductId string */
    if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwBytes) {
        const char* pStr = (const char*)outBuf + pDesc->ProductIdOffset;
        if (ParseVidPidFromHardwareId(pStr, pwVid, pwPid))
            return TRUE;
    }

    return FALSE;
}

BOOL GetUSBVidPid(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    if (pwVid) *pwVid = 0;
    if (pwPid) *pwPid = 0;

    /* Method 1: SetupDi API — walk device tree to find USB parent (CDI primary method) */
    if (GetDevicePathFromHandle(hDrive, pwVid, pwPid))
        return TRUE;

    /* Method 2: Parse from STORAGE_DEVICE_DESCRIPTOR strings (fallback) */
    if (GetUSBVidPidFallback(hDrive, pwVid, pwPid))
        return TRUE;

    return FALSE;
}

static BOOL GetCapacityFromGeometry(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    DISK_GEOMETRY_EX geo;
    ZeroMemory(&geo, sizeof(geo));
    DWORD dwBytes = 0;
    if (DeviceIoControl(hDrive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        NULL, 0, &geo, sizeof(geo), &dwBytes, NULL)) {
        DWORD dwCapMB = (DWORD)(geo.DiskSize.QuadPart / (1024 * 1024));
        if (dwCapMB > pInfo->dwCapacityMB) pInfo->dwCapacityMB = dwCapMB;
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * Legacy SMART IOCTL path
 *
 * SMART_SEND_DRIVE_COMMAND / SMART_RCV_DRIVE_DATA: bDriveNumber is the
 * IDE target on that controller (0..3), not PhysicalDriveN. The handle
 * is already \\.\PhysicalDriveN, so bDriveNumber must be 0.
 * ============================================================ */
BOOL EnableSMART(HANDLE hDrive, int nDrive)
{
    SENDCMDINPARAMS cip;
    SENDCMDOUTPARAMS cop;
    DWORD dwBytes = 0;
    ZeroMemory(&cip, sizeof(cip));
    ZeroMemory(&cop, sizeof(cop));

    cip.cBufferSize                 = 0;
    cip.irDriveRegs.bFeaturesReg    = SMART_ENABLE;
    cip.irDriveRegs.bSectorCountReg = 1;
    cip.irDriveRegs.bSectorNumberReg= 1;
    cip.irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    cip.irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    cip.irDriveRegs.bDriveHeadReg   = 0xA0;
    cip.irDriveRegs.bCommandReg     = SMART_CMD;
    cip.bDriveNumber                = 0;  /* handle is already the drive */

    return DeviceIoControl(hDrive, SMART_SEND_DRIVE_COMMAND,
        &cip, sizeof(SENDCMDINPARAMS) - 1,
        &cop, sizeof(SENDCMDOUTPARAMS) - 1,
        &dwBytes, NULL);
}

BOOL GetIdentifyData(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf,  sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = IDENTIFY_BUFFER_SIZE;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = ID_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    WORD* pIdent = (WORD*)pCop->bBuffer;

    if (IsBufferAllZero((BYTE*)pIdent, 64)) return FALSE;

    SwapATAString(pInfo->szSerial,   &pIdent[10], 10);
    SwapATAString(pInfo->szFirmware, &pIdent[23], 4);
    SwapATAString(pInfo->szModel,    &pIdent[27], 20);

    DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = ((unsigned __int64)pIdent[100]) |
                    ((unsigned __int64)pIdent[101] << 16) |
                    ((unsigned __int64)pIdent[102] << 32) |
                    ((unsigned __int64)pIdent[103] << 48);
    }
    if (qwSectors == 0) qwSectors = (unsigned __int64)dwSectors28;
    pInfo->dwCapacityMB = (DWORD)(qwSectors * 512 / (1024 * 1024));

    /* Word 82-84: Command set/feature support */
    pInfo->bSMART_Supported = (pIdent[82] & 0x0001) ? TRUE : FALSE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;

    /* Word 217: Nominal Media Rotation Rate
     * 0x0001 = non-rotating (SSD), >= 0x0401 = RPM */
    pInfo->wRotationRate = pIdent[217];

    /* Word 76/77: SATA gen / negotiated speed → szProtocol */
    FillAtaProtocolFromIdent(pInfo, pIdent);

    /* Word 88: Ultra DMA mode support 
     * for performance calculation — current vs max UDMA mode)
     * Bits 0-7 = supported modes, Bits 8-15 = selected mode */
    /* (We store this for potential use by CalculatePerformance) */

    return TRUE;
}

BOOL GetSMARTAttributes(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + READ_ATTRIBUTE_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf, sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = READ_ATTRIBUTE_BUFFER_SIZE;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_DATA;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    if (IsBufferAllZero(pCop->bBuffer + 2, 30)) return FALSE;

    return FillSmartData(pInfo, pCop->bBuffer) > 0;
}

BOOL GetSMARTThresholds(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + READ_THRESHOLD_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf,  sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = READ_THRESHOLD_BUFFER_SIZE;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_THRESHOLDS;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    FillSmartThreshold(pInfo, pCop->bBuffer);
    return TRUE;
}

BOOL GetSMARTPredictFailure(HANDLE hDrive, int nDrive, BOOL* pbFail)
{
#pragma pack(push,1)
    typedef struct {
        DWORD        cBufferSize;
        DRIVERSTATUS DriverStatus;
        BYTE         bBuffer[16];
    } MY_OUTPARAMS;
#pragma pack(pop)

    SENDCMDINPARAMS cip;
    MY_OUTPARAMS    cop;
    DWORD dwBytes = 0;
    ZeroMemory(&cip, sizeof(cip));
    ZeroMemory(&cop, sizeof(cop));
    *pbFail = FALSE;

    cip.cBufferSize                 = 0;
    cip.irDriveRegs.bFeaturesReg    = SMART_RETURN_STATUS;
    cip.irDriveRegs.bSectorCountReg = 1;
    cip.irDriveRegs.bSectorNumberReg= 1;
    cip.irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    cip.irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    cip.irDriveRegs.bDriveHeadReg   = 0xA0;
    cip.irDriveRegs.bCommandReg     = SMART_CMD;
    cip.bDriveNumber                = 0;  /* handle is already the drive */

    BOOL bOK = DeviceIoControl(hDrive, SMART_SEND_DRIVE_COMMAND,
        &cip, sizeof(SENDCMDINPARAMS) - 1,
        &cop, sizeof(MY_OUTPARAMS), &dwBytes, NULL);

    if (bOK && cop.DriverStatus.bDriverError == 0) {
        if (cop.bBuffer[3] == 0xF4 && cop.bBuffer[4] == 0x2C)
            *pbFail = TRUE;
    }
    return bOK;
}

/* ============================================================
 * SMART Error Log reading
 * Provides diagnostic detail beyond attribute raw values
 * ============================================================ */
static BOOL ReadSMARTLog(HANDLE hDrive, int nDrive, BYTE bLogAddr,
                         BYTE* pOutBuf, DWORD dwBufSize)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + 512];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf, sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = 512;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_LOG;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= bLogAddr;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, pCop->bBuffer, dwCopy);
    return TRUE;
}

static BOOL ReadSMARTLogATAPassthrough(HANDLE hDrive, BYTE bLogAddr,
                                       BYTE* pOutBuf, DWORD dwBufSize)
{
    /* Use ATA pass-through to read SMART log */
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4 + 512];
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED | ATA_FLAGS_DATA_IN;
    pApt->DataTransferLength = 512;
    pApt->TimeOutValue       = 10;
    pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

    pApt->CurrentTaskFile[0] = SMART_READ_LOG;     /* Features */
    pApt->CurrentTaskFile[1] = 1;                   /* Sector count */
    pApt->CurrentTaskFile[2] = bLogAddr;            /* LBA Low = log address */
    pApt->CurrentTaskFile[3] = SMART_CYL_LOW;       /* LBA Mid */
    pApt->CurrentTaskFile[4] = SMART_CYL_HI;        /* LBA High */
    pApt->CurrentTaskFile[5] = 0xA0;                /* Device */
    pApt->CurrentTaskFile[6] = SMART_CMD;            /* Command */

    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL))
        return FALSE;

    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, dwCopy);
    return TRUE;
}

static BOOL ReadSMARTLogSAT(HANDLE hDrive, BYTE bLogAddr,
                            BYTE* pOutBuf, DWORD dwBufSize)
{
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;
    BYTE data[512];
    ZeroMemory(data, sizeof(data));

    if (!SATSendCommand(hDrive, SMART_READ_LOG, 1, bLogAddr,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, 512, &method))
        return FALSE;

    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, data, dwCopy);
    return TRUE;
}

BOOL GetSMARTErrorLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTErrorLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTErrorLogSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLogSAT(hDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLogSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLogSAT(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * Native ATA pass-through
 * ============================================================ */
static BOOL ATAPassThrough(HANDLE hDrive, BYTE bCommand, BYTE bFeatures,
    BYTE bSectorCount, BYTE bLBALow, BYTE bLBAMid, BYTE bLBAHigh,
    BYTE bDevice, BYTE* pDataBuf, DWORD dwDataLen, BOOL bDataIn)
{
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4 + 512];
    if (dwDataLen > 512) return FALSE;
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED |
                               (bDataIn ? ATA_FLAGS_DATA_IN : 0);
    pApt->DataTransferLength = dwDataLen;
    pApt->TimeOutValue       = 10;
    pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

    pApt->CurrentTaskFile[0] = bFeatures;
    pApt->CurrentTaskFile[1] = bSectorCount;
    pApt->CurrentTaskFile[2] = bLBALow;
    pApt->CurrentTaskFile[3] = bLBAMid;
    pApt->CurrentTaskFile[4] = bLBAHigh;
    pApt->CurrentTaskFile[5] = bDevice;
    pApt->CurrentTaskFile[6] = bCommand;

    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL))
        return FALSE;

    if (bDataIn && pDataBuf && dwDataLen > 0)
        memcpy(pDataBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, dwDataLen);
    return TRUE;
}

BOOL GetIdentifyDataATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[IDENTIFY_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!ATAPassThrough(hDrive, ID_CMD, 0, 1, 0, 0, 0, 0xA0,
                        data, IDENTIFY_BUFFER_SIZE, TRUE))
        return FALSE;

    if (IsBufferAllZero(data, 64)) return FALSE;

    WORD* pIdent = (WORD*)data;
    SwapATAString(pInfo->szSerial,   &pIdent[10], 10);
    SwapATAString(pInfo->szFirmware, &pIdent[23], 4);
    SwapATAString(pInfo->szModel,    &pIdent[27], 20);

    DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = ((unsigned __int64)pIdent[100]) |
                    ((unsigned __int64)pIdent[101] << 16) |
                    ((unsigned __int64)pIdent[102] << 32) |
                    ((unsigned __int64)pIdent[103] << 48);
    }
    if (qwSectors == 0) qwSectors = (unsigned __int64)dwSectors28;
    if (qwSectors > 0)
        pInfo->dwCapacityMB = (DWORD)(qwSectors * 512 / (1024 * 1024));

    pInfo->bSMART_Supported = (pIdent[82] & 0x0001) ? TRUE : FALSE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;
    pInfo->wRotationRate    = pIdent[217];
    FillAtaProtocolFromIdent(pInfo, pIdent);
    return TRUE;
}

BOOL GetSMARTAttributesATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_ATTRIBUTE_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    ATAPassThrough(hDrive, SMART_CMD, SMART_ENABLE, 1, 1,
                   SMART_CYL_LOW, SMART_CYL_HI, 0xA0, NULL, 0, FALSE);

    if (!ATAPassThrough(hDrive, SMART_CMD, SMART_READ_DATA, 1, 0,
                        SMART_CYL_LOW, SMART_CYL_HI, 0xA0,
                        data, READ_ATTRIBUTE_BUFFER_SIZE, TRUE))
        return FALSE;

    if (IsBufferAllZero(data + 2, 30) || IsBufferAllFF(data + 2, 30))
        return FALSE;

    return FillSmartData(pInfo, data) > 0;
}

BOOL GetSMARTThresholdsATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_THRESHOLD_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!ATAPassThrough(hDrive, SMART_CMD, SMART_READ_THRESHOLDS, 1, 0,
                        SMART_CYL_LOW, SMART_CYL_HI, 0xA0,
                        data, READ_THRESHOLD_BUFFER_SIZE, TRUE))
        return FALSE;

    FillSmartThreshold(pInfo, data);
    return TRUE;
}

/* ============================================================
 * SAT (SCSI/ATA Translation) — for USB enclosures & SAS
 * ============================================================ */
static BOOL SATSendCommand12(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.CdbLength          = 12;
    sptwb.spt.SenseInfoLength    = 32;
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue       = 30;

    if (pDataBuf && dwDataLen > 0) {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = dwDataLen;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    } else {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_UNSPECIFIED;
        sptwb.spt.DataTransferLength = 0;
        sptwb.spt.DataBufferOffset   = 0;
    }

    sptwb.spt.Cdb[0] = SAT_ATA_PASSTHROUGH_12;
    sptwb.spt.Cdb[1] = bProtocol;
    sptwb.spt.Cdb[2] = (pDataBuf && dwDataLen > 0)
                         ? (SAT_FLAGS_CK_COND | SAT_FLAGS_TDIR_FROM_DEV | SAT_FLAGS_BYTE_BLOCK | SAT_FLAGS_TLEN_SECTOR_CNT)
                         : SAT_FLAGS_CK_COND;
    sptwb.spt.Cdb[3] = bFeatures;
    sptwb.spt.Cdb[4] = bSectorCnt;
    sptwb.spt.Cdb[5] = bLBALow;
    sptwb.spt.Cdb[6] = bCylLow;
    sptwb.spt.Cdb[7] = bCylHigh;
    sptwb.spt.Cdb[8] = 0xA0;
    sptwb.spt.Cdb[9] = bCommand;

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    if (pDataBuf && dwDataLen > 0) {
        dwInLen += dwDataLen;
    }

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL))
        return FALSE;

    if (sptwb.spt.ScsiStatus == 0x08 || sptwb.spt.ScsiStatus == 0x04)
        return FALSE;

    if (pDataBuf && dwDataLen > 0 && sptwb.spt.DataTransferLength > 0) {
        DWORD dwCopy = dwDataLen;
        if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
        if (dwCopy > 512) dwCopy = 512;
        memcpy(pDataBuf, sptwb.DataBuf, dwCopy);
    }

    return TRUE;
}

static BOOL SATSendCommand16(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.CdbLength          = 16;
    sptwb.spt.SenseInfoLength    = 32;
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue       = 30;

    if (pDataBuf && dwDataLen > 0) {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = dwDataLen;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    } else {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_UNSPECIFIED;
        sptwb.spt.DataTransferLength = 0;
        sptwb.spt.DataBufferOffset   = 0;
    }

    /* CK_COND flag (0x20) for SAT commands.
     * Many USB-SATA bridge chips (especially JMicron) require this flag. */
    sptwb.spt.Cdb[0]  = SAT_ATA_PASSTHROUGH_16;
    sptwb.spt.Cdb[1]  = bProtocol;
    sptwb.spt.Cdb[2]  = (pDataBuf && dwDataLen > 0)
                          ? (SAT_FLAGS_CK_COND | SAT_FLAGS_TDIR_FROM_DEV | SAT_FLAGS_BYTE_BLOCK | SAT_FLAGS_TLEN_SECTOR_CNT)
                          : SAT_FLAGS_CK_COND;
    sptwb.spt.Cdb[3]  = 0;
    sptwb.spt.Cdb[4]  = bFeatures;
    sptwb.spt.Cdb[5]  = 0;
    sptwb.spt.Cdb[6]  = bSectorCnt;
    sptwb.spt.Cdb[7]  = 0;
    sptwb.spt.Cdb[8]  = bLBALow;
    sptwb.spt.Cdb[9]  = 0;
    sptwb.spt.Cdb[10] = bCylLow;
    sptwb.spt.Cdb[11] = 0;
    sptwb.spt.Cdb[12] = bCylHigh;
    sptwb.spt.Cdb[13] = 0xA0;  /* LBA mode */
    sptwb.spt.Cdb[14] = bCommand;
    sptwb.spt.Cdb[15] = 0;

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    if (pDataBuf && dwDataLen > 0) {
        dwInLen += dwDataLen;
    }

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL))
        return FALSE;

    /* When CK_COND is set, ScsiStatus may be 0x02 (CHECK CONDITION)
     * which is NORMAL per SAT spec — the ATA status is returned in sense data.
     * Only treat it as failure if ScsiStatus indicates a transport error. */
    if (sptwb.spt.ScsiStatus == 0x08 ||   /* BUSY */
        sptwb.spt.ScsiStatus == 0x04 ||   /* CONDITION MET (abnormal for SAT) */
        sptwb.spt.ScsiStatus == 0x18)     /* RESERVATION CONFLICT */
        return FALSE;

    /* Copy data from embedded buffer to caller's buffer */
    if (pDataBuf && dwDataLen > 0 && sptwb.spt.DataTransferLength > 0) {
        DWORD dwCopy = dwDataLen;
        if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
        if (dwCopy > 512) dwCopy = 512;
        memcpy(pDataBuf, sptwb.DataBuf, dwCopy);
    }

    return TRUE;
}

static BOOL SATSendCommand(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen, SMART_ACCESS_METHOD* pMethod)
{
    if (SATSendCommand16(hDrive, bFeatures, bSectorCnt, bLBALow,
                         bCylLow, bCylHigh, bCommand, bProtocol, pDataBuf, dwDataLen)) {
        if (pMethod) *pMethod = SMART_ACCESS_SAT16;
        return TRUE;
    }
    if (SATSendCommand12(hDrive, bFeatures, bSectorCnt, bLBALow,
                         bCylLow, bCylHigh, bCommand, bProtocol, pDataBuf, dwDataLen)) {
        if (pMethod) *pMethod = SMART_ACCESS_SAT12;
        return TRUE;
    }
    return FALSE;
}

BOOL EnableSMARTSAT(HANDLE hDrive)
{
    SMART_ACCESS_METHOD m = SMART_ACCESS_NONE;
    return SATSendCommand(hDrive, SMART_ENABLE, 1, 1,
        SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
        SAT_PROTO_NON_DATA, NULL, 0, &m);
}

BOOL GetIdentifyDataSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[IDENTIFY_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    if (!SATSendCommand(hDrive, 0, 1, 0, 0, 0,
            ID_CMD, SAT_PROTO_PIO_IN, data, IDENTIFY_BUFFER_SIZE, &method))
        return FALSE;

    if (IsBufferAllZero(data, 64)) return FALSE;

    WORD* pIdent = (WORD*)data;
    SwapATAString(pInfo->szSerial,   &pIdent[10], 10);
    SwapATAString(pInfo->szFirmware, &pIdent[23], 4);
    SwapATAString(pInfo->szModel,    &pIdent[27], 20);

    DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = ((unsigned __int64)pIdent[100]) |
                    ((unsigned __int64)pIdent[101] << 16) |
                    ((unsigned __int64)pIdent[102] << 32) |
                    ((unsigned __int64)pIdent[103] << 48);
    }
    if (qwSectors == 0) qwSectors = (unsigned __int64)dwSectors28;
    if (qwSectors > 0)
        pInfo->dwCapacityMB = (DWORD)(qwSectors * 512 / (1024 * 1024));

    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;
    pInfo->bIsUSB           = TRUE;
    pInfo->eAccessMethod    = method;
    pInfo->wRotationRate    = pIdent[217];
    FillAtaProtocolFromIdent(pInfo, pIdent);
    return TRUE;
}

BOOL GetSMARTAttributesSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_ATTRIBUTE_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    EnableSMARTSAT(hDrive);

    if (!SATSendCommand(hDrive, SMART_READ_DATA, 1, 0x00,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, READ_ATTRIBUTE_BUFFER_SIZE, &method)) {
        pInfo->dwErrSat16 = GetLastError();
        pInfo->dwErrSat12 = pInfo->dwErrSat16;
        return FALSE;
    }

    if (IsBufferAllZero(data + 2, 30) || IsBufferAllFF(data + 2, 30))
        return FALSE;

    if (FillSmartData(pInfo, data) <= 0)
        return FALSE;
    if (pInfo->eAccessMethod == SMART_ACCESS_NONE) pInfo->eAccessMethod = method;
    return TRUE;
}

BOOL GetSMARTThresholdsSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_THRESHOLD_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    if (!SATSendCommand(hDrive, SMART_READ_THRESHOLDS, 1, 0x00,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, READ_THRESHOLD_BUFFER_SIZE, &method))
        return FALSE;

    FillSmartThreshold(pInfo, data);
    return TRUE;
}

/* ============================================================
 * SCSI LOG SENSE
 * ============================================================ */
#define SCSI_LOG_SENSE_CMD          0x4D
#define SCSI_LOGPAGE_TEMPERATURE    0x0D
#define SCSI_LOGPAGE_INFO_EXCEPTIONS 0x2F
#define SCSI_ASC_FAILURE_PREDICTED  0x5D

static BOOL SCSILogSense(HANDLE hDrive, BYTE bPageCode, BYTE* pOut, DWORD dwOutLen, DWORD* pdwLastError)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length              = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.CdbLength           = 10;
    sptwb.spt.SenseInfoLength     = 32;
    sptwb.spt.SenseInfoOffset     = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue        = 10;
    sptwb.spt.DataIn              = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength  = dwOutLen;
    sptwb.spt.DataBufferOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);

    sptwb.spt.Cdb[0] = SCSI_LOG_SENSE_CMD;
    sptwb.spt.Cdb[1] = 0x00;
    sptwb.spt.Cdb[2] = (BYTE)(0x40 | (bPageCode & 0x3F));
    sptwb.spt.Cdb[3] = 0x00;
    sptwb.spt.Cdb[7] = (BYTE)((dwOutLen >> 8) & 0xFF);
    sptwb.spt.Cdb[8] = (BYTE)(dwOutLen & 0xFF);

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf) + dwOutLen;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL)) {
        if (pdwLastError) *pdwLastError = GetLastError();
        return FALSE;
    }
    if (sptwb.spt.ScsiStatus != 0) {
        if (pdwLastError) *pdwLastError = (DWORD)0x10000 + sptwb.spt.ScsiStatus;
        return FALSE;
    }

    /* Copy data from embedded buffer */
    DWORD dwCopy = dwOutLen;
    if (dwCopy > sizeof(sptwb.DataBuf)) dwCopy = sizeof(sptwb.DataBuf);
    if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
    memcpy(pOut, sptwb.DataBuf, dwCopy);

    if (pdwLastError) *pdwLastError = 0;
    return TRUE;
}

BOOL GetSMARTViaLogSense(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE buf[64];
    BOOL bAny = FALSE;
    DWORD dwErr = 0;

    if (SCSILogSense(hDrive, SCSI_LOGPAGE_INFO_EXCEPTIONS, buf, sizeof(buf), &dwErr)) {
        if (buf[0] == SCSI_LOGPAGE_INFO_EXCEPTIONS) {
            if (buf[7] >= 1) {
                BYTE bASC = buf[8];
                if (bASC == SCSI_ASC_FAILURE_PREDICTED) {
                    pInfo->bPredictFailure = TRUE;
                    /* Do NOT set nHealthPercent = 0 here!
                     * Health % and health status are separate.
                     * Predictive failure affects STATUS (Warning),
                     * not the health PERCENTAGE. HDD Sentinel still
                     * shows the calculated health % even when
                     * predictive failure is detected. */
                }
                bAny = TRUE;
            }
        }
    }
    pInfo->dwErrLogSense = dwErr;

    if (SCSILogSense(hDrive, SCSI_LOGPAGE_TEMPERATURE, buf, sizeof(buf), &dwErr)) {
        if (buf[0] == SCSI_LOGPAGE_TEMPERATURE && buf[7] >= 2) {
            BYTE bTemp = buf[9];
            if (bTemp != 0xFF && bTemp > 0 && bTemp <= 150) {
                pInfo->nTemperatureC = (int)bTemp;
                bAny = TRUE;
            }
        }
    }
    if (pInfo->dwErrLogSense == 0) pInfo->dwErrLogSense = dwErr;

    if (bAny) pInfo->eAccessMethod = SMART_ACCESS_SAT16;
    return bAny;
}

/* USB descriptor-only path (last resort) */
BOOL GetIdentifyDataUSB(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (!GetDeviceDescriptor(hDrive, pInfo)) return FALSE;
    GetCapacityFromGeometry(hDrive, pInfo);

    pInfo->bSMART_Supported = FALSE;
    pInfo->bSMART_Enabled   = FALSE;
    /* Only mark USB when the bus really is USB. Native NVMe is a SCSI
     * device to Windows; treating it as USB sent it down SCSI Log Sense
     * instead of the NVMe Health Log. */
    if (GetStorageBusType(hDrive) == 7) {
        pInfo->bIsUSB = TRUE;
        pInfo->eType  = DRIVE_TYPE_USB;
    }
    pInfo->nHealthPercent   = -1;
    pInfo->eAccessMethod    = SMART_ACCESS_STORAGE_QUERY;
    return (pInfo->szModel[0] != '\0');
}

/* ============================================================
 * Storage protocol property path (Windows 8+)
 * ============================================================ */
BOOL GetSMARTViaStorageProtocol(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE buf[sizeof(MY_STORAGE_PROTOCOL_QUERY) + 512 + 64];
    DWORD dwBytes = 0;

    ZeroMemory(buf, sizeof(buf));
    MY_STORAGE_PROTOCOL_QUERY* pQ = (MY_STORAGE_PROTOCOL_QUERY*)buf;
    pQ->PropertyId = (ULONG)StorageDeviceProtocolSpecificProperty;
    pQ->QueryType  = 0;
    pQ->ProtocolSpecific.ProtocolType       = MY_ProtocolTypeAta;
    pQ->ProtocolSpecific.DataType           = MY_AtaDataTypeSmartData;
    pQ->ProtocolSpecific.ProtocolDataOffset = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    pQ->ProtocolSpecific.ProtocolDataLength = 512;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL) ||
        dwBytes < (ULONG)(sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA) + 16)) {
        pInfo->dwErrStorageProtocol = GetLastError();
        return FALSE;
    }

    BYTE* pData = buf + sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    if (IsBufferAllZero(pData + 2, 28)) {
        pInfo->dwErrStorageProtocol = ERROR_INVALID_DATA;
        return FALSE;
    }
    if (FillSmartData(pInfo, pData) <= 0) {
        pInfo->dwErrStorageProtocol = ERROR_INVALID_DATA;
        return FALSE;
    }

    ZeroMemory(buf, sizeof(buf));
    pQ = (MY_STORAGE_PROTOCOL_QUERY*)buf;
    pQ->PropertyId = (ULONG)StorageDeviceProtocolSpecificProperty;
    pQ->QueryType  = 0;
    pQ->ProtocolSpecific.ProtocolType       = MY_ProtocolTypeAta;
    pQ->ProtocolSpecific.DataType           = MY_AtaDataTypeSmartThresholds;
    pQ->ProtocolSpecific.ProtocolDataOffset = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    pQ->ProtocolSpecific.ProtocolDataLength = 512;

    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL) &&
        dwBytes >= (ULONG)(sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA) + 16)) {
        pData = buf + sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
        FillSmartThreshold(pInfo, pData);
    }

    pInfo->eAccessMethod = SMART_ACCESS_STORAGE_QUERY;
    return TRUE;
}

/* ============================================================
 * NVMe paths
 * ============================================================ */
BOOL GetNVMeIdentifyController(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE ident[4096];
    DWORD nCopied = 0;
    ZeroMemory(ident, sizeof(ident));

    /* Native NvmeMini / protocol query on a USB handle crashes RTL9210. */
    if (GetStorageBusType(hDrive) == 7)
        return FALSE;

    if (!QueryNVMeProtocol(hDrive, MY_NVMeDataTypeIdentify, 1,
                           ident, sizeof(ident), &nCopied) || nCopied < 72) {
        nCopied = 4096;
        if (!NvmeMiniportAdmin(hDrive, 6, 0, 1, ident, 4096) &&
            !NvmeIntelRstAdmin(hDrive, 0x06, 0, 1, ident, 4096))
            return FALSE;
    }

    /* Store identify controller data (never more than the 4096-byte page). */
    CopyNvmeIdentBuf(pInfo, ident, nCopied ? nCopied : 4096);
    pInfo->bGotNVMeIdent = TRUE;

    /* Extract key strings (NVMe uses direct ASCII, no byte-swap needed) */
    memcpy(pInfo->szSerial, ident + 4, 20);
    pInfo->szSerial[20] = '\0';
    TrimStr(pInfo->szSerial);

    memcpy(pInfo->szModel, ident + 24, 40);
    pInfo->szModel[40] = '\0';
    TrimStr(pInfo->szModel);

    memcpy(pInfo->szFirmware, ident + 64, 8);
    pInfo->szFirmware[8] = '\0';
    TrimStr(pInfo->szFirmware);

    /* Extract temperature thresholds */
    pInfo->wNVMeWarnTempThreshold = ReadLE16(pInfo->nvmeIdent.WCTEMP);
    pInfo->wNVMeCritTempThreshold = ReadLE16(pInfo->nvmeIdent.CCTEMP);

    return (pInfo->szModel[0] != '\0' || pInfo->szSerial[0] != '\0');
}

BOOL GetNVMeHealthLog(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE health[sizeof(NVME_HEALTH_INFO_LOG)];
    DWORD nCopied = 0;
    ZeroMemory(health, sizeof(health));

    if (!QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, NVME_LOG_PAGE_HEALTH_INFO,
                           health, sizeof(health), &nCopied)) {
        pInfo->dwErrNvmeProtocol = g_dwLastNvmeQueryErr ? g_dwLastNvmeQueryErr : GetLastError();
        return FALSE;
    }

    memcpy(&pInfo->nvmeHealth, health,
           nCopied < sizeof(NVME_HEALTH_INFO_LOG) ? nCopied : sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bIsNVMe          = TRUE;
    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled   = TRUE;
    pInfo->eAccessMethod    = SMART_ACCESS_NVME_PROTOCOL;

    return TRUE;
}

BOOL GetNVMeHealthLogFallback(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    /* QueryNVMeProtocol already tries adapter vs device and several NSIDs. */
    return GetNVMeHealthLog(hDrive, pInfo);
}

#ifndef NVME_PASS_THROUGH_SRB_IO_CODE
#define NVME_STORPORT_DRIVER 0xE000
#define NVME_PASS_THROUGH_SRB_IO_CODE CTL_CODE(NVME_STORPORT_DRIVER, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_INTEL_NVME_PASS_THROUGH
#define IOCTL_INTEL_NVME_PASS_THROUGH CTL_CODE(0xf000, 0xA02, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#pragma pack(push, 1)
typedef struct _MY_SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} MY_SRB_IO_CONTROL;

typedef struct _MY_NVME_PT {
    MY_SRB_IO_CONTROL SrbIoCtrl;
    DWORD VendorSpecific[6];
    DWORD NVMeCmd[16];
    DWORD CplEntry[4];
    DWORD Direction;
    DWORD QueueId;
    DWORD DataBufferLen;
    DWORD MetaDataLen;
    DWORD ReturnBufferLen;
    UCHAR DataBuffer[4096];
} MY_NVME_PT;

typedef struct _MY_INTEL_NVME_PT {
    MY_SRB_IO_CONTROL SRB;
    BYTE  Version;
    BYTE  PathId;
    BYTE  TargetId;
    BYTE  Lun;
    DWORD NVMeCmd[16];
    DWORD CplEntry[4];
    DWORD QueueId;
    DWORD ParamBufLen;
    DWORD ReturnBufferLen;
    BYTE  Rsvd[0x28];
    BYTE  DataBuffer[0x1000];
} MY_INTEL_NVME_PT;
#pragma pack(pop)

static HANDLE OpenScsiAdapterFromDrive(HANDLE hDrive, SCSI_ADDRESS* pAddr)
{
    SCSI_ADDRESS addr;
    DWORD dw = 0;
    char sz[32];
    HANDLE hScsi;
    ZeroMemory(&addr, sizeof(addr));
    addr.Length = sizeof(addr);
    if (!DeviceIoControl(hDrive, IOCTL_SCSI_GET_ADDRESS,
            NULL, 0, &addr, sizeof(addr), &dw, NULL))
        return INVALID_HANDLE_VALUE;
    if (pAddr) *pAddr = addr;
    safe_snprintf(sz, "\\\\.\\Scsi%d:", (int)addr.PortNumber);
    hScsi = CreateFileA(sz, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    return hScsi;
}

static BOOL BufferHasData(const BYTE* p, int n)
{
    int i;
    DWORD sum = 0;
    for (i = 0; i < n; i++) sum += p[i];
    return sum != 0;
}

static BOOL NvmeMiniportAdmin(HANDLE hDrive, DWORD cdw0, DWORD nsid, DWORD cdw10,
                              BYTE* pOut, DWORD dwOut)
{
    SCSI_ADDRESS addr;
    HANDLE hScsi = OpenScsiAdapterFromDrive(hDrive, &addr);
    MY_NVME_PT* pt;
    DWORD dwRet = 0;
    BOOL ok;
    if (hScsi == INVALID_HANDLE_VALUE) return FALSE;
    pt = (MY_NVME_PT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MY_NVME_PT));
    if (!pt) { CloseHandle(hScsi); return FALSE; }
    pt->SrbIoCtrl.HeaderLength = sizeof(MY_SRB_IO_CONTROL);
    memcpy(pt->SrbIoCtrl.Signature, "NvmeMini", 8);
    pt->SrbIoCtrl.Timeout = 40;
    pt->SrbIoCtrl.ControlCode = NVME_PASS_THROUGH_SRB_IO_CODE;
    pt->SrbIoCtrl.Length = sizeof(MY_NVME_PT) - sizeof(MY_SRB_IO_CONTROL);
    pt->Direction = 2;
    pt->DataBufferLen = 4096;
    pt->ReturnBufferLen = sizeof(MY_NVME_PT);
    pt->NVMeCmd[0] = cdw0;
    pt->NVMeCmd[1] = nsid;
    pt->NVMeCmd[10] = cdw10;
    ok = DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, pt, sizeof(*pt), pt, sizeof(*pt), &dwRet, NULL);
    CloseHandle(hScsi);
    if (!ok) {
        g_dwLastNvmeQueryErr = GetLastError();
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (!BufferHasData(pt->DataBuffer, 64)) {
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    memcpy(pOut, pt->DataBuffer, dwOut);
    HeapFree(GetProcessHeap(), 0, pt);
    return TRUE;
}

static BOOL NvmeIntelRstAdmin(HANDLE hDrive, DWORD opcode, DWORD nsid, DWORD cdw10,
                              BYTE* pOut, DWORD dwOut)
{
    SCSI_ADDRESS addr;
    HANDLE hScsi = OpenScsiAdapterFromDrive(hDrive, &addr);
    MY_INTEL_NVME_PT* pt;
    DWORD dwRet = 0;
    BOOL ok;
    if (hScsi == INVALID_HANDLE_VALUE) return FALSE;
    pt = (MY_INTEL_NVME_PT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MY_INTEL_NVME_PT));
    if (!pt) { CloseHandle(hScsi); return FALSE; }
    pt->SRB.HeaderLength = sizeof(MY_SRB_IO_CONTROL);
    memcpy(pt->SRB.Signature, "IntelNvm", 8);
    pt->SRB.Timeout = 10;
    pt->SRB.ControlCode = IOCTL_INTEL_NVME_PASS_THROUGH;
    pt->SRB.Length = sizeof(MY_INTEL_NVME_PT) - sizeof(MY_SRB_IO_CONTROL);
    pt->Version = 1;
    pt->PathId = addr.PathId;
    pt->NVMeCmd[0] = opcode;
    pt->NVMeCmd[1] = nsid;
    pt->NVMeCmd[10] = cdw10;
    pt->ParamBufLen = sizeof(MY_INTEL_NVME_PT) - 0x1000;
    pt->ReturnBufferLen = 0x1000;
    ok = DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, pt, sizeof(*pt), pt, sizeof(*pt), &dwRet, NULL);
    CloseHandle(hScsi);
    if (!ok) {
        g_dwLastNvmeQueryErr = GetLastError();
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (!BufferHasData(pt->DataBuffer, 64)) {
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    memcpy(pOut, pt->DataBuffer, dwOut);
    HeapFree(GetProcessHeap(), 0, pt);
    return TRUE;
}

static BOOL FillNvmeHealthFromBuf(DRIVE_INFO* pInfo, BYTE* health, DWORD n)
{
    memcpy(&pInfo->nvmeHealth, health,
           n < sizeof(NVME_HEALTH_INFO_LOG) ? n : sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bIsNVMe = TRUE;
    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PROTOCOL;
    return TRUE;
}

BOOL GetNVMeHealthLogEx(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE health[512];
    /* Native NvmeMini IOCTL on a USB handle crashes RTL9210 (and is
     * meaningless on USB SAT). Never run this path on bus type 7. */
    if (GetStorageBusType(hDrive) == 7)
        return FALSE;
    /* Do not call IOCTL_STORAGE_PROTOCOL_COMMAND — it bugchecked nvme.sys. */
    if (GetNVMeHealthLog(hDrive, pInfo)) return TRUE;
    if (GetNVMeHealthLogFallback(hDrive, pInfo)) return TRUE;
    ZeroMemory(health, sizeof(health));
    if (NvmeMiniportAdmin(hDrive, 2, 0xFFFFFFFFu, 0x007f0002, health, sizeof(health)))
        return FillNvmeHealthFromBuf(pInfo, health, sizeof(health));
    if (NvmeIntelRstAdmin(hDrive, 0x02, 0xFFFFFFFFu, 0x007f0002, health, sizeof(health)))
        return FillNvmeHealthFromBuf(pInfo, health, sizeof(health));
    return FALSE;
}

/* ============================================================
 * NVMe data extraction helpers
 * Extracts all 128-bit NVMe counters as 64-bit
 * ============================================================ */
static void ExtractNVMeExtendedInfo(DRIVE_INFO* pInfo)
{
    NVME_HEALTH_INFO_LOG* pLog = &pInfo->nvmeHealth;

    /* Temperature */
    WORD wTempK = ReadLE16(pLog->CompositeTemperature);
    if (wTempK > 273 && wTempK < 400)
        pInfo->nTemperatureC = (int)wTempK - 273;

    /* NVMe temperature sensors */
    int i;
    for (i = 0; i < 8; i++) {
        WORD wSensorK = pLog->TempSensor[i];
        if (wSensorK > 273 && wSensorK < 400)
            pInfo->nTempSensor[i] = (int)wSensorK - 273;
        else
            pInfo->nTempSensor[i] = -1;
    }

    /* 128-bit counters → use lower 64 bits (sufficient for practical use) */
    pInfo->qwNVMeDataUnitsRead      = ReadLE64(pLog->DataUnitsRead);
    pInfo->qwNVMeDataUnitsWritten   = ReadLE64(pLog->DataUnitsWritten);
    pInfo->qwNVMeHostReads          = ReadLE64(pLog->HostReadCommands);
    pInfo->qwNVMeHostWrites         = ReadLE64(pLog->HostWriteCommands);
    pInfo->qwNVMeControllerBusyTime = ReadLE64(pLog->ControllerBusyTime);
    pInfo->qwNVMePowerOnHours       = ReadLE64(pLog->PowerOnHours);
    pInfo->qwNVMeUnsafeShutdowns    = ReadLE64(pLog->UnsafeShutdowns);
    pInfo->qwNVMeMediaErrors        = ReadLE64(pLog->MediaErrors);

    /* Convenience 32-bit fields for backwards compat */
    pInfo->dwPowerOnHours    = (DWORD)pInfo->qwNVMePowerOnHours;
    pInfo->dwPowerCycleCount = (DWORD)ReadLE64(pLog->PowerCycles);
}

static BOOL GetNVMeInfo(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BOOL bIdent = GetNVMeIdentifyController(hDrive, pInfo);
    if (!bIdent) GetDeviceDescriptor(hDrive, pInfo);
    GetCapacityFromGeometry(hDrive, pInfo);

    if (GetNVMeHealthLogEx(hDrive, pInfo)) {
        ExtractNVMeExtendedInfo(pInfo);
    } else {
        GetSMARTViaLogSense(hDrive, pInfo);
    }

    pInfo->eType   = DRIVE_TYPE_NVME;
    pInfo->bIsNVMe = TRUE;

    IdentifyDriveParts(pInfo);

    return (pInfo->szModel[0] != '\0' || pInfo->bSMART_Supported);
}

/* ============================================================
 * Drive type detection
 * ============================================================ */
DRIVE_TYPE DetectDriveType(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (pInfo->bIsNVMe) return DRIVE_TYPE_NVME;

    BYTE bus = GetStorageBusType(hDrive);
    if (bus == 12 || bus == 13) return DRIVE_TYPE_EMMC;
    if (bus == 10) return DRIVE_TYPE_SCSI;
    /* bus 11 = SATA: HDD vs SSD decided by rotation rate below */
    if (bus == 7  && !pInfo->bSMART_Supported) return DRIVE_TYPE_USB;

    /* Use stored rotation rate from IDENTIFY if available */
    if (pInfo->wRotationRate == 0x0001) {
        return DRIVE_TYPE_SSD_SATA;
    }
    if (pInfo->wRotationRate >= 0x0401) {
        return DRIVE_TYPE_HDD;
    }

    /* Try IDENTIFY to extract rotation rate */
    BYTE ident[IDENTIFY_BUFFER_SIZE];
    BOOL bGotIdent = FALSE;
    ZeroMemory(ident, sizeof(ident));

    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf,  sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = IDENTIFY_BUFFER_SIZE;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = ID_CMD;

    if (DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL)) {
        SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
        memcpy(ident, pCop->bBuffer, IDENTIFY_BUFFER_SIZE);
        bGotIdent = !IsBufferAllZero(ident, 64);
    }
    if (!bGotIdent) {
        if (ATAPassThrough(hDrive, ID_CMD, 0, 1, 0, 0, 0, 0xA0,
                           ident, IDENTIFY_BUFFER_SIZE, TRUE))
            bGotIdent = !IsBufferAllZero(ident, 64);
    }

    if (bGotIdent) {
        WORD* p = (WORD*)ident;
        WORD wRot = p[217];
        pInfo->wRotationRate = wRot;
        WORD wForm = p[168];
        if (wRot == 0x0001) {
            if (wForm == 0x0003 || wForm == 0x0005) return DRIVE_TYPE_M2_SATA;
            return DRIVE_TYPE_SSD_SATA;
        }
        if (wRot >= 0x0401) return DRIVE_TYPE_HDD;
    }

    /* Heuristic from model name */
    const char* m = pInfo->szModel;
    if (m[0]) {
        if (strstr(m, "SSD") || strstr(m, "Solid") || strstr(m, "SOLID") ||
            strstr(m, "FLASH") || strstr(m, "Flash") || strstr(m, "flash") ||
            strstr(m, "MX") || strstr(m, "860") || strstr(m, "870") ||
            strstr(m, "BX") || strstr(m, "EVO") || strstr(m, "PRO"))
            return DRIVE_TYPE_SSD_SATA;
    }

    if (bus == 7) return DRIVE_TYPE_USB;
    return DRIVE_TYPE_HDD;
}

/* ============================================================
 * Health: state + confidence + evidence
 *
 * Overall eHealthStatus is worst-of evidence (GOOD/CAUTION/BAD/UNKNOWN).
 * nHealthPercent is SECONDARY estimated remaining life only (endurance),
 * never a fake % from worst/thresh or raw*N penalties.
 * nConfidence is completeness of evidence, not "how healthy".
 * ============================================================ */

/* Find threshold value for a given attribute ID */
static BYTE FindThreshold(const DRIVE_INFO* pInfo, BYTE bAttrID)
{
    int j;
    if (!pInfo) return 0;
    for (j = 0; j < 30; j++) {
        if (pInfo->threshData.stThresholds[j].bAttrID == bAttrID)
            return pInfo->threshData.stThresholds[j].bThresholdValue;
    }
    return 0;
}

/* RAW of attribute `id`, or -1 if the attribute is absent. */
static int AttrRawOrNeg1(const DRIVE_INFO* pInfo, BYTE id)
{
    int i;
    if (!pInfo) return -1;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID == id) {
            unsigned __int64 v = GetRawValue48(pInfo->attrData.stAttributes[i].bRawValue);
            if (v > (unsigned __int64)INT_MAX) return INT_MAX;
            return (int)v;
        }
    }
    return -1;
}

static BOOL HasNonZeroThreshold(const DRIVE_INFO* pInfo)
{
    int j;
    if (!pInfo) return FALSE;
    for (j = 0; j < 30; j++) {
        if (pInfo->threshData.stThresholds[j].bAttrID != 0 &&
            pInfo->threshData.stThresholds[j].bThresholdValue > 0)
            return TRUE;
    }
    return FALSE;
}

static int Clamp100(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static BOOL FirmwareFamilyRecognized(const DRIVE_INFO* p)
{
    char szFwU[16];
    if (!p || !p->szFirmware[0]) return FALSE;
    ToUpperCopy(szFwU, sizeof(szFwU), p->szFirmware);
    if (strncmp(szFwU, "HPS", 3) == 0) return TRUE;
    if (strncmp(szFwU, "SBF", 3) == 0) return TRUE;
    if (strncmp(szFwU, "SAFM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "ECFM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "E8FM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "E7FM", 4) == 0) return TRUE;
    if (strstr(szFwU, "SM22") || strstr(szFwU, "SM23") || strstr(szFwU, "SM25"))
        return TRUE;
    return FALSE;
}

static int CountUnknownAttrs(const DRIVE_INFO* pInfo)
{
    int i, n = 0;
    ATTR_DECODE dec;
    if (!pInfo) return 0;
    for (i = 0; i < 30; i++) {
        BYTE id = pInfo->attrData.stAttributes[i].bAttrID;
        if (id == 0) continue;
        GetAttrDecode(id, pInfo, &dec);
        if (dec.eState == ATTR_DECODE_UNKNOWN)
            n++;
    }
    return n;
}

static int CountPresentAttrs(const DRIVE_INFO* pInfo)
{
    int i, n = 0;
    if (!pInfo) return 0;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID != 0)
            n++;
    }
    return n;
}

static BOOL MediaCountersClean(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    if (p->nReallocated > 0) return FALSE;
    if (p->nPendingSectors > 0) return FALSE;
    if (p->nUncorrectable > 0) return FALSE;
    if (p->nCrcErrors > 0) return FALSE;
    if (p->qwNVMeMediaErrors > 0) return FALSE;
    return TRUE;
}

static BOOL HasKnownMediaZeros(const DRIVE_INFO* p)
{
    int nPresent = 0;
    if (!p) return FALSE;
    if (p->nReallocated >= 0) {
        nPresent++;
        if (p->nReallocated != 0) return FALSE;
    }
    if (p->nPendingSectors >= 0) {
        nPresent++;
        if (p->nPendingSectors != 0) return FALSE;
    }
    if (p->nUncorrectable >= 0) {
        nPresent++;
        if (p->nUncorrectable != 0) return FALSE;
    }
    return nPresent > 0;
}

static TEMP_BAND TempBandFromC(int nC)
{
    if (nC <= 0) return TEMP_BAND_UNKNOWN;
    if (nC <= 49) return TEMP_BAND_NORMAL;
    if (nC <= 59) return TEMP_BAND_ELEVATED;
    if (nC <= 69) return TEMP_BAND_HIGH;
    return TEMP_BAND_CRITICAL;
}

static DRIVE_HEALTH_STATUS TempStatusFromBand(TEMP_BAND eBand)
{
    switch (eBand) {
    case TEMP_BAND_NORMAL:   return HEALTH_STATUS_GOOD;
    case TEMP_BAND_ELEVATED: return HEALTH_STATUS_CAUTION;
    case TEMP_BAND_HIGH:     return HEALTH_STATUS_CAUTION;
    case TEMP_BAND_CRITICAL: return HEALTH_STATUS_BAD;
    default:                 return HEALTH_STATUS_UNKNOWN;
    }
}

static void FormatUintGrouped(unsigned long n, char* buf, int nBuf)
{
    char tmp[32];
    int len, i, o, first;
    if (!buf || nBuf <= 0) return;
    (void)_snprintf(tmp, sizeof(tmp), "%lu", n);
    tmp[sizeof(tmp) - 1] = '\0';
    len = (int)strlen(tmp);
    first = len % 3;
    if (first == 0) first = 3;
    o = 0;
    for (i = 0; i < len && o < nBuf - 1; i++) {
        if (i > 0 && i >= first && ((i - first) % 3) == 0) {
            buf[o++] = ' ';
            if (o >= nBuf - 1) break;
        }
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
}

void FormatPowerOnHours(DWORD dwHours, char* szBuf, int nBufLen)
{
    char szN[32];
    if (!szBuf || nBufLen <= 0) return;
    if (dwHours == 0) {
        safe_snprintf_n(szBuf, nBufLen, "нет данных");
        return;
    }
    FormatUintGrouped((unsigned long)dwHours, szN, (int)sizeof(szN));
    safe_snprintf_n(szBuf, nBufLen, "%s ч ≈ %.2f года",
                    szN, (double)dwHours / 8760.0);
}

const char* GetTempBandName(TEMP_BAND eBand, BOOL bLowercase)
{
    switch (eBand) {
    case TEMP_BAND_NORMAL:   return bLowercase ? "норма" : "Норма";
    case TEMP_BAND_ELEVATED: return bLowercase ? "повышена" : "Повышена";
    case TEMP_BAND_HIGH:     return bLowercase ? "высокая" : "Высокая";
    case TEMP_BAND_CRITICAL: return bLowercase ? "критическая" : "Критическая";
    default:                 return "нет данных";
    }
}

static const char* QualityNameRu(NORM_QUALITY q, BOOL bRaw)
{
    switch (q) {
    case NORM_QUALITY_LOW:    return "НИЗКОЕ";
    case NORM_QUALITY_MEDIUM: return bRaw ? "СМЕШАННОЕ" : "СРЕДНЕЕ";
    case NORM_QUALITY_HIGH:   return "ВЫСОКОЕ";
    default:                  return "нет данных";
    }
}

static int Utf8DispWidth(const char* s)
{
    int w = 0;
    const unsigned char* p = (const unsigned char*)s;
    if (!p) return 0;
    while (*p) {
        if (*p < 0x80) { p++; w++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; w++; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; w++; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; w++; }
        else p++;
    }
    return w;
}

static void PadUtf8(char* dst, int nDst, const char* s, int width)
{
    int w;
    size_t used;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    if (s)
        safe_snprintf_n(dst, nDst, "%s", s);
    w = Utf8DispWidth(dst);
    used = strlen(dst);
    while (w < width && used + 1 < (size_t)nDst) {
        dst[used++] = ' ';
        dst[used] = '\0';
        w++;
    }
}

static void FmtIntOrNA(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "нет данных");
    else
        safe_snprintf_n(buf, nBuf, "%d", v);
}

static void LectureAdd(char* buf, int nBuf, const char* s)
{
    size_t used, rem;
    if (!buf || nBuf <= 1 || !s) return;
    used = strlen(buf);
    if (used >= (size_t)nBuf - 1) return;
    rem = (size_t)nBuf - used;
    safe_snprintf_n(buf + used, (int)rem, "%s", s);
}

static void FormatNvmeCritWarnBits(BYTE cw, char* buf, int nBuf)
{
    char tmp[192];
    tmp[0] = '\0';
    if (!buf || nBuf <= 0) return;
    if (cw == 0) {
        safe_snprintf_n(buf, nBuf, "нет");
        return;
    }
    if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", запас" : "запас");
    if (cw & NVME_CRIT_WARN_TEMP_THRESHOLD)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", температура" : "температура");
    if (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", надёжность" : "надёжность");
    if (cw & NVME_CRIT_WARN_READ_ONLY)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", только чтение" : "только чтение");
    if (cw & NVME_CRIT_WARN_VOLATILE_MEM_BACKUP)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", резерв энергозависимой памяти" : "резерв энергозависимой памяти");
    if (cw & NVME_CRIT_WARN_PMR_RO)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", постоянная память только чтение" : "постоянная память только чтение");
    if (tmp[0] == '\0')
        safe_snprintf_n(buf, nBuf, "есть");
    else
        safe_snprintf_n(buf, nBuf, "%s", tmp);
}

void AssessDriveHealth(DRIVE_INFO* pInfo)
{
    BYTE cw;
    BOOL bSpareLow;
    BOOL bHasMediaCounters;
    int nUnknown;
    int nAttr;
    int nNormSame;
    int nConfC, nConfT, nConfM, nConfD, nConfH;

    if (!pInfo) return;

    pInfo->nConfidence         = 0;
    pInfo->nConfCompleteness   = 0;
    pInfo->nConfTransport      = 0;
    pInfo->nConfModelId        = 0;
    pInfo->nConfVendorDecoder  = 0;
    pInfo->nConfHealthAssess   = 0;
    pInfo->nEndurancePercent   = -1;
    pInfo->eReliability        = HEALTH_STATUS_UNKNOWN;
    pInfo->eInterface          = HEALTH_STATUS_UNKNOWN;
    pInfo->eTempStatus         = HEALTH_STATUS_UNKNOWN;
    pInfo->eTempBand           = TEMP_BAND_UNKNOWN;
    pInfo->eNormQuality        = NORM_QUALITY_UNKNOWN;
    pInfo->eRawQuality         = NORM_QUALITY_UNKNOWN;
    pInfo->nReallocated        = -1;
    pInfo->nPendingSectors     = -1;
    pInfo->nUncorrectable      = -1;
    pInfo->nRemapEvents        = -1;
    pInfo->nCrcErrors          = -1;
    pInfo->bThresholdViolation = FALSE;
    pInfo->szEvidence[0]       = '\0';
    pInfo->nHealthPercent      = -1;
    pInfo->eHealthStatus       = HEALTH_STATUS_UNKNOWN;

    if (!pInfo->bSMART_Supported) {
        safe_snprintf(pInfo->szEvidence, "SMART недоступен");
        return;
    }

    pInfo->nReallocated    = AttrRawOrNeg1(pInfo, 0x05);
    pInfo->nPendingSectors = AttrRawOrNeg1(pInfo, 0xC5);
    pInfo->nUncorrectable  = AttrRawOrNeg1(pInfo, 0xC6);
    pInfo->nRemapEvents    = AttrRawOrNeg1(pInfo, 0xC4);
    pInfo->nCrcErrors      = AttrRawOrNeg1(pInfo, 0xC7);

    /* Threshold fail: current value OR worst <= thresh. Skip thresh==0
     * (vendor "not used") and 0/255 garbage normalized values. */
    {
        int i;
        for (i = 0; i < 30; i++) {
            SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[i];
            BYTE bThresh, bVal, bWorst;
            BOOL bValOk, bWorstOk;
            if (pAttr->bAttrID == 0) continue;
            bThresh = FindThreshold(pInfo, pAttr->bAttrID);
            if (bThresh == 0) continue;
            bVal     = pAttr->bAttrValue;
            bWorst   = pAttr->bWorstValue;
            bValOk   = (bVal != 0 && bVal != 255);
            bWorstOk = (bWorst != 0 && bWorst != 255);
            if (bValOk && bVal <= bThresh)
                pInfo->bThresholdViolation = TRUE;
            if (bWorstOk && bWorst <= bThresh)
                pInfo->bThresholdViolation = TRUE;
        }
    }

    cw = pInfo->bIsNVMe ? pInfo->nvmeHealth.CriticalWarning : (BYTE)0;
    bSpareLow = FALSE;
    if (pInfo->bIsNVMe) {
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        if (nSpare < nSpareTh)
            bSpareLow = TRUE;
        if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
            bSpareLow = TRUE;
    }

    /* Endurance: remaining life, or -1 if the drive has no such counter. */
    if (pInfo->bIsNVMe) {
        int nLeft = 100 - (int)pInfo->nvmeHealth.PercentageUsed;
        if (nLeft < 0)   nLeft = 0;
        if (nLeft > 100) nLeft = 100;
        pInfo->nEndurancePercent = nLeft;
    } else if (pInfo->nSSDLifeLeft >= 0 && pInfo->nSSDLifeLeft <= 100) {
        pInfo->nEndurancePercent = pInfo->nSSDLifeLeft;
    } else {
        pInfo->nEndurancePercent = -1;
    }

    /* Secondary estimated health = endurance only. Never worst/thresh. */
    if (pInfo->nEndurancePercent >= 0)
        pInfo->nHealthPercent = pInfo->nEndurancePercent;
    else
        pInfo->nHealthPercent = -1;

    /* Reliability axis — C0/C3/B0/POH/temp-below-60 are not media failures. */
    if (pInfo->bPredictFailure || pInfo->bThresholdViolation ||
        (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED) ||
        (cw & NVME_CRIT_WARN_READ_ONLY)) {
        pInfo->eReliability = HEALTH_STATUS_BAD;
    } else if (pInfo->nReallocated > 0 || pInfo->nPendingSectors > 0 ||
               pInfo->nUncorrectable > 0 || pInfo->qwNVMeMediaErrors > 0 ||
               bSpareLow) {
        pInfo->eReliability = HEALTH_STATUS_CAUTION;
    } else {
        pInfo->eReliability = HEALTH_STATUS_GOOD;
    }

    /* Interface axis: NVMe has no CRC analog — GOOD if SMART is present. */
    if (pInfo->bIsNVMe)
        pInfo->eInterface = HEALTH_STATUS_GOOD;
    else if (pInfo->nCrcErrors > 0)
        pInfo->eInterface = HEALTH_STATUS_CAUTION;
    else
        pInfo->eInterface = HEALTH_STATUS_GOOD;

    /* Temperature band (SSD-focused). 48 C is NORMAL. Does not by itself
     * flip overall health; HIGH (>=60) still CAUTION via existing rule. */
    pInfo->eTempBand = TempBandFromC(pInfo->nTemperatureC);
    if (cw & NVME_CRIT_WARN_TEMP_THRESHOLD)
        pInfo->eTempBand = TEMP_BAND_CRITICAL;
    pInfo->eTempStatus = TempStatusFromBand(pInfo->eTempBand);

    /* Overall state: worst-of listed evidence. Predictive failure is BAD.
     * Unknown vendor RAW, C0, C3, B0, POH, 48 C are NOT penalties. */
    if (pInfo->bPredictFailure || pInfo->bThresholdViolation ||
        (cw & NVME_CRIT_WARN_READ_ONLY) ||
        (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED)) {
        pInfo->eHealthStatus = HEALTH_STATUS_BAD;
    } else if (pInfo->nReallocated > 0 ||
               pInfo->nPendingSectors > 0 ||
               pInfo->nUncorrectable > 0 ||
               bSpareLow ||
               (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH) ||
               (cw & NVME_CRIT_WARN_TEMP_THRESHOLD) ||
               pInfo->qwNVMeMediaErrors > 0 ||
               pInfo->nCrcErrors > 0 ||
               pInfo->nTemperatureC >= 60 ||
               (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 10) ||
               (pInfo->bGotErrorLog && pInfo->nErrorLogCount > 0)) {
        pInfo->eHealthStatus = HEALTH_STATUS_CAUTION;
    } else {
        pInfo->eHealthStatus = HEALTH_STATUS_GOOD;
    }

    nUnknown = CountUnknownAttrs(pInfo);
    nAttr    = CountPresentAttrs(pInfo);

    /* Normalized SMART usefulness: identical 100/100/50 is LOW, not "perfect". */
    nNormSame = 0;
    if (!pInfo->bIsNVMe && nAttr >= 8) {
        int i;
        int nMinV = 255, nMaxV = 0, nDistinctish = 0;
        for (i = 0; i < 30; i++) {
            const SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
            BYTE bThresh, bVal, bWorst;
            BOOL bSame;
            if (pA->bAttrID == 0) continue;
            bVal   = pA->bAttrValue;
            bWorst = pA->bWorstValue;
            bThresh = FindThreshold(pInfo, pA->bAttrID);
            bSame = FALSE;
            if (bVal == 100 && bWorst == 100 && (bThresh == 50 || bThresh == 0))
                bSame = TRUE;
            else if (bVal == bWorst && (bThresh == 0 || bThresh == 50) &&
                     (bVal == 100 || bVal == 200))
                bSame = TRUE;
            if (bSame) nNormSame++;
            if (bVal < nMinV) nMinV = bVal;
            if (bVal > nMaxV) nMaxV = bVal;
        }
        if (nNormSame * 100 >= nAttr * 70)
            pInfo->eNormQuality = NORM_QUALITY_LOW;
        else if ((nMaxV - nMinV) >= 10)
            pInfo->eNormQuality = NORM_QUALITY_HIGH;
        else
            pInfo->eNormQuality = NORM_QUALITY_MEDIUM;
        (void)nDistinctish;
    }

    /* RAW quality: known media counters vs vendor-unknown packing. */
    if (pInfo->bIsNVMe) {
        pInfo->eRawQuality = NORM_QUALITY_HIGH;
    } else {
        int nMedia = 0;
        if (pInfo->nReallocated >= 0) nMedia++;
        if (pInfo->nPendingSectors >= 0) nMedia++;
        if (pInfo->nUncorrectable >= 0) nMedia++;
        if (pInfo->nRemapEvents >= 0) nMedia++;
        if (pInfo->nCrcErrors >= 0) nMedia++;
        if (nMedia >= 3 && (nAttr == 0 || nUnknown * 2 < nAttr))
            pInfo->eRawQuality = NORM_QUALITY_HIGH;
        else if (nMedia == 0 || (nAttr > 0 && nUnknown * 100 >= nAttr * 70))
            pInfo->eRawQuality = NORM_QUALITY_LOW;
        else
            pInfo->eRawQuality = NORM_QUALITY_MEDIUM;
    }

    /* Split confidence. Unknown encodings go ONLY into vendor-decoder. */
    nConfC = 50;
    if (pInfo->bIsNVMe || HasNonZeroThreshold(pInfo))
        nConfC += 20;
    bHasMediaCounters = pInfo->bIsNVMe ||
        (pInfo->nReallocated >= 0) ||
        (pInfo->nPendingSectors >= 0) ||
        (pInfo->nUncorrectable >= 0);
    if (bHasMediaCounters)
        nConfC += 10;
    if (pInfo->nTemperatureC > 0)
        nConfC += 10;
    if (pInfo->nEndurancePercent >= 0 || pInfo->bIsNVMe)
        nConfC += 10;
    nConfC = Clamp100(nConfC);

    if (pInfo->bIsUSB) {
        nConfT = pInfo->bSMART_Supported ? 90 : 40;
    } else {
        nConfT = 100;
    }

    if (pInfo->eController != CONTROLLER_UNKNOWN)
        nConfM = 99;
    else if (FirmwareFamilyRecognized(pInfo) || IsPhisonFamily(pInfo))
        nConfM = 90;
    else
        nConfM = 70;

    if (pInfo->bIsNVMe) {
        nConfD = 100;
    } else if (nUnknown == 0) {
        nConfD = 100;
    } else {
        nConfD = 100 - 10 * nUnknown;
        if (nConfD < 40) nConfD = 40;
    }

    nConfH = 55;
    if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD &&
        MediaCountersClean(pInfo) &&
        !pInfo->bPredictFailure &&
        !pInfo->bThresholdViolation) {
        nConfH = 95;
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
        nConfH = 85;
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
        nConfH = 70;
    } else {
        nConfH = 55;
    }
    if (pInfo->eNormQuality == NORM_QUALITY_LOW)
        nConfH -= 5;
    if (nUnknown >= 3)
        nConfH -= 5;
    if (HasKnownMediaZeros(pInfo) && nConfH < 70)
        nConfH = 70;
    nConfH = Clamp100(nConfH);

    pInfo->nConfCompleteness  = nConfC;
    pInfo->nConfTransport     = nConfT;
    pInfo->nConfModelId       = nConfM;
    pInfo->nConfVendorDecoder = nConfD;
    pInfo->nConfHealthAssess  = nConfH;
    pInfo->nConfidence = (20 * nConfC + 10 * nConfT + 15 * nConfM +
                          20 * nConfD + 35 * nConfH + 50) / 100;
    pInfo->nConfidence = Clamp100(pInfo->nConfidence);

    /* One-line Russian evidence. Full words; lecture has the matrix. */
    if (pInfo->bIsNVMe) {
        char szCW[128];
        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));
        safe_snprintf(pInfo->szEvidence,
            "Критическое предупреждение: %s. Ошибки носителя: %llu.",
            szCW,
            (unsigned long long)pInfo->qwNVMeMediaErrors);
    } else {
        char szR[24], szPend[24], szU[24];
        FmtIntOrNA(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        FmtIntOrNA(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        FmtIntOrNA(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        safe_snprintf(pInfo->szEvidence,
            "Переназначенные сектора: %s  Ожидающие сектора: %s  Неисправимые сектора: %s  Нарушение порога: %s  Прогноз отказа: %s",
            szR, szPend, szU,
            pInfo->bThresholdViolation ? "да" : "нет",
            pInfo->bPredictFailure     ? "да" : "нет");
    }
}

static void LectureAddF(char* buf, int nBuf, const char* fmt, ...)
{
    char tmp[768];
    va_list ap;
    if (!buf || nBuf <= 1 || !fmt) return;
    va_start(ap, fmt);
    (void)_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    tmp[sizeof(tmp) - 1] = '\0';
    va_end(ap);
    LectureAdd(buf, nBuf, tmp);
}

static void LectureAddSplitConfidence(char* buf, int nBuf, const DRIVE_INFO* p)
{
    char c1[40], c2[16];
    LectureAdd(buf, nBuf, "Слои уверенности:\r\n");
    PadUtf8(c1, (int)sizeof(c1), "Полнота данных", 26);
    safe_snprintf(c2, "%d%%", p->nConfCompleteness);
    LectureAddF(buf, nBuf, "  %s%s\r\n", c1, c2);
    PadUtf8(c1, (int)sizeof(c1), "Транспорт", 26);
    safe_snprintf(c2, "%d%%", p->nConfTransport);
    LectureAddF(buf, nBuf, "  %s%s\r\n", c1, c2);
    PadUtf8(c1, (int)sizeof(c1), "Идентификация модели", 26);
    safe_snprintf(c2, "%d%%", p->nConfModelId);
    LectureAddF(buf, nBuf, "  %s%s\r\n", c1, c2);
    PadUtf8(c1, (int)sizeof(c1), "Вендорский декодер", 26);
    safe_snprintf(c2, "%d%%", p->nConfVendorDecoder);
    LectureAddF(buf, nBuf, "  %s%s\r\n", c1, c2);
    PadUtf8(c1, (int)sizeof(c1), "Оценка здоровья", 26);
    safe_snprintf(c2, "%d%%", p->nConfHealthAssess);
    LectureAddF(buf, nBuf, "  %s%s\r\n\r\n", c1, c2);
}

static void LectureMatrixRow(char* buf, int nBuf,
                             const char* evid, const char* val, const char* infl)
{
    char c1[72], c2[48], c3[40];
    PadUtf8(c1, (int)sizeof(c1), evid, 28);
    PadUtf8(c2, (int)sizeof(c2), val, 22);
    PadUtf8(c3, (int)sizeof(c3), infl, 16);
    LectureAddF(buf, nBuf, "  %s%s%s\r\n", c1, c2, c3);
}

static void LectureFmtInt(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "нет данных");
    else
        safe_snprintf_n(buf, nBuf, "%d", v);
}

static void LectureAddUnknownIdList(char* buf, int nBuf, const DRIVE_INFO* pInfo)
{
    int ui, nUnk = 0;
    char szIds[192];
    szIds[0] = '\0';
    for (ui = 0; ui < 30; ui++) {
        ATTR_DECODE dec;
        BYTE id = pInfo->attrData.stAttributes[ui].bAttrID;
        char piece[16];
        if (id == 0) continue;
        GetAttrDecode(id, pInfo, &dec);
        if (dec.eState != ATTR_DECODE_UNKNOWN) continue;
        safe_snprintf(piece, "%s%02Xh", nUnk ? ", " : "", id);
        {
            size_t used = strlen(szIds);
            size_t rem = sizeof(szIds) - used;
            if (rem > 1)
                safe_snprintf_n(szIds + used, (int)rem, "%s", piece);
        }
        nUnk++;
    }
    if (nUnk > 0) {
        LectureAddF(buf, nBuf,
            "Атрибуты %s не оцениваются: нет доверенного профиля RAW-кодировки "
            "для этого контроллера/прошивки. Большое RAW-значение не доказывает сбои. "
            "Unknown ≠ Bad.\r\n",
            szIds);
    }
}

void FormatHealthLecture(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    BYTE cw;
    BOOL bSpareLow;
    const char* szState;
    char szPoh[64];
    char szTempBand[32];
    int nUnknown;
    int nPos, nCritFail, nMediaDeg, nUnresolved;
    int nC0, nC3, nB0, nB1, nF5;

    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (nBufLen < 2) return;

    if (!pInfo) {
        safe_snprintf_n(szBuf, nBufLen, "Нет выбранного диска");
        return;
    }

    szState = GetHealthStatusName(pInfo->eHealthStatus);
    LectureAddF(szBuf, nBufLen, "Состояние: %s\r\n", szState);
    LectureAddF(szBuf, nBufLen, "Уверенность: %d%%\r\n\r\n", pInfo->nConfidence);

    if (!pInfo->bSMART_Supported) {
        if (pInfo->bIsUSB && IsLikelyUsbFlashDrive(pInfo)) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Это USB-флешка: контроллер флешки обычно не отдаёт "
                "атрибуты SMART, поэтому состояние не оценивается. Числа здоровья не выдумываются.\r\n");
        } else if (pInfo->bIsUSB) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. USB-мост или корпус не пропускает команды SMART. "
                "Без исходных данных оценка не ставится — программа не подставляет фиктивные проценты.\r\n");
        } else if (pInfo->bIsNVMe) {
            LectureAddF(szBuf, nBufLen,
                "Не удалось прочитать журнал здоровья NVMe (код ошибки Windows %lu). "
                "Возможные причины: нет прав администратора, драйвер не поддерживает запрос, "
                "или устройство занято. Без журнала здоровья проценты не выдумываются.\r\n",
                (unsigned long)pInfo->dwErrNvmeProtocol);
        } else {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Без исходных данных оценка не ставится — "
                "программа не подставляет фиктивные проценты. Запустите программу "
                "от имени администратора и нажмите «Обновить».\r\n");
        }
        return;
    }

    LectureAddSplitConfidence(szBuf, nBufLen, pInfo);

    cw = pInfo->bIsNVMe ? pInfo->nvmeHealth.CriticalWarning : (BYTE)0;
    bSpareLow = FALSE;
    if (pInfo->bIsNVMe) {
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        if (nSpare < nSpareTh)
            bSpareLow = TRUE;
        if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
            bSpareLow = TRUE;
    }

    FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
    if (pInfo->nTemperatureC > 0)
        safe_snprintf(szTempBand, "%d °C · %s",
                      pInfo->nTemperatureC,
                      GetTempBandName(pInfo->eTempBand, TRUE));
    else
        safe_snprintf(szTempBand, "нет данных");

    nUnknown = CountUnknownAttrs(pInfo);
    nC0 = AttrRawOrNeg1(pInfo, 0xC0);
    nC3 = AttrRawOrNeg1(pInfo, 0xC3);
    nB0 = AttrRawOrNeg1(pInfo, 0xB0);
    nB1 = AttrRawOrNeg1(pInfo, 0xB1);
    nF5 = AttrRawOrNeg1(pInfo, 0xF5);

    nPos = 0;
    nCritFail = 0;
    nMediaDeg = 0;
    nUnresolved = nUnknown;

    if (!pInfo->bPredictFailure) nPos++;
    if (pInfo->nReallocated == 0) nPos++;
    if (pInfo->nPendingSectors == 0) nPos++;
    if (pInfo->nUncorrectable == 0) nPos++;
    if (pInfo->nRemapEvents == 0) nPos++;
    if (pInfo->nCrcErrors == 0) nPos++;
    if (pInfo->nEndurancePercent >= 0) nPos++;
    if (pInfo->bIsNVMe && pInfo->qwNVMeMediaErrors == 0) nPos++;
    if (pInfo->bIsNVMe && !bSpareLow) nPos++;

    if (pInfo->bPredictFailure || pInfo->bThresholdViolation ||
        (cw & NVME_CRIT_WARN_READ_ONLY) ||
        (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED))
        nCritFail++;
    if (pInfo->nReallocated > 0 || pInfo->nPendingSectors > 0 ||
        pInfo->nUncorrectable > 0 || pInfo->qwNVMeMediaErrors > 0)
        nMediaDeg++;

    if (!pInfo->bIsNVMe && pInfo->eNormQuality != NORM_QUALITY_UNKNOWN) {
        LectureAddF(szBuf, nBufLen, "Качество нормализованных SMART: %s\r\n",
                    QualityNameRu(pInfo->eNormQuality, FALSE));
        LectureAddF(szBuf, nBufLen, "Качество vendor RAW: %s\r\n",
                    QualityNameRu(pInfo->eRawQuality, TRUE));
        if (pInfo->eNormQuality == NORM_QUALITY_LOW)
            LectureAdd(szBuf, nBufLen,
                "Одинаковые Value/Worst/Threshold (100/100/50) не означают, что всё идеально — "
                "у этой прошивки нормализованные числа слабо различают состояние.\r\n");
        LectureAdd(szBuf, nBufLen, "\r\n");
    }

    if (pInfo->bIsNVMe) {
        char szCW[192];
        char szSpare[32], szMedia[32], szLife[32];
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        int nUsed    = (int)pInfo->nvmeHealth.PercentageUsed;

        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));

        LectureAdd(szBuf, nBufLen, "Основные признаки:\r\n");
        if (cw == 0) {
            LectureAdd(szBuf, nBufLen, "  ✓ Критическое предупреждение: нет\r\n");
        } else {
            LectureAddF(szBuf, nBufLen, "  ✗ Критическое предупреждение: %s\r\n", szCW);
        }
        if (!bSpareLow)
            LectureAddF(szBuf, nBufLen, "  ✓ Доступный запас: %d%%\r\n", nSpare);
        else
            LectureAddF(szBuf, nBufLen, "  ✗ Доступный запас: %d%% (порог %d%%)\r\n",
                        nSpare, nSpareTh);
        if (pInfo->qwNVMeMediaErrors == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Ошибки носителя: 0\r\n");
        else
            LectureAddF(szBuf, nBufLen, "  ✗ Ошибки носителя: %llu\r\n",
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "  ✓ Заявленный остаток ресурса: %d%%   (износ NAND, не здоровье)\r\n",
                pInfo->nEndurancePercent);

        LectureAdd(szBuf, nBufLen, "\r\nКонтекст (не штраф):\r\n");
        LectureAddF(szBuf, nBufLen, "  Температура: %s. ", szTempBand);
        LectureAddF(szBuf, nBufLen,
            "Время на высокой температуре: предупреждение %lu мин, критическая %lu мин.\r\n",
            (unsigned long)pInfo->nvmeHealth.WarningCompTempTime,
            (unsigned long)pInfo->nvmeHealth.CriticalCompTempTime);
        LectureAddF(szBuf, nBufLen,
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n", szPoh);
        LectureAddF(szBuf, nBufLen,
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            (unsigned long)pInfo->dwPowerCycleCount);
        LectureAddF(szBuf, nBufLen,
            "  Небезопасные выключения: %llu. Контекст, не штраф.\r\n",
            (unsigned long long)pInfo->qwNVMeUnsafeShutdowns);

        LectureAdd(szBuf, nBufLen, "\r\nМатрица улик:\r\n");
        LectureMatrixRow(szBuf, nBufLen, "Улика", "Значение", "Влияние");
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         pInfo->bPredictFailure ? "FAIL" : "PASS",
                         pInfo->bPredictFailure ? "−" : "+");
        safe_snprintf(szSpare, "%d%%", nSpare);
        LectureMatrixRow(szBuf, nBufLen, "Доступный запас", szSpare,
                         bSpareLow ? "−" : "+");
        safe_snprintf(szMedia, "%llu", (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureMatrixRow(szBuf, nBufLen, "Ошибки носителя", szMedia,
                         pInfo->qwNVMeMediaErrors == 0 ? "+" : "−");
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, "Остаток ресурса", szLife, "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Температура", szT, "0");
        }
        {
            char szH[24];
            if (pInfo->dwPowerOnHours > 0)
                safe_snprintf(szH, "%lu ч", (unsigned long)pInfo->dwPowerOnHours);
            else
                safe_snprintf(szH, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Наработка", szH, "0");
        }
        LectureMatrixRow(szBuf, nBufLen, "Контроллер",
                         GetControllerName(pInfo->eController),
                         pInfo->eController == CONTROLLER_UNKNOWN ? "−уверенность" : "0");

        LectureAdd(szBuf, nBufLen, "\r\nИтог:\r\n");
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
            LectureAdd(szBuf, nBufLen, "  GOOD because:\r\n");
        } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
            LectureAdd(szBuf, nBufLen, "  CAUTION because:\r\n");
        } else {
            LectureAdd(szBuf, nBufLen, "  BAD because:\r\n");
        }
        LectureAddF(szBuf, nBufLen, "    %d сильных положительных признаков\r\n", nPos);
        LectureAddF(szBuf, nBufLen, "    %d критических отказов\r\n", nCritFail);
        LectureAddF(szBuf, nBufLen, "    %d признаков деградации носителя\r\n", nMediaDeg);
        LectureAdd(szBuf, nBufLen,
            "Использованный ресурс — износ NAND, его не смешивают с запасом и ошибками. "
            "Мы не превращаем использованный ресурс, запас, критическое предупреждение, "
            "ошибки носителя и температуру в один процент здоровья.\r\n");
        return;
    }

    /* ATA / SATA SMART */
    {
        char szR[24], szPend[24], szU[24], szCrc[24], szRemap[24], szLife[24];
        LectureFmtInt(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        LectureFmtInt(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        LectureFmtInt(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        LectureFmtInt(szCrc,  (int)sizeof(szCrc),  pInfo->nCrcErrors);
        LectureFmtInt(szRemap,(int)sizeof(szRemap),pInfo->nRemapEvents);

        LectureAdd(szBuf, nBufLen, "Основные признаки:\r\n");
        if (pInfo->nReallocated == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Переназначенные сектора: 0\r\n");
        else if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Переназначенные сектора: %d\r\n", pInfo->nReallocated);
        if (pInfo->nPendingSectors == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Ожидающие сектора: 0\r\n");
        else if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Ожидающие сектора: %d\r\n", pInfo->nPendingSectors);
        if (pInfo->nUncorrectable == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Неисправимые сектора: 0\r\n");
        else if (pInfo->nUncorrectable > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Неисправимые сектора: %d\r\n", pInfo->nUncorrectable);
        if (pInfo->nRemapEvents == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ События переназначения: 0\r\n");
        else if (pInfo->nRemapEvents > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ События переназначения: %d\r\n", pInfo->nRemapEvents);
        if (pInfo->nCrcErrors == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ CRC-ошибки: 0\r\n");
        else if (pInfo->nCrcErrors > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ CRC-ошибки: %d\r\n", pInfo->nCrcErrors);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "  ✓ Заявленный остаток ресурса: %d%%   (это износ NAND, не здоровье)\r\n",
                pInfo->nEndurancePercent);

        if (nUnknown > 0) {
            LectureAdd(szBuf, nBufLen, "\r\nНаблюдение:\r\n");
            LectureAdd(szBuf, nBufLen,
                "  ⚠ Есть vendor-specific SMART-счётчики с нестандартными RAW, которые нельзя "
                "надёжно трактовать без профиля контроллера/прошивки. Unknown ≠ Bad: B0 не штрафует "
                "здоровье, пока декодер не доказал, что число — физические ошибки.\r\n");
        }

        LectureAdd(szBuf, nBufLen, "\r\nКонтекст (не штраф):\r\n");
        LectureAddF(szBuf, nBufLen,
            "  Температура: %s. Время на высокой температуре: нет данных.\r\n",
            szTempBand);
        LectureAddF(szBuf, nBufLen,
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n", szPoh);
        LectureAddF(szBuf, nBufLen,
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            (unsigned long)pInfo->dwPowerCycleCount);
        if (nC0 >= 0 && DriveTreatsC0AsPowerLoss(pInfo)) {
            LectureAddF(szBuf, nBufLen,
                "  Аварийные отключения питания (C0): %d. Тяжесть: INFO. "
                "Тренд: нет данных (нужна повторная выборка). "
                "Находка только при росте счётчика, не по абсолютному числу.\r\n",
                nC0);
        } else if (nC0 >= 0) {
            LectureAddF(szBuf, nBufLen,
                "  Парковки при выключении (C0): %d. Контекст, не штраф. "
                "Тренд: нет данных (нужна повторная выборка).\r\n",
                nC0);
        }
        if (nC3 >= 0) {
            LectureAddF(szBuf, nBufLen,
                "  ECC (C3) RAW: %d. Тренд: нет данных (нужна повторная выборка). "
                "Влияние на здоровье: нет. "
                "ECC correction — штатный механизм NAND, это не неисправимые сектора. "
                "Абсолютный RAW без тренда не оценивается.\r\n",
                nC3);
        }

        LectureAdd(szBuf, nBufLen, "\r\nМатрица улик:\r\n");
        LectureMatrixRow(szBuf, nBufLen, "Улика", "Значение", "Влияние");
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         (pInfo->bPredictFailure || pInfo->bThresholdViolation) ? "FAIL" : "PASS",
                         (pInfo->bPredictFailure || pInfo->bThresholdViolation) ? "−" : "+");
        if (pInfo->nReallocated >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Переназначенные", szR,
                             pInfo->nReallocated == 0 ? "+" : "−");
        if (pInfo->nPendingSectors >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Ожидающие", szPend,
                             pInfo->nPendingSectors == 0 ? "+" : "−");
        if (pInfo->nUncorrectable >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Неисправимые", szU,
                             pInfo->nUncorrectable == 0 ? "+" : "−");
        if (pInfo->nRemapEvents >= 0)
            LectureMatrixRow(szBuf, nBufLen, "События переназначения", szRemap,
                             pInfo->nRemapEvents == 0 ? "+" : "−");
        if (pInfo->nCrcErrors >= 0)
            LectureMatrixRow(szBuf, nBufLen, "CRC", szCrc,
                             pInfo->nCrcErrors == 0 ? "+" : "−");
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, "Остаток ресурса", szLife, "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Температура", szT, "0");
        }
        {
            char szH[24];
            if (pInfo->dwPowerOnHours > 0)
                safe_snprintf(szH, "%lu ч", (unsigned long)pInfo->dwPowerOnHours);
            else
                safe_snprintf(szH, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Наработка", szH, "0");
        }
        if (nC3 >= 0) {
            char szEcc[24];
            safe_snprintf(szEcc, "%d", nC3);
            LectureMatrixRow(szBuf, nBufLen, "ECC (C3)", szEcc, "UNKNOWN");
        }
        if (nB0 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nB0);
            LectureMatrixRow(szBuf, nBufLen, "B0", szB, "UNKNOWN");
        }
        if (nB1 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nB1);
            LectureMatrixRow(szBuf, nBufLen, "B1", szB, "UNKNOWN");
        }
        if (nF5 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nF5);
            LectureMatrixRow(szBuf, nBufLen, "F5", szB, "UNKNOWN");
        }
        {
            char szCtl[64];
            const char* ctl = GetControllerName(pInfo->eController);
            if (pInfo->eController == CONTROLLER_PHISON && nUnknown > 0)
                safe_snprintf(szCtl, "Phison, упаковка RAW не подтверждена");
            else
                safe_snprintf(szCtl, "%s", ctl);
            LectureMatrixRow(szBuf, nBufLen, "Контроллер", szCtl,
                             nUnknown > 0 ? "−уверенность" : "0");
        }

        LectureAdd(szBuf, nBufLen, "\r\nИтог:\r\n");
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD)
            LectureAdd(szBuf, nBufLen, "  GOOD because:\r\n");
        else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION)
            LectureAdd(szBuf, nBufLen, "  CAUTION because:\r\n");
        else
            LectureAdd(szBuf, nBufLen, "  BAD because:\r\n");
        LectureAddF(szBuf, nBufLen, "    %d сильных положительных признаков\r\n", nPos);
        LectureAddF(szBuf, nBufLen, "    %d критических отказов\r\n", nCritFail);
        LectureAddF(szBuf, nBufLen, "    %d признаков деградации носителя\r\n", nMediaDeg);
        LectureAddF(szBuf, nBufLen, "    %d vendor-specific счётчиков не расшифрованы\r\n",
                    nUnresolved);

        LectureAdd(szBuf, nBufLen,
            "Мы не переводим SMART Value/Worst/Threshold в процент здоровья: "
            "нормализованная шкала 100 против 200 — это не «в два раза здоровее». "
            "Сырые счётчики — факты, а не шкала от 0 до 100.\r\n");
        if (pInfo->nEndurancePercent >= 0)
            LectureAdd(szBuf, nBufLen,
                "Заявленный остаток ресурса (A9) — цифра контроллера про износ NAND, "
                "это не здоровье диска и не означает, что диск новый.\r\n");
        else
            LectureAdd(szBuf, nBufLen,
                "Для этого диска заявленный остаток ресурса (износ) не задан — "
                "процент не выдумывается.\r\n");
        LectureAddUnknownIdList(szBuf, nBufLen, pInfo);
    }
}

int CalculateHealthNVMe(DRIVE_INFO* pInfo)
{
    AssessDriveHealth(pInfo);
    return pInfo ? pInfo->nHealthPercent : -1;
}

int CalculateHealth(DRIVE_INFO* pInfo)
{
    AssessDriveHealth(pInfo);
    return pInfo ? pInfo->nHealthPercent : -1;
}

DRIVE_HEALTH_STATUS DetermineHealthStatus(DRIVE_INFO* pInfo)
{
    if (!pInfo) return HEALTH_STATUS_UNKNOWN;
    AssessDriveHealth(pInfo);
    return pInfo->eHealthStatus;
}

int CalculatePerformance(DRIVE_INFO* pInfo)
{
    if (pInfo->bIsUSB && !pInfo->bSMART_Supported)
        return -1;

    int i, j;


    int nCRCPerf = 100;
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[i];
        if (pAttr->bAttrID != 0xC7) continue;
        DWORD dwCRC = GetRawValue(pAttr->bRawValue);
        if      (dwCRC == 0)   nCRCPerf = 100;
        else if (dwCRC < 10)   nCRCPerf = 75;
        else                   nCRCPerf = 50;
        break;
    }


    if (pInfo->bIsNVMe || pInfo->eType == DRIVE_TYPE_SSD_SATA || pInfo->eType == DRIVE_TYPE_M2_SATA) {
        int nPerf = (nCRCPerf * 25 + 100 * 75) / 100;
        if (nPerf < 0)   nPerf = 0;
        if (nPerf > 100) nPerf = 100;
        return nPerf;
    }


    static const BYTE sPerfIDs[] = { 0x07, 0x08, 0x02, 0x00 };
    int nPerfSum = 0, nPerfCount = 0;
    for (i = 0; sPerfIDs[i] != 0; i++) {
        BYTE bTargetID = sPerfIDs[i];
        for (j = 0; j < 30; j++) {
            SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[j];
            if (pAttr->bAttrID != bTargetID) continue;
            BYTE bThresh = 0;
            int k;
            for (k = 0; k < 30; k++) {
                if (pInfo->threshData.stThresholds[k].bAttrID == bTargetID) {
                    bThresh = pInfo->threshData.stThresholds[k].bThresholdValue;
                    break;
                }
            }
            int nVal = (int)pAttr->bAttrValue;
            if (bThresh > 0 && nVal > bThresh) {

                int nRange = 100 - (int)bThresh;
                int nAttrPerf = (nRange > 0)
                    ? ((nVal - (int)bThresh) * 100 / nRange)
                    : 100;
                if (nAttrPerf > 100) nAttrPerf = 100;
                nPerfSum += nAttrPerf;
            } else if (bThresh == 0 && nVal > 0) {

                nPerfSum += 100;
            } else {
                nPerfSum += 0;
            }
            nPerfCount++;
            break;
        }
    }

    int nSmartPerf = (nPerfCount > 0) ? (nPerfSum / nPerfCount) : 100;


    int nDMAPerf = 100;

    if (nCRCPerf <= 50)
        nDMAPerf = 60;
    else if (nCRCPerf <= 75)
        nDMAPerf = 80;


    int nPerf = (nSmartPerf * 25 + nDMAPerf * 50 + nCRCPerf * 25) / 100;
    if (nPerf < 0)   nPerf = 0;
    if (nPerf > 100) nPerf = 100;
    return nPerf;
}

/* ============================================================
 * SSD-specific indicator extraction
 * Extracts vendor-specific SSD health info
 * ============================================================ */
void ExtractSSDIndicators(DRIVE_INFO* pInfo)
{
    int i;
    IdentifyDriveParts(pInfo);
    pInfo->nSSDLifeLeft     = -1;
    pInfo->nSSDTotalWritesGB = -1;
    pInfo->nSSDAvgEraseCount = -1;
    pInfo->nSSDMaxEraseCount = -1;
    pInfo->nSSDMinEraseCount = -1;
    pInfo->nSSDWearLevelingCount = -1;

    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0) continue;

        switch (pA->bAttrID) {
        /* Remaining Life / SSD Life Left */
        case 0xA9:
            if (pInfo->nSSDLifeLeft < 0)
                pInfo->nSSDLifeLeft = (int)pA->bAttrValue;
            break;
        case 0xE7:
            if (pInfo->nSSDLifeLeft < 0) {
                int v = (int)GetRawValue16Lo(pA->bRawValue);
                if (v >= 0 && v <= 100)
                    pInfo->nSSDLifeLeft = v;
                else if (pA->bAttrValue >= 0 && pA->bAttrValue <= 100)
                    pInfo->nSSDLifeLeft = (int)pA->bAttrValue;
            }
            break;

        /* Total writes / NAND writes */
        case 0xE9:
            if (pInfo->nSSDTotalWritesGB < 0)
                pInfo->nSSDTotalWritesGB = (int)GetRawValue(pA->bRawValue);
            break;
        case 0xF9:
            if (pInfo->nSSDTotalWritesGB < 0)
                pInfo->nSSDTotalWritesGB = (int)GetRawValue(pA->bRawValue);
            break;
        /* Host writes: 48-bit LBA count (512 B). Skip tiny RAWs (vendor packing). */
        case 0xF1:
        case 0xF3: {
            unsigned __int64 nLBA, nGiB;
            if (pInfo->nSSDTotalWritesGB >= 0) break;
            nLBA = GetRawValue48(pA->bRawValue);
            if (nLBA < 2048ULL) break;
            nGiB = nLBA / (1024ULL * 1024ULL * 2ULL);
            if (nGiB > 4000000ULL) nGiB = 4000000ULL;
            pInfo->nSSDTotalWritesGB = (int)nGiB;
            break;
        }

        /* Average erase count */
        case 0xA7:
            if (pInfo->nSSDAvgEraseCount < 0)
                pInfo->nSSDAvgEraseCount = (int)GetRawValue(pA->bRawValue);
            break;
        case 0xAD: {
            /* 0xAD: Samsung = wear leveling only; Intel = avg erase only.
             * Other vendors may fill both when neither is set yet. */
            int nAd = (int)GetRawValue(pA->bRawValue);
            if (pInfo->eController == CONTROLLER_SAMSUNG) {
                if (pInfo->nSSDWearLevelingCount < 0)
                    pInfo->nSSDWearLevelingCount = nAd;
            } else if (pInfo->eController == CONTROLLER_INTEL) {
                if (pInfo->nSSDAvgEraseCount < 0)
                    pInfo->nSSDAvgEraseCount = nAd;
            } else {
                if (pInfo->nSSDWearLevelingCount < 0)
                    pInfo->nSSDWearLevelingCount = nAd;
                if (pInfo->nSSDAvgEraseCount < 0)
                    pInfo->nSSDAvgEraseCount = nAd;
            }
            break;
        }
        case 0xEA:
            if (pInfo->nSSDAvgEraseCount < 0)
                pInfo->nSSDAvgEraseCount = (int)GetRawValue(pA->bRawValue);
            break;

        /* Max erase count */
        case 0xA5:
            if (pInfo->nSSDMaxEraseCount < 0)
                pInfo->nSSDMaxEraseCount = (int)GetRawValue(pA->bRawValue);
            break;

        /* Min erase count */
        case 0xA6:
            if (pInfo->nSSDMinEraseCount < 0)
                pInfo->nSSDMinEraseCount = (int)GetRawValue(pA->bRawValue);
            break;
        }
    }
}

/* ============================================================
 * Temperature extraction from ATA attributes
 * ============================================================ */
static void ExtractTemperatureFromATA(DRIVE_INFO* pInfo)
{
    int i;
    IdentifyDriveParts(pInfo);
    /* Primary: 0xC2 (Temperature) */
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0xC2) {
            int t = (int)GetRawValue16Lo(pA->bRawValue);
            if (t <= 0 || t > 150) t = (int)pA->bAttrValue;
            if (t > 0 && t <= 150) { pInfo->nTemperatureC = t; return; }
        }
    }
    /* Fallback: 0xBE (Airflow Temperature) */
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0xBE && pInfo->nTemperatureC < 0) {
            int t = (int)pA->bAttrValue;
            if (t > 0 && t <= 150) pInfo->nTemperatureC = t;
            return;
        }
        if (pA->bAttrID == 0xE7 && pInfo->nTemperatureC < 0) {
            /* Phison: 0xE7 is life remaining, not °C. Brand (Kingston/ADATA)
             * must not trigger this — only the controller. */
            if (pInfo->eController == CONTROLLER_PHISON)
                continue;
            {
                int nT = (int)GetRawValue16Lo(pA->bRawValue);
                if (nT > 0 && nT <= 150) pInfo->nTemperatureC = nT;
            }
        }
    }
}

static void ExtractCommonATACounters(DRIVE_INFO* pInfo)
{
    int i;
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0x09 && pInfo->dwPowerOnHours == 0)
            pInfo->dwPowerOnHours = GetRawValue(pA->bRawValue);
        else if (pA->bAttrID == 0x0C && pInfo->dwPowerCycleCount == 0)
            pInfo->dwPowerCycleCount = GetRawValue(pA->bRawValue);
    }
}

/* ============================================================
 * SMART data validation
 *
 * Validates the SMART read data buffer by:
 * 1. Checking that it's not all zeros
 * 2. Counting valid (non-zero ID) attributes
 * 3. Optional checksum verification (last byte of 512)
 * ============================================================ */
static BOOL ValidateSmartData(const BYTE* pRawBuf, int nBufLen)
{
    /* Check not all zero */
    if (IsBufferAllZero(pRawBuf, nBufLen)) return FALSE;

    /* Check at least one valid attribute ID exists */
    int nValidAttrs = 0;
    int i;
    for (i = 0; i < 30; i++) {
        BYTE bID = pRawBuf[2 + i * 12];
        if (bID != 0) nValidAttrs++;
    }
    if (nValidAttrs == 0) return FALSE;

    return TRUE;
}

/* ============================================================
 * FillSmartData 
 *
 * Parses the raw 512-byte SMART data buffer
 * by iterating through 12-byte attribute entries, skipping
 * entries with ID=0, and compacting into the Attribute array.
 * This is more robust than simple memcpy because it validates
 * each entry individually and removes gaps.
 * ============================================================ */
static int FillSmartData(DRIVE_INFO* pInfo, const BYTE* pRawBuf)
{
    int nCount = 0;
    int i;
    ZeroMemory(&pInfo->attrData, sizeof(SMART_ATTRIBUTE_DATA));
    pInfo->attrData.wRevisionNumber = (WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8);

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        BYTE bID = pEntry[0];
        if (bID == 0) continue;

        /* ATA 12-byte attr: ID, flags[1..2], value, worst, raw[5..10], reserved[11]. */
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[nCount];
        pA->bAttrID      = bID;
        pA->wStatusFlags = (WORD)pEntry[1] | ((WORD)pEntry[2] << 8);
        pA->bAttrValue   = pEntry[3];
        pA->bWorstValue  = pEntry[4];
        memcpy(pA->bRawValue, &pEntry[5], 6);
        pA->bReserved    = pEntry[11];
        nCount++;
    }
    return nCount;
}

/* ============================================================
 * FillSmartThreshold 
 *
 * Matches thresholds by attribute ID to the
 * already-parsed attribute array. This handles misaligned or
 * vendor-specific threshold data correctly.
 * ============================================================ */
static int FillSmartThreshold(DRIVE_INFO* pInfo, const BYTE* pRawBuf)
{
    int nCount = 0;
    int i, j;
    ZeroMemory(&pInfo->threshData, sizeof(SMART_THRESHOLD_DATA));
    pInfo->threshData.wRevisionNumber = (WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8);

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        BYTE bID = pEntry[0];
        if (bID == 0) continue;

        /* Find matching attribute by ID */
        for (j = 0; j < 30; j++) {
            if (pInfo->attrData.stAttributes[j].bAttrID == bID) {
                pInfo->threshData.stThresholds[j].bAttrID = bID;
                pInfo->threshData.stThresholds[j].bThresholdValue = pEntry[1];
                nCount++;
                break;
            }
        }
    }
    return nCount;
}

/* ============================================================
 * Multi-path SMART acquisition for an internal/SATA drive
 *
 * Priority order (most modern/reliable first):
 *   USB drives: 1. SAT (SCSI/ATA Translation - A1h/85h)
 *              2. Storage Protocol query
 *              3. ATA PASS THROUGH
 *              4. Legacy IOCTL
 *
 * Internal drives:
 *   1. ATA PASS THROUGH (IOCTL_ATA_PASS_THROUGH)  ← CDI preferred
 *   2. Legacy IOCTL (SMART_RCV_DRIVE_DATA)          ← CDI fallback
 *   3. SAT (SCSI/ATA Translation - A1h/85h)
 *   4. Storage Protocol query
 *
 * Prefers ATA Pass-Through for internal drives
 * but SAT-first for USB drives (bridge chips need SCSI commands).
 * ============================================================ */
static BOOL AcquireATASMART(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo, BOOL bReadLogs)
{
    BOOL bAttr = FALSE, bThresh = FALSE;

    /* Enable SMART via both paths */
    EnableSMART(hDrive, nDrive);
    EnableSMARTSAT(hDrive);

    /* ---- For USB drives: prioritize SAT  ---- */
    if (pInfo->bIsUSB) {
        if (GetSMARTAttributesSAT(hDrive, pInfo)) {
            bAttr = TRUE;
            pInfo->eAccessMethod = SMART_ACCESS_SAT16;
            bThresh = GetSMARTThresholdsSAT(hDrive, pInfo);
        }
        if (!bAttr && GetSMARTViaStorageProtocol(hDrive, pInfo)) {
            bAttr = TRUE;
            bThresh = TRUE;
        }
    }

    /* ---- Path 1: ATA PASS THROUGH  ---- */
    if (!bAttr && GetSMARTAttributesATAPassthrough(hDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_ATA_PASSTHROUGH;
        bThresh = GetSMARTThresholdsATAPassthrough(hDrive, pInfo);
    }

    /* ---- Path 2: Legacy IOCTL ---- */
    if (!bAttr && GetSMARTAttributes(hDrive, nDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_LEGACY_IOCTL;
        bThresh = GetSMARTThresholds(hDrive, nDrive, pInfo);
    }

    /* ---- Path 3: SAT (SCSI A1h/85h) — for USB enclosures ---- */
    if (!bAttr && GetSMARTAttributesSAT(hDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_SAT16;
        bThresh = GetSMARTThresholdsSAT(hDrive, pInfo);
    }

    /* ---- Path 4: Windows storage protocol query ---- */
    if (!bAttr && GetSMARTViaStorageProtocol(hDrive, pInfo)) {
        bAttr = TRUE;
        bThresh = TRUE;
    }

    if (bAttr) {
        /* Validate SMART data */
        if (!ValidateSmartData((const BYTE*)&pInfo->attrData,
                               sizeof(SMART_ATTRIBUTE_DATA))) {
            /* Data may be invalid — but still try to use what we have */
        }

        GetSMARTPredictFailure(hDrive, nDrive, &pInfo->bPredictFailure);
        ExtractTemperatureFromATA(pInfo);
        ExtractCommonATACounters(pInfo);
        ExtractSSDIndicators(pInfo);

        /* SMART error / self-test logs: full scan only. Extra SMART READ LOG
         * on USB/HDD every refresh can AV the usbstor/atapi stack. */
        if (bReadLogs) {
            if (!GetSMARTErrorLog(hDrive, nDrive, pInfo)) {
                if (!GetSMARTErrorLogATAPassthrough(hDrive, pInfo))
                    GetSMARTErrorLogSAT(hDrive, pInfo);
            }
            if (!GetSMARTSelfTestLog(hDrive, nDrive, pInfo)) {
                if (!GetSMARTSelfTestLogATAPassthrough(hDrive, pInfo))
                    GetSMARTSelfTestLogSAT(hDrive, pInfo);
            }
        }
    } else {
        pInfo->bSMART_Supported = FALSE;
        pInfo->nHealthPercent   = -1;
    }
    (void)bThresh;
    return bAttr;
}

/* ============================================================
 * USB bridge chip detection
 * CDI uses VID/PID matching to select the correct command type
 * for each bridge chip family.
 * ============================================================ */
USB_BRIDGE_TYPE DetectUsbBridgeType(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    WORD vid = pInfo->wUsbVid;
    WORD pid = pInfo->wUsbPid;

    if (vid == 0 || pid == 0) {
        GetUSBVidPid(hDrive, &pInfo->wUsbVid, &pInfo->wUsbPid);
        vid = pInfo->wUsbVid;
        pid = pInfo->wUsbPid;
    }

    /* JMicron SATA bridges */
    if (vid == 0x152D) {
        if (pid == 0x0583 || pid == 0x0586 || pid == 0x058C || pid == 0x058F)
            return USB_BRIDGE_NVME_JMICRON;  /* NVMe bridges */
        if (pid == 0x0578 || pid == 0x0565 || pid == 0x0562 || pid == 0x0566 ||
            pid == 0x0579 || pid == 0x0577 || pid == 0x2566 || pid == 0x0571)
            return USB_BRIDGE_JMICRON;       /* SATA bridges */
        /* Default JMicron → try JMicron SAT first */
        return USB_BRIDGE_JMICRON;
    }

    /* ASMedia bridges */
    if (vid == 0x174C) {
        if (pid == 0x2362 || pid == 0x2364)
            return USB_BRIDGE_NVME_ASMEDIA;
        if (pid == 0x1352 || pid == 0x1351)
            return USB_BRIDGE_ASM1352R;
        return USB_BRIDGE_SAT;  /* Other ASMedia → standard SAT */
    }

    /* Realtek bridges (RTL9210/B dual-mode and other USB-SATA) */
    if (vid == 0x0BDA) {
        if ((pid & 0xFF00) == 0x9200 || pid == 0x9210 || pid == 0x9211 ||
            pid == 0x9220 || pid == 0x9221)
            return USB_BRIDGE_NVME_REALTEK;
        return USB_BRIDGE_SAT;
    }

    /* Sunplus bridges */
    if (vid == 0x04FC) return USB_BRIDGE_SUNPLUS;
    if (vid == 0x04E8 && pid == 0x5100) return USB_BRIDGE_SUNPLUS; /* Samsung USB */

    /* Cypress/I-O Data bridges */
    if (vid == 0x04B4) return USB_BRIDGE_CYPRESS;
    if (vid == 0x04BB) return USB_BRIDGE_IO_DATA;  /* I-O Data */

    /* Prolific bridges */
    if (vid == 0x067B) return USB_BRIDGE_PROLIFIC;

    /* Logitec bridges */
    if (vid == 0x0789) return USB_BRIDGE_LOGITEC;

    /* VIA Labs NVMe bridges */
    if (vid == 0x2109) {
        if (pid == 0x0900 || pid == 0x0901 || pid == 0x0902)
            return USB_BRIDGE_NVME_VLI;
        return USB_BRIDGE_SAT;
    }

    /* Initio bridges */
    if (vid == 0x13FD) return USB_BRIDGE_SAT;

    /* Western Digital bridges */
    if (vid == 0x1058) return USB_BRIDGE_SAT;

    /* Seagate bridges */
    if (vid == 0x0BC2) return USB_BRIDGE_SAT;

    /* FMA NL6221 NVMe bridge */
    if (vid == 0x0BDA) {
        /* Already handled above for Realtek NVMe bridges, but
         * additional PIDs for FMA-branded NVMe bridges */
        if (pid == 0x9220 || pid == 0x9221)
            return USB_BRIDGE_NVME_FMA;
        return USB_BRIDGE_NVME_REALTEK;
    }

    /* Default: try standard SAT */
    return USB_BRIDGE_SAT;
}

/* ============================================================
 * NVMe-over-USB: JMicron JMS583/586 (CDI-style)
 * JMicron NVMe bridges use vendor-specific SCSI commands
 * with opcode 0xA1 and "NVMe" signature in the data buffer.
 * ============================================================ */
typedef struct _CDI_SAT_PASSTHROUGH_BUF_4K {
    SCSI_PASS_THROUGH spt;
    ULONG  Filler;
    BYTE   SenseBuf[32];
    BYTE   DataBuf[4096];
} CDI_SAT_PASSTHROUGH_BUF_4K;

/* Heap SPT buffer for Realtek 0xE4 (Identify 4096 / Health 512).
 * Stack 4K SPT was OK for a single probe; keep it off the stack anyway. */
static CDI_SAT_PASSTHROUGH_BUF_4K* AllocSptBuf4K(void)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* p =
        (CDI_SAT_PASSTHROUGH_BUF_4K*)VirtualAlloc(
            NULL, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p)
        ZeroMemory(p, sizeof(*p));
    return p;
}

static void FreeSptBuf4K(CDI_SAT_PASSTHROUGH_BUF_4K* p)
{
    if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

static BOOL NVMeIdentifyJMicron(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Send NVMe Identify command request */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;   /* NVMe PASS THROUGH */
    sptwb.spt.Cdb[1] = 0x80;   /* ADMIN command */
    sptwb.spt.Cdb[4] = 0x02;   /* Identify */

    /* NVMe signature and Identify Controller command */
    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';
    sptwb.DataBuf[8] = 0x06;   /* NVMe Identify opcode */
    sptwb.DataBuf[0x30] = 0x01; /* CNS = 1 (Controller) */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read NVMe Identify response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;   /* NVMe PASS THROUGH */
    sptwb.spt.Cdb[1] = 0x82;   /* ADMIN + DMA-IN */
    sptwb.spt.Cdb[4] = 0x10;   /* Transfer length */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Extract model/serial/firmware from NVMe Identify data.
     * NVMe uses direct ASCII (no byte-swap needed), unlike ATA.
     * memcpy for NVMe strings. */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        /* NVMe Identify Controller: Serial at offset 4 (20 bytes),
         * Model at offset 24 (40 bytes), Firmware at offset 64 (8 bytes) */
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);

        /* Extract temperature thresholds from NVMe Identify */
        pInfo->wNVMeWarnTempThreshold = ReadLE16(pInfo->nvmeIdent.WCTEMP);
        pInfo->wNVMeCritTempThreshold = ReadLE16(pInfo->nvmeIdent.CCTEMP);
    }

    return (pInfo->szModel[0] != '\0');
}

static BOOL NVMeHealthLogJMicron(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Request NVMe Health Log */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;
    sptwb.spt.Cdb[1] = 0x80;   /* ADMIN */
    sptwb.spt.Cdb[4] = 0x02;   /* Get Log Page */

    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';
    sptwb.DataBuf[8] = 0x02;   /* NVMe Get Log Page opcode */
    sptwb.DataBuf[0x30] = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read Health Log response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;
    sptwb.spt.Cdb[1] = 0x82;   /* ADMIN + DMA-IN */
    sptwb.spt.Cdb[4] = 0x02;   /* Transfer length for 512 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: ASMedia ASM2362 (CDI-style)
 * ASMedia uses vendor-specific opcode 0xE6 for Identify
 * and 0xE7 for Get Log Page.
 * ============================================================ */
static BOOL NVMeIdentifyASMedia(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0] = 0xE6;   /* ASMedia NVMe Pass-Through */
    sptwb.spt.Cdb[1] = 0x06;   /* Identify */
    sptwb.spt.Cdb[3] = 0x01;   /* CNS = 1 */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* NVMe Identify uses direct ASCII, no byte-swap needed.
     * Uses memcpy for NVMe strings. */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);

        /* Extract temperature thresholds from NVMe Identify */
        pInfo->wNVMeWarnTempThreshold = ReadLE16(pInfo->nvmeIdent.WCTEMP);
        pInfo->wNVMeCritTempThreshold = ReadLE16(pInfo->nvmeIdent.CCTEMP);
    }

    return (pInfo->szModel[0] != '\0');
}

static BOOL NVMeHealthLogASMedia(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0] = 0xE7;   /* ASMedia NVMe Get Log */
    sptwb.spt.Cdb[1] = 0x02;   /* Log Page 02h (Health) */
    sptwb.spt.Cdb[3] = NVME_LOG_PAGE_HEALTH_INFO;

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: Realtek RTL9210 (CDI-style)
 * Realtek uses vendor-specific opcode 0xE4 for Read and
 * 0xE5 for Write, with subcommands in CDB[3].
 * ============================================================ */
static BOOL NVMeIdentifyRealtek(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* psptwb;
    DWORD dwReturned = 0;
    DWORD length;
    BYTE* pIdentBuf;
    BOOL bOk;

    psptwb = AllocSptBuf4K();
    if (!psptwb)
        return FALSE;

    psptwb->spt.Length             = sizeof(SCSI_PASS_THROUGH);
    psptwb->spt.PathId             = 0;
    psptwb->spt.TargetId           = 0;
    psptwb->spt.Lun                = 0;
    psptwb->spt.SenseInfoLength    = 32;
    psptwb->spt.DataIn             = SCSI_IOCTL_DATA_IN;
    psptwb->spt.DataTransferLength = 4096;
    psptwb->spt.TimeOutValue       = 8;
    psptwb->spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    psptwb->spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    psptwb->spt.CdbLength          = 16;

    psptwb->spt.Cdb[0] = 0xE4;   /* Realtek NVMe Read — never 0xE5 (write) */
    psptwb->spt.Cdb[1] = (BYTE)(4096);         /* Transfer length low */
    psptwb->spt.Cdb[2] = (BYTE)(4096 >> 8);    /* Transfer length high */
    psptwb->spt.Cdb[3] = 0x06;   /* Identify */
    psptwb->spt.Cdb[4] = 0x01;   /* CNS = 1 */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            psptwb, length, psptwb, length, &dwReturned, NULL)) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    /* NVMe Identify uses direct ASCII, no byte-swap needed.
     * CopyNvmeIdentBuf never copies more than 4096 into nvmeIdent. */
    pIdentBuf = psptwb->DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);

        pInfo->wNVMeWarnTempThreshold = ReadLE16(pInfo->nvmeIdent.WCTEMP);
        pInfo->wNVMeCritTempThreshold = ReadLE16(pInfo->nvmeIdent.CCTEMP);
    }

    bOk = (pInfo->szModel[0] != '\0');
    FreeSptBuf4K(psptwb);
    return bOk;
}

static BOOL NVMeHealthLogRealtek(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* psptwb;
    DWORD dwReturned = 0;
    DWORD length;
    DWORD nCopy;

    psptwb = AllocSptBuf4K();
    if (!psptwb)
        return FALSE;

    psptwb->spt.Length             = sizeof(SCSI_PASS_THROUGH);
    psptwb->spt.PathId             = 0;
    psptwb->spt.TargetId           = 0;
    psptwb->spt.Lun                = 0;
    psptwb->spt.SenseInfoLength    = 32;
    psptwb->spt.DataIn             = SCSI_IOCTL_DATA_IN;
    psptwb->spt.DataTransferLength = 512;
    psptwb->spt.TimeOutValue       = 8;
    psptwb->spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    psptwb->spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    psptwb->spt.CdbLength          = 16;

    psptwb->spt.Cdb[0] = 0xE4;   /* Realtek NVMe Read — never 0xE5 (write) */
    psptwb->spt.Cdb[1] = (BYTE)(512);          /* Transfer length low */
    psptwb->spt.Cdb[2] = (BYTE)(512 >> 8);     /* Transfer length high */
    psptwb->spt.Cdb[3] = 0x02;   /* Get Log Page */
    psptwb->spt.Cdb[4] = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            psptwb, length, psptwb, length, &dwReturned, NULL)) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    if (IsBufferAllZero(psptwb->DataBuf, sizeof(NVME_HEALTH_INFO_LOG))) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    nCopy = sizeof(NVME_HEALTH_INFO_LOG);
    if (nCopy > 512)
        nCopy = 512;
    memcpy(&pInfo->nvmeHealth, psptwb->DataBuf, nCopy);
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    FreeSptBuf4K(psptwb);
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: VLI VL716/VL717 (CDI-style)
 * VLI NVMe bridges use vendor-specific SCSI commands.
 * VL716 uses opcode 0xC0 for NVMe passthrough with a
 * proprietary command structure similar to JMicron.
 * ============================================================ */
static BOOL NVMeIdentifyVLI(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Send NVMe Identify command request */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x01;   /* Admin command */
    sptwb.spt.Cdb[2]  = 0x06;   /* Identify */
    sptwb.spt.Cdb[3]  = 0x01;   /* CNS = 1 (Controller) */
    sptwb.spt.Cdb[6]  = 0x00;   /* Namespace = 0 */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length 512*2 */

    /* NVMe signature */
    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read NVMe Identify response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x02;   /* DMA-IN */
    sptwb.spt.Cdb[10] = 0x20;   /* Transfer length for 4096 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* NVMe Identify uses direct ASCII, no byte-swap needed */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);

        pInfo->wNVMeWarnTempThreshold = ReadLE16(pInfo->nvmeIdent.WCTEMP);
        pInfo->wNVMeCritTempThreshold = ReadLE16(pInfo->nvmeIdent.CCTEMP);
    }

    return (pInfo->szModel[0] != '\0');
}

static BOOL NVMeHealthLogVLI(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Request NVMe Health Log */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x01;   /* Admin command */
    sptwb.spt.Cdb[2]  = 0x02;   /* Get Log Page */
    sptwb.spt.Cdb[3]  = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length */

    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read Health Log response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x02;   /* DMA-IN */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length for 512 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: FMA NL6221 (CDI-style)
 * FMA NL6221 uses Realtek-compatible commands with slight
 * variations. We try the Realtek protocol first, then
 * fallback to a generic NVMe-over-USB approach.
 * ============================================================ */

/* ============================================================
 * Generic NVMe-over-USB bridge detection and SMART reading
 * Tries all known bridge protocols in sequence
 * when the bridge type is unknown or auto-detect fails.
 * ============================================================ */
static BOOL NVMeOverUSBTryAll(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    /* Try known NVMe-over-USB bridge protocols. Never shotgun JMicron /
     * ASMedia / VLI / native NvmeMini / 0xE4 at a known Realtek RTL9210.
     * Realtek 0xE4 is a one-shot in ScanDrivesEx only. */

    if (IsRealtekNvmeUsbBridge(pInfo))
        return FALSE;

    /* 1. JMicron JMS583/586 */
    if (NVMeIdentifyJMicron(hDrive, pInfo)) {
        if (NVMeHealthLogJMicron(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 2. ASMedia ASM2362 */
    if (NVMeIdentifyASMedia(hDrive, pInfo)) {
        if (NVMeHealthLogASMedia(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 3. Realtek RTL9210 — 0xE4 only from ScanDrivesEx, never here. */

    /* 4. VLI VL716/VL717 */
    if (NVMeIdentifyVLI(hDrive, pInfo)) {
        if (NVMeHealthLogVLI(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 5. Native NVMe protocol query. GetNVMeIdentifyController /
     * GetNVMeHealthLogEx refuse USB (bus type 7), so this is a no-op
     * on USB handles. Thunderbolt/internal NVMe is not USB. */
    if (GetNVMeIdentifyController(hDrive, pInfo)) {
        if (GetNVMeHealthLogEx(hDrive, pInfo)) {
            return TRUE;
        }
    }

    return FALSE;
}
BOOL RefreshDriveSmart(int nDriveIndex, DRIVE_INFO* pInfo)
{
    HANDLE hDrive;
    if (!pInfo) return FALSE;
    if (IsLikelyUsbFlashDrive(pInfo)) return FALSE;
    /* USB (incl. Realtek RTL9210): never IOCTL on the 30s timer.
     * Keep the last full-scan snapshot. Native NvmeMini / vendor 0xE4
     * on a USB handle crashes the process. */
    if (pInfo->bIsUSB) return FALSE;
    if (!OpenDrive(nDriveIndex, &hDrive)) return FALSE;

    if (pInfo->bIsNVMe) {
        /* Native NVMe only. Do not shotgun NVMeOverUSBTryAll here. */
        GetNVMeHealthLogEx(hDrive, pInfo);
        ExtractNVMeExtendedInfo(pInfo);
        pInfo->nHealthPercent = pInfo->bSMART_Supported
            ? CalculateHealthNVMe(pInfo) : -1;
    } else if (pInfo->bSMART_Supported) {
        AcquireATASMART(hDrive, nDriveIndex, pInfo, FALSE);
        pInfo->nHealthPercent = CalculateHealth(pInfo);
    }
    pInfo->nPerformancePercent = CalculatePerformance(pInfo);
    pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
    IdentifyDriveParts(pInfo);

    CloseHandle(hDrive);
    return TRUE;
}


/* Heuristic: consumer USB flash almost never exposes SMART. Known HDD/SSD
 * enclosure bridges (RTL9210, JMicron, ASMedia, Realtek NVMe, …) are NOT flash. */
BOOL UsbBridgeLooksLikeEnclosure(const DRIVE_INFO* p)
{
    char hay[384];
    if (!p) return FALSE;
    /* USB VID of known enclosure chips — even when INQUIRY is the disk. */
    if (p->wUsbVid == 0x0BDA || p->wUsbVid == 0x152D || p->wUsbVid == 0x174C ||
        p->wUsbVid == 0x2109 || p->wUsbVid == 0x13FD || p->wUsbVid == 0x1058 ||
        p->wUsbVid == 0x0BC2)
        return TRUE;
    switch (p->eUsbBridgeType) {
    case USB_BRIDGE_NVME_JMICRON:
    case USB_BRIDGE_NVME_ASMEDIA:
    case USB_BRIDGE_NVME_REALTEK:
    case USB_BRIDGE_NVME_VLI:
    case USB_BRIDGE_NVME_FMA:
    case USB_BRIDGE_ASM1352R:
    case USB_BRIDGE_JMICRON:
    case USB_BRIDGE_SUNPLUS:
    case USB_BRIDGE_CYPRESS:
    case USB_BRIDGE_IO_DATA:
    case USB_BRIDGE_LOGITEC:
    case USB_BRIDGE_PROLIFIC:
        return TRUE;
    default:
        break;
    }
    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);
    if (strstr(hay, "RTL9210") || strstr(hay, "RTL921") ||
        strstr(hay, "Realtek") || strstr(hay, "REALTEK") ||
        strstr(hay, "JMS") || strstr(hay, "JMicron") || strstr(hay, "JMICRON") ||
        strstr(hay, "ASMedia") || strstr(hay, "ASMEDIA") ||
        strstr(hay, "ASM1") || strstr(hay, "ASM2") ||
        strstr(hay, "VL71") || strstr(hay, "VL716") || strstr(hay, "VL717"))
        return TRUE;
    return FALSE;
}

BOOL IsLikelyUsbFlashDrive(const DRIVE_INFO* p)
{
    char hay[384];
    if (!p || !p->bIsUSB || p->bIsNVMe) return FALSE;
    if (UsbBridgeLooksLikeEnclosure(p)) return FALSE;

    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);

    /* SATA/NVMe SSD in an enclosure (Kingston A400, etc.) is NOT a stick. */
    if (strstr(hay, "A400") || strstr(hay, "SA400") ||
        strstr(hay, "SSD") || strstr(hay, "NVMe") || strstr(hay, "NVME"))
        return FALSE;

    /* Brand alone is not enough: Kingston/SanDisk also make SATA SSDs.
     * Match flash product names only. */
    if (strstr(hay, "Cruzer") || strstr(hay, "DataTraveler") ||
        strstr(hay, "JetFlash") || strstr(hay, "Flash Drive") ||
        strstr(hay, "USB DISK") || strstr(hay, "USB Flash") ||
        strstr(hay, "3.2Gen") || strstr(hay, "Ultra Fit") ||
        strstr(hay, "UFD") != NULL)
        return TRUE;
    return FALSE;
}

int ScanDrives(DRIVE_INFO* pDrives, int nMaxDrives)
{
    return ScanDrivesEx(pDrives, nMaxDrives, FALSE);
}

int ScanDrivesEx(DRIVE_INFO* pDrives, int nMaxDrives, BOOL bMeasureSpeed)
{
    int nFound = 0;
    int nDrive;
    const int nScanLimit = 32;

    /* Never probe 4MB PhysicalDrive read/write — USB flash AVs/hangs. */
    (void)bMeasureSpeed;
    bMeasureSpeed = FALSE;

    for (nDrive = 0; nDrive < nScanLimit && nFound < nMaxDrives; nDrive++) {
        HANDLE hDrive;
        if (!OpenDrive(nDrive, &hDrive)) continue;

        DRIVE_INFO* pInfo = &pDrives[nFound];
        ZeroMemory(pInfo, sizeof(DRIVE_INFO));
        pInfo->nDriveIndex    = nDrive;
        pInfo->nTemperatureC  = -1;
        pInfo->nHealthPercent = -1;
        pInfo->nConfidence    = 0;
        pInfo->nEndurancePercent = -1;
        pInfo->eReliability   = HEALTH_STATUS_UNKNOWN;
        pInfo->eInterface     = HEALTH_STATUS_UNKNOWN;
        pInfo->eTempStatus    = HEALTH_STATUS_UNKNOWN;
        pInfo->nReallocated   = -1;
        pInfo->nPendingSectors = -1;
        pInfo->nUncorrectable = -1;
        pInfo->nRemapEvents   = -1;
        pInfo->nCrcErrors     = -1;
        pInfo->eTempBand      = TEMP_BAND_UNKNOWN;
        pInfo->eNormQuality   = NORM_QUALITY_UNKNOWN;
        pInfo->eRawQuality    = NORM_QUALITY_UNKNOWN;
        pInfo->bThresholdViolation = FALSE;
        pInfo->szEvidence[0]  = '\0';
        pInfo->nReadSpeedMBs  = -1;
        pInfo->nWriteSpeedMBs = -1;
        pInfo->eType          = DRIVE_TYPE_UNKNOWN;
        pInfo->eAccessMethod  = SMART_ACCESS_NONE;
        pInfo->eHealthStatus  = HEALTH_STATUS_UNKNOWN;
        pInfo->eVendor        = VENDOR_UNKNOWN;
        pInfo->eController    = CONTROLLER_UNKNOWN;
        pInfo->eNand          = NAND_UNKNOWN;
        pInfo->nSSDLifeLeft   = -1;
        pInfo->nSSDTotalWritesGB = -1;
        pInfo->nSSDAvgEraseCount = -1;
        pInfo->nSSDMaxEraseCount = -1;
        pInfo->nSSDMinEraseCount = -1;
        pInfo->nSSDWearLevelingCount = -1;
        {
            int ts;
            for (ts = 0; ts < 8; ts++) pInfo->nTempSensor[ts] = -1;
        }

        BYTE busType = GetStorageBusType(hDrive);

        /* --------------------- NVMe (not USB bridges) --------------------- */
        if (IsNVMeDrive(hDrive) && busType != 7) {
            GetNVMeInfo(hDrive, pInfo);

            if (pInfo->szModel[0] == '\0') GetDeviceDescriptor(hDrive, pInfo);
            if (pInfo->dwCapacityMB == 0)  GetCapacityFromGeometry(hDrive, pInfo);

            pInfo->nHealthPercent      = pInfo->bSMART_Supported
                                         ? CalculateHealthNVMe(pInfo) : -1;
            pInfo->nPerformancePercent = CalculatePerformance(pInfo);
            pInfo->eType               = DRIVE_TYPE_NVME;
            pInfo->bIsNVMe             = TRUE;
            pInfo->eHealthStatus       = DetermineHealthStatus(pInfo);
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);

            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* --------------------- eMMC / SD --------------------- */
        if (busType == 12 || busType == 13) {
            GetDeviceDescriptor(hDrive, pInfo);
            GetCapacityFromGeometry(hDrive, pInfo);
            pInfo->eType = (busType == 13) ? DRIVE_TYPE_SD : DRIVE_TYPE_EMMC;
            pInfo->bSMART_Supported = FALSE;
            pInfo->nHealthPercent   = -1;
            pInfo->eHealthStatus    = HEALTH_STATUS_UNKNOWN;
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);
            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* --------------------- ATA / USB / SAS --------------------- */
        /* try ATA Pass-Through first for IDENTIFY,
         * then fall back to legacy IOCTL, then SAT/USB. */
        BOOL bIdentOK = FALSE;

        if (GetIdentifyDataATAPassthrough(hDrive, pInfo)) bIdentOK = TRUE;
        if (!bIdentOK && GetIdentifyData(hDrive, nDrive, pInfo)) bIdentOK = TRUE;
        if (!bIdentOK && GetIdentifyDataSAT(hDrive, pInfo)) {
            bIdentOK = TRUE;
            if (busType == 7) pInfo->bIsUSB = TRUE;
        }
        if (!bIdentOK) {
            bIdentOK = GetIdentifyDataUSB(hDrive, pInfo);
            if (!bIdentOK) {
                CloseHandle(hDrive);
                continue;
            }
        }

        if (pInfo->szModel[0] == '\0' || pInfo->szSerial[0] == '\0')
            GetDeviceDescriptor(hDrive, pInfo);
        if (pInfo->dwCapacityMB == 0)
            GetCapacityFromGeometry(hDrive, pInfo);

        if (busType == 7 && !pInfo->bIsUSB) pInfo->bIsUSB = TRUE;
        pInfo->eType = DetectDriveType(hDrive, pInfo);

        IdentifyDriveParts(pInfo);

        if (pInfo->bIsUSB) {
            GetBridgeIdentity(hDrive, pInfo);
            GetUSBVidPid(hDrive, &pInfo->wUsbVid, &pInfo->wUsbPid);
            pInfo->eUsbBridgeType = DetectUsbBridgeType(hDrive, pInfo);

            /* If no VID/PID from SetupDi, try heuristic detection from
             * the bridge vendor/product strings in STORAGE_DEVICE_DESCRIPTOR.
             * uses this as a fallback. */
            if (pInfo->wUsbVid == 0 && pInfo->wUsbPid == 0) {
                /* Try to detect NVMe bridge from product name */
                char szUpper[17];
                int k;
                for (k = 0; k < 16 && pInfo->szBridgeProduct[k]; k++)
                    szUpper[k] = (char)toupper((unsigned char)pInfo->szBridgeProduct[k]);
                szUpper[k] = '\0';

                char szUpperV[9];
                for (k = 0; k < 8 && pInfo->szBridgeVendor[k]; k++)
                    szUpperV[k] = (char)toupper((unsigned char)pInfo->szBridgeVendor[k]);
                szUpperV[k] = '\0';

                /* JMicron NVMe bridges often show "JMS583" in product ID */
                if (strstr(szUpper, "JMS583") || strstr(szUpper, "JMS586") ||
                    strstr(szUpper, "0583")   || strstr(szUpper, "0586"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_JMICRON;

                /* ASMedia NVMe bridges show "ASM2362" in product ID */
                else if (strstr(szUpper, "ASM2362") || strstr(szUpper, "2362"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_ASMEDIA;

                /* Realtek NVMe bridges show "RTL9210" in product ID */
                else if (strstr(szUpper, "RTL9210") || strstr(szUpper, "9210"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_REALTEK;

                /* VLI NVMe bridges show "VL716" or "VL717" */
                else if (strstr(szUpper, "VL716") || strstr(szUpper, "VL717"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_VLI;

                /* Also check the T10 vendor ID for known NVMe bridge makers */
                else if (strstr(szUpperV, "JMICRON"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_JMICRON;
                else if (strstr(szUpperV, "ASMEDIA"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_ASMEDIA;
                else if (strstr(szUpperV, "REALTEK"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_REALTEK;
                else if (strstr(szUpperV, "VLI"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_VLI;
            }
        }

        /* SMART data — multi-path acquisition */
        /* Skip SAT / vendor NVMe-over-USB retries on likely USB flash.
         * Still list the stick with model/capacity. Do not send Realtek 0xE4
         * or SAT to SanDisk/Kingston UFD on every refresh. */
        if (pInfo->bIsUSB && !pInfo->bIsNVMe && IsLikelyUsbFlashDrive(pInfo)) {
            pInfo->bSMART_Supported = FALSE;
        }
        else if (pInfo->bIsUSB && !pInfo->bIsNVMe) {
            /* ---- USB drive SMART acquisition ---- */
            USB_BRIDGE_TYPE bridge = pInfo->eUsbBridgeType;

            if (IsRealtekNvmeUsbBridge(pInfo) ||
                bridge == USB_BRIDGE_NVME_REALTEK ||
                bridge == USB_BRIDGE_NVME_FMA) {
                /* RTL9210B dual-mode (SATA via SAT, NVMe via 0xE4).
                 * SAT first: this enclosure often holds a SATA SSD;
                 * 0xE4 on SATA is useless. One-shot 0xE4 only if SAT
                 * SMART failed. Stay USB so RefreshDriveSmart never
                 * native-IOCTLs it. Do NOT TryAll, GetNVMeHealthLogEx,
                 * or IOCTL_STORAGE_PROTOCOL_COMMAND. */
                BOOL got = FALSE;

                /* 1) SATA behind RTL9210: AcquireATASMART is SAT-first on USB.
                 *    Earlier GetIdentifyDataSAT may already have the model;
                 *    still fetch SMART attributes. */
                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                    pInfo->nHealthPercent = CalculateHealth(pInfo);
                    pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                    /* ExtractTemperature/counters/SSD indicators:
                     * already inside AcquireATASMART. */
                }

                /* 2) NVMe behind RTL9210: one-shot vendor 0xE4 */
                if (!got) {
                    if (NVMeIdentifyRealtek(hDrive, pInfo) &&
                        NVMeHealthLogRealtek(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        pInfo->nHealthPercent = CalculateHealthNVMe(pInfo);
                        pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_JMICRON) {
                /* JMS583/586 dual-mode (SATA via SAT, NVMe via vendor).
                 * SAT first like RTL9210: enclosure may hold a SATA SSD.
                 * One-shot JMicron identify+health only if SAT SMART failed.
                 * Stay USB so RefreshDriveSmart never native-IOCTLs it.
                 * Do NOT TryAll, GetNVMeHealthLogEx, or NvmeMini. */
                BOOL got = FALSE;

                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                    pInfo->nHealthPercent = CalculateHealth(pInfo);
                    pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                }

                if (!got) {
                    if (NVMeIdentifyJMicron(hDrive, pInfo) &&
                        NVMeHealthLogJMicron(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        pInfo->nHealthPercent = CalculateHealthNVMe(pInfo);
                        pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_ASMEDIA) {
                /* ASM2362 dual-mode (SATA via SAT, NVMe via vendor).
                 * SAT first like RTL9210. One-shot ASMedia identify+health
                 * only if SAT SMART failed. Stay USB. Do NOT TryAll,
                 * GetNVMeHealthLogEx, or NvmeMini. */
                BOOL got = FALSE;

                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                    pInfo->nHealthPercent = CalculateHealth(pInfo);
                    pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                }

                if (!got) {
                    if (NVMeIdentifyASMedia(hDrive, pInfo) &&
                        NVMeHealthLogASMedia(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        pInfo->nHealthPercent = CalculateHealthNVMe(pInfo);
                        pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_VLI) {
                /* NVMe via VLI bridge (VL716/VL717) */
                if (NVMeIdentifyVLI(hDrive, pInfo)) {
                    if (NVMeHealthLogVLI(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                    }
                }
            }

            if (!pInfo->bIsNVMe &&
                !IsRealtekNvmeUsbBridge(pInfo) &&
                bridge != USB_BRIDGE_NVME_REALTEK &&
                bridge != USB_BRIDGE_NVME_FMA &&
                bridge != USB_BRIDGE_NVME_JMICRON &&
                bridge != USB_BRIDGE_NVME_ASMEDIA &&
                bridge != USB_BRIDGE_JMICRON &&
                bridge != USB_BRIDGE_SUNPLUS && bridge != USB_BRIDGE_CYPRESS &&
                bridge != USB_BRIDGE_IO_DATA && bridge != USB_BRIDGE_LOGITEC &&
                bridge != USB_BRIDGE_PROLIFIC) {
                /* Try generic NVMe-over-USB detection.
                 * Never shotgun vendor cmds at RTL9210 / JMS583 / ASM2362. */
                if (NVMeOverUSBTryAll(hDrive, pInfo)) {
                    ExtractNVMeExtendedInfo(pInfo);
                    pInfo->bIsNVMe = TRUE;
                    pInfo->eType = DRIVE_TYPE_NVME;
                }
            }

            /* If not NVMe-over-USB, or NVMe detection failed, try SATA SMART via SAT.
             * Realtek RTL9210/FMA and JMicron/ASMedia NVMe: SAT + one vendor
             * passthrough already handled above; no TryAll, GetNVMeHealthLogEx,
             * or second AcquireATASMART. */
            if (!pInfo->bIsNVMe &&
                !IsRealtekNvmeUsbBridge(pInfo) &&
                bridge != USB_BRIDGE_NVME_REALTEK &&
                bridge != USB_BRIDGE_NVME_FMA &&
                bridge != USB_BRIDGE_NVME_JMICRON &&
                bridge != USB_BRIDGE_NVME_ASMEDIA) {
                if (pInfo->bSMART_Supported) {
                    AcquireATASMART(hDrive, nDrive, pInfo, TRUE);
                } else {
                    /* Try SAT regardless of IDENTIFY's SMART supported bit */
                    if (GetSMARTAttributesSAT(hDrive, pInfo)) {
                        GetSMARTThresholdsSAT(hDrive, pInfo);
                        pInfo->bSMART_Supported = TRUE;
                        ExtractTemperatureFromATA(pInfo);
                        ExtractCommonATACounters(pInfo);
                        ExtractSSDIndicators(pInfo);
                        GetSMARTErrorLogSAT(hDrive, pInfo);
                        GetSMARTSelfTestLogSAT(hDrive, pInfo);
                    } else if (GetSMARTViaStorageProtocol(hDrive, pInfo)) {
                        pInfo->bSMART_Supported = TRUE;
                        ExtractTemperatureFromATA(pInfo);
                        ExtractCommonATACounters(pInfo);
                        ExtractSSDIndicators(pInfo);
                    } else if (GetNVMeHealthLogEx(hDrive, pInfo)) {
                        /* Fallback: detect if this is actually NVMe via USB.
                         * GetNVMeHealthLogEx already refuses bus type 7. */
                        pInfo->bIsNVMe = TRUE;
                        pInfo->eType   = DRIVE_TYPE_NVME;
                        ExtractNVMeExtendedInfo(pInfo);
                        if (pInfo->szModel[0] == '\0' || pInfo->szSerial[0] == '\0')
                            GetNVMeIdentifyController(hDrive, pInfo);
                    } else if (GetSMARTViaLogSense(hDrive, pInfo)) {
                        pInfo->bSMART_Supported = TRUE;
                    }
                }
            }
        }
        else if (pInfo->bSMART_Supported) {
            AcquireATASMART(hDrive, nDrive, pInfo, TRUE);
        }

        if (pInfo->bIsNVMe) {
            pInfo->nHealthPercent = pInfo->bSMART_Supported
                                   ? CalculateHealthNVMe(pInfo) : -1;
        } else {
            pInfo->nHealthPercent = CalculateHealth(pInfo);
        }
        pInfo->nPerformancePercent = CalculatePerformance(pInfo);
        pInfo->eHealthStatus = DetermineHealthStatus(pInfo);
        IdentifyDriveParts(pInfo);
        FillDriveProtocol(pInfo);

        CloseHandle(hDrive);
        nFound++;
    }

    return nFound;
}

/* ============================================================
 * Formatting
 * ============================================================ */
void FormatSize(DWORD dwMB, char* szBuf, int nBufLen)
{
    if (dwMB >= 1024 * 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f TB", (double)dwMB / (1024.0 * 1024.0));
    else if (dwMB >= 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f GB", (double)dwMB / 1024.0);
    else
        safe_snprintf_n(szBuf, nBufLen, "%u MB", (unsigned)dwMB);
}

