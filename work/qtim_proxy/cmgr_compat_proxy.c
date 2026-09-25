/* CMGR32 companion proxy.
 *
 * CMGR32 owns the second QuickTime dispatcher used by GetMovieTime (0x06).
 * All selectors are forwarded to CMGR32R.DLL except calls bound to a fake
 * controller/movie handle. The QTIM proxy publishes the complete fake-handle
 * lifetime in a generation-numbered shared journal, so CMGR does not depend
 * on observing a selector while a controller is active.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "controller_shared.h"

static HMODULE g_real = NULL;
FARPROC g_real_entry = NULL;
FARPROC g_real_initialize = NULL;
FARPROC g_real_terminate = NULL;
FARPROC g_real_terminate_task = NULL;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_shared_mapping = NULL;
static GtControllerSharedState *g_shared = NULL;
static int g_selftest_requested = 0;
static int g_selftest_done = 0;
static int g_selftest_running = 0;

typedef struct SeenController {
    uint32_t controller_handle;
    uint32_t movie_handle;
    uint32_t generation;
    int retired;
} SeenController;

enum { SEEN_CONTROLLER_CAPACITY = 32768 };
static SeenController g_seen_controllers[SEEN_CONTROLLER_CAPACITY];
static int g_seen_controller_count = 0;
static uint32_t g_last_journal_generation = 0;
static int g_journal_desynced = 0;
static CRITICAL_SECTION g_cmgr_lock;
static int g_cmgr_lock_ready = 0;

static int build_trace_ini_path(HMODULE self, char *path, size_t path_size) {
    char module_path[MAX_PATH];
    char current_dir[MAX_PATH];
    char *separator;
    DWORD length;
    int written;

    if (!path || path_size == 0) return 0;
    length = GetModuleFileNameA(self, module_path, sizeof(module_path));
    if (length > 0 && length < sizeof(module_path)) {
        module_path[length] = '\0';
        separator = strrchr(module_path, '\\');
        if (!separator) separator = strrchr(module_path, '/');
        if (separator) {
            *separator = '\0';
            written = snprintf(path, path_size, "%s\\qtim_compat.ini", module_path);
            return written >= 0 && (size_t)written < path_size;
        }
    }

    length = GetCurrentDirectoryA(sizeof(current_dir), current_dir);
    if (length == 0 || length >= sizeof(current_dir)) return 0;
    written = snprintf(path, path_size, "%s\\qtim_compat.ini", current_dir);
    return written >= 0 && (size_t)written < path_size;
}

static int trace_requested(HMODULE self) {
    char value[16];
    char ini_path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("QTIM_COMPAT_TRACE", value,
                                          sizeof(value));
    if (length && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                   value[0] == 't' || value[0] == 'T')) return 1;
    if (!build_trace_ini_path(self, ini_path, sizeof(ini_path))) return 0;
    return GetPrivateProfileIntA("trace", "enabled", 0, ini_path) != 0;
}

static void log_text(const char *text) {
    DWORD written;
    if (g_log == INVALID_HANDLE_VALUE) return;
    WriteFile(g_log, text, (DWORD)strlen(text), &written, NULL);
}

static void log_dispatch(uint32_t sel, uint32_t arg0, uint32_t arg1,
                         uint32_t arg2, uint32_t arg3, uint32_t ret) {
    char buf[320];
    int written;
    if (g_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\tcmgr_selector\tsel=0x%04X\targ0=%08X\t"
                       "arg1=%08X\targ2=%08X\targ3=%08X\tret=%08X\n",
                       (unsigned long)GetTickCount(), (unsigned)sel,
                       (unsigned)arg0, (unsigned)arg1, (unsigned)arg2,
                       (unsigned)arg3, (unsigned)ret);
    if (written > 0) log_text(buf);
}

static void log_controller_unhandled(uint32_t sel, uint32_t arg0,
                                     uint32_t arg1, uint32_t arg2,
                                     uint32_t arg3, uint32_t controller_handle,
                                     uint32_t movie_handle) {
    char buf[320];
    int written;
    if (g_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\tcontroller_unhandled\tsel=0x%04X\t"
                       "arg0=%08X\targ1=%08X\targ2=%08X\targ3=%08X\t"
                       "controller=%08X\tmovie=%08X\n",
                       (unsigned long)GetTickCount(), (unsigned)sel,
                       (unsigned)arg0, (unsigned)arg1, (unsigned)arg2,
                       (unsigned)arg3, (unsigned)controller_handle,
                       (unsigned)movie_handle);
    if (written > 0) log_text(buf);
}

static int fake_controller_format(uint32_t value) {
    return gt_controller_fake_handle_format(value);
}

static void clear_movie_bindings_locked(uint32_t movie_handle) {
    int i;
    if (!movie_handle) return;
    for (i = 0; i < g_seen_controller_count; i++) {
        if (g_seen_controllers[i].movie_handle == movie_handle)
            g_seen_controllers[i].movie_handle = 0;
    }
}

static SeenController *find_seen_controller_locked(uint32_t controller_handle) {
    int i;
    for (i = g_seen_controller_count - 1; i >= 0; i--) {
        if (g_seen_controllers[i].controller_handle == controller_handle)
            return &g_seen_controllers[i];
    }
    return NULL;
}

static void apply_journal_event_locked(const GtControllerJournalEntry *entry) {
    SeenController *seen;
    if (!entry || !entry->generation) return;
    switch (entry->event_type) {
    case GT_CONTROLLER_EVENT_MOVIE_BIND:
        clear_movie_bindings_locked(entry->movie_handle);
        break;
    case GT_CONTROLLER_EVENT_MOVIE_UNBIND:
        clear_movie_bindings_locked(entry->movie_handle);
        break;
    case GT_CONTROLLER_EVENT_CONTROLLER_CREATE:
        clear_movie_bindings_locked(entry->movie_handle);
        seen = find_seen_controller_locked(entry->controller_handle);
        if (!seen) {
            if (g_seen_controller_count >= SEEN_CONTROLLER_CAPACITY) {
                g_journal_desynced = 1;
                break;
            }
            seen = &g_seen_controllers[g_seen_controller_count++];
        }
        seen->controller_handle = entry->controller_handle;
        seen->movie_handle = entry->movie_handle;
        seen->generation = entry->generation;
        seen->retired = 0;
        break;
    case GT_CONTROLLER_EVENT_CONTROLLER_DISPOSE:
        seen = find_seen_controller_locked(entry->controller_handle);
        if (!seen) {
            if (g_seen_controller_count >= SEEN_CONTROLLER_CAPACITY) {
                g_journal_desynced = 1;
                break;
            }
            seen = &g_seen_controllers[g_seen_controller_count++];
            memset(seen, 0, sizeof(*seen));
            seen->controller_handle = entry->controller_handle;
        }
        if (!seen->movie_handle) seen->movie_handle = entry->movie_handle;
        seen->generation = entry->generation;
        seen->retired = 1;
        break;
    default:
        break;
    }
}

static void sync_shared_journal_locked(void) {
    GtControllerSharedSnapshot snapshot;
    uint32_t delta;
    uint32_t offset;
    GtControllerJournalEntry entry;

    if (!g_shared || !gt_controller_shared_read_snapshot(g_shared, &snapshot))
        return;
    delta = snapshot.journal_generation - g_last_journal_generation;
    if (delta == 0) return;
    if (delta > GT_CONTROLLER_JOURNAL_CAPACITY) {
        /* The opaque fake-controller format remains a second safety net. We
         * cannot reconstruct retired movie bindings after a ring overrun, so
         * clear the stale table and keep absorbing unmistakable fake handles. */
        memset(g_seen_controllers, 0, sizeof(g_seen_controllers));
        g_seen_controller_count = 0;
        g_journal_desynced = 1;
        g_last_journal_generation = snapshot.journal_generation;
        return;
    }
    for (offset = 1; offset <= delta; offset++) {
        uint32_t generation = g_last_journal_generation + offset;
        if (!gt_controller_shared_read_event(g_shared, generation, &entry)) {
            g_journal_desynced = 1;
            g_last_journal_generation = snapshot.journal_generation;
            return;
        }
        apply_journal_event_locked(&entry);
    }
    g_last_journal_generation = snapshot.journal_generation;
}

static int value_in_args(uint32_t value, uint32_t arg0, uint32_t arg1,
                         uint32_t arg2, uint32_t arg3) {
    return value != 0 && (value == arg0 || value == arg1 || value == arg2 ||
                          value == arg3);
}

static int seen_controller_in_args_locked(uint32_t arg0, uint32_t arg1,
                                          uint32_t arg2, uint32_t arg3,
                                          uint32_t *controller_handle,
                                          uint32_t *movie_handle) {
    int i;
    SeenController *seen;
    for (i = g_seen_controller_count - 1; i >= 0; i--) {
        seen = &g_seen_controllers[i];
        if (value_in_args(seen->controller_handle, arg0, arg1, arg2, arg3)) {
            if (controller_handle) *controller_handle = seen->controller_handle;
            if (movie_handle) *movie_handle = seen->movie_handle;
            return 1;
        }
    }
    /* Keep the guard effective for live or retired fake handles even if the
     * journal was overrun. Never use a movie handle as a fallback key. */
    {
        uint32_t args[4] = {arg0, arg1, arg2, arg3};
        for (i = 0; i < 4; i++) {
            if (fake_controller_format(args[i])) {
                if (controller_handle) *controller_handle = args[i];
                if (movie_handle) *movie_handle = 0;
                return 1;
            }
        }
    }
    return 0;
}

static int run_controller_reuse_selftest(void) {
    const uint32_t movie_handle = 0x03C84C08u;
    const uint32_t controller_handle = 0x80C6FFFFu;
    uint32_t controller = 0;
    uint32_t movie = 0;
    int movie_match;
    int controller_match;

    EnterCriticalSection(&g_cmgr_lock);
    memset(g_seen_controllers, 0, sizeof(g_seen_controllers));
    g_seen_controller_count = 1;
    g_journal_desynced = 0;
    g_seen_controllers[0].controller_handle = controller_handle;
    g_seen_controllers[0].movie_handle = movie_handle;
    g_seen_controllers[0].retired = 1;
    movie_match = seen_controller_in_args_locked(movie_handle, 0, 0, 0x40,
                                                  &controller, &movie);
    controller_match = seen_controller_in_args_locked(controller_handle, 0, 0,
                                                       0x40, &controller,
                                                       &movie);
    memset(g_seen_controllers, 0, sizeof(g_seen_controllers));
    g_seen_controller_count = 0;
    LeaveCriticalSection(&g_cmgr_lock);
    return !movie_match && controller_match;
}

static void maybe_run_controller_reuse_selftest(void) {
    int passed;
    if (!g_selftest_requested || g_selftest_done || g_selftest_running) return;
    g_selftest_done = 1;
    g_selftest_running = 1;
    passed = run_controller_reuse_selftest();
    g_selftest_running = 0;
    if (g_log != INVALID_HANDLE_VALUE)
        log_text(passed ? "controller_reuse_selftest=PASS\n"
                        : "controller_reuse_selftest=FAIL\n");
}

static uint32_t shared_movie_time(uint32_t movie_handle) {
    GtControllerSharedSnapshot snapshot;
    uint32_t elapsed;
    uint64_t time;
    if (!g_shared || !gt_controller_shared_read_snapshot(g_shared, &snapshot) ||
        snapshot.active == 0 || !snapshot.timescale ||
        (movie_handle != snapshot.movie_handle &&
         movie_handle != snapshot.controller_handle)) return 0;
    elapsed = GetTickCount() - snapshot.start_tick;
    time = (uint64_t)elapsed * snapshot.timescale / 1000u;
    if (time >= snapshot.duration) return snapshot.duration;
    return (uint32_t)time;
}

static uint32_t handled_result(uint32_t *f, uint32_t result) {
    f[7] = result;
    return 1;
}

/* Same frame layout as qtim_compat_proxy.c. */
uint32_t __cdecl compat_dispatch(uint32_t *f) {
    uint32_t sel = f[4] & 0xFFFFu;
    uint32_t arg0 = f[12];
    uint32_t arg1 = f[13];
    uint32_t arg2 = f[14];
    uint32_t arg3 = f[15];
    uint32_t controller_handle = 0;
    uint32_t movie_handle = 0;
    uint32_t time = 0;
    int bound;
    int active_match = 0;

    maybe_run_controller_reuse_selftest();

    EnterCriticalSection(&g_cmgr_lock);
    sync_shared_journal_locked();
    bound = seen_controller_in_args_locked(arg0, arg1, arg2, arg3,
                                           &controller_handle, &movie_handle);
    if (sel == 0x06 && bound) {
        GtControllerSharedSnapshot snapshot;
        if (gt_controller_shared_read_snapshot(g_shared, &snapshot) &&
            snapshot.active != 0 &&
            snapshot.controller_handle == controller_handle) active_match = 1;
    }
    LeaveCriticalSection(&g_cmgr_lock);

    if (sel == 0x06 && bound && active_match) {
        time = shared_movie_time(movie_handle ? movie_handle : controller_handle);
        log_dispatch(sel, arg0, arg1, arg2, arg3, time);
        return handled_result(f, time);
    }
    if (bound) {
        log_controller_unhandled(sel, arg0, arg1, arg2, arg3,
                                 controller_handle, movie_handle);
        return handled_result(f, 0);
    }
    return 0;
}

__attribute__((naked)) void proxy_EntryPoint(void) {
    __asm__ volatile (
        "pushfl                         \n\t"
        "pushal                         \n\t"
        "leal  (%%esp), %%eax           \n\t"
        "pushl %%eax                    \n\t"
        "call  _compat_dispatch         \n\t"
        "addl  $4, %%esp                \n\t"
        "testl %%eax, %%eax             \n\t"
        "jz    1f                       \n\t"
        "popal                          \n\t"
        "popfl                          \n\t"
        "ret                            \n\t"
        "1:                             \n\t"
        "popal                          \n\t"
        "popfl                          \n\t"
        "jmp   *_g_real_entry           \n\t"
        ::: "memory");
}

__attribute__((naked)) void proxy_CMgrInitialize(void) {
    __asm__ volatile ("jmp *_g_real_initialize\n\t" ::: "memory");
}

__attribute__((naked)) void proxy_CMgrTerminate(void) {
    __asm__ volatile ("jmp *_g_real_terminate\n\t" ::: "memory");
}

__attribute__((naked)) void proxy_CMgrTerminateTask(void) {
    __asm__ volatile ("jmp *_g_real_terminate_task\n\t" ::: "memory");
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_cmgr_lock);
        g_cmgr_lock_ready = 1;
        g_selftest_requested = GetEnvironmentVariableA(
            "QTIM_COMPAT_SELFTEST", NULL, 0) != 0;
        g_real = LoadLibraryA("CMGR32R.DLL");
        if (!g_real) return FALSE;
        g_real_entry = GetProcAddress(g_real, "_EntryPoint");
        g_real_initialize = GetProcAddress(g_real, "_CMgrInitialize");
        g_real_terminate = GetProcAddress(g_real, "_CMgrTerminate");
        g_real_terminate_task = GetProcAddress(g_real, "_CMgrTerminateTask");
        if (!g_real_entry || !g_real_initialize || !g_real_terminate ||
            !g_real_terminate_task) return FALSE;
        g_shared = gt_controller_shared_open(&g_shared_mapping);
        if (trace_requested(self)) {
            g_log = CreateFileA("cmgr_compat_trace.log", GENERIC_WRITE,
                                FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
        }
        log_text("cmgr_compat_proxy attached\n");
    } else if (reason == DLL_PROCESS_DETACH) {
        gt_controller_shared_close(&g_shared_mapping, &g_shared);
        if (g_log != INVALID_HANDLE_VALUE) {
            log_text("cmgr_compat_proxy detached\n");
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
        if (g_real) {
            FreeLibrary(g_real);
            g_real = NULL;
        }
        if (g_cmgr_lock_ready) {
            DeleteCriticalSection(&g_cmgr_lock);
            g_cmgr_lock_ready = 0;
        }
    }
    return TRUE;
}
