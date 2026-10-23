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
#include "menu_tools.h"

#include <mmsystem.h>
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
/* HALFTONE presents from the game thread go through the helper thread
 * (g_async): g_snap is the frame handed over; each helper stretches from its
 * own copy.  g_snap and g_wake are freed on a FreeLibrary detach, which can
 * only happen after every helper has exited. */
#define GAME_BYTES ((size_t)GAME_W * GAME_H * 4u)
static void *g_snap;
static volatile LONG g_snap_pending;
static HANDLE g_wake;
static volatile LONG g_async;
/* Bumped for every helper created; only the newest helper keeps running. */
static volatile LONG g_helper_gen;
/* Serializes helper stretches (copy + stretch) so a retiring helper cannot
 * land a frame after the current helper's newer one.  Only helpers take it. */
static CRITICAL_SECTION g_helper_lock;

typedef struct HelperParam {
    HMODULE self;
    HWND hwnd;
    void *bits;
    LONG gen;
} HelperParam;
/* Counts draws into the shadow by anyone but the proxy's own SMC drawing. */
static volatile LONG g_foreign_draws;
/* Told when a modal move/size loop on the main window starts and ends; both
 * happen on the window's thread. */
static GtScaleModalFn g_modal_cb;
static int g_in_modal;

/* Fade pacing.  FUN_004090F0 draws its 50 (51 with exe Phase 6) palette
 * steps with StretchDIBits and no wait at all, so a fade is over within a
 * few ms and, with scaling on, mostly coalesced into one present.  Each
 * step's call returns to FADE_STEP_RET, or, with exe Phase 3 (b), to the
 * "jmp FADE_STEP_RET" that ends its code cave: steps are spaced so that 50
 * steps take g_fade_ms, and with scaling on every step is presented.  The
 * schedule restarts after a pause of FADE_RESET_MS plus one step; it is
 * anchored at the first step, so late wake-ups do not add up. */
#define FADE_STEP_RET 0x00409289u
#define FADE_CAVE_RET 0x00438561u
enum { FADE_STEPS = 50, FADE_RESET_MS = 100, FADE_MS_DEFAULT = 50,
       FADE_MS_MAX = 2000 };
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static int g_fade_ms = FADE_MS_DEFAULT;
static int g_fade_only; /* scaling off: only the StretchDIBits hook */
static LARGE_INTEGER g_qpc_freq;
static LONGLONG g_fade_start, g_fade_last;
static int g_fade_index;
static HANDLE g_fade_timer;
static uintptr_t g_fade_cave_ret; /* 0 unless the Phase 3 cave is verified */
/* Steps per fade read from the loop branch (50: jl, 51: jle with exe Phase
 * 6), so back-to-back fades each get their own schedule; 0 if unknown. */
static int g_fade_steps;

/* While the modal move/size loop runs, a timer on the main window lets the
 * proxy keep movies going (the loop dispatches WM_TIMER). */
enum { MODAL_TIMER_ID = 0x4754, MODAL_TIMER_MS = 10 };
static GtScaleModalTickFn g_modal_tick_cb;

/* The move/size loop and the menu loop are reported as one modal state. */
static int g_sizemove_loop, g_menu_loop;

static void modal_loop_changed(int entering) {
    if (entering == g_in_modal) return;
    g_in_modal = entering;
    if (g_modal_cb) g_modal_cb(entering);
}

static void modal_loops_update(HWND hwnd) {
    int entering = g_sizemove_loop || g_menu_loop;
    if (entering && !g_in_modal && g_modal_tick_cb)
        SetTimer(hwnd, MODAL_TIMER_ID, MODAL_TIMER_MS, NULL);
    if (!entering) KillTimer(hwnd, MODAL_TIMER_ID);
    modal_loop_changed(entering);
}

void gt_scale_set_modal_callback(GtScaleModalFn fn) {
    g_modal_cb = fn;
}

void gt_scale_set_modal_tick_callback(GtScaleModalTickFn fn) {
    g_modal_tick_cb = fn;
}

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

/* Moves a window rectangle into area (keeping its size), its top-left corner
 * winning when it is larger than the area. */
static RECT place_in_area(RECT win, const RECT *area) {
    int dx = 0, dy = 0;
    if (win.right > area->right) dx = area->right - win.right;
    if (win.left + dx < area->left) dx = area->left - win.left;
    if (win.bottom > area->bottom) dy = area->bottom - win.bottom;
    if (win.top + dy < area->top) dy = area->top - win.top;
    OffsetRect(&win, dx, dy);
    return win;
}

static int use_halftone(const RECT *dst) {
    int w = dst->right - dst->left;
    int h = dst->bottom - dst->top;
    if (g_filter == FILTER_NEAREST) return 0;
    if (g_filter == FILTER_SMOOTH) return 1;
    return !(w % GAME_W == 0 && h % GAME_H == 0 && w / GAME_W == h / GAME_H);
}

static int parse_scale(const char *v);
static RECT clamp_window_size(RECT win, const RECT *work, int fw, int fh);

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
    ok &= auto_multiple(1920, 1032, 16, 59) == 2; /* with the menu bar */
    ok &= auto_multiple(2560, 1392, 16, 59) == 3;
    {
        RECT area, w;
        SetRect(&area, 0, 0, 1920, 1032);
        SetRect(&w, 1000, 500, 2168, 1391); /* 3x: off the right and bottom */
        w = place_in_area(w, &area);
        ok &= w.left == 752 && w.top == 141 && w.right == 1920 &&
              w.bottom == 1032;
        SetRect(&w, -50, -20, 542, 435);
        w = place_in_area(w, &area);
        ok &= w.left == 0 && w.top == 0;
        SetRect(&w, 10, 10, 2010, 1110); /* larger than the area */
        w = place_in_area(w, &area);
        ok &= w.left == 0 && w.top == 0;
    }
    ok &= gt_tools_selftest();
    {
        RECT work, w;
        SetRect(&work, 0, 0, 1920, 1032);
        SetRect(&w, 100, 100, 100 + 4000 + 16, 100 + 3000 + 59); /* too big */
        w = clamp_window_size(w, &work, 16, 59);
        ok &= w.right - w.left == 1920 && w.bottom - w.top == 1032;
        SetRect(&w, 10, 20, 10 + 100 + 16, 20 + 50 + 59); /* too small */
        w = clamp_window_size(w, &work, 16, 59);
        ok &= w.left == 10 && w.right - w.left == GAME_W + 16 &&
              w.bottom - w.top == GAME_H + 59;
    }
    ok &= parse_scale("1") == 1 && parse_scale("off") == SCALE_OFF &&
          parse_scale("0") == SCALE_OFF && parse_scale("auto") == SCALE_AUTO &&
          parse_scale("") == SCALE_AUTO && parse_scale("9") == SCALE_MAX;
    return ok;
}

/* ---- presenting ---- */

/* Stretches a 576x416 top-down 32bpp image onto the window (or target). */
static void stretch_to_window(HWND hwnd, HDC target, const void *bits,
                              RECT dst) {
    HDC dc;
    RECT client;
    dc = target ? target : GetDC(hwnd);
    if (dc && GetClientRect(hwnd, &client) && client.right > 0 &&
        client.bottom > 0) {
        if (use_halftone(&dst)) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
        }
        StretchDIBits(dc, dst.left, dst.top, dst.right - dst.left,
                      dst.bottom - dst.top, 0, 0, GAME_W, GAME_H, bits,
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
}

static void present(HDC target) {
    HWND hwnd = g_main;
    RECT dst;
    int on_main;
    if (!hwnd || !g_shadow || !g_lock_ready) return;
    on_main = GetCurrentThreadId() == g_main_thread;
    EnterCriticalSection(&g_present_lock);
    InterlockedExchange(&g_dirty, 0);
    g_last_present = GetTickCount();
    if (on_main) GdiFlush();
    dst = g_dst;
    if (on_main && g_async && use_halftone(&dst)) {
        /* HALFTONE costs 5-7 ms per present: hand a snapshot of the frame
         * to the helper thread instead of stalling the game loop.  WM_PAINT
         * (target != NULL) goes the same way, so every HALFTONE stretch
         * comes from the helper in order and an older frame can never land
         * after a newer one; the helper paints the whole client area. */
        memcpy(g_snap, g_bits, GAME_BYTES);
        InterlockedExchange(&g_snap_pending, 1);
        LeaveCriticalSection(&g_present_lock);
        SetEvent(g_wake);
        return;
    }
    if (g_async) {
        /* A synchronous present while a helper exists (e.g. right after a
         * resize from a HALFTONE size to a whole multiple): order it after
         * any helper stretch in flight and drop a pending older snapshot.
         * Lock order as in the helper: g_helper_lock, then g_present_lock. */
        LeaveCriticalSection(&g_present_lock);
        EnterCriticalSection(&g_helper_lock);
        EnterCriticalSection(&g_present_lock);
        InterlockedExchange(&g_snap_pending, 0);
        stretch_to_window(hwnd, target, g_bits, g_dst);
        LeaveCriticalSection(&g_present_lock);
        LeaveCriticalSection(&g_helper_lock);
        return;
    }
    stretch_to_window(hwnd, target, g_bits, dst);
    LeaveCriticalSection(&g_present_lock);
}

static void mark_dirty_own(void) {
    g_last_dirty = GetTickCount();
    InterlockedExchange(&g_dirty, 1);
}

/* For draws by gundam.exe and the stock QuickTime DLL. */
static void mark_dirty(void) {
    InterlockedIncrement(&g_foreign_draws);
    mark_dirty_own();
}

static void update_dst(HWND hwnd) {
    RECT client;
    if (!GetClientRect(hwnd, &client)) return;
    if (client.right <= 0 || client.bottom <= 0) return;
    EnterCriticalSection(&g_present_lock);
    g_dst = fit_rect(client.right, client.bottom);
    LeaveCriticalSection(&g_present_lock);
}

/* Copies a frame under the present lock and stretches it after releasing
 * it, so the game thread never waits for a helper-side stretch. */
static void helper_present_locked(HWND hwnd, void *bits, int from_snapshot) {
    RECT dst;
    EnterCriticalSection(&g_present_lock);
    if (from_snapshot) {
        if (!InterlockedExchange(&g_snap_pending, 0)) {
            LeaveCriticalSection(&g_present_lock);
            return;
        }
        memcpy(bits, g_snap, GAME_BYTES);
    } else {
        InterlockedExchange(&g_dirty, 0);
        g_last_present = GetTickCount();
        dst = g_dst;
        if (!use_halftone(&dst)) {
            /* The game thread presents synchronously in this mode: stay
             * under the lock so an older frame cannot land after a newer
             * one. */
            stretch_to_window(hwnd, NULL, g_bits, dst);
            LeaveCriticalSection(&g_present_lock);
            return;
        }
        memcpy(bits, g_bits, GAME_BYTES);
    }
    dst = g_dst;
    LeaveCriticalSection(&g_present_lock);
    stretch_to_window(hwnd, NULL, bits, dst);
}

/* Lock order: g_helper_lock, then g_present_lock (the game thread takes
 * g_helper_lock only for a synchronous present while a helper exists; a
 * HALFTONE snapshot present never waits for it).  A helper whose generation
 * is no longer current draws nothing. */
static void helper_present(HWND hwnd, void *bits, int from_snapshot,
                           LONG gen) {
    EnterCriticalSection(&g_helper_lock);
    if (g_helper_gen == gen) helper_present_locked(hwnd, bits, from_snapshot);
    LeaveCriticalSection(&g_helper_lock);
}

static DWORD WINAPI present_thread(LPVOID param) {
    HelperParam *hp = (HelperParam *)param;
    HMODULE self = hp->self;
    HWND mine = hp->hwnd;
    void *bits = hp->bits;
    LONG gen = hp->gen;
    HeapFree(GetProcessHeap(), 0, hp);
    while (!g_stop) {
        HWND hwnd = g_main;
        DWORD now;
        if (g_wake) WaitForSingleObject(g_wake, 4);
        else Sleep(4);
        /* A window adopted later gets its own helper. */
        if (!hwnd || hwnd != mine || g_main != mine ||
            g_helper_gen != gen || !IsWindow(hwnd))
            break;
        if (IsIconic(hwnd)) {
            InterlockedExchange(&g_snap_pending, 0);
            continue;
        }
        if (g_snap_pending && bits) {
            helper_present(hwnd, bits, 1, gen);
            continue;
        }
        if (!g_dirty) continue;
        now = GetTickCount();
        if ((DWORD)(now - g_last_dirty) >= IDLE_PRESENT_MS ||
            (DWORD)(now - g_last_present) >= STALE_PRESENT_MS) {
            if (bits) helper_present(hwnd, bits, 0, gen);
            else present(NULL);
        }
    }
    if (bits) VirtualFree(bits, 0, MEM_RELEASE);
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

/* ---- the menu bar ----
 *
 * The game's window has no menu and its window procedure (FUN_0040F230)
 * handles no WM_COMMAND, so the proxy's commands cannot clash with it.  The
 * game swallows WM_SYSCHAR (Alt+Q quits), so menu mnemonics would not work:
 * the labels have none, and the menu is mouse only.  The bar is only added
 * while scaling is active: the window is then resizable and the client area
 * is letterboxed, so the bar's height takes nothing off the game screen. */
enum {
    CMD_FIRST = 0x4700,
    CMD_EXIT = 0x4701,
    CMD_SIZE_1 = 0x4711, /* ..CMD_SIZE_1 + MENU_SIZES - 1 */
    CMD_FULLSCREEN = 0x4721,
    CMD_FILTER_AUTO = 0x4731, /* + FILTER_AUTO / NEAREST / SMOOTH */
    CMD_HIDE_MENU = 0x4741,
    CMD_REMEMBER = 0x4742,
    CMD_RESET_WINDOW = 0x4743,
    CMD_SCREENSHOT = 0x4761,
    CMD_OPEN_FIRST = 0x4770, /* + GtToolsTarget */
    CMD_VERSION = 0x47F0,
    CMD_LAST = 0x47FF,
    MENU_SIZES = 4,
    /* In the window (system) menu: a multiple of 16, as SC_* are compared
     * with wParam & 0xFFF0. */
    SYS_SHOW_MENU = 0x4750
};
/* A notice shown in the title bar for a while (the window has no other place
 * to say something without a dialog). */
enum { NOTICE_TIMER_ID = 0x4755, NOTICE_MS = 2500 };
/* A menu-bar press the busy game reads this late, with the button already up,
 * is dropped instead of opening the menu out of the blue. */
enum { MENU_PRESS_STALE_MS = 500 };
static int g_menu_enabled = 1; /* [display] menu: the bar is shown */
static HMENU g_menu;
static int g_notice_shown;
static char g_ini_path[MAX_PATH]; /* where menu choices are saved; "" if none */
/* Set while gt_scale_service_frame_input dispatches input of the busy game
 * (a movie, a timed wait, a file load): a menu opened from there runs inside
 * the game's own call. */
static int g_service_busy;

static int window_has_menu(HWND hwnd) {
    return GetMenu(hwnd) != NULL;
}

/* Window size outside the client area for the current style and menu. */
static void frame_extent(HWND hwnd, int *w, int *h) {
    RECT r;
    SetRect(&r, 0, 0, 0, 0);
    AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE),
                       window_has_menu(hwnd),
                       (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE));
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

static HMENU build_menu(void) {
    HMENU bar = CreateMenu();
    HMENU game = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU size = CreatePopupMenu();
    HMENU filter = CreatePopupMenu();
    HMENU tools = CreatePopupMenu();
    HMENU help = CreatePopupMenu();
    int k;
    if (!bar || !game || !view || !size || !filter || !tools || !help) {
        if (bar) DestroyMenu(bar);
        if (tools) DestroyMenu(tools);
        if (game) DestroyMenu(game);
        if (view) DestroyMenu(view);
        if (size) DestroyMenu(size);
        if (filter) DestroyMenu(filter);
        if (help) DestroyMenu(help);
        return NULL;
    }
    AppendMenuW(game, MF_STRING, CMD_EXIT, L"ゲームを終了...");
    for (k = 1; k <= MENU_SIZES; k++) {
        wchar_t label[48];
        if (k == 1)
            lstrcpyW(label, L"1 倍 (576×416)");
        else
            wsprintfW(label, L"%d 倍 (%d×%d)", k, GAME_W * k, GAME_H * k);
        AppendMenuW(size, MF_STRING, CMD_SIZE_1 + k - 1, label);
    }
    AppendMenuW(filter, MF_STRING, CMD_FILTER_AUTO + FILTER_AUTO,
                L"自動 (整数倍はくっきり、それ以外はなめらか)");
    AppendMenuW(filter, MF_STRING, CMD_FILTER_AUTO + FILTER_NEAREST,
                L"くっきり");
    AppendMenuW(filter, MF_STRING, CMD_FILTER_AUTO + FILTER_SMOOTH,
                L"なめらか");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)size, L"ウィンドウの大きさ");
    AppendMenuW(view, MF_STRING, CMD_FULLSCREEN, L"全画面表示\tAlt+Enter");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_POPUP, (UINT_PTR)filter, L"拡大の補間");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, CMD_HIDE_MENU, L"メニューバーを隠す");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, CMD_REMEMBER,
                L"ウィンドウの位置と大きさを記憶する");
    AppendMenuW(view, MF_STRING, CMD_RESET_WINDOW,
                L"位置と大きさを最初の状態に戻す");
    AppendMenuW(tools, MF_STRING, CMD_SCREENSHOT, L"スクリーンショットを保存");
    AppendMenuW(tools, MF_STRING, CMD_OPEN_FIRST + GT_OPEN_SHOT_DIR,
                L"スクリーンショットのフォルダを開く");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, CMD_OPEN_FIRST + GT_OPEN_SAVE_DIR,
                L"セーブデータのフォルダを開く");
    AppendMenuW(tools, MF_STRING, CMD_OPEN_FIRST + GT_OPEN_GAME_DIR,
                L"ゲームのフォルダを開く");
    AppendMenuW(tools, MF_STRING, CMD_OPEN_FIRST + GT_OPEN_INI,
                L"設定ファイル (qtim_compat.ini) を開く");
    /* The links are in the about window. */
    AppendMenuW(help, MF_STRING, CMD_VERSION, L"バージョン情報...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)game, L"ゲーム");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"表示");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"ツール");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"ヘルプ");
    return bar;
}

/* The work area of the monitor the window is on. */
static RECT window_work_area(HWND hwnd) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                         &mi))
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    return mi.rcWork;
}

/* Largest multiple the size menu offers for the window's monitor. */
static int largest_multiple(HWND hwnd) {
    RECT area = window_work_area(hwnd);
    int fw, fh, k;
    frame_extent(hwnd, &fw, &fh);
    k = auto_multiple(area.right - area.left, area.bottom - area.top, fw, fh);
    return k > MENU_SIZES ? MENU_SIZES : k;
}

/* The multiple the client area is at exactly, else 0. */
static int current_multiple(HWND hwnd) {
    RECT c;
    int k;
    if (g_fullscreen || IsZoomed(hwnd) || IsIconic(hwnd) ||
        !GetClientRect(hwnd, &c) || c.right % GAME_W)
        return 0;
    k = c.right / GAME_W;
    return k >= 1 && c.bottom == GAME_H * k ? k : 0;
}

/* Resizes the window so that the client area is k times the game screen,
 * keeping its top-left corner where it fits. */
static void set_multiple(HWND hwnd, int k) {
    RECT win, area;
    int fw, fh;
    if (g_fullscreen || k < 1) return;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (!GetWindowRect(hwnd, &win)) return;
    frame_extent(hwnd, &fw, &fh);
    win.right = win.left + GAME_W * k + fw;
    win.bottom = win.top + GAME_H * k + fh;
    area = window_work_area(hwnd);
    win = place_in_area(win, &area);
    SetWindowPos(hwnd, NULL, win.left, win.top, win.right - win.left,
                 win.bottom - win.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    scale_log("menu\tsize=%d", k);
}

/* ---- saving menu choices ---- */

static void notice_clear(HWND hwnd) {
    KillTimer(hwnd, NOTICE_TIMER_ID);
    if (g_notice_shown) {
        g_notice_shown = 0;
        SetWindowTextA(hwnd, "Gundam Tactics"); /* the game's own title */
    }
}

static void title_notice(HWND hwnd, const wchar_t *text) {
    SetWindowTextW(hwnd, text);
    g_notice_shown = 1;
    SetTimer(hwnd, NOTICE_TIMER_ID, NOTICE_MS, NULL);
}

/* Writes [display] key=value to the proxy's INI.  The game has no manifest,
 * so under Program Files the write lands in the user's VirtualStore and is
 * read back from there; a failure keeps the choice for this run only. */
static void save_setting(HWND hwnd, const char *key, const char *value) {
    if (g_ini_path[0] &&
        WritePrivateProfileStringA("display", key, value, g_ini_path)) {
        scale_log("menu\tsaved\t%s=%s", key, value);
        return;
    }
    scale_log("menu\tsave_failed\t%s=%s\terr=%lu", key, value,
              (unsigned long)GetLastError());
    title_notice(hwnd, L"Gundam Tactics - 設定を保存できませんでした"
                       L"(今回だけ有効です)");
}

static void set_filter(HWND hwnd, int filter) {
    static const char *const names[] = {"auto", "nearest", "smooth"};
    if (filter < FILTER_AUTO || filter > FILTER_SMOOTH) return;
    if (g_lock_ready) EnterCriticalSection(&g_present_lock);
    g_filter = filter;
    if (g_lock_ready) LeaveCriticalSection(&g_present_lock);
    InvalidateRect(hwnd, NULL, FALSE);
    scale_log("menu\tfilter=%d", filter);
    save_setting(hwnd, "filter", names[filter]);
}

/* Shows or hides the bar, keeping the client area (and so the game screen)
 * the same size; a maximized window stays maximized. */
static void show_menu_bar(HWND hwnd, int show) {
    RECT c, win, area;
    int fw, fh, zoomed;
    if (!g_menu || (show != 0) == window_has_menu(hwnd)) return;
    g_menu_enabled = show != 0;
    save_setting(hwnd, "menu", show ? "1" : "0");
    if (g_fullscreen) return; /* applied when fullscreen ends */
    zoomed = IsZoomed(hwnd);
    if (!GetClientRect(hwnd, &c) || !GetWindowRect(hwnd, &win)) return;
    SetMenu(hwnd, show ? g_menu : NULL);
    scale_log("menu\tbar=%d", show != 0);
    if (zoomed || IsIconic(hwnd)) return;
    frame_extent(hwnd, &fw, &fh);
    win.right = win.left + c.right + fw;
    win.bottom = win.top + c.bottom + fh;
    area = window_work_area(hwnd);
    win = place_in_area(win, &area);
    SetWindowPos(hwnd, NULL, win.left, win.top, win.right - win.left,
                 win.bottom - win.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

/* ---- remembering the window's position and size ([window]) ----
 *
 * The INI's [window] holds the window's top-left corner (screen) and its
 * client size (so that the bar, the frame or the theme can change without
 * changing the game screen), as the window is when neither maximized,
 * minimized nor fullscreen, plus whether it was maximized.  It is written
 * when it changes, never at exit: the game ends the process inside its
 * WM_DESTROY (exit(0) from FUN_0040F230), so no later hook can be counted on.
 * QTIM_DISPLAY_SCALE (a developer override) turns it off. */
static int g_remember_window = 1; /* [display] remember_window */
static int g_scale_from_env;
static int g_normal_valid;
static RECT g_normal_win;
static int g_normal_cw, g_normal_ch;
static char g_window_saved[200]; /* [window] as last read or written */
static int g_size_state = -1;    /* last WM_SIZE wParam */
static int g_window_resetting;   /* no saves while reset_window runs */

static void note_normal(HWND hwnd) {
    RECT w, c;
    if (g_fullscreen || IsZoomed(hwnd) || IsIconic(hwnd)) return;
    if (!GetWindowRect(hwnd, &w) || !GetClientRect(hwnd, &c) ||
        c.right <= 0 || c.bottom <= 0)
        return;
    g_normal_win = w;
    g_normal_cw = c.right;
    g_normal_ch = c.bottom;
    g_normal_valid = 1;
}

/* "key=value\n..." for [window]. */
static void format_window(char *out, size_t cap, long left, long top, int cw,
                          int ch, int maximized) {
    snprintf(out, cap,
             "left=%ld\ntop=%ld\nclient_width=%d\nclient_height=%d\n"
             "maximized=%d\n",
             left, top, cw, ch, maximized ? 1 : 0);
}

static void save_window_state(HWND hwnd) {
    char text[200], section[204];
    size_t i, n;
    int maximized;
    if (!g_remember_window || g_scale_from_env || !g_ini_path[0] || !hwnd ||
        g_window_resetting)
        return;
    note_normal(hwnd);
    if (!g_normal_valid) return;
    maximized = g_fullscreen
        ? g_saved_placement.showCmd == SW_SHOWMAXIMIZED
        : IsZoomed(hwnd) != 0;
    format_window(text, sizeof(text), g_normal_win.left, g_normal_win.top,
                  g_normal_cw, g_normal_ch, maximized);
    if (strcmp(text, g_window_saved) == 0) return;
    /* WritePrivateProfileSection takes NUL-separated lines and replaces the
     * whole section in one write. */
    n = strlen(text);
    memcpy(section, text, n);
    for (i = 0; i < n; i++)
        if (section[i] == '\n') section[i] = '\0';
    section[n] = '\0';
    section[n + 1] = '\0';
    if (WritePrivateProfileSectionA("window", section, g_ini_path)) {
        lstrcpynA(g_window_saved, text, sizeof(g_window_saved));
        scale_log("window\tsaved\tleft=%ld\ttop=%ld\tclient=%dx%d\tmax=%d",
                  g_normal_win.left, g_normal_win.top, g_normal_cw,
                  g_normal_ch, maximized);
    } else {
        scale_log("window\tsave_failed\terr=%lu", (unsigned long)GetLastError());
    }
}

/* Keeps a remembered window usable: at least the game size, at most the
 * work area (each side on its own: the client is letterboxed anyway). */
static RECT clamp_window_size(RECT win, const RECT *work, int fw, int fh) {
    int cw = win.right - win.left - fw, ch = win.bottom - win.top - fh;
    int max_cw = work->right - work->left - fw;
    int max_ch = work->bottom - work->top - fh;
    if (cw > max_cw) cw = max_cw;
    if (ch > max_ch) ch = max_ch;
    if (cw < GAME_W) cw = GAME_W;
    if (ch < GAME_H) ch = GAME_H;
    win.right = win.left + cw + fw;
    win.bottom = win.top + ch + fh;
    return win;
}

typedef struct CaptionCheck {
    RECT band;
    int visible;
} CaptionCheck;

static BOOL CALLBACK caption_on_monitor(HMONITOR mon, HDC dc, LPRECT r,
                                        LPARAM param) {
    CaptionCheck *c = (CaptionCheck *)param;
    MONITORINFO mi;
    RECT both;
    (void)dc;
    (void)r;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(mon, &mi) &&
        IntersectRect(&both, &c->band, &mi.rcWork) &&
        both.right - both.left >= 64) {
        c->visible = 1;
        return FALSE;
    }
    return TRUE;
}

/* Puts the window where [window] says, if it says so.  Returns 0 to fall
 * back to the scale setting. */
static int restore_window(HWND hwnd, int fw, int fh) {
    char v[32], text[200];
    long left, top;
    int cw, ch, maximized;
    RECT win, work;
    MONITORINFO mi;
    CaptionCheck check;
    if (!g_remember_window || g_scale_from_env || !g_ini_path[0]) return 0;
    GetPrivateProfileStringA("window", "client_width", "", v, sizeof(v),
                             g_ini_path);
    if (!v[0]) return 0;
    cw = atoi(v);
    GetPrivateProfileStringA("window", "client_height", "", v, sizeof(v),
                             g_ini_path);
    ch = atoi(v);
    GetPrivateProfileStringA("window", "left", "", v, sizeof(v), g_ini_path);
    if (!v[0]) return 0;
    left = atol(v);
    GetPrivateProfileStringA("window", "top", "", v, sizeof(v), g_ini_path);
    if (!v[0]) return 0;
    top = atol(v);
    maximized = GetPrivateProfileIntA("window", "maximized", 0, g_ini_path) != 0;
    if (cw <= 0 || ch <= 0 || cw > 30000 || ch > 30000 || left < -30000 ||
        left > 30000 || top < -30000 || top > 30000)
        return 0;
    format_window(text, sizeof(text), left, top, cw, ch, maximized);
    SetRect(&win, (int)left, (int)top, (int)left + cw + fw,
            (int)top + ch + fh);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromRect(&win, MONITOR_DEFAULTTONEAREST), &mi))
        return 0;
    work = mi.rcWork;
    win = clamp_window_size(win, &work, fw, fh);
    /* The caption must be reachable: a monitor may have gone or shrunk. */
    memset(&check, 0, sizeof(check));
    SetRect(&check.band, win.left, win.top, win.right,
            win.top + GetSystemMetrics(SM_CYCAPTION));
    EnumDisplayMonitors(NULL, NULL, caption_on_monitor, (LPARAM)&check);
    if (!check.visible) win = place_in_area(win, &work);
    SetWindowPos(hwnd, NULL, win.left, win.top, win.right - win.left,
                 win.bottom - win.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    note_normal(hwnd);
    lstrcpynA(g_window_saved, text, sizeof(g_window_saved));
    if (maximized) ShowWindow(hwnd, SW_MAXIMIZE);
    scale_log("window\trestored\tleft=%ld\ttop=%ld\tclient=%dx%d\tmax=%d"
              "\tmoved=%d",
              win.left, win.top, win.right - win.left - fw,
              win.bottom - win.top - fh, maximized, !check.visible);
    return 1;
}

/* Where the window opens without [window]: the scale setting, centred in
 * the work area of its monitor. */
static void default_placement(HWND hwnd, int fw, int fh, int *k_out) {
    RECT work = window_work_area(hwnd);
    int k = g_scale_request > 0
                ? g_scale_request
                : auto_multiple(work.right - work.left, work.bottom - work.top,
                                fw, fh);
    int win_w = GAME_W * k + fw, win_h = GAME_H * k + fh;
    SetWindowPos(hwnd, NULL,
                 work.left + ((work.right - work.left) - win_w) / 2,
                 work.top + ((work.bottom - work.top) - win_h) / 2, win_w,
                 win_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    note_normal(hwnd);
    if (k_out) *k_out = k;
}

static void reset_window(HWND hwnd) {
    int fw, fh;
    if (g_fullscreen) return;
    /* Restoring a maximized window sends WM_SIZE, which would save the old
     * normal placement again: nothing is saved until [window] is gone. */
    g_window_resetting = 1;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    frame_extent(hwnd, &fw, &fh);
    default_placement(hwnd, fw, fh, NULL);
    if (g_ini_path[0])
        WritePrivateProfileStringA("window", NULL, NULL, g_ini_path);
    g_window_saved[0] = '\0';
    g_window_resetting = 0;
    scale_log("window\treset");
}

static void add_system_menu_item(HWND hwnd) {
    HMENU sys = GetSystemMenu(hwnd, FALSE);
    if (!sys) return;
    AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sys, MF_STRING, SYS_SHOW_MENU, L"メニューバーを表示");
}

/* The shadow is exactly what is on screen (movie frames included), at the
 * game's own 576x416: copied under the present lock, saved by a worker. */
static void save_screenshot(HWND hwnd) {
    int ok;
    if (!g_bits || !g_lock_ready) return;
    GdiFlush();
    EnterCriticalSection(&g_present_lock);
    ok = gt_tools_screenshot(hwnd, g_bits, GAME_W, GAME_H);
    LeaveCriticalSection(&g_present_lock);
    scale_log("menu\tscreenshot\tstarted=%d", ok);
    if (!ok)
        title_notice(hwnd, L"Gundam Tactics - スクリーンショットを"
                           L"保存できませんでした");
}

static void tools_notice(HWND hwnd, WPARAM what) {
    switch (what) {
    case GT_NOTICE_SHOT_SAVED:
        title_notice(hwnd, L"Gundam Tactics - スクリーンショットを"
                           L"保存しました (ピクチャ\\GundamTactics)");
        break;
    case GT_NOTICE_SHOT_FAILED:
        title_notice(hwnd, L"Gundam Tactics - スクリーンショットを"
                           L"保存できませんでした");
        break;
    case GT_NOTICE_NO_SAVE_DIR:
        title_notice(hwnd, L"Gundam Tactics - セーブデータのフォルダが"
                           L"見つかりません");
        break;
    case GT_NOTICE_OPEN_FAILED:
        title_notice(hwnd, L"Gundam Tactics - 開けませんでした");
        break;
    default:
        break;
    }
    scale_log("menu\tnotice=%u", (unsigned)what);
}

static void toggle_fullscreen(HWND hwnd);

static void menu_init_popup(HWND hwnd) {
    int k, cur = current_multiple(hwnd), top = largest_multiple(hwnd);
    if (!g_menu) return;
    EnableMenuItem(g_menu, CMD_EXIT,
                   MF_BYCOMMAND | (g_service_busy ? MF_GRAYED : MF_ENABLED));
    for (k = 1; k <= MENU_SIZES; k++) {
        UINT id = CMD_SIZE_1 + k - 1;
        EnableMenuItem(g_menu, id, MF_BYCOMMAND |
                       (k <= top || k == cur ? MF_ENABLED : MF_GRAYED));
        CheckMenuItem(g_menu, id,
                      MF_BYCOMMAND | (k == cur ? MF_CHECKED : MF_UNCHECKED));
    }
    CheckMenuItem(g_menu, CMD_FULLSCREEN,
                  MF_BYCOMMAND | (g_fullscreen ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_menu, CMD_REMEMBER,
                  MF_BYCOMMAND |
                      (g_remember_window ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu, CMD_REMEMBER,
                   MF_BYCOMMAND | (g_scale_from_env ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(g_menu, CMD_RESET_WINDOW,
                   MF_BYCOMMAND | (g_fullscreen ? MF_GRAYED : MF_ENABLED));
    CheckMenuRadioItem(g_menu, CMD_FILTER_AUTO + FILTER_AUTO,
                       CMD_FILTER_AUTO + FILTER_SMOOTH,
                       CMD_FILTER_AUTO + g_filter, MF_BYCOMMAND);
}

static void system_menu_init_popup(HWND hwnd) {
    HMENU sys = GetSystemMenu(hwnd, FALSE);
    if (sys)
        EnableMenuItem(sys, SYS_SHOW_MENU,
                       MF_BYCOMMAND | (g_menu && !window_has_menu(hwnd) &&
                                               !g_fullscreen
                                           ? MF_ENABLED : MF_GRAYED));
}

static void menu_command(HWND hwnd, UINT id) {
    if (id >= CMD_SIZE_1 && id < CMD_SIZE_1 + MENU_SIZES) {
        char value[8];
        int k = (int)(id - CMD_SIZE_1) + 1;
        set_multiple(hwnd, k);
        /* The size the next start opens at (scale=1 is 1x, not off);
         * with [window] remembered, that wins. */
        wsprintfA(value, "%d", k);
        save_setting(hwnd, "scale", value);
        save_window_state(hwnd);
    } else if (id >= CMD_FILTER_AUTO + FILTER_AUTO &&
               id <= CMD_FILTER_AUTO + FILTER_SMOOTH) {
        set_filter(hwnd, (int)(id - CMD_FILTER_AUTO));
    } else if (id == CMD_HIDE_MENU) {
        show_menu_bar(hwnd, 0);
        save_window_state(hwnd);
    } else if (id == CMD_REMEMBER) {
        g_remember_window = !g_remember_window;
        save_setting(hwnd, "remember_window", g_remember_window ? "1" : "0");
        if (g_remember_window) {
            g_window_saved[0] = '\0';
            save_window_state(hwnd);
        }
    } else if (id == CMD_RESET_WINDOW) {
        reset_window(hwnd);
    } else if (id == CMD_VERSION) {
        if (!gt_tools_about(hwnd))
            title_notice(hwnd, L"Gundam Tactics - 開けませんでした");
        scale_log("menu\tabout");
    } else if (id == CMD_SCREENSHOT) {
        save_screenshot(hwnd);
    } else if (id >= CMD_OPEN_FIRST + GT_OPEN_SHOT_DIR &&
               id <= CMD_OPEN_FIRST + GT_OPEN_INI) {
        /* The shell runs on a worker thread: in the game's thread it could
         * pump messages into the game or stall it. */
        if (!gt_tools_open(hwnd, (GtToolsTarget)(id - CMD_OPEN_FIRST),
                           g_ini_path))
            title_notice(hwnd, L"Gundam Tactics - 開けませんでした");
        scale_log("menu\topen=%u", id - CMD_OPEN_FIRST);
    } else if (id == CMD_FULLSCREEN) {
        toggle_fullscreen(hwnd);
    } else if (id == CMD_EXIT) {
        /* The game's own WM_CLOSE handler asks for confirmation. */
        if (!g_service_busy) PostMessageA(hwnd, WM_CLOSE, 0, 0);
    }
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
        note_normal(hwnd);
        g_fullscreen = 1;
        /* No bar in fullscreen.  The placement above was taken with it, so
         * it goes back before the placement is restored. */
        if (g_menu && window_has_menu(hwnd)) SetMenu(hwnd, NULL);
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
        if (g_menu && g_menu_enabled) SetMenu(hwnd, g_menu);
        SetWindowPlacement(hwnd, &g_saved_placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        save_window_state(hwnd);
    }
    scale_log("fullscreen=%d", g_fullscreen);
}

/* ---- caption drag while the game is busy ----
 *
 * USER32's modal move loop blocks the game's thread until the button is
 * released, so a movie the game composites itself (battle movies, map
 * sprites) stops while the window is dragged.  A caption press found by
 * gt_scale_service_frame_input (the game is busy there: a movie, a wait, a
 * load) therefore starts this drag instead: the window is captured and moved
 * to follow the cursor on every later service call and every PeekMessage /
 * GetMessage of the game, and the game keeps running.  Mouse messages for the
 * window are swallowed until the release that ends the drag has gone by, so
 * the game never sees the drag as input.  Presses the game's own loop
 * dispatches, border presses and double-clicks still use USER32's loop. */
static int g_drag_active; /* following the cursor */
static int g_drag_eat;    /* swallowing mouse messages until the release */
static int g_drag_release_seen; /* the drag's button-up message went by */
/* Started from the button state while older input of the game was queued in
 * front of the drag's own press: until that press goes by, the window's
 * mouse messages are the game's and pass, and no capture is taken (its
 * press, scanned under a capture, would arrive as a client click). */
static int g_drag_detached;
static RECT g_drag_client0; /* client area (screen) when it started */
/* The caption band (window rect above the client area, in screen
 * coordinates) of every detached drag whose own press has not gone by yet:
 * the user may drag again while the busy game still has not read the input
 * in front of an earlier one. */
enum { DRAG_BANDS = 32 };
static RECT g_band_rect[DRAG_BANDS];
static LONG g_band_top[DRAG_BANDS];
/* The span of message times the press can have: from when the button was
 * last seen up to when it was first seen down (or the drag started). */
static DWORD g_band_tmin[DRAG_BANDS], g_band_tmax[DRAG_BANDS];
static int g_band_count;
static int is_frame_hit(WPARAM hit);
static POINT g_drag_cursor0;
static RECT g_drag_rect0;
/* Where the left button was first seen down (sampled once per tick by the
 * service and the game's message polls), so that a press whose cursor has
 * already left the caption when it is acted on is still known as a caption
 * press.  A sample is used by one drag at most, and only while the window is
 * where it was when the sample was taken. */
static int g_press_sampled;    /* the current button-down was sampled */
static int g_press_on_caption; /* ...and the cursor was on the caption */
static POINT g_press_pos;
static RECT g_press_rect, g_press_client;
static DWORD g_press_sample_tick;
static DWORD g_press_up_tick;   /* last sample that saw the button up */
static DWORD g_press_down_tick; /* sample that first saw this press */
static DWORD g_drag_start, g_drag_end_tick;
static unsigned g_drag_moves;

static int left_button_down(void) {
    int key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return GetAsyncKeyState(key) < 0;
}

static int is_mouse_message(UINT m) {
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) ||
           (m >= WM_NCMOUSEMOVE && m <= WM_NCMBUTTONDBLCLK);
}

static void drag_finish_eat(void) {
    if (!g_drag_eat) return;
    g_drag_eat = 0;
    g_drag_detached = 0;
    g_band_count = 0;
    scale_log("drag\tinput_done");
    if (g_main && GetCapture() == g_main) ReleaseCapture();
}

static void drag_end(const char *why) {
    if (!g_drag_active) return;
    g_drag_active = 0;
    /* The button is up: the next press is sampled afresh. */
    g_press_sampled = 0;
    g_press_on_caption = 0;
    g_drag_end_tick = GetTickCount();
    scale_log("drag\tend\tms=%lu\tmoves=%u\twhy=%s",
              (unsigned long)(g_drag_end_tick - g_drag_start), g_drag_moves,
              why);
    if (g_drag_moves && g_main) save_window_state(g_main);
}

static int is_button_press(UINT m) {
    return m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN ||
           m == WM_LBUTTONDBLCLK || m == WM_RBUTTONDBLCLK ||
           m == WM_MBUTTONDBLCLK || m == WM_NCLBUTTONDOWN ||
           m == WM_NCRBUTTONDOWN || m == WM_NCMBUTTONDOWN ||
           m == WM_NCLBUTTONDBLCLK;
}

/* A mouse message of the window while the drag's input is swallowed.
 * Returns 1 when it is swallowed.  The drag's release ends the swallowing;
 * so does a new press once the drag is over (the release went by unseen),
 * and that press is game input.  There is no timeout: the release may wait
 * in the queue behind other input for as long as the game is busy, and until
 * then the game only misses cursor moves. */
/* The drag's own press, recognised by the screen point where it was made:
 * the caption band of the window as it was when the drag started (the window
 * has moved since, so a hit test now would not tell). */
static int band_index(POINT pt, DWORD time) {
    int i;
    for (i = 0; i < g_band_count; i++)
        if (PtInRect(&g_band_rect[i], pt) && pt.y < g_band_top[i] &&
            (LONG)(time - g_band_tmin[i]) >= 0 &&
            (LONG)(g_band_tmax[i] - time) >= 0)
            return i;
    return -1;
}

/* Returns 0 when the list is full (no drag is started then). */
static int band_add(const RECT *window, LONG client_top, DWORD tmin,
                    DWORD tmax) {
    if (g_band_count == DRAG_BANDS) return 0;
    g_band_rect[g_band_count] = *window;
    g_band_top[g_band_count] = client_top;
    g_band_tmin[g_band_count] = tmin;
    g_band_tmax[g_band_count] = tmax;
    g_band_count++;
    return 1;
}

static void band_remove(int i) {
    size_t n;
    if (i < 0 || i >= g_band_count) return;
    n = (size_t)(g_band_count - i - 1);
    memmove(g_band_rect + i, g_band_rect + i + 1, n * sizeof(g_band_rect[0]));
    memmove(g_band_top + i, g_band_top + i + 1, n * sizeof(g_band_top[0]));
    memmove(g_band_tmin + i, g_band_tmin + i + 1, n * sizeof(g_band_tmin[0]));
    memmove(g_band_tmax + i, g_band_tmax + i + 1, n * sizeof(g_band_tmax[0]));
    g_band_count--;
}

static int in_detached_band(POINT pt, DWORD time) {
    return band_index(pt, time) >= 0;
}

static int is_detached_press(UINT m, POINT pt, DWORD time) {
    return (m == WM_LBUTTONDOWN || m == WM_NCLBUTTONDOWN) &&
           in_detached_band(pt, time);
}

/* While detached, a message that ends the provisional drags without being
 * swallowed: a press made after the last drag ended while the drags' own
 * presses never went by.  (A pending drag's double-click is handled in
 * drag_eat_message: it ends only that drag.) */
static int ends_detached(UINT m, POINT pt, DWORD time) {
    if ((m == WM_LBUTTONDBLCLK || m == WM_NCLBUTTONDBLCLK) &&
        in_detached_band(pt, time))
        return 1; /* stops the queue drain; drag_eat_message decides */
    return !g_drag_active && is_button_press(m) &&
           (LONG)(time - g_drag_end_tick) > 0;
}

static int drag_eat_message(UINT m, POINT pt, DWORD time) {
    if (g_drag_detached) {
        int band = band_index(pt, time);
        if ((m == WM_LBUTTONDBLCLK || m == WM_NCLBUTTONDBLCLK) && band >= 0) {
            /* That drag's press was the second click of a double-click: it
             * goes to the window (maximize as usual) and only that drag
             * stops waiting; newer pending drags keep theirs. */
            band_remove(band);
            g_drag_detached = g_band_count > 0;
            scale_log("drag\tdetached_dblclk\tpending=%d", g_band_count);
            if (!g_drag_detached) {
                drag_end("detached_dblclk");
                drag_finish_eat();
            }
            return 0;
        }
        /* Checked first: a newer press is never the drag's own. */
        if (ends_detached(m, pt, time)) {
            scale_log("drag\tdetached_end\tmsg=%04x", m);
            drag_end("detached_end");
            drag_finish_eat();
            return 0;
        }
        if (is_detached_press(m, pt, time)) {
            /* A detached drag's own press: swallowed.  With more pending,
             * what follows it is passed (its moves and release are
             * non-client messages, nothing for the game) until the last
             * one, whose input is swallowed up to its release. */
            band_remove(band_index(pt, time));
            g_drag_detached = g_band_count > 0;
            return 1;
        }
        return 0; /* older input: the game's */
    }
    if (!g_drag_active && is_button_press(m)) {
        scale_log("drag\tpress_after_drag\tmsg=%04x", m);
        drag_finish_eat();
        return 0;
    }
    if (m == WM_LBUTTONUP || m == WM_NCLBUTTONUP) {
        drag_end("release");
        drag_finish_eat();
    }
    return 1;
}

static void drag_begin(HWND hwnd, const MSG *press) {
    GetWindowRect(hwnd, &g_drag_rect0);
    g_drag_cursor0 = press->pt;
    g_press_on_caption = 0; /* this press is used */
    g_drag_active = 1;
    g_drag_eat = 1;
    g_drag_detached = 0;
    g_drag_release_seen = 0;
    g_drag_moves = 0;
    g_drag_start = GetTickCount();
    SetCapture(hwnd);
    scale_log("drag\tbegin\twait_ms=%lu",
              (unsigned long)(g_drag_start - press->time));
}

/* The user is pressing the caption now, but the press is queued behind
 * input the busy game has not read yet (a click that skipped a movie, a
 * key): follow the cursor from here and leave that input to the game. */

static void window_rects(HWND hwnd, RECT *window, RECT *client) {
    POINT origin = {0, 0};
    GetWindowRect(hwnd, window);
    GetClientRect(hwnd, client);
    ClientToScreen(hwnd, &origin);
    OffsetRect(client, origin.x, origin.y);
}

/* ---- the cursor over the frame while the game hides it ----
 *
 * The game calls ShowCursor(FALSE) for the Bandai logo (FUN_00402D30) and
 * shows it again after the fade-out.  The count applies over every window of
 * the thread, the caption included, so the cursor could not be seen to grab
 * the caption.  While the game's count is below zero and the cursor is on the
 * main window's non-client area, the proxy raises the count to zero (checked
 * once per tick with the press sample); over the client area it stays hidden
 * as the game wants.  The game's own ShowCursor calls see its own count. */
typedef int (WINAPI *PFN_ShowCursor)(BOOL);
static PFN_ShowCursor x_ShowCursor;
static int g_game_cursor_count; /* the game's count, once known */
static int g_game_cursor_known;
static int g_cursor_forced;     /* extra ShowCursor(TRUE) calls by the proxy */

static void frame_cursor_update(HWND hwnd) {
    POINT pt;
    LRESULT hit = HTNOWHERE;
    int over_frame = 0;
    if (GetCursorPos(&pt) && WindowFromPoint(pt) == hwnd) {
        hit = DefWindowProcA(
            hwnd, WM_NCHITTEST, 0,
            (LPARAM)(((DWORD)(WORD)pt.y << 16) | (DWORD)(WORD)pt.x));
        over_frame = hit != HTCLIENT && hit != HTNOWHERE && hit != HTERROR &&
                     hit != HTTRANSPARENT;
    }
    /* While the game is busy (QuickTime's initialisation, the logo, loads)
     * it reads no input, so no WM_SETCURSOR comes and the cursor over the
     * title bar, menu bar and border kept whatever it last was (the busy
     * cursor of the launch).  Set it as DefWindowProc would for that part
     * of the frame; the modal loops set their own. */
    if (over_frame && !g_in_modal)
        DefWindowProcA(hwnd, WM_SETCURSOR, (WPARAM)hwnd,
                       MAKELPARAM((WORD)hit, WM_MOUSEMOVE));
    if (!x_ShowCursor || !g_game_cursor_known) return;
    if (g_game_cursor_count >= 0) over_frame = 0;
    while (over_frame && g_game_cursor_count + g_cursor_forced < 0) {
        x_ShowCursor(TRUE);
        g_cursor_forced++;
    }
    while (!over_frame && g_cursor_forced > 0) {
        x_ShowCursor(FALSE);
        g_cursor_forced--;
    }
}

/* Takes back the proxy's extra counts (the count is per thread: only on the
 * window's thread). */
static void frame_cursor_release(void) {
    if (GetCurrentThreadId() != g_main_thread) return;
    while (g_cursor_forced > 0) {
        x_ShowCursor(FALSE);
        g_cursor_forced--;
    }
}

static int WINAPI exe_ShowCursor(BOOL show) {
    int r;
    if (GetCurrentThreadId() != g_main_thread) return x_ShowCursor(show);
    frame_cursor_release();
    r = x_ShowCursor(show);
    g_game_cursor_count = r;
    g_game_cursor_known = 1;
    /* Over the frame, show it again at once rather than at the next tick
     * (the game still gets its own count back). */
    if (g_main) frame_cursor_update(g_main);
    return r;
}

static void sample_press(HWND hwnd) {
    DWORD now = GetTickCount();
    int key;
    if (now == g_press_sample_tick) return;
    g_press_sample_tick = now;
    frame_cursor_update(hwnd);
    key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (GetAsyncKeyState(key) >= 0) {
        g_press_sampled = 0;
        g_press_on_caption = 0;
        g_press_up_tick = now;
        return;
    }
    if (g_press_sampled) return;
    g_press_sampled = 1;
    g_press_down_tick = now;
    g_press_on_caption = GetCursorPos(&g_press_pos) &&
        WindowFromPoint(g_press_pos) == hwnd &&
        DefWindowProcA(hwnd, WM_NCHITTEST, 0,
                       (LPARAM)(((DWORD)(WORD)g_press_pos.y << 16) |
                                (DWORD)(WORD)g_press_pos.x)) == HTCAPTION;
    if (g_press_on_caption)
        window_rects(hwnd, &g_press_rect, &g_press_client);
}

static void drag_begin_detached(HWND hwnd) {
    RECT now;
    DWORD tmax = GetTickCount();
    if (g_press_on_caption &&
        (!GetWindowRect(hwnd, &now) || !EqualRect(&now, &g_press_rect)))
        g_press_on_caption = 0; /* the window moved since: stale */
    if (g_press_on_caption) {
        /* From where the press was seen: the window has not moved since. */
        g_drag_cursor0 = g_press_pos;
        g_drag_rect0 = g_press_rect;
        g_drag_client0 = g_press_client;
        g_press_on_caption = 0; /* this press is used */
        tmax = g_press_down_tick;
    } else {
        if (!GetCursorPos(&g_drag_cursor0)) return;
        window_rects(hwnd, &g_drag_rect0, &g_drag_client0);
    }
    /* The press came after the button was last seen up. */
    if (!band_add(&g_drag_rect0, g_drag_client0.top, g_press_up_tick, tmax))
        return;
    g_drag_active = 1;
    g_drag_eat = 1;
    g_drag_detached = 1;
    g_drag_release_seen = 0;
    g_drag_moves = 0;
    g_drag_start = GetTickCount();
    scale_log("drag\tbegin\tdetached=1\tpending=%d", g_band_count);
}

/* Moves the window to the cursor; ends the drag once the button is up. */
static void drag_update(void) {
    POINT pt;
    RECT r;
    int x, y;
    if (g_drag_active && g_main && GetCursorPos(&pt)) {
        x = g_drag_rect0.left + pt.x - g_drag_cursor0.x;
        y = g_drag_rect0.top + pt.y - g_drag_cursor0.y;
        if (GetWindowRect(g_main, &r) && (r.left != x || r.top != y)) {
            SetWindowPos(g_main, NULL, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_NOOWNERZORDER);
            g_drag_moves++;
        }
        if (!left_button_down()) {
            drag_end("button_up");
            /* The release is already queued for the window; later presses
             * must not be routed here as client clicks. */
            if (!g_drag_detached && GetCapture() == g_main) ReleaseCapture();
        }
    }
}

/* During a drag: follows the cursor and drops the window's queued mouse
 * messages (the release ends the swallowing). */
static void drag_pump(void) {
    MSG m;
    if (!g_drag_active && !g_drag_eat) return;
    if (!g_main || GetCurrentThreadId() != g_main_thread) return;
    drag_update();
    while (g_drag_eat &&
           PeekMessageA(&m, NULL, 0, 0,
                        PM_NOREMOVE | PM_NOYIELD | PM_QS_INPUT |
                            PM_QS_SENDMESSAGE) &&
           m.hwnd == g_main && is_mouse_message(m.message)) {
        if (g_drag_detached && (ends_detached(m.message, m.pt, m.time) ||
                                !is_detached_press(m.message, m.pt, m.time))) {
            /* Older input stays queued for the game; a double-click or a
             * newer press ends the provisional drag. */
            drag_eat_message(m.message, m.pt, m.time);
            break;
        }
        if (!g_drag_detached && !g_drag_active && is_button_press(m.message)) {
            drag_eat_message(m.message, m.pt, m.time); /* ends swallowing */
            break;
        }
        if (!PeekMessageA(&m, g_main, m.message, m.message,
                          PM_REMOVE | PM_NOYIELD | PM_QS_INPUT |
                              PM_QS_SENDMESSAGE))
            break;
        drag_eat_message(m.message, m.pt, m.time);
    }
}

static LRESULT CALLBACK scale_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
    if (g_drag_eat && is_mouse_message(msg)) {
        DWORD pos = GetMessagePos();
        POINT pt;
        pt.x = (SHORT)LOWORD(pos);
        pt.y = (SHORT)HIWORD(pos);
        if (!drag_eat_message(msg, pt, (DWORD)GetMessageTime()))
            goto not_eaten;
        /* Retrieved by the game's own loop: not game input either. */
        return 0;
    }
not_eaten:
    switch (msg) {
    case WM_NCLBUTTONDOWN:
        /* A caption/border press the busy game reads only after the button
         * came up: USER32's move/size loop would start late and move the
         * window from a stale point, so it is dropped. */
        if (is_frame_hit(wp) && wp != HTMAXBUTTON && !left_button_down()) {
            scale_log("frame_press_stale\thit=%lu\tage=%lu",
                      (unsigned long)wp,
                      (unsigned long)(GetTickCount() - GetMessageTime()));
            return 0;
        }
        /* Likewise a menu-bar press: the menu would open long after the
         * click, out of the blue. */
        if (wp == HTMENU && !left_button_down() &&
            (DWORD)(GetTickCount() - (DWORD)GetMessageTime()) >
                MENU_PRESS_STALE_MS) {
            scale_log("menu_press_stale\tage=%lu",
                      (unsigned long)(GetTickCount() - GetMessageTime()));
            return 0;
        }
        if (wp == HTMENU)
            scale_log("menu_press\tage=%lu\tservice=%d",
                      (unsigned long)(GetTickCount() - GetMessageTime()),
                      g_service_busy);
        break;
    case WM_CAPTURECHANGED:
        /* A detached drag holds no capture: the game's own SetCapture and
         * ReleaseCapture for its older click come by here. */
        if (g_drag_active && !g_drag_detached && (HWND)lp != hwnd) {
            scale_log("drag\tcapture_lost");
            drag_end("capture_lost"); /* saves the place it was moved to */
            g_drag_eat = 0;
        }
        break;
    case WM_LBUTTONDOWN:
        /* A press on the menu bar while the game still holds the capture
         * of its previous click (the busy game had not read that click's
         * release yet) is scanned as a client click at the top edge.  It
         * is the menu's: never the game's. */
        if (GetCapture() == hwnd && g_menu && GetMenu(hwnd) == g_menu) {
            DWORD pos = GetMessagePos();
            if (DefWindowProcA(hwnd, WM_NCHITTEST, 0, (LPARAM)pos) == HTMENU) {
                DWORD age = GetTickCount() - (DWORD)GetMessageTime();
                ReleaseCapture();
                if (!left_button_down() && age > MENU_PRESS_STALE_MS) {
                    scale_log("menu_press_stale\tage=%lu\tcaptured=1",
                              (unsigned long)age);
                    return 0;
                }
                scale_log("menu_press\tage=%lu\tservice=%d\tcaptured=1",
                          (unsigned long)age, g_service_busy);
                DefWindowProcA(hwnd, WM_NCLBUTTONDOWN, HTMENU, (LPARAM)pos);
                return 0;
            }
        }
        lp = map_mouse(lp, &g_dst);
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
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
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(hwnd, GWL_STYLE),
                           window_has_menu(hwnd),
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
            drag_end("minimized");
            g_minimized = 1;
            g_size_state = SIZE_MINIMIZED;
            return CallWindowProcA(g_orig_proc, hwnd, msg, SIZE_MINIMIZED,
                                   MAKELPARAM(0, 0));
        }
        update_dst(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        /* Maximized, or restored from maximized (not from minimized). */
        if (!g_in_modal &&
            (wp == SIZE_MAXIMIZED ||
             (wp == SIZE_RESTORED && g_size_state == SIZE_MAXIMIZED)))
            save_window_state(hwnd);
        g_size_state = (int)wp;
        if (g_minimized) {
            g_minimized = 0;
            return CallWindowProcA(g_orig_proc, hwnd, msg, SIZE_RESTORED,
                                   MAKELPARAM(GAME_W, GAME_H));
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1L << 29))) {
            drag_end("fullscreen");
            toggle_fullscreen(hwnd);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0; /* no beep for Alt+Enter */
        break;
    case WM_ENTERSIZEMOVE:
        g_sizemove_loop = 1;
        modal_loops_update(hwnd);
        break;
    case WM_EXITSIZEMOVE:
        g_sizemove_loop = 0;
        modal_loops_update(hwnd);
        save_window_state(hwnd);
        break;
    /* The menu loop blocks the game's thread like the move/size loop. */
    case WM_ENTERMENULOOP:
        g_menu_loop = 1;
        modal_loops_update(hwnd);
        break;
    case WM_EXITMENULOOP:
        g_menu_loop = 0;
        modal_loops_update(hwnd);
        break;
    case WM_INITMENUPOPUP:
        if (HIWORD(lp)) system_menu_init_popup(hwnd);
        else menu_init_popup(hwnd);
        break;
    case WM_COMMAND:
        if (lp == 0 && HIWORD(wp) == 0 && LOWORD(wp) >= CMD_FIRST &&
            LOWORD(wp) <= CMD_LAST) {
            drag_end("menu");
            menu_command(hwnd, LOWORD(wp));
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        /* Alt alone (or F10) would give the menu bar the keyboard, and the
         * arrow keys the game scrolls the map with would then walk the
         * menu.  The menu is for the mouse only. */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
        if ((wp & 0xFFF0) == SYS_SHOW_MENU) {
            show_menu_bar(hwnd, 1);
            return 0;
        }
        break;
    case GT_TOOLS_NOTICE:
        tools_notice(hwnd, wp);
        return 0;
    case WM_TIMER:
        if (wp == NOTICE_TIMER_ID) {
            notice_clear(hwnd);
            return 0;
        }
        if (wp != MODAL_TIMER_ID) break;
        if (g_in_modal && g_modal_tick_cb) g_modal_tick_cb();
        return 0;
    case WM_NCDESTROY: {
        WNDPROC orig = g_orig_proc;
        KillTimer(hwnd, MODAL_TIMER_ID);
        KillTimer(hwnd, NOTICE_TIMER_ID);
        frame_cursor_release();
        g_sizemove_loop = g_menu_loop = 0;
        modal_loop_changed(0);
        /* An attached menu goes with the window. */
        if (g_menu && GetMenu(hwnd) != g_menu) DestroyMenu(g_menu);
        g_menu = NULL;
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
    int k;
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
    /* Built even when hidden ([display] menu=0): the window menu can show
     * it. */
    if (!GetMenu(hwnd)) {
        g_menu = build_menu();
        if (g_menu && g_menu_enabled && !SetMenu(hwnd, g_menu)) {
            DestroyMenu(g_menu);
            g_menu = NULL;
        }
        if (g_menu) add_system_menu_item(hwnd);
    }
    SetRect(&frame, 0, 0, 0, 0);
    AdjustWindowRectEx(&frame, (DWORD)style, window_has_menu(hwnd),
                       (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE));
    k = 0; /* 0 in the log: placed from [window] */
    if (!restore_window(hwnd, frame.right - frame.left,
                        frame.bottom - frame.top))
        default_placement(hwnd, frame.right - frame.left,
                          frame.bottom - frame.top, &k);
    update_dst(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);

    /* The helper thread keeps the proxy loaded until it exits. */
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)(uintptr_t)present_thread, &self)) {
        HelperParam *hp = (HelperParam *)HeapAlloc(GetProcessHeap(),
                                                   HEAP_ZERO_MEMORY,
                                                   sizeof(*hp));
        if (!g_snap)
            g_snap = VirtualAlloc(NULL, GAME_BYTES, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
        if (!g_wake && g_snap)
            g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        void *bits = NULL;
        thread = NULL;
        if (hp) {
            hp->self = self;
            hp->hwnd = hwnd;
            /* The helper's private copy; it frees it when it exits. */
            if (g_snap && g_wake)
                bits = VirtualAlloc(NULL, GAME_BYTES,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
            hp->bits = bits;
            hp->gen = InterlockedIncrement(&g_helper_gen);
            /* hp belongs to the thread from here on (it frees it at once). */
            thread = CreateThread(NULL, 0, present_thread, hp, 0, NULL);
        }
        /* Without a helper able to stretch snapshots, present() stays
         * synchronous (and the helper, if any, polls every 4 ms). */
        InterlockedExchange(&g_async, thread && bits ? 1 : 0);
        if (thread) {
            CloseHandle(thread);
        } else {
            if (bits) VirtualFree(bits, 0, MEM_RELEASE);
            if (hp) HeapFree(GetProcessHeap(), 0, hp);
            FreeLibrary(self);
        }
    }
    scale_log("active\thwnd=%08X\tmultiple=%d\tclient=%dx%d\tfilter=%d"
              "\tmenu=%d",
              (unsigned)(uintptr_t)hwnd, k, GAME_W * k, GAME_H * k, g_filter,
              window_has_menu(hwnd));
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
typedef DWORD (WINAPI *PFN_GetTickCount)(void);
typedef BOOL (WINAPI *PFN_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD,
                                    LPOVERLAPPED);
typedef LONG (WINAPI *PFN_GetMessageTime)(void);

static PFN_GetDC x_GetDC;
static PFN_ReleaseDC x_ReleaseDC;
static PFN_PeekMessageA x_PeekMessageA;
static PFN_GetMessageA x_GetMessageA;
static PFN_BitBlt x_BitBlt;
static PFN_StretchBlt x_StretchBlt;
static PFN_StretchDIBits x_StretchDIBits;
static PFN_PatBlt x_PatBlt;
static PFN_SetPixel x_SetPixel;
static PFN_GetTickCount x_GetTickCount;
static PFN_ReadFile x_ReadFile;
static PFN_GetMessageTime x_GetMessageTime;
static DWORD g_input_tick; /* GetTickCount value of the last service */

/* Hit-test codes whose button-down starts a window move/size loop (or the
 * maximize toggle) and nothing else.  Close, minimize and the system menu
 * stay queued for the game's own loop: WM_CLOSE runs the game's shutdown
 * code, and minimize pauses its BGM. */
static int is_frame_hit(WPARAM hit) {
    return hit == HTCAPTION || hit == HTMAXBUTTON ||
           (hit >= HTLEFT && hit <= HTBOTTOMRIGHT);
}

/* ---- button releases held back for the game ----
 *
 * The game calls SetCapture on a button press and acts on some presses at
 * once (a click that skips a movie starts the next one, which reads no
 * input), so the release stays queued and the window keeps the capture:
 * every later press, the caption included, arrives as a client click behind
 * that release, and input is read in order.  While the user is pressing the
 * caption or border at that moment, such a queued release is taken out of
 * the queue and kept here, the capture is released, and the game's own next
 * PeekMessage/GetMessage returns the release before anything newer, with
 * the input-time fix applied as for any retrieved message.  Presses are
 * never held: their release would then be scanned without the capture the
 * press sets and arrive as WM_NCLBUTTONUP.  Main window thread only. */
enum { HELD_CAPACITY = 16 };
static MSG g_held[HELD_CAPACITY];
static int g_held_first, g_held_count;
/* GetMessageTime for the game after it removed a held message. */
static int g_held_time_active;
static LONG g_held_time;

static GtScaleInputTimeFn g_input_time_cb;

void gt_scale_set_input_time_callback(GtScaleInputTimeFn fn) {
    g_input_time_cb = fn;
}

static int is_button_release(UINT message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
           message == WM_MBUTTONUP;
}

static int hold_input(const MSG *msg) {
    if (g_held_count >= HELD_CAPACITY) return 0;
    g_held[(g_held_first + g_held_count) % HELD_CAPACITY] = *msg;
    g_held_count++;
    return 1;
}

/* Copies the oldest held message to msg if it passes the caller's filter,
 * removing it with PM_REMOVE. */
static int take_held_input(LPMSG msg, HWND hwnd, UINT first, UINT last,
                           UINT remove) {
    const MSG *m;
    if (!g_held_count || !msg || GetCurrentThreadId() != g_main_thread)
        return 0;
    /* While a drag owns the mouse, held releases wait for its end (their
     * ReleaseCapture would end it); keyboard input may go first meanwhile,
     * the game's key handler only sets a flag.  A detached drag holds no
     * capture and its press is newer than them: they go first. */
    if ((g_drag_active || g_drag_eat) && !g_drag_detached) return 0;
    m = &g_held[g_held_first];
    if (hwnd && hwnd != m->hwnd) return 0;
    if ((first || last) && (m->message < first || m->message > last))
        return 0;
    *msg = *m;
    if (remove & PM_REMOVE) {
        g_held_first = (g_held_first + 1) % HELD_CAPACITY;
        g_held_count--;
        g_held_time_active = 1;
        g_held_time = g_input_time_cb ? g_input_time_cb(msg) : (LONG)msg->time;
        scale_log("input_released\tmsg=%04x\tleft=%d", msg->message,
                  g_held_count);
    }
    return 1;
}

/* 1 while the user holds the (logical) left button on the main window's
 * caption, border or maximize button. */
/* The hit-test code under the cursor while the (logical) left button is
 * down on the main window, else HTNOWHERE. */
static LRESULT press_hit_now(HWND hwnd) {
    POINT pt;
    int key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (GetAsyncKeyState(key) >= 0 || !GetCursorPos(&pt) ||
        WindowFromPoint(pt) != hwnd)
        return HTNOWHERE;
    return DefWindowProcA(
        hwnd, WM_NCHITTEST, 0,
        (LPARAM)(((DWORD)(WORD)pt.y << 16) | (DWORD)(WORD)pt.x));
}

static void service_once_per_tick(DWORD now) {
    if (now == g_input_tick) return;
    g_input_tick = now;
    gt_scale_service_frame_input();
}

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

/* The game's only clock read (tick/60 s helper at 0x40C010).  Its waits spin
 * on it without reading the queue, e.g. the 3 s Bandai logo before the
 * opening movie, so a caption press there used to wait for the next poll.
 * Serviced at most once per tick value (about 16 ms). */
static DWORD WINAPI exe_GetTickCount(void) {
    DWORD now = x_GetTickCount();
    service_once_per_tick(now);
    return now;
}

/* Scene loads (images, scenario data) read files without polling either. */
static BOOL WINAPI exe_ReadFile(HANDLE file, LPVOID buf, DWORD n, LPDWORD read,
                                LPOVERLAPPED ov) {
    service_once_per_tick(GetTickCount());
    return x_ReadFile(file, buf, n, read, ov);
}

static void present_at_poll(void) {
    if (g_dirty && GetCurrentThreadId() == g_main_thread &&
        (DWORD)(GetTickCount() - g_last_present) >= PUMP_PRESENT_MIN_MS)
        present(NULL);
}

static BOOL WINAPI exe_PeekMessageA(LPMSG msg, HWND hwnd, UINT first,
                                    UINT last, UINT remove) {
    BOOL r;
    present_at_poll();
    if (g_main && GetCurrentThreadId() == g_main_thread) sample_press(g_main);
    drag_pump();
    if (take_held_input(msg, hwnd, first, last, remove)) return TRUE;
    r = x_PeekMessageA(msg, hwnd, first, last, remove);
    if (r && (remove & PM_REMOVE)) g_held_time_active = 0;
    return r;
}

static BOOL WINAPI exe_GetMessageA(LPMSG msg, HWND hwnd, UINT first,
                                   UINT last) {
    BOOL r;
    /* GetMessageA may block: never leave a frame unshown behind it. */
    if (g_dirty && GetCurrentThreadId() == g_main_thread) present(NULL);
    drag_pump();
    if (take_held_input(msg, hwnd, first, last, PM_REMOVE)) return TRUE;
    r = x_GetMessageA(msg, hwnd, first, last);
    if (r && r != -1) g_held_time_active = 0;
    return r;
}

static LONG WINAPI exe_GetMessageTime(void) {
    if (g_held_time_active && GetCurrentThreadId() == g_main_thread)
        return g_held_time;
    return x_GetMessageTime();
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

static LONGLONG qpc_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* Waits until the QPC value target: a high-resolution timer (or Sleep(1))
 * for most of it, then yields for the last half millisecond. */
static void wait_until(LONGLONG target) {
    for (;;) {
        LONGLONG left_us, now = qpc_now();
        if (now >= target) return;
        service_once_per_tick(GetTickCount());
        left_us = (target - now) * 1000000 / g_qpc_freq.QuadPart;
        if (left_us > 700 && g_fade_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(left_us - 500) * 10; /* relative, 100 ns units */
            if (SetWaitableTimer(g_fade_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(g_fade_timer, (DWORD)(left_us / 1000) + 2);
                continue;
            }
        }
        if (left_us > 2000) Sleep(1);
        else SwitchToThread();
    }
}

/* Returns FADE_CAVE_RET when the exe Phase 3 (b) cave is there, i.e. the
 * instruction at it is "jmp FADE_STEP_RET". */
static uintptr_t find_fade_cave_ret(void) {
    const BYTE *p = (const BYTE *)FADE_CAVE_RET;
    LONG rel;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x00400000u) return 0;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE9) return 0;
    memcpy(&rel, p + 1, sizeof(rel));
    if (FADE_CAVE_RET + 5u + (uintptr_t)rel != FADE_STEP_RET) return 0;
    return FADE_CAVE_RET;
}

/* "add word [esp+12h],2; cmp word [esp+12h],64h; jl/jle" at FADE_STEP_RET. */
static int find_fade_steps(void) {
    static const BYTE loop[] = {0x66, 0x83, 0x44, 0x24, 0x12, 0x02, 0x66, 0x83,
                                0x7C, 0x24, 0x12, 0x64, 0x0F};
    const BYTE *p = (const BYTE *)FADE_STEP_RET;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x00400000u) return 0;
    if (IsBadReadPtr(p, sizeof(loop) + 1) || memcmp(p, loop, sizeof(loop)))
        return 0;
    if (p[sizeof(loop)] == 0x8C) return 50;
    if (p[sizeof(loop)] == 0x8E) return 51;
    return 0;
}

static void fade_step_begin(void) {
    LONGLONG now = qpc_now();
    LONGLONG step = g_qpc_freq.QuadPart * g_fade_ms / (1000 * FADE_STEPS);
    LONGLONG reset = g_qpc_freq.QuadPart * FADE_RESET_MS / 1000 + step;
    if (!g_fade_last || now - g_fade_last > reset ||
        (g_fade_steps && g_fade_index >= g_fade_steps)) {
        if (g_fade_index)
            scale_log("fade\tsteps=%d\tms=%d", g_fade_index,
                      (int)((g_fade_last - g_fade_start) * 1000 /
                            g_qpc_freq.QuadPart));
        g_fade_start = now;
        g_fade_index = 0;
    } else {
        wait_until(g_fade_start + step * g_fade_index);
    }
    g_fade_index++;
}

static int WINAPI exe_StretchDIBits(HDC d, int x, int y, int cx, int cy,
                                    int sx, int sy, int scx, int scy,
                                    const VOID *bits, const BITMAPINFO *bmi,
                                    UINT usage, DWORD rop) {
    uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    int fade = g_fade_ms > 0 &&
        (ret == FADE_STEP_RET || (g_fade_cave_ret && ret == g_fade_cave_ret));
    int r;
    if (fade) fade_step_begin();
    r = x_StretchDIBits(d, x, y, cx, cy, sx, sy, scx, scy, bits, bmi, usage,
                        rop);
    if (is_shadow(d)) {
        mark_dirty();
        if (fade) present(NULL);
    }
    if (fade) g_fade_last = qpc_now();
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

/* ---- the stock QuickTime DLL's slow audio-device calls ----
 *
 * QuickTime's initialisation (selector 0x39, before the opening) opens and
 * closes the MIDI mapper, about 100 ms each (seconds on some machines), and
 * nothing reads the window's queue meanwhile.  On the window's thread these
 * calls run on a short-lived worker thread instead, with the same arguments
 * and result, while the window's thread services caption/border presses.
 * Callbacks, if any, are addressed by the arguments (window, thread id or
 * function), not by the calling thread. */
typedef MMRESULT (WINAPI *PFN_midiOutOpen)(LPHMIDIOUT, UINT, DWORD_PTR,
                                          DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *PFN_midiOutClose)(HMIDIOUT);
typedef UINT (WINAPI *PFN_waveOutGetNumDevs)(void);
static PFN_midiOutOpen x_midiOutOpen;
static PFN_midiOutClose x_midiOutClose;
static PFN_waveOutGetNumDevs x_waveOutGetNumDevs;

enum { CALL_MIDI_OPEN, CALL_MIDI_CLOSE, CALL_WAVE_NUMDEVS };
typedef struct DeviceCall {
    int kind;
    DWORD_PTR a[5];
    DWORD_PTR result;
} DeviceCall;

static DWORD WINAPI device_call_thread(LPVOID param) {
    DeviceCall *c = (DeviceCall *)param;
    switch (c->kind) {
    case CALL_MIDI_OPEN:
        c->result = x_midiOutOpen((LPHMIDIOUT)c->a[0], (UINT)c->a[1], c->a[2],
                                  c->a[3], (DWORD)c->a[4]);
        break;
    case CALL_MIDI_CLOSE:
        c->result = x_midiOutClose((HMIDIOUT)c->a[0]);
        break;
    default:
        c->result = x_waveOutGetNumDevs();
        break;
    }
    return 0;
}

static void run_device_call(DeviceCall *c) {
    HANDLE th = NULL;
    DWORD start = GetTickCount();
    int serviced = 0;
    if (g_hooks_active && g_main && GetCurrentThreadId() == g_main_thread)
        th = CreateThread(NULL, 0, device_call_thread, c, 0, NULL);
    if (!th) {
        device_call_thread(c);
        return;
    }
    for (;;) {
        /* Wakes for input and sent messages, and polls every 15 ms for a
         * press that could not be taken yet. */
        MSG m;
        DWORD r = MsgWaitForMultipleObjects(1, &th, FALSE, 15,
                                            QS_MOUSEBUTTON | QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_FAILED) {
            WaitForSingleObject(th, INFINITE);
            break;
        }
        PeekMessageA(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        gt_scale_service_frame_input();
        serviced++;
    }
    CloseHandle(th);
    scale_log("device_call\tkind=%d\tms=%lu\twakes=%d", c->kind,
              (unsigned long)(GetTickCount() - start), serviced);
}

static MMRESULT WINAPI qt_midiOutOpen(LPHMIDIOUT h, UINT id, DWORD_PTR cb,
                                      DWORD_PTR inst, DWORD flags) {
    DeviceCall c = {CALL_MIDI_OPEN, {(DWORD_PTR)h, id, cb, inst, flags}, 0};
    run_device_call(&c);
    return (MMRESULT)c.result;
}

static MMRESULT WINAPI qt_midiOutClose(HMIDIOUT h) {
    DeviceCall c = {CALL_MIDI_CLOSE, {(DWORD_PTR)h, 0, 0, 0, 0}, 0};
    run_device_call(&c);
    return (MMRESULT)c.result;
}

static UINT WINAPI qt_waveOutGetNumDevs(void) {
    DeviceCall c = {CALL_WAVE_NUMDEVS, {0, 0, 0, 0, 0}, 0};
    run_device_call(&c);
    return (UINT)c.result;
}

/* ---- the game's message boxes ----
 *
 * MessageBox centres its window on the monitor, not on its owner.  The
 * original 576x416 window opened in the middle of the screen, so the game's
 * boxes (the exit confirmation of its WM_CLOSE handler, the 0x7E notice,
 * the "Wrong ... handle" debug boxes) looked centred on it; once the window
 * is moved or enlarged they appear somewhere else.  While the window exists,
 * a box the game shows from its thread with no owner or the main window as
 * owner is moved, as it is activated, to the centre of the window (kept in
 * the window's work area) by a CBT hook on that thread for that call only. */
typedef int (WINAPI *PFN_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT);
static PFN_MessageBoxA x_MessageBoxA;
static HHOOK g_msgbox_hook;
static HWND g_msgbox_center; /* the window to centre the next box on */

static LRESULT CALLBACK msgbox_cbt(int code, WPARAM wp, LPARAM lp) {
    HHOOK hook = g_msgbox_hook;
    if (code == HCBT_ACTIVATE && g_msgbox_center) {
        HWND dlg = (HWND)wp;
        char cls[16];
        RECT owner, box, area;
        if (GetClassNameA(dlg, cls, sizeof(cls)) &&
            lstrcmpA(cls, "#32770") == 0 &&
            GetWindowRect(g_msgbox_center, &owner) &&
            GetWindowRect(dlg, &box)) {
            int w = box.right - box.left, h = box.bottom - box.top;
            box.left = owner.left + ((owner.right - owner.left) - w) / 2;
            box.top = owner.top + ((owner.bottom - owner.top) - h) / 2;
            box.right = box.left + w;
            box.bottom = box.top + h;
            area = window_work_area(g_msgbox_center);
            box = place_in_area(box, &area);
            SetWindowPos(dlg, NULL, box.left, box.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            scale_log("msgbox\tcentred\tat=%ld,%ld", box.left, box.top);
        }
        /* One box per call: later dialogs of that call stay as they are. */
        g_msgbox_center = NULL;
    }
    return CallNextHookEx(hook, code, wp, lp);
}

static int WINAPI exe_MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption,
                                  UINT type) {
    HWND main = g_main;
    HHOOK hook = NULL;
    int r;
    if (main && (!owner || owner == main) &&
        GetCurrentThreadId() == g_main_thread && IsWindowVisible(main) &&
        !IsIconic(main) && !g_msgbox_hook) {
        g_msgbox_center = main;
        hook = SetWindowsHookExA(WH_CBT, msgbox_cbt, NULL,
                                 GetCurrentThreadId());
        g_msgbox_hook = hook;
    }
    r = x_MessageBoxA(owner, text, caption, type);
    if (hook) {
        UnhookWindowsHookEx(hook);
        g_msgbox_hook = NULL;
        g_msgbox_center = NULL;
    }
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
    {"kernel32.dll", "GetTickCount", (void *)(uintptr_t)exe_GetTickCount,
     (void **)&x_GetTickCount},
    {"kernel32.dll", "ReadFile", (void *)(uintptr_t)exe_ReadFile,
     (void **)&x_ReadFile},
    {"user32.dll", "GetMessageTime", (void *)(uintptr_t)exe_GetMessageTime,
     (void **)&x_GetMessageTime},
    {"user32.dll", "ShowCursor", (void *)(uintptr_t)exe_ShowCursor,
     (void **)&x_ShowCursor},
    {"user32.dll", "MessageBoxA", (void *)(uintptr_t)exe_MessageBoxA,
     (void **)&x_MessageBoxA},
};

/* With scaling off: fade pacing only. */
static const HookSpec k_fade_hooks[] = {
    {"gdi32.dll", "StretchDIBits", (void *)(uintptr_t)exe_StretchDIBits,
     (void **)&x_StretchDIBits},
};

static const HookSpec k_qt_device_hooks[] = {
    {"winmm.dll", "midiOutOpen", (void *)(uintptr_t)qt_midiOutOpen,
     (void **)&x_midiOutOpen},
    {"winmm.dll", "midiOutClose", (void *)(uintptr_t)qt_midiOutClose,
     (void **)&x_midiOutClose},
    {"winmm.dll", "waveOutGetNumDevs", (void *)(uintptr_t)qt_waveOutGetNumDevs,
     (void **)&x_waveOutGetNumDevs},
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

enum { HOOK_CAPACITY = 32 };
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
    /* 1 is the original size with scaling on (resizable, the menu bar);
     * what the size menu saves for 1x.  0 and garbage stay off. */
    if (n < 1) return SCALE_OFF;
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
    g_ini_path[0] = '\0';
    if (ini_path) lstrcpynA(g_ini_path, ini_path, sizeof(g_ini_path));
    read_setting("QTIM_DISPLAY_SCALE", "scale", "auto", ini_path, value,
                 sizeof(value));
    g_scale_request = parse_scale(value);
    read_setting("QTIM_DISPLAY_FILTER", "filter", "auto", ini_path, value,
                 sizeof(value));
    g_filter = parse_filter(value);
    g_start_fullscreen = ini_path &&
        GetPrivateProfileIntA("display", "fullscreen", 0, ini_path) != 0;
    g_menu_enabled = !ini_path ||
        GetPrivateProfileIntA("display", "menu", 1, ini_path) != 0;
    g_remember_window = !ini_path ||
        GetPrivateProfileIntA("display", "remember_window", 1, ini_path) != 0;
    g_scale_from_env = GetEnvironmentVariableA("QTIM_DISPLAY_SCALE", NULL, 0) != 0;
    read_setting("QTIM_FADE_MS", "fade_ms", "50", ini_path, value,
                 sizeof(value));
    g_fade_ms = atoi(value);
    if (g_fade_ms < 0) g_fade_ms = 0;
    if (g_fade_ms > FADE_MS_MAX) g_fade_ms = FADE_MS_MAX;
    if (g_fade_ms && !QueryPerformanceFrequency(&g_qpc_freq)) g_fade_ms = 0;
    if (g_fade_ms) {
        g_fade_cave_ret = find_fade_cave_ret();
        g_fade_steps = find_fade_steps();
    }
    if (g_fade_ms) {
        g_fade_timer = CreateWaitableTimerExW(
            NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!g_fade_timer) g_fade_timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (g_scale_request == SCALE_OFF) {
        scale_log("off\treason=disabled");
        if (g_fade_ms) {
            if (install_hooks(GetModuleHandleA(NULL), k_fade_hooks, 1, 1)) {
                g_fade_only = 1;
            } else {
                restore_hooks();
                g_fade_ms = 0;
            }
        }
        scale_log("fade_pacing\tms=%d\tfade_cave=%d\tfade_steps=%d", g_fade_ms,
                  g_fade_cave_ret != 0, g_fade_steps);
        return 0;
    }
    InitializeCriticalSection(&g_present_lock);
    InitializeCriticalSection(&g_helper_lock);
    g_lock_ready = 1;
    if (!install_hooks(GetModuleHandleA(NULL), k_exe_hooks,
                       sizeof(k_exe_hooks) / sizeof(k_exe_hooks[0]), 1) ||
        !install_hooks(qt_module, k_qt_hooks,
                       sizeof(k_qt_hooks) / sizeof(k_qt_hooks[0]), 0) ||
        !install_hooks(qt_module, k_qt_device_hooks,
                       sizeof(k_qt_device_hooks) /
                           sizeof(k_qt_device_hooks[0]), 0)) {
        restore_hooks();
        g_fade_ms = 0;
        scale_log("off\treason=iat_patch_failed");
        return 0;
    }
    g_hooks_active = 1;
    scale_log("hooked\tslots=%u\tscale=%d\tfilter=%d\tfullscreen=%d\tfade_ms=%d"
              "\tfade_cave=%d\tfade_steps=%d", g_record_count, g_scale_request,
              g_filter,
              g_start_fullscreen, g_fade_ms, g_fade_cave_ret != 0,
              g_fade_steps);
    return 1;
}

void gt_scale_detach(int process_exit) {
    if (!process_exit && x_ShowCursor) frame_cursor_release();
    if (g_fade_only) {
        g_fade_only = 0;
        restore_hooks();
    }
    if (!process_exit && g_fade_timer) {
        CloseHandle(g_fade_timer);
        g_fade_timer = NULL;
    }
    if (!g_hooks_active) return;
    g_hooks_active = 0;
    InterlockedExchange(&g_stop, 1);
    restore_hooks();
    /* At process exit the window and the helper thread are already gone or
     * about to be; only a FreeLibrary detach needs the window put back.
     * (The helper thread holds a module reference, so that detach can only
     * happen once the thread has left.) */
    if (!process_exit && g_main && IsWindow(g_main) && g_orig_proc &&
        GetWindowLongA(g_main, GWL_WNDPROC) == (LONG)(uintptr_t)scale_wndproc) {
        /* The window goes back to the game without the proxy's menu. */
        if (g_menu) {
            if (GetMenu(g_main) == g_menu) SetMenu(g_main, NULL);
            DestroyMenu(g_menu);
            g_menu = NULL;
            GetSystemMenu(g_main, TRUE); /* the default window menu */
        }
        KillTimer(g_main, NOTICE_TIMER_ID);
        if (g_notice_shown) SetWindowTextA(g_main, "Gundam Tactics");
        SetWindowLongA(g_main, GWL_WNDPROC, (LONG)(uintptr_t)g_orig_proc);
    }
    if (!process_exit) {
        /* No helper is left (each holds a module reference). */
        g_async = 0;
        if (g_wake) CloseHandle(g_wake);
        g_wake = NULL;
        if (g_snap) VirtualFree(g_snap, 0, MEM_RELEASE);
        g_snap = NULL;
    }
}

void gt_scale_service_frame_input(void) {
    HWND hwnd = g_main;
    MSG msg;
    /* g_service_busy: a modal loop may reach a clock read again */
    if (!g_hooks_active || !hwnd || GetCurrentThreadId() != g_main_thread ||
        g_service_busy)
        return;
    sample_press(hwnd);
    if (g_drag_eat) drag_pump();
    /* Only a drag in progress, or a captured one whose release is still to
     * be swallowed, stops the scan: a detached drag can wait seconds for the
     * busy game to read the input in front of it, and a new caption press
     * meanwhile starts a new drag. */
    if (g_drag_active || (g_drag_eat && !g_drag_detached)) return;
    g_service_busy = 1;
    /* Input is read in order, so a filtered peek for the press cannot get
     * past an earlier mouse move (the cursor crossing the client area or the
     * caption on its way).  While a button message is queued, hand the
     * moves at the head to the window as the game's own loop would (its
     * WM_MOUSEMOVE only records the position) until the press is reached.
     * Sent messages are processed too, as by any PeekMessage: input can wait
     * behind them (the activation that a click on the window starts). */
    while (HIWORD(GetQueueStatus(QS_MOUSEBUTTON)) & QS_MOUSEBUTTON) {
        if (g_drag_eat) {
            /* A pending drag's own press may have reached the head. */
            drag_pump();
            if (g_drag_active || (g_drag_eat && !g_drag_detached)) break;
        }
        if (!PeekMessageA(&msg, NULL, 0, 0,
                          PM_NOREMOVE | PM_NOYIELD | PM_QS_INPUT |
                              PM_QS_SENDMESSAGE) ||
            msg.hwnd != hwnd)
            break;
        if (msg.message != WM_MOUSEMOVE && msg.message != WM_NCMOUSEMOVE &&
            msg.message != WM_NCLBUTTONUP &&
            !((msg.message == WM_NCLBUTTONDOWN ||
               msg.message == WM_NCLBUTTONDBLCLK) && is_frame_hit(msg.wParam)) &&
            !(msg.message == WM_NCLBUTTONDOWN && msg.wParam == HTMENU)) {
            /* A release in front: hold it for the game, but only while the
             * user is pressing the frame now. */
            int released = 0;
            LRESULT hit = press_hit_now(hwnd);
            /* Where the press began decides once it was sampled: a press
             * in the client that slid onto the caption is not a drag. */
            int caption = g_press_sampled ?
                (g_press_on_caption && left_button_down()) :
                hit == HTCAPTION;
            /* The menu bar too: once the release is held and the game's
             * capture is gone, the press behind it is scanned as a menu-bar
             * press and opens the menu below, instead of waiting for the
             * busy game and being dropped as stale. */
            if (!caption && !is_frame_hit((WPARAM)hit) && hit != HTMENU)
                break;
            if (!is_button_release(msg.message) ||
                g_held_count >= HELD_CAPACITY ||
                !PeekMessageA(&msg, hwnd, msg.message, msg.message,
                              PM_REMOVE | PM_NOYIELD | PM_QS_INPUT |
                                  PM_QS_SENDMESSAGE)) {
                /* Anything else in front (a click the game has not read
                 * yet, a key): move the window anyway if it is the caption
                 * (borders and the maximize button wait for the game). */
                if (caption) {
                    drag_begin_detached(hwnd);
                    drag_pump();
                }
                break;
            }
            hold_input(&msg);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
                released = 1;
            }
            scale_log("input_held\tmsg=%04x\tcount=%d\tcapture_released=%d",
                      msg.message, g_held_count, released);
            continue;
        }
        if (!PeekMessageA(&msg, hwnd, msg.message, msg.message,
                          PM_REMOVE | PM_NOYIELD | PM_QS_INPUT |
                              PM_QS_SENDMESSAGE))
            break;
        if (msg.message == WM_NCLBUTTONDOWN ||
            msg.message == WM_NCLBUTTONDBLCLK)
            scale_log("frame_input\tmsg=%04x\thit=%lu\twait_ms=%lu",
                      msg.message, (unsigned long)msg.wParam,
                      (unsigned long)(GetTickCount() - msg.time));
        if (msg.message == WM_NCLBUTTONDOWN && msg.wParam == HTCAPTION) {
            /* Follow the cursor without blocking the game. */
            drag_begin(hwnd, &msg);
            drag_pump();
            break;
        }
        /* A border press (or a caption double-click) enters USER32's modal
         * move/size loop here, which pumps the queue itself until the button
         * is released; a menu-bar press enters the menu loop, which runs
         * until the menu is closed.  Both keep the proxy's movies going
         * (modal_loops_update). */
        DispatchMessageA(&msg);
    }
    g_service_busy = 0;
}

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM out) {
    char cls[16];
    if (GetClassNameA(hwnd, cls, sizeof(cls)) && lstrcmpA(cls, "Gundam") == 0) {
        *(HWND *)out = hwnd;
        return FALSE;
    }
    return TRUE;
}

int gt_scale_adopt_early(void) {
    HWND hwnd = NULL;
    if (!g_hooks_active || g_main || g_failed) return 0;
    EnumThreadWindows(GetCurrentThreadId(), find_game_window, (LPARAM)&hwnd);
    if (!hwnd || !is_main_window(hwnd)) return 0;
    scale_log("adopted_early");
    /* Nothing pumps messages until QuickTime's initialisation returns, so
     * paint the enlarged client now instead of leaving it undrawn. */
    UpdateWindow(hwnd);
    return 1;
}

HDC gt_scale_get_dc(HWND hwnd) {
    if (g_hooks_active && is_main_window(hwnd)) return acquire_shadow();
    return GetDC(hwnd);
}

void gt_scale_release_dc(HWND hwnd, HDC dc) {
    if (is_shadow(dc)) {
        mark_dirty_own();
        release_shadow_dc();
        return;
    }
    ReleaseDC(hwnd, dc);
}

int gt_scale_foreign_draws(LONG *count) {
    if (!g_hooks_active || !g_main || g_failed) return 0;
    if (count) *count = g_foreign_draws;
    return 1;
}

HWND gt_scale_main_window(void) {
    return g_hooks_active ? g_main : NULL;
}
