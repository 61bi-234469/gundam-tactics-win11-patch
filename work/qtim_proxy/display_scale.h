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
 * DllMain: window setup is deferred to the first hooked call.
 * [display] fade_ms (QTIM_FADE_MS, default 50, 0 = original) paces the
 * game's palette fade; with scaling off only gundam.exe's StretchDIBits is
 * hooked for it. */
int gt_scale_attach(const char *ini_path, HMODULE qt_module, GtScaleLogFn log);

/* Undoes the IAT hooks and the window subclass.  Must run before any other
 * IAT hook restoration in the proxy (hooks chain through earlier ones). */
void gt_scale_detach(int process_exit);

/* For the proxy's own drawing onto a window: same as GetDC/ReleaseDC, but
 * the main window yields the shadow DC and releasing it presents it. */
HDC gt_scale_get_dc(HWND hwnd);
void gt_scale_release_dc(HWND hwnd, HDC dc);

/* Returns 1 while scaling is active and stores how many times gundam.exe or
 * the stock QuickTime DLL has drawn into the shadow (the proxy's own
 * gt_scale_get_dc drawing is not counted).  Returns 0 when scaling is off. */
int gt_scale_foreign_draws(LONG *count);

/* The scaled main window, or NULL before it is adopted or with scaling off. */
HWND gt_scale_main_window(void);

/* For the movie loops, timed waits and file loads, which do not read the
 * message queue: dispatches queued caption/border presses on the main window
 * so the window can be moved and resized there.  No-op unless scaling is
 * active. */
void gt_scale_service_frame_input(void);

/* Called on the window's thread when USER32's modal move/size loop starts
 * (entering = 1) and ends (entering = 0).  The game's thread is blocked
 * inside that loop; the proxy keeps the movies it can drive itself playing
 * (see gt_scale_set_modal_tick_callback) and pauses the others.
 * Set before gt_scale_attach. */
typedef void (*GtScaleModalFn)(int entering);
void gt_scale_set_modal_callback(GtScaleModalFn fn);

/* Called on the window's thread about every 10-16 ms while that loop runs
 * (a WM_TIMER on the main window), so the proxy can keep movies playing.
 * Set before gt_scale_attach. */
typedef void (*GtScaleModalTickFn)(void);
void gt_scale_set_modal_tick_callback(GtScaleModalTickFn fn);

/* Called on the window's thread when gundam.exe retrieves a button release
 * the scaling code held back (see display_scale.c); returns the time
 * GetMessageTime should report for it (the proxy's input-time fix).  Set
 * before gt_scale_attach. */
typedef LONG (*GtScaleInputTimeFn)(const MSG *msg);
void gt_scale_set_input_time_callback(GtScaleInputTimeFn fn);

/* Sets up scaling on the calling thread's "Gundam" window now instead of at
 * its first hooked DC or message call.  The proxy calls it on the first
 * QuickTime selector, so the window is enlarged before QuickTime's
 * initialisation (which can take seconds) rather than after it.  Returns 1
 * when the window was adopted by this call. */
int gt_scale_adopt_early(void);

/* Returns 1 when all checks of the pure helpers pass. */
int gt_scale_selftest(void);

#endif
