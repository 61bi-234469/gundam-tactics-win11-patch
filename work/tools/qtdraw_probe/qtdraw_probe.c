/*
 * Investigation-only probe used to design the window scaling.
 *
 * Installed as QTIM32R.DLL between the compatibility proxy (QTIM32.DLL) and
 * the stock QuickTime 2.x DLL, which is renamed to QTIM32S.DLL. Every export
 * jumps straight to QTIM32S. On attach it hooks the stock DLL's USER32/GDI32
 * imports that can reach the screen, plus LoadLibraryA/GetProcAddress so the
 * components QuickTime loads later (DCI32.QTC, DCIMAN32) are covered too, and
 * writes one line per call to qtdraw_probe.log in the current directory.
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static HMODULE g_real;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static volatile LONG g_in_log;

void *p_2, *p_101, *p_102, *p_103, *p_104, *p_105, *p_107, *p_109, *p_315;
void *p_505, *p_506, *p_507, *p_508, *p_509, *p_510, *p_511, *p_512, *p_513,
     *p_514, *p_515, *p_516, *p_517, *p_518, *p_519, *p_520, *p_521, *p_522,
     *p_523, *p_524, *p_525, *p_526, *p_527, *p_528, *p_529;

#define THUNK(N) \
    __attribute__((naked)) void thunk_##N(void) { __asm__("jmp *_p_" #N); }
THUNK(2) THUNK(101) THUNK(102) THUNK(103) THUNK(104) THUNK(105) THUNK(107)
THUNK(109) THUNK(315) THUNK(505) THUNK(506) THUNK(507) THUNK(508) THUNK(509)
THUNK(510) THUNK(511) THUNK(512) THUNK(513) THUNK(514) THUNK(515) THUNK(516)
THUNK(517) THUNK(518) THUNK(519) THUNK(520) THUNK(521) THUNK(522) THUNK(523)
THUNK(524) THUNK(525) THUNK(526) THUNK(527) THUNK(528) THUNK(529)

static void plog(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    int n;
    DWORD written;
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (InterlockedCompareExchange(&g_in_log, 1, 0) != 0) return;
    n = snprintf(buf, sizeof(buf), "%lu\ttid=%lu\t", (unsigned long)GetTickCount(),
                 (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    EnterCriticalSection(&g_lock);
    WriteFile(g_log, buf, n, &written, NULL);
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_in_log, 0);
}

static const char *hwnd_desc(HWND hwnd, char *out, size_t cap) {
    char cls[64] = "", title[64] = "";
    RECT r = {0, 0, 0, 0};
    if (!hwnd) { snprintf(out, cap, "NULL"); return out; }
    InterlockedExchange(&g_in_log, 1);
    GetClassNameA(hwnd, cls, sizeof(cls));
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClientRect(hwnd, &r);
    InterlockedExchange(&g_in_log, 0);
    snprintf(out, cap, "%08X[%s|%s|client=%ldx%ld|vis=%d]", (unsigned)(uintptr_t)hwnd,
             cls, title, (long)r.right, (long)r.bottom, IsWindowVisible(hwnd) ? 1 : 0);
    return out;
}

static const char *dc_desc(HDC hdc, char *out, size_t cap) {
    HWND hwnd = hdc ? WindowFromDC(hdc) : NULL;
    DWORD type = hdc ? GetObjectType(hdc) : 0;
    char h[192];
    snprintf(out, cap, "%08X(type=%lu,wnd=%s)", (unsigned)(uintptr_t)hdc,
             (unsigned long)type, hwnd ? hwnd_desc(hwnd, h, sizeof(h)) : "-");
    return out;
}

#define CALLER ((unsigned)(uintptr_t)__builtin_return_address(0))

static HDC (WINAPI *r_GetDC)(HWND);
static HDC WINAPI h_GetDC(HWND hwnd) {
    HDC dc = r_GetDC(hwnd);
    char a[192];
    plog("GetDC\tcaller=%08X\thwnd=%s\t-> %08X", CALLER, hwnd_desc(hwnd, a, sizeof(a)),
         (unsigned)(uintptr_t)dc);
    return dc;
}
static HDC (WINAPI *r_GetDCEx)(HWND, HRGN, DWORD);
static HDC WINAPI h_GetDCEx(HWND hwnd, HRGN rgn, DWORD flags) {
    HDC dc = r_GetDCEx(hwnd, rgn, flags);
    char a[192];
    plog("GetDCEx\tcaller=%08X\thwnd=%s\trgn=%08X\tflags=%08lX\t-> %08X", CALLER,
         hwnd_desc(hwnd, a, sizeof(a)), (unsigned)(uintptr_t)rgn,
         (unsigned long)flags, (unsigned)(uintptr_t)dc);
    return dc;
}
static int (WINAPI *r_ReleaseDC)(HWND, HDC);
static int WINAPI h_ReleaseDC(HWND hwnd, HDC dc) {
    plog("ReleaseDC\tcaller=%08X\thwnd=%08X\thdc=%08X", CALLER,
         (unsigned)(uintptr_t)hwnd, (unsigned)(uintptr_t)dc);
    return r_ReleaseDC(hwnd, dc);
}
static BOOL (WINAPI *r_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BOOL WINAPI h_BitBlt(HDC d, int x, int y, int cx, int cy, HDC s, int sx,
                            int sy, DWORD rop) {
    char a[256], b[256];
    plog("BitBlt\tcaller=%08X\tdst=%s\t%d,%d,%d,%d\tsrc=%s\t%d,%d\trop=%08lX", CALLER,
         dc_desc(d, a, sizeof(a)), x, y, cx, cy, dc_desc(s, b, sizeof(b)), sx, sy,
         (unsigned long)rop);
    return r_BitBlt(d, x, y, cx, cy, s, sx, sy, rop);
}
static BOOL (WINAPI *r_PatBlt)(HDC, int, int, int, int, DWORD);
static BOOL WINAPI h_PatBlt(HDC d, int x, int y, int cx, int cy, DWORD rop) {
    char a[256];
    plog("PatBlt\tcaller=%08X\tdst=%s\t%d,%d,%d,%d\trop=%08lX", CALLER,
         dc_desc(d, a, sizeof(a)), x, y, cx, cy, (unsigned long)rop);
    return r_PatBlt(d, x, y, cx, cy, rop);
}
static int (WINAPI *r_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int,
                                         UINT, UINT, const VOID *,
                                         const BITMAPINFO *, UINT);
static int WINAPI h_SetDIBitsToDevice(HDC d, int x, int y, DWORD cx, DWORD cy,
                                      int sx, int sy, UINT start, UINT lines,
                                      const VOID *bits, const BITMAPINFO *bmi,
                                      UINT use) {
    char a[256];
    int r = r_SetDIBitsToDevice(d, x, y, cx, cy, sx, sy, start, lines, bits, bmi, use);
    plog("SetDIBitsToDevice\tcaller=%08X\tdst=%s\t%d,%d,%lu,%lu\tsrc=%d,%d\tlines=%u+%u"
         "\tbih=%ldx%ld@%u\t-> %d", CALLER, dc_desc(d, a, sizeof(a)), x, y,
         (unsigned long)cx, (unsigned long)cy, sx, sy, start, lines,
         bmi ? (long)bmi->bmiHeader.biWidth : 0, bmi ? (long)bmi->bmiHeader.biHeight : 0,
         bmi ? bmi->bmiHeader.biBitCount : 0, r);
    return r;
}
static HDC (WINAPI *r_CreateDCA)(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA *);
static HDC WINAPI h_CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR port, const DEVMODEA *dm) {
    HDC dc = r_CreateDCA(drv, dev, port, dm);
    plog("CreateDCA\tcaller=%08X\tdriver=%s\tdevice=%s\t-> %08X", CALLER,
         drv ? drv : "(null)", dev ? dev : "(null)", (unsigned)(uintptr_t)dc);
    return dc;
}
static BOOL (WINAPI *r_GetDCOrgEx)(HDC, LPPOINT);
static BOOL WINAPI h_GetDCOrgEx(HDC d, LPPOINT pt) {
    char a[256];
    BOOL r = r_GetDCOrgEx(d, pt);
    plog("GetDCOrgEx\tcaller=%08X\thdc=%s\t-> %d (%ld,%ld)", CALLER,
         dc_desc(d, a, sizeof(a)), r, pt ? (long)pt->x : 0, pt ? (long)pt->y : 0);
    return r;
}
static BOOL (WINAPI *r_MoveWindow)(HWND, int, int, int, int, BOOL);
static BOOL WINAPI h_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL rp) {
    char a[192];
    plog("MoveWindow\tcaller=%08X\thwnd=%s\t%d,%d,%d,%d", CALLER,
         hwnd_desc(hwnd, a, sizeof(a)), x, y, cx, cy);
    return r_MoveWindow(hwnd, x, y, cx, cy, rp);
}
static BOOL (WINAPI *r_GetWindowRect)(HWND, LPRECT);
static BOOL WINAPI h_GetWindowRect(HWND hwnd, LPRECT r) {
    BOOL ok = r_GetWindowRect(hwnd, r);
    plog("GetWindowRect\tcaller=%08X\thwnd=%08X\t-> %ld,%ld,%ld,%ld", CALLER,
         (unsigned)(uintptr_t)hwnd, r ? (long)r->left : 0, r ? (long)r->top : 0,
         r ? (long)r->right : 0, r ? (long)r->bottom : 0);
    return ok;
}
static BOOL (WINAPI *r_ClientToScreen)(HWND, LPPOINT);
static BOOL WINAPI h_ClientToScreen(HWND hwnd, LPPOINT pt) {
    BOOL ok = r_ClientToScreen(hwnd, pt);
    plog("ClientToScreen\tcaller=%08X\thwnd=%08X\t-> %ld,%ld", CALLER,
         (unsigned)(uintptr_t)hwnd, pt ? (long)pt->x : 0, pt ? (long)pt->y : 0);
    return ok;
}
static BOOL (WINAPI *r_GetCursorPos)(LPPOINT);
static BOOL WINAPI h_GetCursorPos(LPPOINT pt) {
    BOOL ok = r_GetCursorPos(pt);
    plog("GetCursorPos\tcaller=%08X", CALLER);
    return ok;
}
static HHOOK (WINAPI *r_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
static HHOOK WINAPI h_SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) {
    HHOOK h = r_SetWindowsHookExA(id, fn, mod, tid);
    plog("SetWindowsHookExA\tcaller=%08X\tid=%d\ttid=%lu\t-> %08X", CALLER, id,
         (unsigned long)tid, (unsigned)(uintptr_t)h);
    return h;
}
static int hook_module(HMODULE module, const char *label);
static HMODULE (WINAPI *r_LoadLibraryA)(LPCSTR);
static HMODULE WINAPI h_LoadLibraryA(LPCSTR name) {
    HMODULE m = r_LoadLibraryA(name);
    plog("LoadLibraryA\tcaller=%08X\tname=%s\t-> %08X", CALLER, name ? name : "(null)",
         (unsigned)(uintptr_t)m);
    /* Only QuickTime components; never patch system DLL import tables. */
    if (m && name && (strstr(name, ".QTC") || strstr(name, ".qtc") ||
                      strstr(name, "DCI") || strstr(name, "dci")))
        hook_module(m, name);
    return m;
}
static FARPROC (WINAPI *r_GetProcAddress)(HMODULE, LPCSTR);
static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = r_GetProcAddress(m, name);
    char mod[MAX_PATH] = "";
    GetModuleFileNameA(m, mod, sizeof(mod));
    if ((uintptr_t)name > 0xFFFF)
        plog("GetProcAddress\tcaller=%08X\tmodule=%s\tname=%s\t-> %08X", CALLER, mod,
             name, (unsigned)(uintptr_t)p);
    return p;
}

typedef struct { const char *name; void *hook; void **real; } HookSpec;
static HookSpec g_specs[] = {
    {"GetDC", (void *)h_GetDC, (void **)&r_GetDC},
    {"GetDCEx", (void *)h_GetDCEx, (void **)&r_GetDCEx},
    {"ReleaseDC", (void *)h_ReleaseDC, (void **)&r_ReleaseDC},
    {"BitBlt", (void *)h_BitBlt, (void **)&r_BitBlt},
    {"PatBlt", (void *)h_PatBlt, (void **)&r_PatBlt},
    {"SetDIBitsToDevice", (void *)h_SetDIBitsToDevice, (void **)&r_SetDIBitsToDevice},
    {"CreateDCA", (void *)h_CreateDCA, (void **)&r_CreateDCA},
    {"GetDCOrgEx", (void *)h_GetDCOrgEx, (void **)&r_GetDCOrgEx},
    {"MoveWindow", (void *)h_MoveWindow, (void **)&r_MoveWindow},
    {"GetWindowRect", (void *)h_GetWindowRect, (void **)&r_GetWindowRect},
    {"ClientToScreen", (void *)h_ClientToScreen, (void **)&r_ClientToScreen},
    {"GetCursorPos", (void *)h_GetCursorPos, (void **)&r_GetCursorPos},
    {"SetWindowsHookExA", (void *)h_SetWindowsHookExA, (void **)&r_SetWindowsHookExA},
    {"LoadLibraryA", (void *)h_LoadLibraryA, (void **)&r_LoadLibraryA},
    {"GetProcAddress", (void *)h_GetProcAddress, (void **)&r_GetProcAddress},
};

static int hook_module(HMODULE module, const char *label) {
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS32 nt;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    IMAGE_DATA_DIRECTORY dir;
    int count = 0;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (imp = (PIMAGE_IMPORT_DESCRIPTOR)(base + dir.VirtualAddress); imp->Name; imp++) {
        DWORD *names, *iat;
        if (!imp->OriginalFirstThunk) continue;
        names = (DWORD *)(base + imp->OriginalFirstThunk);
        iat = (DWORD *)(base + imp->FirstThunk);
        for (; *names; names++, iat++) {
            const char *fn;
            unsigned i;
            if (IMAGE_SNAP_BY_ORDINAL32(*names)) continue;
            fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + *names))->Name;
            for (i = 0; i < sizeof(g_specs) / sizeof(g_specs[0]); i++) {
                DWORD old;
                if (lstrcmpA(fn, g_specs[i].name) != 0) continue;
                if (*iat == (DWORD)(uintptr_t)g_specs[i].hook) break;
                if (!*g_specs[i].real) *g_specs[i].real = (void *)(uintptr_t)*iat;
                if (VirtualProtect(iat, 4, PAGE_READWRITE, &old)) {
                    *iat = (DWORD)(uintptr_t)g_specs[i].hook;
                    VirtualProtect(iat, 4, old, &old);
                    count++;
                }
                break;
            }
        }
    }
    plog("hooked\tmodule=%s\tslots=%d", label, count);
    return count;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)self; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
        g_log = CreateFileA("qtdraw_probe.log", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        g_real = LoadLibraryA("QTIM32S.DLL");
        if (!g_real) return FALSE;
#define R(N, NAME) p_##N = (void *)GetProcAddress(g_real, NAME)
#define O(N) p_##N = (void *)GetProcAddress(g_real, (LPCSTR)(uintptr_t)N)
        R(2, "_EntryPoint"); R(101, "Flip16"); R(102, "Flip16Many"); R(103, "Flip32");
        R(104, "Flip32Many"); R(105, "FreeMemory"); R(107, "GetMemory");
        R(109, "ReallocateMemory"); R(315, "VidWindowHook");
        O(505); O(506); O(507); O(508); O(509); O(510); O(511); O(512); O(513);
        O(514); O(515); O(516); O(517); O(518); O(519); O(520); O(521); O(522);
        O(523); O(524); O(525); O(526); O(527); O(528); O(529);
        hook_module(g_real, "QTIM32S.DLL");
    }
    return TRUE;
}
