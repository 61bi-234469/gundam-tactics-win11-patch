/* Startup timing diagnostics (off by default).
 *
 * With [diag] startup=1 in qtim_compat.ini (or QTIM_DIAG_STARTUP=1), the
 * proxy appends a few lines per launch to qtim_startup.log next to the DLL:
 * when the first QuickTime selectors arrive and how long the stock
 * QuickTime DLL's audio/device calls take.  Used to find what makes the
 * QuickTime initialisation (selector 0x39) occasionally take seconds. */
#ifndef GT_STARTUP_DIAG_H
#define GT_STARTUP_DIAG_H

#include <stdint.h>
#include <windows.h>

/* Reads the setting and, when on, opens the log and hooks qt_module's
 * imports.  The log goes in the folder of ini_path.  Safe to call from
 * DllMain. */
void gt_diag_attach(const char *ini_path, HMODULE qt_module);

/* Undoes the hooks and, unless the process is exiting, closes the log. */
void gt_diag_detach(int process_exit);

/* Called for every QuickTime selector before it is handled or forwarded. */
void gt_diag_selector(uint32_t sel);

/* Writes one free-form line (no-op when diagnostics are off). */
void gt_diag_note(const char *text);

#endif
