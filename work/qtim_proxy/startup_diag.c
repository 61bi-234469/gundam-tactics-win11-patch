/* Startup timing diagnostics.  See startup_diag.h.
 *
 * The hooks only time and forward; they never change arguments or results.
 * During startup (until a few selectors after QuickTime's initialisation)
 * every hooked call is logged; after that only calls of SLOW_MS or more. */
#include "startup_diag.h"

#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { SLOW_MS = 100, MAX_LINES = 400, STARTUP_SELECTORS = 12 };

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static LARGE_INTEGER g_freq, g_t0;
static uintptr_t g_qt_base;
static volatile LONG g_lines;
static volatile LONG g_startup = 1;
static unsigned g_seen_count;
static uint32_t g_seen[STARTUP_SELECTORS];
static uint32_t g_last_sel = 0xFFFFFFFFu;
static double g_last_sel_ms;

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 /
           (double)g_freq.QuadPart;
}

static void diag_log(const char *fmt, ...) {
    char buf[320];
    va_list ap;
    int n;
    DWORD written;
    if (g_file == INVALID_HANDLE_VALUE) return;
    if (InterlockedIncrement(&g_lines) > MAX_LINES) return;
    n = snprintf(buf, sizeof(buf), "%10.1f\t%lu\t", now_ms(),
                 (unsigned long)GetCurrentThreadId());
    if (n < 0 || n >= (int)sizeof(buf) - 2) return;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    n = (int)strlen(buf);
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_lock);
    WriteFile(g_file, buf, (DWORD)n, &written, NULL);
    LeaveCriticalSection(&g_lock);
}

static void log_call(const char *name, double start, uintptr_t ra,
                     unsigned long result) {
    double ms = now_ms() - start;
    if (!g_startup && ms < SLOW_MS) return;
    diag_log("call\t%s\tms=%.1f\tret=%lu\tcaller=QT+%05X", name, ms, result,
             (unsigned)(ra - g_qt_base));
}

/* ---- timed wrappers (stdcall, same signatures as the imports) ---- */

typedef MMRESULT (WINAPI *PFN_midiOutOpen)(LPHMIDIOUT, UINT, DWORD_PTR,
                                          DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *PFN_midiOutClose)(HMIDIOUT);
typedef MMRESULT (WINAPI *PFN_midiOutGetDevCapsA)(UINT_PTR, LPMIDIOUTCAPSA,
                                                 UINT);
typedef UINT (WINAPI *PFN_waveOutGetNumDevs)(void);
typedef MMRESULT (WINAPI *PFN_waveOutGetDevCapsA)(UINT_PTR, LPWAVEOUTCAPSA,
                                                 UINT);
typedef MMRESULT (WINAPI *PFN_waveOutOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX,
                                          DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *PFN_waveOutClose)(HWAVEOUT);
typedef HDC (WINAPI *PFN_CreateDCA)(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA *);
typedef DWORD (WINAPI *PFN_GetProfileStringA)(LPCSTR, LPCSTR, LPCSTR, LPSTR,
                                             DWORD);
typedef HMODULE (WINAPI *PFN_LoadLibraryA)(LPCSTR);

static PFN_midiOutOpen r_midiOutOpen;
static PFN_midiOutClose r_midiOutClose;
static PFN_midiOutGetDevCapsA r_midiOutGetDevCapsA;
static PFN_waveOutGetNumDevs r_waveOutGetNumDevs;
static PFN_waveOutGetDevCapsA r_waveOutGetDevCapsA;
static PFN_waveOutOpen r_waveOutOpen;
static PFN_waveOutClose r_waveOutClose;
static PFN_CreateDCA r_CreateDCA;
static PFN_CreateDCA r_CreateICA;
static PFN_GetProfileStringA r_GetProfileStringA;
static PFN_LoadLibraryA r_LoadLibraryA;

#define RA() ((uintptr_t)__builtin_return_address(0))

static MMRESULT WINAPI d_midiOutOpen(LPHMIDIOUT h, UINT id, DWORD_PTR cb,
                                     DWORD_PTR inst, DWORD flags) {
    double t = now_ms();
    MMRESULT r = r_midiOutOpen(h, id, cb, inst, flags);
    log_call("midiOutOpen", t, RA(), r);
    return r;
}

static MMRESULT WINAPI d_midiOutClose(HMIDIOUT h) {
    double t = now_ms();
    MMRESULT r = r_midiOutClose(h);
    log_call("midiOutClose", t, RA(), r);
    return r;
}

static MMRESULT WINAPI d_midiOutGetDevCapsA(UINT_PTR id, LPMIDIOUTCAPSA caps,
                                            UINT cb) {
    double t = now_ms();
    MMRESULT r = r_midiOutGetDevCapsA(id, caps, cb);
    log_call("midiOutGetDevCapsA", t, RA(), r);
    return r;
}

static UINT WINAPI d_waveOutGetNumDevs(void) {
    double t = now_ms();
    UINT r = r_waveOutGetNumDevs();
    log_call("waveOutGetNumDevs", t, RA(), r);
    return r;
}

static MMRESULT WINAPI d_waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA caps,
                                            UINT cb) {
    double t = now_ms();
    MMRESULT r = r_waveOutGetDevCapsA(id, caps, cb);
    log_call("waveOutGetDevCapsA", t, RA(), r);
    return r;
}

static MMRESULT WINAPI d_waveOutOpen(LPHWAVEOUT h, UINT id,
                                     LPCWAVEFORMATEX fmt, DWORD_PTR cb,
                                     DWORD_PTR inst, DWORD flags) {
    double t = now_ms();
    MMRESULT r = r_waveOutOpen(h, id, fmt, cb, inst, flags);
    log_call("waveOutOpen", t, RA(), r);
    return r;
}

static MMRESULT WINAPI d_waveOutClose(HWAVEOUT h) {
    double t = now_ms();
    MMRESULT r = r_waveOutClose(h);
    log_call("waveOutClose", t, RA(), r);
    return r;
}

static HDC WINAPI d_CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR out,
                              const DEVMODEA *dm) {
    double t = now_ms();
    HDC r = r_CreateDCA(drv, dev, out, dm);
    log_call("CreateDCA", t, RA(), r != NULL);
    return r;
}

static HDC WINAPI d_CreateICA(LPCSTR drv, LPCSTR dev, LPCSTR out,
                              const DEVMODEA *dm) {
    double t = now_ms();
    HDC r = r_CreateICA(drv, dev, out, dm);
    log_call("CreateICA", t, RA(), r != NULL);
    return r;
}

static DWORD WINAPI d_GetProfileStringA(LPCSTR app, LPCSTR key, LPCSTR def,
                                        LPSTR out, DWORD cap) {
    double t = now_ms();
    DWORD r = r_GetProfileStringA(app, key, def, out, cap);
    log_call("GetProfileStringA", t, RA(), r);
    return r;
}

static HMODULE WINAPI d_LoadLibraryA(LPCSTR name) {
    double t = now_ms();
    HMODULE r = r_LoadLibraryA(name);
    log_call("LoadLibraryA", t, RA(), r != NULL);
    return r;
}

/* ---- IAT patching of the stock QuickTime DLL ---- */

typedef struct DiagHook {
    const char *dll;
    const char *name;
    void *hook;
    void **real;
} DiagHook;

static const DiagHook k_hooks[] = {
    {"winmm.dll", "midiOutOpen", (void *)(uintptr_t)d_midiOutOpen,
     (void **)&r_midiOutOpen},
    {"winmm.dll", "midiOutClose", (void *)(uintptr_t)d_midiOutClose,
     (void **)&r_midiOutClose},
    {"winmm.dll", "midiOutGetDevCapsA", (void *)(uintptr_t)d_midiOutGetDevCapsA,
     (void **)&r_midiOutGetDevCapsA},
    {"winmm.dll", "waveOutGetNumDevs", (void *)(uintptr_t)d_waveOutGetNumDevs,
     (void **)&r_waveOutGetNumDevs},
    {"winmm.dll", "waveOutGetDevCapsA", (void *)(uintptr_t)d_waveOutGetDevCapsA,
     (void **)&r_waveOutGetDevCapsA},
    {"winmm.dll", "waveOutOpen", (void *)(uintptr_t)d_waveOutOpen,
     (void **)&r_waveOutOpen},
    {"winmm.dll", "waveOutClose", (void *)(uintptr_t)d_waveOutClose,
     (void **)&r_waveOutClose},
    {"gdi32.dll", "CreateDCA", (void *)(uintptr_t)d_CreateDCA,
     (void **)&r_CreateDCA},
    {"gdi32.dll", "CreateICA", (void *)(uintptr_t)d_CreateICA,
     (void **)&r_CreateICA},
    {"kernel32.dll", "GetProfileStringA", (void *)(uintptr_t)d_GetProfileStringA,
     (void **)&r_GetProfileStringA},
    {"kernel32.dll", "LoadLibraryA", (void *)(uintptr_t)d_LoadLibraryA,
     (void **)&r_LoadLibraryA},
};
#define HOOK_COUNT (sizeof(k_hooks) / sizeof(k_hooks[0]))

static DWORD *g_slots[HOOK_COUNT];

static DWORD *find_slot(HMODULE module, const char *dll, const char *name) {
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    PIMAGE_NT_HEADERS32 nt;
    IMAGE_DATA_DIRECTORY dir;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
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

static int setting_on(const char *ini_path) {
    char value[8];
    DWORD n = GetEnvironmentVariableA("QTIM_DIAG_STARTUP", value,
                                      sizeof(value));
    if (n && n < sizeof(value)) return value[0] == '1';
    return ini_path &&
           GetPrivateProfileIntA("diag", "startup", 0, ini_path) != 0;
}

void gt_diag_attach(const char *ini_path, HMODULE qt_module) {
    char log_path[MAX_PATH];
    char *sep;
    SYSTEMTIME st;
    unsigned i, hooked = 0;
    if (!ini_path || !setting_on(ini_path)) return;
    if (lstrlenA(ini_path) >= MAX_PATH) return;
    lstrcpyA(log_path, ini_path);
    sep = strrchr(log_path, '\\');
    if (!sep || (size_t)(sep + 1 - log_path) + sizeof("qtim_startup.log") >
                    sizeof(log_path))
        return;
    lstrcpyA(sep + 1, "qtim_startup.log");
    g_file = CreateFileA(log_path, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) return;
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_qt_base = (uintptr_t)qt_module;
    for (i = 0; i < HOOK_COUNT; i++) {
        DWORD *slot = find_slot(qt_module, k_hooks[i].dll, k_hooks[i].name);
        if (!slot || !*slot) continue;
        *k_hooks[i].real = (void *)(uintptr_t)*slot;
        if (!write_slot(slot, (DWORD)(uintptr_t)k_hooks[i].hook)) continue;
        g_slots[i] = slot;
        hooked++;
    }
    GetLocalTime(&st);
    diag_log("attach\t%04u-%02u-%02u %02u:%02u:%02u\tpid=%lu\thooks=%u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)GetCurrentProcessId(), hooked);
}

void gt_diag_detach(int process_exit) {
    unsigned i;
    if (g_file == INVALID_HANDLE_VALUE) return;
    for (i = 0; i < HOOK_COUNT; i++) {
        if (g_slots[i] && *g_slots[i] == (DWORD)(uintptr_t)k_hooks[i].hook)
            write_slot(g_slots[i], (DWORD)(uintptr_t)*k_hooks[i].real);
        g_slots[i] = NULL;
    }
    /* At process exit another (terminated) thread may own g_lock, and
     * waiting on it there ends the process early: skip the log line and
     * leave the handle and lock to the OS. */
    if (process_exit) return;
    diag_log("detach");
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    DeleteCriticalSection(&g_lock);
}

/* Logs the first call of each of the first STARTUP_SELECTORS distinct
 * selectors and how long the previous one took to hand over to the next
 * (for 0x39 that is the whole QuickTime initialisation). */
void gt_diag_selector(uint32_t sel) {
    unsigned i;
    double t;
    if (g_file == INVALID_HANDLE_VALUE || !g_startup) return;
    EnterCriticalSection(&g_lock);
    if (!g_startup || sel == g_last_sel) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    t = now_ms();
    for (i = 0; i < g_seen_count; i++)
        if (g_seen[i] == sel) break;
    if (i == g_seen_count) {
        if (g_last_sel != 0xFFFFFFFFu)
            diag_log("selector\t0x%02X\tfirst\tprev=0x%02X\tprev_to_this_ms=%.1f",
                     (unsigned)sel, (unsigned)g_last_sel, t - g_last_sel_ms);
        else
            diag_log("selector\t0x%02X\tfirst", (unsigned)sel);
        g_seen[g_seen_count++] = sel;
        if (g_seen_count == STARTUP_SELECTORS) {
            InterlockedExchange(&g_startup, 0);
            diag_log("startup_done");
        }
    }
    g_last_sel = sel;
    g_last_sel_ms = t;
    LeaveCriticalSection(&g_lock);
}

void gt_diag_note(const char *text) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    diag_log("%s", text);
}
