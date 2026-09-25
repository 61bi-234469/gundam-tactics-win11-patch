/* Window scaling for the main "Gundam" window.
 *
 * gundam.exe draws every frame with GDI straight into a 576x416 window DC.
 * When scaling is on, the DCs that gundam.exe and the stock QuickTime DLL
 * (QTIM32R.DLL) ask for on the main window are replaced by one 576x416
 * shadow memory DC, and the shadow is stretched onto the real window.  Mouse
 * coordinates are mapped back to 576x416 in a window subclass. */
#ifndef GT_DISPLAY_SCALE_H
#define GT_DISPLAY_SCALE_H

#include <windows.h>

typedef void (*GtScaleLogFn)(const char *line);

/* Reads [display] from ini_path (and QTIM_DISPLAY_SCALE / QTIM_DISPLAY_FILTER)
 * and, unless scaling is off, installs the IAT hooks.  qt_module is the stock
 * QuickTime DLL.  Returns 1 when scaling is active.  Safe to call from
 * DllMain: window setup is deferred to the first hooked call. */
int gt_scale_attach(const char *ini_path, HMODULE qt_module, GtScaleLogFn log);

/* Undoes the IAT hooks and the window subclass.  Must run before any other
 * IAT hook restoration in the proxy (hooks chain through earlier ones). */
void gt_scale_detach(int process_exit);

/* For the proxy's own drawing onto a window: same as GetDC/ReleaseDC, but
 * the main window yields the shadow DC and releasing it presents it. */
HDC gt_scale_get_dc(HWND hwnd);
void gt_scale_release_dc(HWND hwnd, HDC dc);

/* For the movie loops, which do not read the message queue: dispatches
 * queued caption/border presses on the main window so the window can be
 * moved and resized during movies.  No-op unless scaling is active. */
void gt_scale_service_frame_input(void);

/* Returns 1 when all checks of the pure helpers pass. */
int gt_scale_selftest(void);

#endif
