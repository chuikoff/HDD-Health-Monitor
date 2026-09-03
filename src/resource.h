/* ============================================================================
 *  HDDHealth Monitor - Resource identifier definitions
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *
 *  Author  : Ari Sohandri Putra
 *  Company : ARImetic Inc.
 *  Sponsor : https://github.com/sponsors/arisohandriputra/
 *  License : MIT
 *
 *  This header assigns numeric IDs to the resources embedded in app.rc:
 *    - IDI_APPICON : main application icon
 *    - IDI_HDD     : drive / tray icon (currently reuses IDI_APPICON)
 *    - IDR_MAINMENU: (reserved, currently unused - menu is built at runtime)
 *    - IDR_VERSION : resource ID for the VS_VERSION_INFO block
 * ============================================================================
 */

#pragma once
#ifndef RESOURCE_H
#define RESOURCE_H

#define IDI_APPICON     101
#define IDI_HDD         102
#define IDR_MAINMENU    103

#define IDR_VERSION     1

/* Info-panel USB adapter row (CreateWindowExU8 / SetDlgItemTextU8) */
#define IDC_ADAPTER_LABEL   1027
#define IDC_ADAPTER_STATIC  1028

/* Brand / controller / NAND identity rows */
#define IDC_BRAND_LABEL         1029
#define IDC_BRAND_STATIC        1030
#define IDC_CONTROLLER_LABEL    1031
#define IDC_CONTROLLER_STATIC   1032
#define IDC_NAND_LABEL          1033
#define IDC_NAND_STATIC         1034

#endif
