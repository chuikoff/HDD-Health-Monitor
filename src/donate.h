/* ============================================================================
 *  DriveMonitor - Boosty donate URL
 *  ---------------------------------------------------------------------------
 *  License : MIT (see LICENSE file in the project root)
 * ============================================================================
 */

#pragma once
#ifndef DONATE_H
#define DONATE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define DONATE_URL      "https://boosty.to/chuikoff"

/* Open DONATE_URL in the default browser. Implemented in mainwnd.cpp. */
BOOL OpenDonatePage(HWND hParent);

#endif /* DONATE_H */
