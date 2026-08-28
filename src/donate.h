/* ============================================================================
 *  HDDHealth Monitor - Donate module header
 *  ---------------------------------------------------------------------------
 *  This project is 100% Free and Open Source Software (FOSS).
 *
 *  License : MIT (see LICENSE file in the project root)
 * ============================================================================
 */

#pragma once
#ifndef DONATE_H
#define DONATE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  Public constants                                                  */
/* ------------------------------------------------------------------ */

/* Boosty page for chuikoff.
   Opening this URL is the only action the Donate UI performs. */
#define DONATE_URL      "https://boosty.to/chuikoff"

/* Author / project attribution strings, kept in one place so that
   every UI surface (About dialog, donate dialog, version info)
   stays perfectly consistent. */
#define DONATE_AUTHOR   "chuikoff"
#define DONATE_PRODUCT  "DriveMonitor"

/* Dialog control identifiers - intentionally scoped to the donate
   dialog so they do not collide with the main window IDs. */
#define IDC_DONATE_OPEN_BTN     4101
#define IDC_DONATE_CLOSE_BTN    4102
#define IDC_DONATE_LINK_STATIC  4103

/* Donate dialog dimensions (dialog units -> pixels at 96 DPI). */
#define DONATE_DLG_W    440
#define DONATE_DLG_H    320

#ifdef __cplusplus
extern "C" {
#endif

/* Open the Boosty page in the user's default browser.
   Returns TRUE on success, FALSE if ShellExecute failed. */
BOOL    Donate_OpenSponsorsPage(HWND hParent);

/* Show the modal Donate dialog.  The dialog explains that the
   program is free / open source and offers a single support
   button that opens the Boosty page. */
void    Donate_ShowDialog(HWND hParent);

/* Startup hook - kept for source-level compatibility with the
   previous License_Startup() call site.  Does nothing useful and
   always returns TRUE because the program is unconditionally free. */
BOOL    Donate_Startup(HWND hParent);

#ifdef __cplusplus
}
#endif

#endif /* DONATE_H */
