/* ============================================================================
 *  HDDHealth Monitor - Multi-Language System Header
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  Author  : Ari Sohandri Putra / Multi-Language Module
 *  License : MIT
 * ============================================================================
 */

#pragma once
#ifndef LANG_H
#define LANG_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "smart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_ZH_CN = 0,   /* 简体中文 (Simplified Chinese) - 默认 */
    LANG_EN    = 1,   /* English */
    LANG_COUNT = 2
} APP_LANG_ID;

typedef enum {
    /* Main Window & Menus */
    STR_APP_TITLE,
    STR_MENU_FILE,
    STR_MENU_VIEW,
    STR_MENU_LANG,
    STR_MENU_HELP,
    STR_MENU_SCREENSHOT,
    STR_MENU_SAVETEXT,
    STR_MENU_EXIT,
    STR_MENU_HISTORY,
    STR_MENU_DONATE,
    STR_MENU_ABOUT,
    STR_MENU_SHOW_WINDOW,
    STR_LANG_NAME_ZH,
    STR_LANG_NAME_EN,

    /* Main Window Labels */
    STR_LBL_DRIVES,
    STR_LBL_PERF,
    STR_LBL_HEALTH,
    STR_LBL_MODEL,
    STR_LBL_SERIAL,
    STR_LBL_FIRMWARE,
    STR_LBL_CAPACITY,
    STR_LBL_TEMPERATURE,
    STR_LBL_SMART,
    STR_LBL_READ_SPEED,
    STR_BTN_HISTORY_GRAPH,
    STR_NO_DRIVES_FOUND,

    /* List View Columns */
    STR_COL_ID,
    STR_COL_ATTRIBUTE,
    STR_COL_VALUE,
    STR_COL_STATUS,

    /* Status Badges */
    STR_STATUS_OK,
    STR_STATUS_WARNING,
    STR_STATUS_FAILED,
    STR_STATUS_NA,

    /* Drive Details & S.M.A.R.T. Texts */
    STR_TYPE_PREFIX,
    STR_HEALTH_FMT,
    STR_HEALTH_NA,
    STR_PERF_FMT,
    STR_PERF_NA,
    STR_DRIVE_FMT,
    STR_TRAY_HEALTH_FMT,
    STR_TRAY_HEALTH_NA_FMT,
    STR_TRAY_SCANNING,
    STR_NVME_HEALTH_LOG_FMT,
    STR_NVME_HEALTH_LOG_ZH_FMT,
    STR_NVME_NOT_READABLE,
    STR_USB_NOT_SUPPORTED,
    STR_USB_SAT_PASSTHROUGH_FMT,
    STR_USB_LIMITED_SCSI,
    STR_SUPPORTED_FMT,
    STR_NOT_SUPPORTED,
    STR_ENABLED,
    STR_DISABLED,
    STR_DETECTED,
    STR_NOT_AVAILABLE,
    STR_UNAVAILABLE,

    /* NVMe Critical Warning Flags */
    STR_NVME_CRIT_HEADER,
    STR_NVME_CRIT_SPARE,
    STR_NVME_CRIT_TEMP,
    STR_NVME_CRIT_RELIABILITY,
    STR_NVME_CRIT_READ_ONLY,
    STR_NVME_CRIT_VOLATILE_MEM,

    /* Alert Titles & Messages */
    STR_ALERT_OVERHEAT_TITLE,
    STR_ALERT_OVERHEAT_MSG,
    STR_ALERT_TEMP_WARN_TITLE,
    STR_ALERT_TEMP_WARN_MSG,
    STR_ALERT_HEALTH_CRIT_TITLE,
    STR_ALERT_HEALTH_CRIT_MSG,
    STR_ALERT_HEALTH_WARN_TITLE,
    STR_ALERT_HEALTH_WARN_MSG,
    STR_ALERT_FAILURE_PRED_TITLE,
    STR_ALERT_FAILURE_PRED_MSG,
    STR_ALERT_NVME_CRIT_TITLE,
    STR_ALERT_REALLOC_TITLE,
    STR_ALERT_REALLOC_MSG,
    STR_ALERT_UNCORRECT_TITLE,
    STR_ALERT_UNCORRECT_MSG,

    /* Screenshot Dialogs */
    STR_SS_SUCCESS_MSG,
    STR_SS_SUCCESS_TITLE,
    STR_SS_FAIL_MSG,
    STR_SS_FAIL_TITLE,

    /* Report Export Dialogs */
    STR_REPORT_TITLE,
    STR_REPORT_SAVED_MSG,
    STR_REPORT_SAVED_TITLE,
    STR_REPORT_FAIL_MSG,
    STR_REPORT_FAIL_TITLE,

    /* About Dialog */
    STR_ABOUT_TITLE,
    STR_ABOUT_DESC,
    STR_ABOUT_FOSS,
    STR_ABOUT_SPONSOR_LINK,
    STR_ABOUT_COPYRIGHT,
    STR_ABOUT_AUTHOR,
    STR_BTN_OK,
    STR_BTN_DONATE,
    STR_BTN_CLOSE,

    /* History Graph Window */
    STR_HIST_TITLE,
    STR_HIST_TAB_HEALTH,
    STR_HIST_TAB_ATTRS,
    STR_HIST_LBL_DRIVE,
    STR_HIST_LBL_ATTR,
    STR_HIST_BTN_CLEAR,
    STR_HIST_NO_DATA,

    /* History Graph Dropdown Tracked Attributes */
    STR_HATTR_TEMP,
    STR_HATTR_REALLOC,
    STR_HATTR_PENDING,
    STR_HATTR_UNCORRECT,
    STR_HATTR_POH,
    STR_HATTR_POWER_CYCLES,
    STR_HATTR_READ_ERR_RATE,
    STR_HATTR_CRC_ERR,

    /* Donate Dialog */
    STR_DONATE_TITLE,
    STR_DONATE_HEADER,
    STR_DONATE_EXPLAIN,
    STR_DONATE_AUTHOR_LINE,
    STR_DONATE_FAIL_MSG,
    STR_DONATE_FAIL_TITLE,

    /* S.M.A.R.T. Units & Suffixes */
    STR_UNIT_WRITTEN,
    STR_UNIT_LIFE_REMAINING,
    STR_UNIT_WEAR_CYCLES,
    STR_UNIT_FAILURES,
    STR_UNIT_RESERVED_SPACE,
    STR_UNIT_FLYING_HOURS,
    STR_UNIT_HOURS,
    STR_UNIT_MINUTES,
    STR_UNIT_UNITS,

    /* Fallback / Special USB rows */
    STR_ROW_USB_NO_SMART,
    STR_ROW_NVME_RUN_ADMIN,
    STR_ROW_NVME_WIN10_VER,
    STR_ROW_SCSI_PREDICT,
    STR_ROW_SCSI_TEMP,
    STR_ROW_SCSI_NOTE,
    STR_SCSI_PRED_FAIL,
    STR_SCSI_NO_PRED_FAIL,

    STR_COUNT
} STRING_ID;

/* Initialize language system based on Windows settings (Default: LANG_ZH_CN) */
void           Lang_Init(void);

/* Get current active language */
APP_LANG_ID    Lang_GetCurrent(void);

/* Switch language */
void           Lang_SetCurrent(APP_LANG_ID lang);

/* UTF-8 string to wide-character (thread-safe static ring buffers) */
const wchar_t* Utf8ToW(const char* utf8);

/* Get translated UTF-8 string by ID */
const char*    LStr(STRING_ID id);

/* Get translated Wide-string by ID */
const wchar_t* LStrW(STRING_ID id);

/* Get translated drive type name */
const char*    Lang_GetDriveTypeName(DRIVE_TYPE eType);
const wchar_t* Lang_GetDriveTypeNameW(DRIVE_TYPE eType);

/* Get translated health status name */
const char*    Lang_GetHealthStatusName(DRIVE_HEALTH_STATUS eStatus);
const wchar_t* Lang_GetHealthStatusNameW(DRIVE_HEALTH_STATUS eStatus);

/* Get translated S.M.A.R.T. attribute name */
const char*    Lang_GetSmartAttrName(BYTE bAttrID);
const wchar_t* Lang_GetSmartAttrNameW(BYTE bAttrID);

/* Get translated History graph tracked attribute short name */
const char*    Lang_GetHistoryTrackedAttrName(int index);
const wchar_t* Lang_GetHistoryTrackedAttrNameW(int index);

#ifdef __cplusplus
}
#endif

#endif /* LANG_H */
