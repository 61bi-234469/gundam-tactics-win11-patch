/* Window scaling for the main "Gundam" window.  See display_scale.h.
 *
 * Who draws on the main window (traced with work/tools/qtdraw_probe):
 *   - gundam.exe: GetDC per scene, then BitBlt/StretchBlt/StretchDIBits/
 *     PatBlt/SetPixel; WM_PAINT repaints from its back buffer.
 *   - QTIM32R.DLL (cvid movies): GetDCEx -> 18 BitBlt strips -> ReleaseDC per
 *     frame, polling GetDCEx ~20k times a second.
 *   - this proxy (SMC movies): gt_scale_get_dc -> StretchDIBits.
 * All of them get the same shadow DC.  Each acquisition does SaveDC and each
 * release RestoreDC, so state a caller leaves behind (clip regions, palette)
 * does not leak into the next one, as with real cache DCs.
 *
 * Presenting (stretching the shadow onto the window) happens on the game
 * thread at frame boundaries: when a shadow DC is released (a QuickTime
 * frame), and when the game polls its message queue (the scene loops draw a
 * frame between polls).  A helper thread presents what is left once drawing
 * has paused, for the loops that never poll (fades, battle movies). */
#include "display_scale.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GAME_W = 576, GAME_H = 416 };
enum { FILTER_AUTO = 0, FILTER_NEAREST = 1, FILTER_SMOOTH = 2 };
enum { SCALE_OFF = -1, SCALE_AUTO = 0, SCALE_MAX = 8 };
enum { SAVE_STACK = 64 };
/* Presents at the message-poll boundary are spaced at least this far apart. */
enum { PUMP_PRESENT_MIN_MS = 8 };
/* The helper thread presents once drawing has been idle this long, or when
 * the last present is this old. */
enum { IDLE_PRESENT_MS = 12, STALE_PRESENT_MS = 50 };

static GtScaleLogFn g_log;
static int g_hooks_active;
static int g_scale_request = SCALE_AUTO;
static int g_filter = FILTER_AUTO;
static int g_start_fullscreen;

static volatile LONG g_failed;
static HWND volatile g_main;
static DWORD g_main_thread;
static HDC g_shadow;
static HBITMAP g_shadow_bmp;
static HGDIOBJ g_shadow_old_bmp;
static void *g_bits;
static BITMAPINFO g_bmi;
static RECT g_dst;
static CRITICAL_SECTION g_present_lock;
static int g_lock_ready;
static volatile LONG g_dirty;
static volatile DWORD g_last_dirty;
static volatile DWORD g_last_present;
static int g_save_levels[SAVE_STACK];
static int g_save_depth;
static WNDPROC g_orig_proc;
static int g_minimized;
static int g_fullscreen;
static LONG g_saved_style;
static WINDOWPLACEMENT g_saved_placement;
static volatile LONG g_stop;

static void scale_log(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    int n;
    if (!g_log) return;
    n = snprintf(buf, sizeof(buf), "%lu\tdisplay_scale\t",
                 (unsigned long)GetTickCount());
    if (n < 0 || n >= (int)sizeof(buf) - 2) return;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    n = (int)strlen(buf);
    buf[n] = '\n';
    buf[n + 1] = '\0';
    g_log(buf);
}

/* ---- pure helpers (covered by gt_scale_selftest) ---- */

/* Largest rectangle with the game's aspect ratio centred in cw x ch. */
static RECT fit_rect(int cw, int ch) {
    RECT r;
    int w, h;
    if (cw <= 0 || ch <= 0) {
        SetRect(&r, 0, 0, 0, 0);
        return r;
    }
    if ((long long)cw * GAME_H <= (long long)ch * GAME_W) {
        w = cw;
        h = (int)(((long long)cw * GAME_H + GAME_W / 2) / GAME_W);
    } else {
        h = ch;
        w = (int)(((long long)ch * GAME_W + GAME_H / 2) / GAME_H);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    r.left = (cw - w) / 2;
    r.top = (ch - h) / 2;
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

static int map_axis(int v, int origin, int extent, int size) {
    long long g;
    if (extent <= 0) return v;
    if (v <= origin) return 0;
    g = (long long)(v - origin) * size / extent;
    return g >= size ? size - 1 : (int)g;
}

/* Client coordinates -> 576x416 game coordinates, clamped to the screen. */
static LPARAM map_mouse(LPARAM lp, const RECT *dst) {
    int x = (short)LOWORD(lp);
    int y = (short)HIWORD(lp);
    int gx = map_axis(x, dst->left, dst->right - dst->left, GAME_W);
    int gy = map_axis(y, dst->top, dst->bottom - dst->top, GAME_H);
    return MAKELPARAM(gx, gy);
}

/* Largest whole multiple of the game screen whose window fits the area. */
static int auto_multiple(int area_w, int area_h, int frame_w, int frame_h) {
    int kx = (area_w - frame_w) / GAME_W;
    int ky = (area_h - frame_h) / GAME_H;
    int k = kx < ky ? kx : ky;
    if (k < 1) k = 1;
    if (k > SCALE_MAX) k = SCALE_MAX;
    return k;
}

static int use_halftone(const RECT *dst) {
    int w = dst->right - dst->left;
    int h = dst->bottom - dst->top;
    if (g_filter == FILTER_NEAREST) return 0;
    if (g_filter == FILTER_SMOOTH) return 1;
    return !(w % GAME_W == 0 && h % GAME_H == 0 && w / GAME_W == h / GAME_H);
}

int gt_scale_selftest(void) {
    int ok = 1;
    RECT r;
    LPARAM lp;
    r = fit_rect(1152, 832);
    ok &= r.left == 0 && r.top == 0 && r.right == 1152 && r.bottom == 832;
    r = fit_rect(1920, 1017); /* wide: pillarbox */
    ok &= r.bottom - r.top == 1017 && r.right - r.left == 1408 &&
          r.left == 256;
    r = fit_rect(1000, 1000); /* tall: letterbox */
    ok &= r.right - r.left == 1000 && r.bottom - r.top == 722 && r.top == 139;
    SetRect(&r, 100, 0, 1252, 832);
    lp = map_mouse(MAKELPARAM(100, 0), &r);
    ok &= LOWORD(lp) == 0 && HIWORD(lp) == 0;
    lp = map_mouse(MAKELPARAM(1251, 831), &r);
    ok &= LOWORD(lp) == 575 && HIWORD(lp) == 415;
    lp = map_mouse(MAKELPARAM(677, 417), &r);
    ok &= LOWORD(lp) == 288 && HIWORD(lp) == 208;
    lp = map_mouse(MAKELPARAM((WORD)-5, 900), &r); /* captured, outside */
    ok &= LOWORD(lp) == 0 && HIWORD(lp) == 415;
    ok &= auto_multiple(1920, 1032, 16, 39) == 2;
    ok &= auto_multiple(2560, 1392, 16, 39) == 3;
    ok &= auto_multiple(1024, 728, 16, 39) == 1;
    return ok;
}

/* ---- presenting ---- */

static void present(HDC target) {
    HWND hwnd = g_main;
    HDC dc;
    RECT client;
    RECT dst;
    if (!hwnd || !g_shadow || !g_lock_ready) return;
    EnterCriticalSection(&g_present_lock);
    InterlockedExchange(&g_dirty, 0);
    g_last_present = GetTickCount();
    if (GetCurrentThreadId() == g_main_thread) GdiFlush();
    dc = target ? target : GetDC(hwnd);
    if (dc && GetClientRect(hwnd, &client) && client.right > 0 &&
        client.bottom > 0) {
        dst = g_dst;
        if (use_halftone(&dst)) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
        }
        StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left,
                      dst.bottom - dst.top, 0, 0, GAME_W, GAME_H, g_bits,
                      &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        if (dst.left > 0) PatBlt(dc, 0, 0, dst.left, client.bottom, BLACKNESS);
        if (dst.right < client.right)
            PatBlt(dc, dst.right, 0, client.right - dst.right, client.bottom,
                   BLACKNESS);
        if (dst.top > 0)
            PatBlt(dc, dst.left, 0, dst.right - dst.left, dst.top, BLACKNESS);
        if (dst.bottom < client.bottom)
            PatBlt(dc, dst.left, dst.bottom, dst.right - dst.left,
                   client.bottom - dst.bottom, BLACKNESS);
    }
    if (dc && !target) ReleaseDC(hwnd, dc);
    LeaveCriticalSection(&g_present_lock);
}

static void mark_dirty(void) {
    g_last_dirty = GetTickCount();
    InterlockedExchange(&g_dirty, 1);
}

static void update_dst(HWND hwnd) {
    RECT client;
    if (!GetClientRect(hwnd, &client)) return;
    if (client.right <= 0 || client.bottom <= 0) return;
    EnterCriticalSection(&g_present_lock);
    g_dst = fit_rect(client.right, client.bottom);
    LeaveCriticalSection(&g_present_lock);
}

static DWORD WINAPI present_thread(LPVOID param) {
    HMODULE self = (HMODULE)param;
    while (!g_stop) {
        HWND hwnd = g_main;
        DWORD now;
        Sleep(4);
        if (!hwnd || !IsWindow(hwnd)) break;
        if (!g_dirty || IsIconic(hwnd)) continue;
        now = GetTickCount();
        if ((DWORD)(now - g_last_dirty) >= IDLE_PRESENT_MS ||
            (DWORD)(now - g_last_present) >= STALE_PRESENT_MS)
            present(NULL);
    }
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

/* ---- window ---- */

static void toggle_fullscreen(HWND hwnd) {
    if (!g_fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        g_saved_placement.length = sizeof(g_saved_placement);
        if (!GetWindowPlacement(hwnd, &g_saved_placement)) return;
        if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                             &mi)) return;
        g_saved_style = GetWindowLongA(hwnd, GWL_STYLE);
        g_fullscreen = 1;
        SetWindowLongA(hwnd, GWL_STYLE,
                       (g_saved_style & ~(WS_CAPTION | WS_THICKFRAME |
                                          WS_MAXIMIZE)) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        g_fullscreen = 0;
        SetWindowLongA(hwnd, GWL_STYLE, g_saved_style);
        SetWindowPlacement(hwnd, &g_saved_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    scale_log("fullscreen=%d", g_fullscreen);
}

static LRESULT CALLBACK scale_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        lp = map_mouse(lp, &g_dst);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        /* The shadow holds exactly what is on screen, so repaint from it
         * rather than letting the game copy its back buffer over a movie. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (dc && !IsIconic(hwnd)) present(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT r;
        SetRect(&r, 0, 0, GAME_W, GAME_H);
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE));
        if (mmi && !g_fullscreen) {
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }
    case WM_SIZE:
        /* The game only reacts to minimize (pauses BGM) and to the restore
         * that follows it (resumes BGM); every other resize is ours. */
        if (wp == SIZE_MINIMIZED) {
            g_minimized = 1;
            return CallWindowProcA(g_orig_proc, hwnd, msg, SIZE_MINIMIZED,
                                   MAKELPARAM(0, 0));
        }
        update_dst(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        if (g_minimized) {
            g_minimized = 0;
            return CallWindowProcA(g_orig_proc, hwnd, msg, SIZE_RESTORED,
                                   MAKELPARAM(GAME_W, GAME_H));
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1L << 29))) {
            toggle_fullscreen(hwnd);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0; /* no beep for Alt+Enter */
        break;
    case WM_NCDESTROY: {
        WNDPROC orig = g_orig_proc;
        SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)(uintptr_t)orig);
        g_main = NULL;
        return CallWindowProcA(orig, hwnd, msg, wp, lp);
    }
    default:
        break;
    }
    return CallWindowProcA(g_orig_proc, hwnd, msg, wp, lp);
}

static void release_shadow(void) {
    if (g_shadow && g_shadow_old_bmp) SelectObject(g_shadow, g_shadow_old_bmp);
    if (g_shadow_bmp) DeleteObject(g_shadow_bmp);
    if (g_shadow) DeleteDC(g_shadow);
    g_shadow = NULL;
    g_shadow_bmp = NULL;
    g_shadow_old_bmp = NULL;
    g_bits = NULL;
}

/* Runs on the window's thread, inside the first hooked call that names the
 * main window. */
static int setup_window(HWND hwnd) {
    HDC wdc;
    LONG style;
    RECT frame;
    MONITORINFO mi;
    int k;
    int win_w, win_h;
    HMODULE self = NULL;
    HANDLE thread;
    WNDPROC orig;

    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = GAME_W;
    g_bmi.bmiHeader.biHeight = -GAME_H; /* top-down */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    wdc = GetDC(hwnd);
    if (!wdc) return 0;
    g_shadow = CreateCompatibleDC(wdc);
    g_shadow_bmp = CreateDIBSection(wdc, &g_bmi, DIB_RGB_COLORS, &g_bits,
                                    NULL, 0);
    ReleaseDC(hwnd, wdc);
    if (!g_shadow || !g_shadow_bmp || !g_bits) {
        release_shadow();
        return 0;
    }
    g_shadow_old_bmp = SelectObject(g_shadow, g_shadow_bmp);

    g_main_thread = GetCurrentThreadId();
    g_main = hwnd;
    orig = (WNDPROC)(uintptr_t)SetWindowLongA(
        hwnd, GWL_WNDPROC, (LONG)(uintptr_t)scale_wndproc);
    if (!orig) {
        g_main = NULL;
        release_shadow();
        return 0;
    }
    g_orig_proc = orig;

    style = GetWindowLongA(hwnd, GWL_STYLE) | WS_THICKFRAME | WS_MAXIMIZEBOX;
    SetWindowLongA(hwnd, GWL_STYLE, style);
    SetRect(&frame, 0, 0, 0, 0);
    AdjustWindowRectEx(&frame, (DWORD)style, FALSE,
                       (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE));
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                         &mi))
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    k = g_scale_request > 0
            ? g_scale_request
            : auto_multiple(mi.rcWork.right - mi.rcWork.left,
                            mi.rcWork.bottom - mi.rcWork.top,
                            frame.right - frame.left,
                            frame.bottom - frame.top);
    win_w = GAME_W * k + (frame.right - frame.left);
    win_h = GAME_H * k + (frame.bottom - frame.top);
    SetWindowPos(hwnd, NULL,
                 mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - win_w) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - win_h) / 2,
                 win_w, win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    update_dst(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);

    /* The helper thread keeps the proxy loaded until it exits. */
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)(uintptr_t)present_thread, &self)) {
        thread = CreateThread(NULL, 0, present_thread, self, 0, NULL);
        if (thread) CloseHandle(thread);
        else FreeLibrary(self);
    }
    scale_log("active\thwnd=%08X\tmultiple=%d\tclient=%dx%d\tfilter=%d",
              (unsigned)(uintptr_t)hwnd, k, GAME_W * k, GAME_H * k, g_filter);
    if (g_start_fullscreen) toggle_fullscreen(hwnd);
    return 1;
}

/* Adopts the "Gundam" window of this process the first time it is seen from
 * its own thread. */
static int is_main_window(HWND hwnd) {
    char cls[16];
    DWORD pid = 0;
    if (!hwnd || g_failed) return 0;
    if (g_main) return hwnd == g_main && GetCurrentThreadId() == g_main_thread;
    if (GetWindowThreadProcessId(hwnd, &pid) != GetCurrentThreadId() ||
        pid != GetCurrentProcessId())
        return 0;
    if (!GetClassNameA(hwnd, cls, sizeof(cls)) || lstrcmpA(cls, "Gundam") != 0)
        return 0;
    if (!setup_window(hwnd)) {
        InterlockedExchange(&g_failed, 1);
        scale_log("setup_failed\thwnd=%08X", (unsigned)(uintptr_t)hwnd);
        return 0;
    }
    return 1;
}

static HDC acquire_shadow(void) {
    int level = SaveDC(g_shadow);
    if (g_save_depth < SAVE_STACK) g_save_levels[g_save_depth] = level;
    g_save_depth++;
    return g_shadow;
}

static void release_shadow_dc(void) {
    if (g_save_depth > 0) {
        g_save_depth--;
        if (g_save_depth < SAVE_STACK && g_save_levels[g_save_depth] > 0)
            RestoreDC(g_shadow, g_save_levels[g_save_depth]);
    }
    if (g_dirty) present(NULL);
}

static int is_shadow(HDC dc) {
    return dc && dc == g_shadow;
}

/* ---- hooks: gundam.exe (chained to whatever the slot held) ---- */

typedef HDC (WINAPI *PFN_GetDC)(HWND);
typedef int (WINAPI *PFN_ReleaseDC)(HWND, HDC);
typedef BOOL (WINAPI *PFN_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *PFN_GetMessageA)(LPMSG, HWND, UINT, UINT);
typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int,
                                  DWORD);
typedef BOOL (WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int,
                                      int, int, DWORD);
typedef int (WINAPI *PFN_StretchDIBits)(HDC, int, int, int, int, int, int,
                                        int, int, const VOID *,
                                        const BITMAPINFO *, UINT, DWORD);
typedef BOOL (WINAPI *PFN_PatBlt)(HDC, int, int, int, int, DWORD);
typedef COLORREF (WINAPI *PFN_SetPixel)(HDC, int, int, COLORREF);

static PFN_GetDC x_GetDC;
static PFN_ReleaseDC x_ReleaseDC;
static PFN_PeekMessageA x_PeekMessageA;
static PFN_GetMessageA x_GetMessageA;
static PFN_BitBlt x_BitBlt;
static PFN_StretchBlt x_StretchBlt;
static PFN_StretchDIBits x_StretchDIBits;
static PFN_PatBlt x_PatBlt;
static PFN_SetPixel x_SetPixel;

static HDC WINAPI exe_GetDC(HWND hwnd) {
    if (is_main_window(hwnd)) return acquire_shadow();
    return x_GetDC(hwnd);
}

static int WINAPI exe_ReleaseDC(HWND hwnd, HDC dc) {
    if (is_shadow(dc)) {
        release_shadow_dc();
        return 1;
    }
    return x_ReleaseDC(hwnd, dc);
}

static void present_at_poll(void) {
    if (g_dirty && GetCurrentThreadId() == g_main_thread &&
        (DWORD)(GetTickCount() - g_last_present) >= PUMP_PRESENT_MIN_MS)
        present(NULL);
}

static BOOL WINAPI exe_PeekMessageA(LPMSG msg, HWND hwnd, UINT first,
                                    UINT last, UINT remove) {
    present_at_poll();
    return x_PeekMessageA(msg, hwnd, first, last, remove);
}

static BOOL WINAPI exe_GetMessageA(LPMSG msg, HWND hwnd, UINT first,
                                   UINT last) {
    /* GetMessageA may block: never leave a frame unshown behind it. */
    if (g_dirty && GetCurrentThreadId() == g_main_thread) present(NULL);
    return x_GetMessageA(msg, hwnd, first, last);
}

static BOOL WINAPI exe_BitBlt(HDC d, int x, int y, int cx, int cy, HDC s,
                              int sx, int sy, DWORD rop) {
    BOOL r = x_BitBlt(d, x, y, cx, cy, s, sx, sy, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static BOOL WINAPI exe_StretchBlt(HDC d, int x, int y, int cx, int cy, HDC s,
                                  int sx, int sy, int scx, int scy,
                                  DWORD rop) {
    BOOL r = x_StretchBlt(d, x, y, cx, cy, s, sx, sy, scx, scy, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static int WINAPI exe_StretchDIBits(HDC d, int x, int y, int cx, int cy,
                                    int sx, int sy, int scx, int scy,
                                    const VOID *bits, const BITMAPINFO *bmi,
                                    UINT usage, DWORD rop) {
    int r = x_StretchDIBits(d, x, y, cx, cy, sx, sy, scx, scy, bits, bmi,
                            usage, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static BOOL WINAPI exe_PatBlt(HDC d, int x, int y, int cx, int cy, DWORD rop) {
    BOOL r = x_PatBlt(d, x, y, cx, cy, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static COLORREF WINAPI exe_SetPixel(HDC d, int x, int y, COLORREF c) {
    COLORREF r = x_SetPixel(d, x, y, c);
    if (is_shadow(d)) mark_dirty();
    return r;
}

/* ---- hooks: stock QuickTime (nothing else hooks its imports) ---- */

static HDC WINAPI qt_GetDC(HWND hwnd) {
    if (is_main_window(hwnd)) return acquire_shadow();
    return GetDC(hwnd);
}

static HDC WINAPI qt_GetDCEx(HWND hwnd, HRGN rgn, DWORD flags) {
    if (is_main_window(hwnd)) {
        /* GetDCEx owns rgn on success (DCX_INTERSECTRGN/EXCLUDERGN). */
        if (rgn && (flags & (DCX_INTERSECTRGN | DCX_EXCLUDERGN)))
            DeleteObject(rgn);
        return acquire_shadow();
    }
    return GetDCEx(hwnd, rgn, flags);
}

static int WINAPI qt_ReleaseDC(HWND hwnd, HDC dc) {
    if (is_shadow(dc)) {
        release_shadow_dc();
        return 1;
    }
    return ReleaseDC(hwnd, dc);
}

static int WINAPI qt_FillRect(HDC dc, const RECT *rc, HBRUSH brush) {
    int r = FillRect(dc, rc, brush);
    if (is_shadow(dc)) mark_dirty();
    return r;
}

static BOOL WINAPI qt_BitBlt(HDC d, int x, int y, int cx, int cy, HDC s,
                             int sx, int sy, DWORD rop) {
    BOOL r = BitBlt(d, x, y, cx, cy, s, sx, sy, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static BOOL WINAPI qt_PatBlt(HDC d, int x, int y, int cx, int cy, DWORD rop) {
    BOOL r = PatBlt(d, x, y, cx, cy, rop);
    if (is_shadow(d)) mark_dirty();
    return r;
}

static int WINAPI qt_SetDIBitsToDevice(HDC d, int x, int y, DWORD cx,
                                       DWORD cy, int sx, int sy, UINT start,
                                       UINT lines, const VOID *bits,
                                       const BITMAPINFO *bmi, UINT usage) {
    int r = SetDIBitsToDevice(d, x, y, cx, cy, sx, sy, start, lines, bits,
                              bmi, usage);
    if (is_shadow(d)) mark_dirty();
    return r;
}

/* ---- IAT patching ---- */

typedef struct HookSpec {
    const char *dll;
    const char *name;
    void *hook;
    void **prev; /* NULL: the hook calls the system function itself */
} HookSpec;

static const HookSpec k_exe_hooks[] = {
    {"user32.dll", "GetDC", (void *)(uintptr_t)exe_GetDC, (void **)&x_GetDC},
    {"user32.dll", "ReleaseDC", (void *)(uintptr_t)exe_ReleaseDC,
     (void **)&x_ReleaseDC},
    {"user32.dll", "PeekMessageA", (void *)(uintptr_t)exe_PeekMessageA,
     (void **)&x_PeekMessageA},
    {"user32.dll", "GetMessageA", (void *)(uintptr_t)exe_GetMessageA,
     (void **)&x_GetMessageA},
    {"gdi32.dll", "BitBlt", (void *)(uintptr_t)exe_BitBlt, (void **)&x_BitBlt},
    {"gdi32.dll", "StretchBlt", (void *)(uintptr_t)exe_StretchBlt,
     (void **)&x_StretchBlt},
    {"gdi32.dll", "StretchDIBits", (void *)(uintptr_t)exe_StretchDIBits,
     (void **)&x_StretchDIBits},
    {"gdi32.dll", "PatBlt", (void *)(uintptr_t)exe_PatBlt, (void **)&x_PatBlt},
    {"gdi32.dll", "SetPixel", (void *)(uintptr_t)exe_SetPixel,
     (void **)&x_SetPixel},
};

static const HookSpec k_qt_hooks[] = {
    {"user32.dll", "GetDC", (void *)(uintptr_t)qt_GetDC, NULL},
    {"user32.dll", "GetDCEx", (void *)(uintptr_t)qt_GetDCEx, NULL},
    {"user32.dll", "ReleaseDC", (void *)(uintptr_t)qt_ReleaseDC, NULL},
    {"user32.dll", "FillRect", (void *)(uintptr_t)qt_FillRect, NULL},
    {"gdi32.dll", "BitBlt", (void *)(uintptr_t)qt_BitBlt, NULL},
    {"gdi32.dll", "PatBlt", (void *)(uintptr_t)qt_PatBlt, NULL},
    {"gdi32.dll", "SetDIBitsToDevice", (void *)(uintptr_t)qt_SetDIBitsToDevice,
     NULL},
};

enum { HOOK_CAPACITY = 24 };
typedef struct HookRecord {
    DWORD *slot;
    DWORD original;
    DWORD replacement;
} HookRecord;
static HookRecord g_records[HOOK_CAPACITY];
static unsigned g_record_count;

static DWORD *find_iat_slot(HMODULE module, const char *dll, const char *name) {
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS32 nt;
    IMAGE_DATA_DIRECTORY dir;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return NULL;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return NULL;
    for (imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress);
         imp->Name; imp++) {
        DWORD *names, *iat;
        if (lstrcmpiA((const char *)(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        names = (DWORD *)(base + imp->OriginalFirstThunk);
        iat = (DWORD *)(base + imp->FirstThunk);
        for (; *names; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL32(*names)) continue;
            if (lstrcmpA((const char *)((IMAGE_IMPORT_BY_NAME *)(base + *names))
                             ->Name, name) == 0)
                return iat;
        }
    }
    return NULL;
}

static int write_slot(DWORD *slot, DWORD value) {
    DWORD old, ignored;
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) return 0;
    *slot = value;
    VirtualProtect(slot, sizeof(DWORD), old, &ignored);
    return 1;
}

static void restore_hooks(void) {
    while (g_record_count > 0) {
        HookRecord *r = &g_records[--g_record_count];
        if (*r->slot == r->replacement) write_slot(r->slot, r->original);
    }
}

/* All or nothing: a partial set could hand out the shadow DC without the
 * matching release or present. */
static int install_hooks(HMODULE module, const HookSpec *specs, unsigned n,
                         int required) {
    unsigned i;
    for (i = 0; i < n; i++) {
        DWORD *slot = find_iat_slot(module, specs[i].dll, specs[i].name);
        HookRecord *r;
        if (!slot) {
            if (required) return 0;
            continue;
        }
        if (g_record_count >= HOOK_CAPACITY || !*slot) return 0;
        r = &g_records[g_record_count];
        r->slot = slot;
        r->original = *slot;
        r->replacement = (DWORD)(uintptr_t)specs[i].hook;
        if (specs[i].prev) *specs[i].prev = (void *)(uintptr_t)*slot;
        if (!write_slot(slot, r->replacement)) return 0;
        g_record_count++;
    }
    return 1;
}

/* ---- configuration ---- */

static int parse_scale(const char *v) {
    int n;
    if (!v[0] || lstrcmpiA(v, "auto") == 0 || lstrcmpiA(v, "fit") == 0)
        return SCALE_AUTO;
    if (lstrcmpiA(v, "off") == 0) return SCALE_OFF;
    n = atoi(v);
    if (n <= 1) return SCALE_OFF;
    return n > SCALE_MAX ? SCALE_MAX : n;
}

static int parse_filter(const char *v) {
    if (lstrcmpiA(v, "nearest") == 0) return FILTER_NEAREST;
    if (lstrcmpiA(v, "smooth") == 0) return FILTER_SMOOTH;
    return FILTER_AUTO;
}

static void read_setting(const char *env, const char *key, const char *def,
                         const char *ini_path, char *out, DWORD cap) {
    DWORD n = GetEnvironmentVariableA(env, out, cap);
    if (n && n < cap) return;
    if (ini_path)
        GetPrivateProfileStringA("display", key, def, out, cap, ini_path);
    else
        lstrcpynA(out, def, (int)cap);
}

int gt_scale_attach(const char *ini_path, HMODULE qt_module, GtScaleLogFn log) {
    char value[32];
    g_log = log;
    read_setting("QTIM_DISPLAY_SCALE", "scale", "auto", ini_path, value,
                 sizeof(value));
    g_scale_request = parse_scale(value);
    read_setting("QTIM_DISPLAY_FILTER", "filter", "auto", ini_path, value,
                 sizeof(value));
    g_filter = parse_filter(value);
    g_start_fullscreen = ini_path &&
        GetPrivateProfileIntA("display", "fullscreen", 0, ini_path) != 0;
    if (g_scale_request == SCALE_OFF) {
        scale_log("off\treason=disabled");
        return 0;
    }
    InitializeCriticalSection(&g_present_lock);
    g_lock_ready = 1;
    if (!install_hooks(GetModuleHandleA(NULL), k_exe_hooks,
                       sizeof(k_exe_hooks) / sizeof(k_exe_hooks[0]), 1) ||
        !install_hooks(qt_module, k_qt_hooks,
                       sizeof(k_qt_hooks) / sizeof(k_qt_hooks[0]), 0)) {
        restore_hooks();
        scale_log("off\treason=iat_patch_failed");
        return 0;
    }
    g_hooks_active = 1;
    scale_log("hooked\tslots=%u\tscale=%d\tfilter=%d\tfullscreen=%d",
              g_record_count, g_scale_request, g_filter, g_start_fullscreen);
    return 1;
}

void gt_scale_detach(int process_exit) {
    if (!g_hooks_active) return;
    g_hooks_active = 0;
    InterlockedExchange(&g_stop, 1);
    restore_hooks();
    /* At process exit the window and the helper thread are already gone or
     * about to be; only a FreeLibrary detach needs the window put back.
     * (The helper thread holds a module reference, so that detach can only
     * happen once the thread has left.) */
    if (!process_exit && g_main && IsWindow(g_main) && g_orig_proc &&
        GetWindowLongA(g_main, GWL_WNDPROC) == (LONG)(uintptr_t)scale_wndproc)
        SetWindowLongA(g_main, GWL_WNDPROC, (LONG)(uintptr_t)g_orig_proc);
}

/* Hit-test codes whose button-down starts a window move/size loop (or the
 * maximize toggle) and nothing else.  Close, minimize and the system menu
 * stay queued for the game's own loop: WM_CLOSE runs the game's shutdown
 * code, and minimize pauses its BGM. */
static int is_frame_hit(WPARAM hit) {
    return hit == HTCAPTION || hit == HTMAXBUTTON ||
           (hit >= HTLEFT && hit <= HTBOTTOMRIGHT);
}

void gt_scale_service_frame_input(void) {
    HWND hwnd = g_main;
    MSG msg;
    if (!g_hooks_active || !hwnd || GetCurrentThreadId() != g_main_thread)
        return;
    while (PeekMessageA(&msg, hwnd, WM_NCLBUTTONDOWN, WM_NCLBUTTONDBLCLK,
                        PM_NOREMOVE)) {
        if (msg.message != WM_NCLBUTTONUP && !is_frame_hit(msg.wParam))
            break;
        if (!PeekMessageA(&msg, hwnd, msg.message, msg.message, PM_REMOVE))
            break;
        /* A caption/border press enters USER32's modal move/size loop here,
         * which pumps the queue itself until the button is released. */
        DispatchMessageA(&msg);
    }
}

HDC gt_scale_get_dc(HWND hwnd) {
    if (g_hooks_active && is_main_window(hwnd)) return acquire_shadow();
    return GetDC(hwnd);
}

void gt_scale_release_dc(HWND hwnd, HDC dc) {
    if (is_shadow(dc)) {
        mark_dirty();
        release_shadow_dc();
        return;
    }
    ReleaseDC(hwnd, dc);
}
