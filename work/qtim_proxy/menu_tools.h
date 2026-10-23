/* The menu bar's tools: saving a screenshot and opening folders, the INI and
 * web pages.  Everything slow (encoding, the shell) runs on a short-lived
 * worker thread, so the game's thread only copies the frame and returns;
 * the worker reports back by posting GT_TOOLS_NOTICE to the main window. */
#ifndef GT_MENU_TOOLS_H
#define GT_MENU_TOOLS_H

#include <windows.h>

/* The version the menu and the about window show; build.py passes it from
 * GT_PATCH_VERSION. */
#ifndef GT_PATCH_VERSION_W
#define GT_PATCH_VERSION_W L"(開発版)"
#endif

/* Posted to the main window when a worker is done: wParam is a
 * GtToolsNotice, the subclass shows it in the title bar. */
#define GT_TOOLS_NOTICE (WM_APP + 0x47)

typedef enum GtToolsNotice {
    GT_NOTICE_NONE = 0,
    GT_NOTICE_SHOT_SAVED,
    GT_NOTICE_SHOT_FAILED,
    GT_NOTICE_OPEN_FAILED,
    GT_NOTICE_NO_SAVE_DIR
} GtToolsNotice;

typedef enum GtToolsTarget {
    GT_OPEN_SHOT_DIR,
    GT_OPEN_SAVE_DIR,
    GT_OPEN_GAME_DIR,
    GT_OPEN_INI
} GtToolsTarget;

/* Saves a copy of a top-down 32bpp (BGRX) image as PNG (BMP if GDI+ is
 * missing) in Pictures\GundamTactics.  Returns 0 if no worker could start. */
int gt_tools_screenshot(HWND notify, const void *bits, int width, int height);

/* Opens target with the shell.  ini_path is the proxy's INI (its folder is
 * the game folder); it may be NULL. */
int gt_tools_open(HWND notify, GtToolsTarget target, const char *ini_path);

/* Shows the about window (patch version and the guide, releases and source
 * links), or brings it to the front when it is already open.  It runs on its own thread with
 * its own message loop and is not owned by the game's window, so the game
 * keeps running and its window procedure is never re-entered.  game is used
 * to centre it.  Returns 0 if it could not be started. */
int gt_tools_about(HWND game);

/* Returns 1 when all checks of the pure helpers pass. */
int gt_tools_selftest(void);

#endif
