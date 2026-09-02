/* ============================================================================
 *  HDDHealth Monitor - English Language Pack
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *  License : MIT
 * ============================================================================
 */

#pragma once
#ifndef LANG_EN_H
#define LANG_EN_H

#include "lang.h"

/* UI String Table (English) */
static const char* const g_Strings_EN[STR_COUNT] = {
    /* Main Window & Menus */
    /* STR_APP_TITLE          */ "HDDHealth Monitor 1.1",
    /* STR_MENU_FILE           */ "&File",
    /* STR_MENU_VIEW           */ "&View",
    /* STR_MENU_LANG           */ "&Language",
    /* STR_MENU_HELP           */ "&Help",
    /* STR_MENU_SCREENSHOT     */ "Save Screenshot\tCtrl+S",
    /* STR_MENU_SAVETEXT       */ "Save to Text File...\tCtrl+T",
    /* STR_MENU_EXIT           */ "Exit",
    /* STR_MENU_HISTORY        */ "History Graph",
    /* STR_MENU_DONATE         */ "Donate...",
    /* STR_MENU_ABOUT          */ "About HDDHealth...",
    /* STR_MENU_SHOW_WINDOW    */ "&Show Window",
    /* STR_LANG_NAME_ZH        */ "简体中文",
    /* STR_LANG_NAME_EN        */ "English",

    /* Main Window Labels */
    /* STR_LBL_DRIVES          */ "DRIVES",
    /* STR_LBL_PERF            */ "DISK PERFORMANCE",
    /* STR_LBL_HEALTH          */ "DISK HEALTH",
    /* STR_LBL_MODEL           */ "Model",
    /* STR_LBL_SERIAL          */ "Serial No.",
    /* STR_LBL_FIRMWARE        */ "Firmware",
    /* STR_LBL_CAPACITY        */ "Capacity",
    /* STR_LBL_TEMPERATURE     */ "Temperature",
    /* STR_LBL_SMART           */ "S.M.A.R.T.",
    /* STR_LBL_READ_SPEED      */ "Sec. Speed",
    /* STR_BTN_HISTORY_GRAPH   */ "History Graph",
    /* STR_NO_DRIVES_FOUND     */ "No drives found",

    /* List View Columns */
    /* STR_COL_ID              */ "ID",
    /* STR_COL_ATTRIBUTE       */ "Attribute",
    /* STR_COL_VALUE           */ "Value / Info",
    /* STR_COL_STATUS          */ "Status",

    /* Status Badges */
    /* STR_STATUS_OK           */ "OK",
    /* STR_STATUS_WARNING      */ "Warning",
    /* STR_STATUS_FAILED       */ "FAILED",
    /* STR_STATUS_NA           */ "--",

    /* Drive Details & S.M.A.R.T. Texts */
    /* STR_TYPE_PREFIX         */ "Type: ",
    /* STR_HEALTH_FMT          */ "Health: %d%%",
    /* STR_HEALTH_NA           */ "Health: N/A",
    /* STR_PERF_FMT            */ "Perf: %d%%",
    /* STR_PERF_NA             */ "Perf: N/A",
    /* STR_DRIVE_FMT           */ "Drive %d",
    /* STR_TRAY_HEALTH_FMT     */ "Drive %d: Health %d%%\n%s",
    /* STR_TRAY_HEALTH_NA_FMT  */ "Drive %d: Health N/A\n%s",
    /* STR_TRAY_SCANNING       */ "HDDHealth Monitor - scanning...",
    /* STR_NVME_HEALTH_LOG_FMT */ "NVMe Health Log   Spare: %d%%   Used: %d%%",
    /* STR_NVME_HEALTH_LOG_ZH_FMT */ "Used Endurance %d%%",
    /* STR_NVME_NOT_READABLE   */ "NVMe - Health Log not readable (run as Administrator)",
    /* STR_USB_NOT_SUPPORTED   */ "Not available (USB bridge not supported)",
    /* STR_USB_SAT_PASSTHROUGH_FMT */ "USB SAT passthrough - %s",
    /* STR_USB_LIMITED_SCSI    */ "USB - Limited (SCSI Log Sense)",
    /* STR_SUPPORTED_FMT       */ "Supported   %s",
    /* STR_NOT_SUPPORTED       */ "Not Supported",
    /* STR_ENABLED             */ "Enabled",
    /* STR_DISABLED            */ "Disabled",
    /* STR_DETECTED            */ "Detected",
    /* STR_NOT_AVAILABLE       */ "Not Available",
    /* STR_UNAVAILABLE         */ "Unavailable",

    /* NVMe Critical Warning Flags */
    /* STR_NVME_CRIT_HEADER    */ "Critical Warning:",
    /* STR_NVME_CRIT_SPARE     */ " [Spare Low]",
    /* STR_NVME_CRIT_TEMP      */ " [Temp High]",
    /* STR_NVME_CRIT_RELIABILITY */ " [Reliability!]",
    /* STR_NVME_CRIT_READ_ONLY */ " [Read Only!]",
    /* STR_NVME_CRIT_VOLATILE_MEM */ " [Volatile Mem]",

    /* Alert Titles & Messages */
    /* STR_ALERT_OVERHEAT_TITLE  */ "!! CRITICAL: Drive Overheating !!",
    /* STR_ALERT_OVERHEAT_MSG    */ "%s\nTemperature: %d °C (critical threshold: %d °C)\nPower down or improve airflow immediately!",
    /* STR_ALERT_TEMP_WARN_TITLE */ "Warning: Drive Temperature High",
    /* STR_ALERT_TEMP_WARN_MSG   */ "%s\nTemperature: %d °C (warning threshold: %d °C)\nCheck system cooling.",
    /* STR_ALERT_HEALTH_CRIT_TITLE */ "!! CRITICAL: Drive Health Very Poor !!",
    /* STR_ALERT_HEALTH_CRIT_MSG */ "%s\nHealth: %d%% — Back up all data immediately!\nDrive failure may be imminent.",
    /* STR_ALERT_HEALTH_WARN_TITLE */ "Warning: Drive Health Degraded",
    /* STR_ALERT_HEALTH_WARN_MSG */ "%s\nHealth: %d%% — Monitor closely and back up data.",
    /* STR_ALERT_FAILURE_PRED_TITLE */ "!! DRIVE FAILURE PREDICTED !!",
    /* STR_ALERT_FAILURE_PRED_MSG */ "%s\nThe drive's SMART firmware predicts imminent failure.\nBack up all data now!",
    /* STR_ALERT_NVME_CRIT_TITLE */ "NVMe Critical Warning",
    /* STR_ALERT_REALLOC_TITLE   */ "Warning: Reallocated Sectors Detected",
    /* STR_ALERT_REALLOC_MSG     */ "%s\nReallocated sectors: %lu\nThis indicates physical damage on the drive surface.",
    /* STR_ALERT_UNCORRECT_TITLE */ "!! Uncorrectable Errors Detected !!",
    /* STR_ALERT_UNCORRECT_MSG   */ "%s\nUncorrectable errors: %lu\nData integrity at risk — back up immediately!",

    /* Screenshot Dialogs */
    /* STR_SS_SUCCESS_MSG      */ "Screenshot saved successfully!\n\n%s\n\nOpen folder now?",
    /* STR_SS_SUCCESS_TITLE    */ "HDDHealth Monitor - Screenshot Saved",
    /* STR_SS_FAIL_MSG         */ "Screenshot failed:\n%s",
    /* STR_SS_FAIL_TITLE       */ "HDDHealth Monitor - Screenshot Error",

    /* Report Export Dialogs */
    /* STR_REPORT_TITLE        */ "HDDHealth Monitor - Drive Health & S.M.A.R.T. Report",
    /* STR_REPORT_SAVED_MSG    */ "Drive Health & S.M.A.R.T. report saved successfully!\n\nFile:\n%s\n\nOpen report now?",
    /* STR_REPORT_SAVED_TITLE  */ "HDDHealth Monitor - Report Saved",
    /* STR_REPORT_FAIL_MSG     */ "Failed to save report:\n%s",
    /* STR_REPORT_FAIL_TITLE   */ "HDDHealth Monitor - Save Error",

    /* About Dialog */
    /* STR_ABOUT_TITLE         */ "About HDDHealth Monitor",
    /* STR_ABOUT_DESC          */ "Monitors HDD health and S.M.A.R.T. data at the low level.",
    /* STR_ABOUT_FOSS          */ "Free Open Source Software",
    /* STR_ABOUT_SPONSOR_LINK  */ "Sponsor this project on GitHub",
    /* STR_ABOUT_COPYRIGHT     */ "Copyright (C) 2026 ARImetic Inc. All Rights Reserved.",
    /* STR_ABOUT_AUTHOR        */ "Author: Ari Sohandri Putra -  Open Source / MIT License",
    /* STR_BTN_OK              */ "OK",
    /* STR_BTN_DONATE          */ "Donate",
    /* STR_BTN_CLOSE           */ "Close",

    /* History Graph Window */
    /* STR_HIST_TITLE          */ "HDDHealth Monitor - History Graph",
    /* STR_HIST_TAB_HEALTH     */ "Health %",
    /* STR_HIST_TAB_ATTRS      */ "S.M.A.R.T. Attributes",
    /* STR_HIST_LBL_DRIVE      */ "Drive:",
    /* STR_HIST_LBL_ATTR       */ "Attr:",
    /* STR_HIST_BTN_CLEAR      */ "Clear History",
    /* STR_HIST_NO_DATA        */ "No history recorded yet for this drive.",

    /* History Graph Dropdown Tracked Attributes */
    /* STR_HATTR_TEMP          */ "Temp (°C)",
    /* STR_HATTR_REALLOC       */ "Realloc'd Sect.",
    /* STR_HATTR_PENDING       */ "Pending Sect.",
    /* STR_HATTR_UNCORRECT     */ "Uncorrect. Sect.",
    /* STR_HATTR_POH           */ "Power-On Hrs",
    /* STR_HATTR_POWER_CYCLES  */ "Power Cycles",
    /* STR_HATTR_READ_ERR_RATE */ "Read Err Rate",
    /* STR_HATTR_CRC_ERR       */ "CRC Errors",

    /* Donate Dialog */
    /* STR_DONATE_TITLE        */ "HDDHealth Monitor - Donate",
    /* STR_DONATE_HEADER       */ "Support HDDHealth Monitor",
    /* STR_DONATE_EXPLAIN      */ "HDDHealth Monitor is 100% Free and Open Source Software.\n"
                                  "There is no license key, no trial, and no activation.\n"
                                  "All features are available to every user, free of charge.\n\n"
                                  "If you find this tool useful and would like to support\n"
                                  "ongoing development, please consider becoming a sponsor.",
    /* STR_DONATE_AUTHOR_LINE  */ "Author : %s (%s)",
    /* STR_DONATE_FAIL_MSG     */ "Unable to open the donations page in your default browser.\n"
                                  "Please visit the following URL manually:\n\n%s",
    /* STR_DONATE_FAIL_TITLE   */ "Open Browser Failed",

    /* S.M.A.R.T. Units & Suffixes */
    /* STR_UNIT_WRITTEN        */ "written",
    /* STR_UNIT_LIFE_REMAINING */ "SSD life remaining",
    /* STR_UNIT_WEAR_CYCLES    */ "wear-leveling cycles",
    /* STR_UNIT_FAILURES       */ "failures",
    /* STR_UNIT_RESERVED_SPACE */ "reserved space",
    /* STR_UNIT_FLYING_HOURS   */ "head flying hours",
    /* STR_UNIT_HOURS          */ "hours",
    /* STR_UNIT_MINUTES        */ "min",
    /* STR_UNIT_UNITS          */ "units",

    /* Fallback / Special USB rows */
    /* STR_ROW_USB_NO_SMART    */ "SMART not available for USB/External drives",
    /* STR_ROW_NVME_RUN_ADMIN  */ "NVMe SMART: Run as Administrator and click Refresh",
    /* STR_ROW_NVME_WIN10_VER  */ "Some NVMe controllers require Windows 10 v1903+ or newer driver",
    /* STR_ROW_SCSI_PREDICT    */ "Predictive Failure (SCSI Informational Exceptions)",
    /* STR_ROW_SCSI_TEMP       */ "Temperature (SCSI Log Page)",
    /* STR_ROW_SCSI_NOTE       */ "Note: full SMART attribute table not exposed by this bridge",
    /* STR_SCSI_PRED_FAIL      */ "Failure predicted",
    /* STR_SCSI_NO_PRED_FAIL   */ "No failure predicted"
};

/* Drive Types (English) */
static const char* const g_DriveTypeNames_EN[] = {
    "Unknown",         /* DRIVE_TYPE_UNKNOWN */
    "HDD",             /* DRIVE_TYPE_HDD */
    "SSD (SATA)",      /* DRIVE_TYPE_SSD_SATA */
    "NVMe SSD",        /* DRIVE_TYPE_NVME */
    "USB/External",    /* DRIVE_TYPE_USB */
    "M.2 SATA SSD",    /* DRIVE_TYPE_M2_SATA */
    "eMMC",            /* DRIVE_TYPE_EMMC */
    "SD Card",         /* DRIVE_TYPE_SD */
    "SCSI/SAS"         /* DRIVE_TYPE_SCSI */
};

/* Health Status (English) */
static const char* const g_HealthStatusNames_EN[] = {
    "Unknown", /* HEALTH_STATUS_UNKNOWN */
    "Good",    /* HEALTH_STATUS_GOOD */
    "Caution", /* HEALTH_STATUS_CAUTION */
    "Bad",     /* HEALTH_STATUS_BAD */
    "Warning"  /* HEALTH_STATUS_WARNING */
};

#endif /* LANG_EN_H */
