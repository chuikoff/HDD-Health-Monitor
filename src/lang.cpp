/* ============================================================================
 *  HDDHealth Monitor - Multi-Language System Implementation
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *  License : MIT
 * ============================================================================
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "lang.h"
#include "lang_zh.h"
#include "lang_en.h"
#include "smart.h"

static APP_LANG_ID s_CurrentLang = LANG_ZH_CN; /* 默认中文 */

void Lang_Init(void)
{
    /* 检测 Windows 系统 UI 语言 */
    LANGID langId = GetUserDefaultUILanguage();
    WORD primaryLang = PRIMARYLANGID(langId);

    if (primaryLang == LANG_CHINESE) {
        s_CurrentLang = LANG_ZH_CN;
    } else if (primaryLang == LANG_ENGLISH) {
        s_CurrentLang = LANG_EN;
    } else {
        /* 检测不到或为其他未知语言时，默认显示中文 (按照用户要求) */
        s_CurrentLang = LANG_ZH_CN;
    }
}

APP_LANG_ID Lang_GetCurrent(void)
{
    return s_CurrentLang;
}

void Lang_SetCurrent(APP_LANG_ID lang)
{
    if (lang >= 0 && lang < LANG_COUNT) {
        s_CurrentLang = lang;
    }
}

const char* LStr(STRING_ID id)
{
    if (id < 0 || id >= STR_COUNT) return "";

    switch (s_CurrentLang) {
    case LANG_ZH_CN:
        if (g_Strings_ZH[id] != NULL) return g_Strings_ZH[id];
        break;
    case LANG_EN:
        if (g_Strings_EN[id] != NULL) return g_Strings_EN[id];
        break;
    default:
        break;
    }

    /* Fallback */
    if (g_Strings_ZH[id] != NULL) return g_Strings_ZH[id];
    if (g_Strings_EN[id] != NULL) return g_Strings_EN[id];
    return "";
}

const char* Lang_GetDriveTypeName(DRIVE_TYPE eType)
{
    int idx = (int)eType;
    if (idx < 0 || idx > (int)DRIVE_TYPE_SCSI) idx = 0;

    switch (s_CurrentLang) {
    case LANG_ZH_CN:
        return g_DriveTypeNames_ZH[idx];
    case LANG_EN:
    default:
        return g_DriveTypeNames_EN[idx];
    }
}

const char* Lang_GetHealthStatusName(DRIVE_HEALTH_STATUS eStatus)
{
    int idx = (int)eStatus;
    if (idx < 0 || idx > (int)HEALTH_STATUS_WARNING) idx = 0;

    switch (s_CurrentLang) {
    case LANG_ZH_CN:
        return g_HealthStatusNames_ZH[idx];
    case LANG_EN:
    default:
        return g_HealthStatusNames_EN[idx];
    }
}

const char* Lang_GetSmartAttrName(BYTE bAttrID)
{
    if (s_CurrentLang == LANG_ZH_CN) {
        int i = 0;
        while (g_SmartAttrNames_ZH[i].szName != NULL) {
            if (g_SmartAttrNames_ZH[i].bID == bAttrID)
                return g_SmartAttrNames_ZH[i].szName;
            i++;
        }
    }
    return GetAttrName(bAttrID);
}

const char* Lang_GetHistoryTrackedAttrName(int index)
{
    switch (index) {
    case 0: return LStr(STR_HATTR_TEMP);
    case 1: return LStr(STR_HATTR_REALLOC);
    case 2: return LStr(STR_HATTR_PENDING);
    case 3: return LStr(STR_HATTR_UNCORRECT);
    case 4: return LStr(STR_HATTR_POH);
    case 5: return LStr(STR_HATTR_POWER_CYCLES);
    case 6: return LStr(STR_HATTR_READ_ERR_RATE);
    case 7: return LStr(STR_HATTR_CRC_ERR);
    default: return "";
    }
}

const wchar_t* Utf8ToW(const char* utf8)
{
    if (!utf8) return L"";
    static wchar_t s_wbufs[16][1024];
    static int s_widx = 0;
    int idx = (s_widx++) % 16;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s_wbufs[idx], 1024);
    s_wbufs[idx][1023] = L'\0';
    return s_wbufs[idx];
}

const wchar_t* LStrW(STRING_ID id)
{
    return Utf8ToW(LStr(id));
}

const wchar_t* Lang_GetDriveTypeNameW(DRIVE_TYPE eType)
{
    return Utf8ToW(Lang_GetDriveTypeName(eType));
}

const wchar_t* Lang_GetHealthStatusNameW(DRIVE_HEALTH_STATUS eStatus)
{
    return Utf8ToW(Lang_GetHealthStatusName(eStatus));
}

const wchar_t* Lang_GetSmartAttrNameW(BYTE bAttrID)
{
    return Utf8ToW(Lang_GetSmartAttrName(bAttrID));
}

const wchar_t* Lang_GetHistoryTrackedAttrNameW(int index)
{
    return Utf8ToW(Lang_GetHistoryTrackedAttrName(index));
}

