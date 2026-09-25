/* QTIM32 compatibility proxy proof of concept.
 *
 * This proxy forwards the real QuickTime 2.x exports to QTIM32R.DLL, but
 * intercepts the primary dispatcher selectors needed for a fixed-DIB movie
 * frame proof:
 *
 *   BX=0x14 GetMoviePict         -> return a fake PicHandle sentinel
 *   BX=0x2E PicHandle to DIB     -> if sentinel, return a generated DIB
 *   BX=0x08 KillPicture          -> if sentinel, no-op
 *
 * The point of this proof is not final playback.  It verifies that the game
 * can accept a proxy-generated DIB through the original GrabFrame -> DrawDIB
 * path without battle-loop patches or GDI IAT hooks.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller_shared.h"
#include "display_scale.h"
#include "movdec.h"

static HMODULE g_real = NULL;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_gdi_log = INVALID_HANDLE_VALUE;
static int g_trace_enabled = 0;
static int g_gdi_trace_enabled = 0;
static int g_mci_trace_enabled = 0;
static int g_midi_loop_fix_enabled = 0;
static int g_input_fix_enabled = 0;
enum {
    GDI_FIX_OFF = 0,
    GDI_FIX_BMI_NEGATIVE = 1,
    GDI_FIX_FLIP_ROWS = 2
};
enum {
    GDI_FIX_CALLER_407392 = 1u << 0,
    GDI_FIX_CALLER_409289 = 1u << 1,
    GDI_FIX_CALLERS_DEFAULT = GDI_FIX_CALLER_407392 | GDI_FIX_CALLER_409289
};
static int g_gdi_fix_mode = GDI_FIX_OFF;
static unsigned g_gdi_fix_callers = GDI_FIX_CALLERS_DEFAULT;
static CRITICAL_SECTION g_state_lock;
static CRITICAL_SECTION g_log_lock;
static int g_locks_ready = 0;
static DWORD g_tls_index = TLS_OUT_OF_INDEXES;
static volatile LONG g_gdi_logging = 0;
static volatile LONG g_message_pump_logged = 0;

enum { CACHE_PATH_MAX = 1024 };

typedef enum MovieAudioMode {
    AUDIO_MODE_UNSET = 0,
    AUDIO_MODE_REALTIME = 1,
    AUDIO_MODE_PICTURE = 2
} MovieAudioMode;

typedef struct MovieMap {
    uint32_t handle;
    char rel_path[128];
    MovDecoder *decoder;
    MovieAudioMode audio_mode;
    uint32_t last_picture_time;
    HWAVEOUT audio_out;
    WAVEHDR audio_header;
    BYTE *audio_buffer;
    DWORD audio_bytes;
    int audio_prepared;
    int audio_streaming;
    uint32_t audio_cursor;
    uint32_t audio_last_movie_time;
    struct AudioChunk *audio_chunks;
} MovieMap;

typedef struct AudioChunk {
    WAVEHDR header;
    BYTE *data;
    int prepared;
    struct AudioChunk *next;
} AudioChunk;

/* Ownership is detached while g_state_lock is held, but all WinMM calls are
 * performed after the lock is released. A failed unprepare/close keeps this
 * object (and every driver-visible header/data buffer) on the retry queue. */
typedef struct AudioTeardown {
    HWAVEOUT audio_out;
    WAVEHDR audio_header;
    BYTE *audio_buffer;
    DWORD audio_bytes;
    int audio_prepared;
    AudioChunk *audio_chunks;
    struct AudioTeardown *next;
} AudioTeardown;

typedef struct ControllerMap {
    uint32_t handle;
    uint32_t movie_handle;
    DWORD start_tick;
    uint32_t last_time;
    uint32_t last_frame;
    HGLOBAL last_dib;
    HWND target_hwnd;
    HDC target_hdc;
    LONG dst_x;
    LONG dst_y;
    LONG dst_cx;
    LONG dst_cy;
    int target_valid;
    int done_logged;
    DWORD result_log_tick[2];
    uint32_t result_log_frame[2];
    int result_log_initialized[2];
} ControllerMap;

typedef struct ControllerTombstone {
    uint32_t controller_handle;
    uint32_t movie_handle;
} ControllerTombstone;

typedef struct MovieFileBinding {
    uint32_t file_handle;
    uint32_t movie_handle;
} MovieFileBinding;

typedef struct FakePicture {
    uint32_t magic;
    uint32_t handle;
    int32_t movie_time;
} FakePicture;

typedef struct CompatThreadState {
    char pending_movie_path[128];
    uint32_t pending_file_handle;
    uint32_t pending_out_slot;
    int pending_close_seen;
    DWORD message_pump_last_tick;
    int message_pump_initialized;
} CompatThreadState;

#define FAKE_PICTURE_MAGIC ((uint32_t)0x47545031u) /* "GTP1" */

static CompatThreadState *get_thread_state(void) {
    CompatThreadState *state;
    if (g_tls_index == TLS_OUT_OF_INDEXES) return NULL;
    state = (CompatThreadState *)TlsGetValue(g_tls_index);
    if (!state) {
        state = (CompatThreadState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               sizeof(*state));
        if (!state || !TlsSetValue(g_tls_index, state)) {
            if (state) HeapFree(GetProcessHeap(), 0, state);
            return NULL;
        }
    }
    return state;
}

static void free_thread_state(void) {
    CompatThreadState *state;
    if (g_tls_index == TLS_OUT_OF_INDEXES) return;
    state = (CompatThreadState *)TlsGetValue(g_tls_index);
    if (state) {
        HeapFree(GetProcessHeap(), 0, state);
        TlsSetValue(g_tls_index, NULL);
    }
}

typedef struct FrameCache {
    char rel_path[128];
    unsigned frame_count;
} FrameCache;

typedef struct EventLogState {
    char name[32];
    uint32_t sel;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t ret;
    int initialized;
} EventLogState;

typedef struct CacheMissLog {
    char rel_path[128];
    char reason[32];
} CacheMissLog;

static MovieMap g_movies[32];
static int g_movie_count = 0;
static ControllerMap g_controllers[32];
static int g_controller_count = 0;
/* Real QuickTime controller handles observed in the Openmovi/PEGS traces are
 * opaque nonzero values in the 0x808xxxxx family. Component Manager uses the
 * low 16 bits as a table index, so 0xFFFF makes this value fail the original
 * index<count lookup while remaining an opaque nonzero value to the game. */
#define FAKE_CONTROLLER_BASE GT_CONTROLLER_FAKE_HANDLE_BASE
static uint32_t g_next_fake_controller = FAKE_CONTROLLER_BASE;
/* One slot per possible 0x80880000..0xffff0000/0x10000 allocation. */
enum { CONTROLLER_TOMBSTONE_CAPACITY = 32768 };
static ControllerTombstone g_controller_tombstones[
    CONTROLLER_TOMBSTONE_CAPACITY];
static int g_controller_tombstone_count = 0;
static MovieFileBinding g_movie_file_bindings[32];
static int g_movie_file_binding_count = 0;
static HANDLE g_shared_mapping = NULL;
static GtControllerSharedState *g_shared = NULL;
static AudioTeardown *g_audio_teardown_queue = NULL;
static FrameCache g_frame_caches[32];
static int g_frame_cache_count = 0;
static BYTE g_seen_selectors[65536];
static EventLogState g_event_log_states[8];
static CacheMissLog g_cache_miss_logs[64];
static int g_cache_miss_log_count = 0;
static int g_selftest_requested = 0;
static int g_selftest_done = 0;
static int g_selftest_running = 0;
static int g_mci_selftest_done = 0;

static MovieMap *find_movie(uint32_t handle);
static ControllerMap *find_controller_for_movie(uint32_t movie_handle);
static HGLOBAL make_decoded_dib(MovieMap *movie, int32_t movie_time);
static void clear_movie_tombstones_locked(uint32_t movie_handle);
static void clear_movie_file_bindings_locked(uint32_t movie_handle);
static void map_movie_handle_locked(uint32_t handle, const char *path);
static int run_controller_reuse_selftest(void);
uint32_t __cdecl compat_dispatch(uint32_t *f);

FARPROC p_EntryPoint;
FARPROC p_Flip16;
FARPROC p_Flip16Many;
FARPROC p_Flip32;
FARPROC p_Flip32Many;
FARPROC p_FreeMemory;
FARPROC p_GetMemory;
FARPROC p_ReallocateMemory;
FARPROC p_VidWindowHook;

FARPROC p_ord_505, p_ord_506, p_ord_507, p_ord_508, p_ord_509;
FARPROC p_ord_510, p_ord_511, p_ord_512, p_ord_513, p_ord_514;
FARPROC p_ord_515, p_ord_516, p_ord_517, p_ord_518, p_ord_519;
FARPROC p_ord_520, p_ord_521, p_ord_522, p_ord_523, p_ord_524;
FARPROC p_ord_525, p_ord_526, p_ord_527, p_ord_528, p_ord_529;

typedef int (WINAPI *PFN_StretchDIBits)(HDC, int, int, int, int, int, int,
                                        int, int, const VOID *, const BITMAPINFO *,
                                        UINT, DWORD);
typedef BOOL (WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int,
                                      int, int, DWORD);
typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef HBITMAP (WINAPI *PFN_CreateDIBSection)(HDC, const BITMAPINFO *, UINT,
                                               VOID **, HANDLE, DWORD);
typedef HBITMAP (WINAPI *PFN_CreateCompatibleBitmap)(HDC, int, int);
typedef HGDIOBJ (WINAPI *PFN_SelectObject)(HDC, HGDIOBJ);
typedef int (WINAPI *PFN_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int,
                                            UINT, UINT, const VOID *,
                                            const BITMAPINFO *, UINT);
typedef int (WINAPI *PFN_GetObjectA)(HGDIOBJ, int, LPVOID);
typedef MCIERROR (WINAPI *PFN_mciSendCommandA)(MCIDEVICEID, UINT, DWORD_PTR,
                                                DWORD_PTR);

static PFN_StretchDIBits p_real_StretchDIBits = NULL;
static PFN_StretchBlt p_real_StretchBlt = NULL;
static PFN_BitBlt p_real_BitBlt = NULL;
static PFN_CreateDIBSection p_real_CreateDIBSection = NULL;
static PFN_CreateCompatibleBitmap p_real_CreateCompatibleBitmap = NULL;
static PFN_SelectObject p_real_SelectObject = NULL;
static PFN_SetDIBitsToDevice p_real_SetDIBitsToDevice = NULL;
static PFN_GetObjectA p_real_GetObjectA = NULL;
static PFN_mciSendCommandA p_real_mciSendCommandA = NULL;

typedef struct GdiIatHookRecord {
    BYTE *slot;
    DWORD original;
    DWORD replacement;
    const char *name;
} GdiIatHookRecord;

enum { GDI_IAT_HOOK_CAPACITY = 16 };
static GdiIatHookRecord g_gdi_iat_hooks[GDI_IAT_HOOK_CAPACITY];
static unsigned g_gdi_iat_hook_count = 0;

typedef struct MciIatHookRecord {
    BYTE *slot;
    DWORD original;
    DWORD replacement;
    const char *name;
} MciIatHookRecord;

enum { MCI_IAT_HOOK_CAPACITY = 4 };
static MciIatHookRecord g_mci_iat_hooks[MCI_IAT_HOOK_CAPACITY];
static unsigned g_mci_iat_hook_count = 0;
static MCIDEVICEID g_mci_last_device = 0;
static UINT g_mci_last_command = 0;
static MCIERROR g_mci_last_result = 0;
static DWORD g_mci_last_status_mode = 0;
static int g_mci_last_state = 0;
static DWORD g_mci_last_midi = 0;
static int g_mci_status_initialized = 0;
static DWORD g_mci_last_emit_tick = 0;
static unsigned long g_mci_suppressed_count = 0;

static int WINAPI trace_StretchDIBits(HDC, int, int, int, int, int, int, int, int,
                                      const VOID *, const BITMAPINFO *, UINT, DWORD);
static BOOL WINAPI trace_StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
static BOOL WINAPI trace_BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
static HBITMAP WINAPI trace_CreateDIBSection(HDC, const BITMAPINFO *, UINT, VOID **,
                                             HANDLE, DWORD);
static HBITMAP WINAPI trace_CreateCompatibleBitmap(HDC, int, int);
static HGDIOBJ WINAPI trace_SelectObject(HDC, HGDIOBJ);
static int WINAPI trace_SetDIBitsToDevice(HDC, int, int, DWORD, DWORD, int, int,
                                          UINT, UINT, const VOID *, const BITMAPINFO *, UINT);
static int WINAPI trace_GetObjectA(HGDIOBJ, int, LPVOID);
static MCIERROR WINAPI trace_mciSendCommandA(MCIDEVICEID, UINT, DWORD_PTR,
                                              DWORD_PTR);

static void log_text(const char *text) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (g_locks_ready) EnterCriticalSection(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_log, text, (DWORD)strlen(text), &written, NULL);
    if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
}

static const char *movie_audio_mode_name(MovieAudioMode mode) {
    return mode == AUDIO_MODE_REALTIME ? "realtime" : "picture";
}

static void set_movie_audio_mode_locked(MovieMap *movie, MovieAudioMode mode,
                                        const char *reason) {
    char buf[256];
    int written;
    if (!movie || mode == AUDIO_MODE_UNSET || movie->audio_mode == mode) return;
    movie->audio_mode = mode;
    if (g_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\taudio_mode\thandle=%08X\tmode=%s\treason=%s\tpath=\"%s\"\n",
                       (unsigned long)GetTickCount(), (unsigned)movie->handle,
                       movie_audio_mode_name(mode), reason ? reason : "unknown",
                       movie->rel_path);
    if (written > 0) log_text(buf);
}

static void reset_movie_audio_position_locked(MovieMap *movie) {
    if (!movie) return;
    movie->last_picture_time = 0;
}

static void gdi_log_text(const char *text) {
    if (g_gdi_log == INVALID_HANDLE_VALUE) return;
    if (g_locks_ready) EnterCriticalSection(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_gdi_log, text, (DWORD)strlen(text), &written, NULL);
    if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
}

static void log_event(const char *event, uint32_t sel, uint32_t arg0, uint32_t arg1, uint32_t ret) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (g_locks_ready) EnterCriticalSection(&g_log_lock);
    EventLogState *state = NULL;
    for (unsigned i = 0; i < sizeof(g_event_log_states) / sizeof(g_event_log_states[0]); i++) {
        if (g_event_log_states[i].initialized &&
            lstrcmpA(g_event_log_states[i].name, event) == 0) {
            state = &g_event_log_states[i];
            break;
        }
        if (!g_event_log_states[i].initialized && !state) {
            state = &g_event_log_states[i];
        }
    }
    if (state) {
        if (state->initialized &&
            state->sel == sel &&
            state->arg0 == arg0 &&
            state->arg1 == arg1 &&
            state->ret == ret) {
            if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
            return;
        }
        lstrcpynA(state->name, event, sizeof(state->name));
        state->sel = sel;
        state->arg0 = arg0;
        state->arg1 = arg1;
        state->ret = ret;
        state->initialized = 1;
    }

    char buf[192];
    int n = snprintf(
        buf,
        sizeof(buf),
        "%lu\t%s\tsel=0x%04X\targ0=%08X\targ1=%08X\tret=%08X\n",
        (unsigned long)GetTickCount(),
        event,
        (unsigned)sel,
        (unsigned)arg0,
        (unsigned)arg1,
        (unsigned)ret);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_log, buf, (DWORD)n, &written, NULL);
    }
    if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
}

static void log_controller_line(const char *event, const ControllerMap *controller,
                                uint32_t movie_time, uint32_t frame) {
    char buf[384];
    int written;
    if (g_log == INVALID_HANDLE_VALUE || !controller) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\t%s\tcontroller=%08X\tmovie=%08X\ttime=%lu\t"
                       "frame=%lu\tdst=%ld,%ld,%ld,%ld\thwnd=%08X\n",
                       (unsigned long)GetTickCount(), event,
                       (unsigned)controller->handle,
                       (unsigned)controller->movie_handle,
                       (unsigned long)movie_time, (unsigned long)frame,
                       (long)controller->dst_x, (long)controller->dst_y,
                       (long)controller->dst_cx, (long)controller->dst_cy,
                       (unsigned)(uintptr_t)controller->target_hwnd);
    if (written > 0) log_text(buf);
}

static void log_controller_result(ControllerMap *controller, uint32_t sel,
                                  uint32_t movie_handle, uint32_t controller_handle,
                                  uint32_t result) {
    char buf[256];
    int written;
    int slot = sel == 0x36 ? 0 : (sel == 0x12 ? 1 : -1);
    DWORD now = GetTickCount();
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (controller && slot >= 0) {
        if (controller->result_log_initialized[slot] &&
            controller->result_log_frame[slot] == controller->last_frame &&
            (DWORD)(now - controller->result_log_tick[slot]) < 250u) return;
        controller->result_log_initialized[slot] = 1;
        controller->result_log_tick[slot] = now;
        controller->result_log_frame[slot] = controller->last_frame;
    }
    written = snprintf(buf, sizeof(buf),
                       "%lu\tcontroller_result\tsel=0x%04X\tmovie=%08X\t"
                       "controller=%08X\tret=%08X\n",
                       (unsigned long)GetTickCount(), (unsigned)sel,
                       (unsigned)movie_handle, (unsigned)controller_handle,
                       (unsigned)result);
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

static void shared_publish_controller_state(const ControllerMap *controller,
                                            const MovieMap *movie,
                                            uint32_t event_type) {
    uint32_t generation;
    if (!controller || !movie || !movie->decoder) return;
    if (!g_shared) g_shared = gt_controller_shared_open(&g_shared_mapping);
    if (!g_shared) return;
    gt_controller_shared_begin_write(g_shared);
    g_shared->movie_handle = movie->handle;
    g_shared->controller_handle = controller->handle;
    g_shared->start_tick = controller->start_tick;
    g_shared->timescale = movdec_movie_timescale(movie->decoder);
    g_shared->duration = movdec_movie_duration(movie->decoder);
    g_shared->active = 1;
    if (event_type != GT_CONTROLLER_EVENT_NONE) {
        generation = g_shared->journal_generation + 1u;
        if (!generation) generation = 1u;
        g_shared->journal_generation = generation;
        g_shared->journal[(generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY].generation = generation;
        g_shared->journal[(generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY].event_type = event_type;
        g_shared->journal[(generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY].movie_handle = movie->handle;
        g_shared->journal[(generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY].controller_handle = controller->handle;
    }
    gt_controller_shared_end_write(g_shared);
}

static void shared_publish_controller(const ControllerMap *controller,
                                      const MovieMap *movie) {
    shared_publish_controller_state(controller, movie,
                                    GT_CONTROLLER_EVENT_CONTROLLER_CREATE);
}

static void shared_refresh_controller(const ControllerMap *controller,
                                      const MovieMap *movie) {
    shared_publish_controller_state(controller, movie,
                                    GT_CONTROLLER_EVENT_NONE);
}

static void shared_publish_movie_event(uint32_t event_type,
                                       uint32_t movie_handle) {
    uint32_t generation;
    uint32_t index;
    if (!movie_handle) return;
    if (!g_shared) g_shared = gt_controller_shared_open(&g_shared_mapping);
    if (!g_shared) return;
    gt_controller_shared_begin_write(g_shared);
    if (g_shared->movie_handle == movie_handle &&
        (event_type == GT_CONTROLLER_EVENT_MOVIE_BIND ||
         event_type == GT_CONTROLLER_EVENT_MOVIE_UNBIND)) {
        g_shared->active = 0;
    }
    generation = g_shared->journal_generation + 1u;
    if (!generation) generation = 1u;
    g_shared->journal_generation = generation;
    index = (generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY;
    g_shared->journal[index].generation = generation;
    g_shared->journal[index].event_type = event_type;
    g_shared->journal[index].movie_handle = movie_handle;
    g_shared->journal[index].controller_handle = 0;
    gt_controller_shared_end_write(g_shared);
}

static void shared_reset_movie_clock(uint32_t movie_handle) {
    MovieMap *movie = find_movie(movie_handle);
    for (int i = g_controller_count - 1; i >= 0; i--) {
        if (g_controllers[i].movie_handle == movie_handle) {
            g_controllers[i].start_tick = GetTickCount();
            g_controllers[i].last_time = 0;
            g_controllers[i].last_frame = UINT32_MAX;
            g_controllers[i].done_logged = 0;
            if (movie && movie->decoder)
                shared_refresh_controller(&g_controllers[i], movie);
        }
    }
}

/* Read arbitrary caller-owned memory without ever dereferencing it directly.
 * VirtualQuery is repeated at page boundaries because a probe can begin in a
 * readable page and cross into an unmapped or guarded page. ReadProcessMemory
 * then copies each validated page chunk into a local buffer.
 */
static size_t copy_readable_memory(const void *src, void *dst, size_t want) {
    SYSTEM_INFO system_info;
    uintptr_t current = (uintptr_t)src;
    uintptr_t end;
    uintptr_t page_size;
    size_t copied = 0;

    if (!src || !dst || !want || current < 0x10000u ||
        current > UINTPTR_MAX - want) return 0;
    end = current + want;
    GetSystemInfo(&system_info);
    page_size = system_info.dwPageSize ? (uintptr_t)system_info.dwPageSize : 4096u;

    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t region_end;
        uintptr_t next_page;
        uintptr_t chunk_end;
        SIZE_T chunk;
        SIZE_T got = 0;

        if (!VirtualQuery((const void *)current, &mbi, sizeof(mbi))) break;
        if ((uintptr_t)mbi.BaseAddress > current || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= current) break;

        next_page = current + (page_size - (current % page_size));
        if (next_page <= current) next_page = end;
        chunk_end = end;
        if (region_end < chunk_end) chunk_end = region_end;
        if (next_page < chunk_end) chunk_end = next_page;
        chunk = (SIZE_T)(chunk_end - current);
        if (!chunk || !ReadProcessMemory(GetCurrentProcess(),
                                         (const void *)current,
                                         (BYTE *)dst + copied, chunk, &got)) {
            copied += (size_t)got;
            break;
        }
        copied += (size_t)got;
        if (got != chunk) break;
        current = chunk_end;
    }
    return copied;
}

static void read_game_mci_state(int *state, DWORD *midi) {
    HMODULE module = GetModuleHandleA(NULL);
    int game_state = 0;
    DWORD midi_device = 0;

    if ((uintptr_t)module == (uintptr_t)0x00400000u) {
        copy_readable_memory((const void *)(uintptr_t)0x0044A150u,
                             &game_state, sizeof(game_state));
        copy_readable_memory((const void *)(uintptr_t)0x0043DDD8u,
                             &midi_device, sizeof(midi_device));
    }
    if (state) *state = game_state;
    if (midi) *midi = midi_device;
}

static void log_mci_call(MCIDEVICEID device, UINT command, DWORD_PTR flags,
                         MCIERROR result, int status_mode_valid,
                         DWORD mode) {
    char buf[320];
    int game_state;
    DWORD midi_device;
    DWORD now;
    int same_key = 0;
    int heartbeat = 0;
    unsigned long suppressed = 0;
    int written;

    if (!g_mci_trace_enabled || g_log == INVALID_HANDLE_VALUE) return;
    now = GetTickCount();
    /* Read these before deciding whether an otherwise identical STATUS/MODE
     * call can be suppressed.  A mode-stable poll is still useful evidence
     * when the game's state or selected MIDI device changes. */
    read_game_mci_state(&game_state, &midi_device);
    if (g_locks_ready) EnterCriticalSection(&g_log_lock);
    if (status_mode_valid) {
        same_key = g_mci_status_initialized &&
            g_mci_last_device == device &&
            g_mci_last_command == command &&
            g_mci_last_result == result &&
            g_mci_last_status_mode == mode &&
            g_mci_last_state == game_state &&
            g_mci_last_midi == midi_device;
        if (same_key) {
            g_mci_suppressed_count++;
            if ((DWORD)(now - g_mci_last_emit_tick) < 5000u) {
                if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
                return;
            }
            heartbeat = 1;
        }
        g_mci_last_device = device;
        g_mci_last_command = command;
        g_mci_last_result = result;
        g_mci_last_status_mode = mode;
        g_mci_last_state = game_state;
        g_mci_last_midi = midi_device;
        g_mci_status_initialized = 1;
        g_mci_last_emit_tick = now;
        suppressed = heartbeat ? g_mci_suppressed_count : 0;
        g_mci_suppressed_count = 0;
    }
    written = snprintf(
        buf, sizeof(buf),
        "mci\ttick=%lu\tdev=%u\tcmd=0x%03X\tflags=0x%X\tret=%lu\t"
        "mode=0x%X\tstate=%d\tmidi=%u\tsuppressed=%lu\n",
        (unsigned long)GetTickCount(), (unsigned)device, (unsigned)command,
        (unsigned)flags, (unsigned long)result, (unsigned)mode, game_state,
        (unsigned)midi_device, suppressed);
    if (written > 0) {
        DWORD bytes_written = 0;
        WriteFile(g_log, buf, (DWORD)written, &bytes_written, NULL);
    }
    if (g_locks_ready) LeaveCriticalSection(&g_log_lock);
}

/* Most of the game's BGM files (Sound\M*GM.MID) end the music long before the
 * End-of-Track event: M02GM has 36 s of notes and then 264 s of silence before
 * EOT.  The game loops BGM only when MCI reports MCI_MODE_STOP (FUN_00421600),
 * and Windows 11's sequencer plays the silent tail in full, so the music stops
 * for minutes.  MCI_PLAY with MCI_TO does not help: the sequencer checks TO
 * only when it reaches the next event, which is that far-away EOT.  So open a
 * copy of such a file whose EOT sits on the last real event, written to
 * <game>\MidiLoop (or %TEMP%); the device then stops at the end of the music
 * and the game's own poll seeks to the start and plays again.  The game's
 * files are not modified. */
enum { MIDI_LOOP_MIN_TAIL_MS = 1000 };
enum { MIDI_FILE_MAX_BYTES = 1024 * 1024 };
enum { MIDI_TEMPO_CAPACITY = 1024 };
enum { MIDI_TRACK_CAPACITY = 64 };

static HMODULE g_self_module = NULL;

/* Notify window of the game's last MCI_PLAY per device.  When idle the game
 * only polls BGM as messages arrive, and MM_MCINOTIFY at the end of the song
 * is what wakes it, so a resume issued by the proxy asks for it too. */
typedef struct MidiPlayCallback {
    MCIDEVICEID device;
    DWORD_PTR callback;
} MidiPlayCallback;

enum { MIDI_PLAY_CALLBACK_CAPACITY = 8 };
static MidiPlayCallback g_midi_play_callbacks[MIDI_PLAY_CALLBACK_CAPACITY];

static void midi_remember_play_callback(MCIDEVICEID device,
                                        DWORD_PTR callback) {
    int slot = -1;
    if (g_locks_ready) EnterCriticalSection(&g_state_lock);
    for (int i = 0; i < MIDI_PLAY_CALLBACK_CAPACITY; i++) {
        if (g_midi_play_callbacks[i].device == device) {
            slot = i;
            break;
        }
        if (slot < 0 && !g_midi_play_callbacks[i].device) slot = i;
    }
    if (slot >= 0) {
        g_midi_play_callbacks[slot].device = callback ? device : 0;
        g_midi_play_callbacks[slot].callback = callback;
    }
    if (g_locks_ready) LeaveCriticalSection(&g_state_lock);
}

static DWORD_PTR midi_play_callback(MCIDEVICEID device) {
    DWORD_PTR callback = 0;
    if (g_locks_ready) EnterCriticalSection(&g_state_lock);
    for (int i = 0; i < MIDI_PLAY_CALLBACK_CAPACITY; i++) {
        if (g_midi_play_callbacks[i].device == device) {
            callback = g_midi_play_callbacks[i].callback;
            break;
        }
    }
    if (g_locks_ready) LeaveCriticalSection(&g_state_lock);
    return callback;
}

typedef struct MidiTempo {
    uint32_t tick;
    uint32_t us_per_quarter;
} MidiTempo;

typedef struct MidiTrackEnd {
    size_t body_start;   /* first byte after the MTrk header */
    size_t eot_start;    /* delta-time VLQ of the EOT event */
    uint32_t eot_prev_tick;
} MidiTrackEnd;

static int midi_read_vlq(const BYTE *data, size_t end, size_t *pos,
                         uint32_t *value) {
    uint32_t v = 0;
    for (int n = 0; n < 4; n++) {
        BYTE b;
        if (*pos >= end) return 0;
        b = data[(*pos)++];
        v = (v << 7) | (b & 0x7Fu);
        if (!(b & 0x80u)) {
            *value = v;
            return 1;
        }
    }
    return 0;
}

static size_t midi_write_vlq(BYTE *out, uint32_t value) {
    BYTE tmp[5];
    size_t n = 0;
    tmp[n++] = (BYTE)(value & 0x7Fu);
    while ((value >>= 7) != 0) tmp[n++] = (BYTE)((value & 0x7Fu) | 0x80u);
    for (size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static double midi_tick_to_ms(const MidiTempo *tempos, int tempo_count,
                              uint32_t division, uint32_t tick) {
    double ms = 0.0;
    uint32_t prev_tick = 0;
    uint32_t us = 500000u;
    for (int i = 0; i < tempo_count && tempos[i].tick <= tick; i++) {
        ms += (double)(tempos[i].tick - prev_tick) * us / division / 1000.0;
        prev_tick = tempos[i].tick;
        us = tempos[i].us_per_quarter;
    }
    return ms + (double)(tick - prev_tick) * us / division / 1000.0;
}

/* Builds the trimmed image when the file has notes and at least
 * MIDI_LOOP_MIN_TAIL_MS of silence before End-of-Track.  Every track keeps
 * its events; only the delta time of each EOT changes so that all tracks end
 * on the last non-EOT event of the song.  Caller frees *out. */
static int midi_build_trimmed(const BYTE *data, DWORD size, BYTE **out,
                              DWORD *out_size, DWORD *end_ms_out,
                              DWORD *eot_ms_out) {
    MidiTempo *tempos = NULL;
    MidiTrackEnd *ends = NULL;
    BYTE *image = NULL;
    int tempo_count = 0;
    uint32_t division;
    unsigned tracks;
    size_t pos = 14;
    size_t written;
    uint32_t last_tick = 0;
    uint32_t eot_tick = 0;
    unsigned long notes = 0;
    int ok = 0;

    if (size < 14 || memcmp(data, "MThd", 4) != 0 ||
        ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]) != 6)
        return 0;
    if (((data[8] << 8) | data[9]) > 1) return 0;
    tracks = (data[10] << 8) | data[11];
    division = (data[12] << 8) | data[13];
    if (tracks == 0 || tracks > MIDI_TRACK_CAPACITY || division == 0 ||
        (division & 0x8000u)) return 0;
    tempos = (MidiTempo *)malloc(sizeof(MidiTempo) * MIDI_TEMPO_CAPACITY);
    ends = (MidiTrackEnd *)calloc(tracks, sizeof(MidiTrackEnd));
    if (!tempos || !ends) goto done;

    for (unsigned t = 0; t < tracks; t++) {
        uint32_t length;
        size_t end;
        uint32_t tick = 0;
        BYTE running = 0;
        int saw_eot = 0;
        if (pos + 8 > size || memcmp(data + pos, "MTrk", 4) != 0) goto done;
        length = ((uint32_t)data[pos + 4] << 24) | (data[pos + 5] << 16) |
                 (data[pos + 6] << 8) | data[pos + 7];
        pos += 8;
        if (length > size - pos) goto done;
        end = pos + length;
        ends[t].body_start = pos;
        while (pos < end && !saw_eot) {
            size_t event_start = pos;
            uint32_t delta;
            BYTE status;
            if (!midi_read_vlq(data, end, &pos, &delta) || pos >= end)
                goto done;
            tick += delta;
            status = data[pos];
            if (status == 0xFF) {
                uint32_t meta_len;
                BYTE type;
                if (pos + 2 > end) goto done;
                type = data[pos + 1];
                pos += 2;
                if (!midi_read_vlq(data, end, &pos, &meta_len) ||
                    meta_len > end - pos) goto done;
                if (type == 0x2F) {
                    saw_eot = 1;
                    ends[t].eot_start = event_start;
                    ends[t].eot_prev_tick = tick - delta;
                    if (tick > eot_tick) eot_tick = tick;
                } else {
                    if (type == 0x51 && meta_len == 3) {
                        if (tempo_count >= MIDI_TEMPO_CAPACITY) goto done;
                        tempos[tempo_count].tick = tick;
                        tempos[tempo_count].us_per_quarter =
                            ((uint32_t)data[pos] << 16) |
                            (data[pos + 1] << 8) | data[pos + 2];
                        tempo_count++;
                    }
                    if (tick > last_tick) last_tick = tick;
                }
                pos += meta_len;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t sysex_len;
                pos++;
                if (!midi_read_vlq(data, end, &pos, &sysex_len) ||
                    sysex_len > end - pos) goto done;
                pos += sysex_len;
                if (tick > last_tick) last_tick = tick;
            } else {
                unsigned data_bytes;
                if (status & 0x80u) {
                    running = status;
                    pos++;
                }
                if (!running) goto done;
                data_bytes = ((running & 0xF0u) == 0xC0u ||
                              (running & 0xF0u) == 0xD0u) ? 1u : 2u;
                if (data_bytes > end - pos) goto done;
                if ((running & 0xF0u) == 0x90u && data[pos + 1] != 0)
                    notes++;
                pos += data_bytes;
                if (tick > last_tick) last_tick = tick;
            }
        }
        if (!saw_eot) goto done;
        pos = end;
    }
    /* Stable insertion sort; format-1 tempo events normally sit in track 0. */
    for (int i = 1; i < tempo_count; i++) {
        MidiTempo value = tempos[i];
        int j = i - 1;
        while (j >= 0 && tempos[j].tick > value.tick) {
            tempos[j + 1] = tempos[j];
            j--;
        }
        tempos[j + 1] = value;
    }
    if (notes == 0 || eot_tick <= last_tick) goto done;
    {
        double end_ms = midi_tick_to_ms(tempos, tempo_count, division,
                                        last_tick);
        double eot_ms = midi_tick_to_ms(tempos, tempo_count, division,
                                        eot_tick);
        if (eot_ms - end_ms < (double)MIDI_LOOP_MIN_TAIL_MS ||
            eot_ms >= 4294967295.0) goto done;
        *end_ms_out = (DWORD)end_ms;
        *eot_ms_out = (DWORD)eot_ms;
    }
    /* Each track only shrinks or keeps its EOT delta, so size is enough. */
    image = (BYTE *)malloc(size);
    if (!image) goto done;
    memcpy(image, data, 14);
    written = 14;
    for (unsigned t = 0; t < tracks; t++) {
        size_t events = ends[t].eot_start - ends[t].body_start;
        size_t length_pos = written + 4;
        uint32_t delta = last_tick >= ends[t].eot_prev_tick ?
            last_tick - ends[t].eot_prev_tick : 0;
        size_t track_length;
        memcpy(image + written, "MTrk", 4);
        written += 8;
        memcpy(image + written, data + ends[t].body_start, events);
        written += events;
        written += midi_write_vlq(image + written, delta);
        image[written++] = 0xFF;
        image[written++] = 0x2F;
        image[written++] = 0x00;
        track_length = written - length_pos - 4;
        image[length_pos] = (BYTE)(track_length >> 24);
        image[length_pos + 1] = (BYTE)(track_length >> 16);
        image[length_pos + 2] = (BYTE)(track_length >> 8);
        image[length_pos + 3] = (BYTE)track_length;
    }
    *out = image;
    *out_size = (DWORD)written;
    image = NULL;
    ok = 1;
done:
    free(image);
    free(ends);
    free(tempos);
    return ok;
}

static BYTE *midi_read_file(const char *path, DWORD max_size, DWORD *size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD file_size;
    DWORD got = 0;
    BYTE *data;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    file_size = GetFileSize(file, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size > max_size) {
        CloseHandle(file);
        return NULL;
    }
    data = (BYTE *)malloc(file_size ? file_size : 1);
    if (data && (!ReadFile(file, data, file_size, &got, NULL) ||
                 got != file_size)) {
        free(data);
        data = NULL;
    }
    CloseHandle(file);
    if (data) *size = file_size;
    return data;
}

/* Writes image to dir\name unless an identical file is already there, and
 * returns its short path (MCI refuses long element paths). */
static int midi_store_copy(const char *dir, const char *name,
                           const BYTE *image, DWORD image_size,
                           char *out_path, size_t out_size) {
    char path[MAX_PATH];
    char temp_path[MAX_PATH];
    DWORD existing_size = 0;
    DWORD short_length;
    BYTE *existing;
    int same = 0;
    int written;

    written = snprintf(path, sizeof(path), "%s\\%s", dir, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return 0;
    CreateDirectoryA(dir, NULL);
    existing = midi_read_file(path, image_size, &existing_size);
    if (existing) {
        same = existing_size == image_size &&
            memcmp(existing, image, image_size) == 0;
        free(existing);
    }
    if (!same) {
        HANDLE file;
        DWORD done = 0;
        int ok;
        written = snprintf(temp_path, sizeof(temp_path), "%s.%lu.tmp", path,
                           (unsigned long)GetCurrentProcessId());
        if (written < 0 || (size_t)written >= sizeof(temp_path)) return 0;
        file = CreateFileA(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return 0;
        ok = WriteFile(file, image, image_size, &done, NULL) &&
            done == image_size;
        CloseHandle(file);
        if (!ok || !MoveFileExA(temp_path, path, MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileA(temp_path);
            return 0;
        }
    }
    short_length = GetShortPathNameA(path, out_path, (DWORD)out_size);
    if (short_length == 0 || short_length >= out_size) {
        if (strlen(path) >= out_size) return 0;
        strcpy(out_path, path);
    }
    return 1;
}

static int midi_open_is_sequencer_file(DWORD_PTR flags,
                                       const MCI_OPEN_PARMSA *open) {
    const char *name = open->lpstrElementName;
    size_t length;
    if (!(flags & MCI_OPEN_ELEMENT) || (flags & MCI_OPEN_ELEMENT_ID) || !name)
        return 0;
    if ((flags & MCI_OPEN_TYPE) && !(flags & MCI_OPEN_TYPE_ID) &&
        open->lpstrDeviceType)
        return lstrcmpiA(open->lpstrDeviceType, "sequencer") == 0;
    length = strlen(name);
    return length > 4 && (lstrcmpiA(name + length - 4, ".mid") == 0 ||
                          lstrcmpiA(name + length - 4, ".rmi") == 0);
}

/* Opens the trimmed copy in place of the game's file.  Returns 1 when that
 * open succeeded; otherwise the caller performs the game's original open. */
static int midi_loop_open(DWORD_PTR flags, DWORD_PTR param,
                          MCIERROR *result) {
    MCI_OPEN_PARMSA open;
    MCI_OPEN_PARMSA *game_open = (MCI_OPEN_PARMSA *)(uintptr_t)param;
    char element[MAX_PATH];
    char copy_path[MAX_PATH];
    char dir[MAX_PATH];
    const char *name;
    BYTE *data;
    BYTE *image = NULL;
    DWORD size = 0;
    DWORD image_size = 0;
    DWORD end_ms = 0;
    DWORD eot_ms = 0;
    DWORD length;
    int stored = 0;
    char buf[3 * MAX_PATH];
    int written;

    if (copy_readable_memory(game_open, &open, sizeof(open)) != sizeof(open) ||
        !midi_open_is_sequencer_file(flags, &open)) return 0;
    if (!lstrcpynA(element, open.lpstrElementName, sizeof(element))) return 0;
    data = midi_read_file(element, MIDI_FILE_MAX_BYTES, &size);
    if (!data) return 0;
    if (!midi_build_trimmed(data, size, &image, &image_size, &end_ms,
                            &eot_ms)) {
        free(data);
        return 0;
    }
    free(data);
    name = strrchr(element, '\\');
    if (!name) name = strrchr(element, '/');
    name = name ? name + 1 : element;
    length = g_self_module ?
        GetModuleFileNameA(g_self_module, dir, sizeof(dir)) : 0;
    if (length && length < sizeof(dir)) {
        char *separator = strrchr(dir, '\\');
        if (separator && (size_t)(separator - dir) + sizeof("\\MidiLoop") <=
                sizeof(dir)) {
            strcpy(separator, "\\MidiLoop");
            stored = midi_store_copy(dir, name, image, image_size, copy_path,
                                     sizeof(copy_path));
        }
    }
    if (!stored) {
        length = GetTempPathA(sizeof(dir), dir);
        if (length && length + sizeof("GundamTacticsMidiLoop") <=
                sizeof(dir)) {
            strcat(dir, "GundamTacticsMidiLoop");
            stored = midi_store_copy(dir, name, image, image_size, copy_path,
                                     sizeof(copy_path));
        }
    }
    free(image);
    if (!stored) {
        log_text("midi_loop\tresult=store_failed\n");
        return 0;
    }
    open.lpstrElementName = copy_path;
    *result = p_real_mciSendCommandA(0, MCI_OPEN, flags, (DWORD_PTR)&open);
    written = snprintf(buf, sizeof(buf),
                       "midi_loop\tdev=%u\tret=%lu\tend_ms=%lu\teot_ms=%lu\t"
                       "game=\"%s\"\tcopy=\"%s\"\n",
                       (unsigned)open.wDeviceID, (unsigned long)*result,
                       (unsigned long)end_ms, (unsigned long)eot_ms, element,
                       copy_path);
    if (written > 0) log_text(buf);
    if (*result != 0) return 0;
    game_open->wDeviceID = open.wDeviceID;
    return 1;
}

static MCIERROR WINAPI trace_mciSendCommandA(MCIDEVICEID device, UINT command,
                                              DWORD_PTR flags,
                                              DWORD_PTR param) {
    MCIERROR result;
    MCI_STATUS_PARMS status;
    DWORD mode = 0;
    int status_mode_valid = 0;
    int fix = g_midi_loop_fix_enabled && p_real_mciSendCommandA;

    if (!(fix && command == MCI_OPEN && midi_loop_open(flags, param,
                                                        &result))) {
        result = p_real_mciSendCommandA ?
            p_real_mciSendCommandA(device, command, flags, param) :
            (MCIERROR)MCIERR_DRIVER_INTERNAL;
    }
    if (fix && result == 0 && command == MCI_PLAY && (flags & MCI_NOTIFY)) {
        MCI_GENERIC_PARMS generic;
        if (copy_readable_memory((const void *)(uintptr_t)param, &generic,
                                 sizeof(generic)) == sizeof(generic))
            midi_remember_play_callback(device, generic.dwCallback);
    } else if (fix && result == 0 && command == MCI_CLOSE) {
        midi_remember_play_callback(device, 0);
    }
    /* The game pauses BGM on minimize (WM_SIZE) and resumes it from the poll,
     * but Windows 11's sequencer rejects MCI_RESUME; a plain MCI_PLAY
     * continues from the paused position.  A song started that way reports
     * PAUSE instead of STOP when it reaches the end, so the game resumes
     * again at the end: rewind first there, as the game does on STOP. */
    if (fix && command == MCI_RESUME &&
        result == MCIERR_UNSUPPORTED_FUNCTION && !(flags & MCI_NOTIFY)) {
        MCI_PLAY_PARMS play;
        MCI_STATUS_PARMS position;
        MCI_STATUS_PARMS length;
        memset(&position, 0, sizeof(position));
        memset(&length, 0, sizeof(length));
        position.dwItem = MCI_STATUS_POSITION;
        length.dwItem = MCI_STATUS_LENGTH;
        if (p_real_mciSendCommandA(device, MCI_STATUS, MCI_STATUS_ITEM,
                                   (DWORD_PTR)&position) == 0 &&
            p_real_mciSendCommandA(device, MCI_STATUS, MCI_STATUS_ITEM,
                                   (DWORD_PTR)&length) == 0 &&
            length.dwReturn != 0 && position.dwReturn >= length.dwReturn) {
            MCI_SEEK_PARMS seek;
            memset(&seek, 0, sizeof(seek));
            p_real_mciSendCommandA(device, MCI_SEEK, MCI_SEEK_TO_START,
                                   (DWORD_PTR)&seek);
        }
        memset(&play, 0, sizeof(play));
        play.dwCallback = midi_play_callback(device);
        result = p_real_mciSendCommandA(
            device, MCI_PLAY,
            (flags & MCI_WAIT) | (play.dwCallback ? MCI_NOTIFY : 0),
            (DWORD_PTR)&play);
        if (g_mci_trace_enabled) {
            char buf[128];
            int written = snprintf(buf, sizeof(buf),
                                   "midi_resume\tdev=%u\tpos=%lu\tlen=%lu\t"
                                   "ret=%lu\n", (unsigned)device,
                                   (unsigned long)position.dwReturn,
                                   (unsigned long)length.dwReturn,
                                   (unsigned long)result);
            if (written > 0) log_text(buf);
        }
    }
    if (command == MCI_STATUS &&
        copy_readable_memory((const void *)(uintptr_t)param, &status,
                             sizeof(status)) == sizeof(status) &&
        status.dwItem == MCI_STATUS_MODE) {
        mode = (DWORD)status.dwReturn;
        status_mode_valid = 1;
    }
    log_mci_call(device, command, flags, result, status_mode_valid, mode);
    return result;
}

static int run_mci_hook_selftest(void) {
    MCI_STATUS_PARMS status;
    MCIERROR result;
    int has_real = p_real_mciSendCommandA != NULL;
    int passed;
    char buf[256];
    int written;

    memset(&status, 0, sizeof(status));
    status.dwItem = MCI_STATUS_MODE;
    result = trace_mciSendCommandA((MCIDEVICEID)0, MCI_STATUS,
                                   MCI_STATUS_ITEM, (DWORD_PTR)&status);
    passed = has_real && result == MCIERR_INVALID_DEVICE_ID;
    written = snprintf(buf, sizeof(buf),
                       "mci_hook_selftest_detail=real=%d\tret=%lu\texpected=%u\n",
                       has_real, (unsigned long)result,
                       (unsigned)MCIERR_INVALID_DEVICE_ID);
    if (written > 0) log_text(buf);
    return passed;
}

static int patch_mci_iat_entry(BYTE *slot, const char *name,
                               void *replacement, void **original) {
    DWORD old_protect;
    DWORD restored_protect;
    void *old_value = (void *)(uintptr_t)(*(DWORD *)slot);
    MciIatHookRecord *record;

    if (!old_value || old_value == replacement) return 0;
    if (g_mci_iat_hook_count >= MCI_IAT_HOOK_CAPACITY) {
        log_text("mci_hook_failure=capacity\n");
        return -1;
    }
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old_protect)) {
        log_text("mci_hook_failure=VirtualProtect_make_writable\n");
        return -1;
    }
    *(DWORD *)slot = (DWORD)(uintptr_t)replacement;
    record = &g_mci_iat_hooks[g_mci_iat_hook_count++];
    record->slot = slot;
    record->original = (DWORD)(uintptr_t)old_value;
    record->replacement = (DWORD)(uintptr_t)replacement;
    record->name = name;
    if (original) *original = old_value;
    if (!VirtualProtect(slot, sizeof(DWORD), old_protect, &restored_protect)) {
        log_text("mci_hook_failure=VirtualProtect_restore_protection\n");
        return -1;
    }
    log_text("mci_hook_installed=1\n");
    return 1;
}

static int restore_mci_iat_hooks(void) {
    int failed = 0;
    while (g_mci_iat_hook_count > 0) {
        MciIatHookRecord *record =
            &g_mci_iat_hooks[g_mci_iat_hook_count - 1];
        DWORD current = *(DWORD *)record->slot;
        DWORD old_protect;
        DWORD restored_protect;
        if (current != record->replacement) {
            g_mci_iat_hook_count--;
            continue;
        }
        if (!VirtualProtect(record->slot, sizeof(DWORD), PAGE_READWRITE,
                            &old_protect)) {
            failed = 1;
            g_mci_iat_hook_count--;
            continue;
        }
        *(DWORD *)record->slot = record->original;
        if (!VirtualProtect(record->slot, sizeof(DWORD), old_protect,
                            &restored_protect)) failed = 1;
        g_mci_iat_hook_count--;
    }
    return failed ? 0 : 1;
}

static int install_mci_iat_hooks(void) {
    HMODULE module = GetModuleHandleA(NULL);
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS32 nt;
    IMAGE_DATA_DIRECTORY directory;
    PIMAGE_IMPORT_DESCRIPTOR imports;
    unsigned installed = 0;
    int failed = 0;

    if (!module) return 0;
    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return 0;
    imports = (PIMAGE_IMPORT_DESCRIPTOR)(base + directory.VirtualAddress);

    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)(base + imports->Name);
        DWORD *original_thunks;
        DWORD *iat_thunks;
        if (lstrcmpiA(dll_name, "winmm.dll") != 0) continue;
        original_thunks = (DWORD *)(base + imports->OriginalFirstThunk);
        iat_thunks = (DWORD *)(base + imports->FirstThunk);
        if (!imports->OriginalFirstThunk) continue;
        for (; *original_thunks; original_thunks++, iat_thunks++) {
            IMAGE_IMPORT_BY_NAME *import_name;
            if (IMAGE_SNAP_BY_ORDINAL32(*original_thunks)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + *original_thunks);
            if (lstrcmpA((const char *)import_name->Name,
                         "mciSendCommandA") == 0) {
                int result = patch_mci_iat_entry(
                    (BYTE *)iat_thunks, "mciSendCommandA",
                    (void *)(uintptr_t)trace_mciSendCommandA,
                    (void **)&p_real_mciSendCommandA);
                if (result > 0) installed++;
                if (result < 0) failed = 1;
            }
        }
    }
    {
        char buf[64];
        int written = snprintf(buf, sizeof(buf),
                               "mci_hooks_installed=%u\n", installed);
        if (written > 0) log_text(buf);
    }
    return failed ? 0 : 1;
}

/* gundam.exe accepts an input message only when GetMessageTime() is strictly
 * newer than the last stamp it stored, and it stores the stamp for every
 * retrieved message including WM_MOUSEMOVE (e.g. FUN_004185c0, FUN_00403640).
 * A button press that lands in the same GetTickCount tick (~15.6 ms on Windows
 * 11) as the preceding mouse move therefore gets the same time and is dropped.
 * Hook the main EXE's PeekMessageA/GetMessageA/GetMessageTime so that input
 * messages other than WM_MOUSEMOVE report a time strictly newer than any time
 * already reported.  Only same-tick collisions are bumped (by 1 ms each);
 * the game's own +1000/+5000 ms click guards keep their meaning. */
typedef BOOL (WINAPI *PFN_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *PFN_GetMessageA)(LPMSG, HWND, UINT, UINT);
typedef LONG (WINAPI *PFN_GetMessageTime)(void);

static PFN_PeekMessageA p_real_PeekMessageA = NULL;
static PFN_GetMessageA p_real_GetMessageA = NULL;
static PFN_GetMessageTime p_real_GetMessageTime = NULL;

typedef struct InputTimeState {
    DWORD thread;
    int have;
    LONG last_reported;  /* newest time reported for any retrieved message */
    LONG last_raw;       /* real time of the last retrieved message */
    LONG last_adjusted;  /* time reported for the last retrieved message */
} InputTimeState;

enum { INPUT_TIME_MAX_BUMP_MS = 100 };

static InputTimeState g_input_time;
static volatile LONG g_input_adjust_count = 0;
static int g_input_selftest_done = 0;

typedef struct InputIatHookRecord {
    BYTE *slot;
    DWORD original;
    DWORD replacement;
    const char *name;
} InputIatHookRecord;

enum { INPUT_IAT_HOOK_CAPACITY = 4 };
static InputIatHookRecord g_input_iat_hooks[INPUT_IAT_HOOK_CAPACITY];
static unsigned g_input_iat_hook_count = 0;

static int is_gated_input_message(UINT message) {
    if (message >= WM_KEYFIRST && message <= WM_KEYLAST) return 1;
    return message > WM_MOUSEMOVE && message <= WM_MOUSELAST;
}

/* Records a retrieved message and returns the time GetMessageTime should
 * report for it. */
static LONG input_time_record(InputTimeState *st, DWORD thread, UINT message,
                              LONG raw) {
    LONG adjusted = raw;
    if (!st->have || st->thread != thread) {
        st->thread = thread;
        st->have = 0;
    }
    if (st->have && is_gated_input_message(message) &&
        (LONG)(raw - st->last_reported) <= 0 &&
        (LONG)(st->last_reported - raw) < INPUT_TIME_MAX_BUMP_MS)
        adjusted = st->last_reported + 1;
    if (!st->have || (LONG)(adjusted - st->last_reported) > 0)
        st->last_reported = adjusted;
    st->have = 1;
    st->last_raw = raw;
    st->last_adjusted = adjusted;
    return adjusted;
}

static LONG input_time_report(const InputTimeState *st, DWORD thread,
                              LONG real) {
    if (st->have && st->thread == thread && real == st->last_raw)
        return st->last_adjusted;
    return real;
}

static void note_retrieved_message(const MSG *msg) {
    LONG adjusted = input_time_record(&g_input_time, GetCurrentThreadId(),
                                      msg->message, (LONG)msg->time);
    if (adjusted != (LONG)msg->time && g_trace_enabled) {
        LONG count = InterlockedIncrement(&g_input_adjust_count);
        if (count <= 32 || (count & 255) == 0) {
            char buf[128];
            int written = snprintf(buf, sizeof(buf),
                                   "input_time_adjust\tcount=%ld\tmsg=%04X\traw=%ld\tadj=%ld\n",
                                   (long)count, (unsigned)msg->message,
                                   (long)msg->time, (long)adjusted);
            if (written > 0) log_text(buf);
        }
    }
}

static BOOL WINAPI hook_PeekMessageA(LPMSG msg, HWND hwnd, UINT first,
                                     UINT last, UINT remove) {
    BOOL result = p_real_PeekMessageA(msg, hwnd, first, last, remove);
    if (result && msg && (remove & PM_REMOVE)) note_retrieved_message(msg);
    return result;
}

static BOOL WINAPI hook_GetMessageA(LPMSG msg, HWND hwnd, UINT first,
                                    UINT last) {
    BOOL result = p_real_GetMessageA(msg, hwnd, first, last);
    if (result != 0 && result != -1 && msg) note_retrieved_message(msg);
    return result;
}

static LONG WINAPI hook_GetMessageTime(void) {
    return input_time_report(&g_input_time, GetCurrentThreadId(),
                             p_real_GetMessageTime());
}

static int run_input_time_selftest(void) {
    InputTimeState st;
    int ok = 1;
    memset(&st, 0, sizeof(st));
    /* move at 1000, press in the same tick -> reported strictly newer */
    ok &= input_time_record(&st, 1, WM_MOUSEMOVE, 1000) == 1000;
    ok &= input_time_record(&st, 1, WM_LBUTTONDOWN, 1000) == 1001;
    ok &= input_time_report(&st, 1, 1000) == 1001;
    ok &= input_time_report(&st, 2, 1000) == 1000;
    /* a later move in the same tick is not bumped; the next press is */
    ok &= input_time_record(&st, 1, WM_MOUSEMOVE, 1000) == 1000;
    ok &= input_time_record(&st, 1, WM_LBUTTONUP, 1000) == 1002;
    /* newer ticks pass through unchanged */
    ok &= input_time_record(&st, 1, WM_LBUTTONDOWN, 1016) == 1016;
    ok &= input_time_record(&st, 1, WM_KEYDOWN, 1016) == 1017;
    ok &= input_time_report(&st, 1, 5000) == 5000;
    /* non-input messages are never bumped; large gaps are not bridged */
    ok &= input_time_record(&st, 1, WM_TIMER, 1016) == 1016;
    ok &= input_time_record(&st, 1, WM_LBUTTONDOWN, 800) == 800;
    return ok;
}

static int patch_input_iat_entry(BYTE *slot, const char *name,
                                 void *replacement, void **original) {
    DWORD old_protect;
    DWORD restored_protect;
    void *old_value = (void *)(uintptr_t)(*(DWORD *)slot);
    InputIatHookRecord *record;

    if (!old_value || old_value == replacement) return 0;
    if (g_input_iat_hook_count >= INPUT_IAT_HOOK_CAPACITY) return -1;
    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old_protect))
        return -1;
    if (original) *original = old_value;
    *(DWORD *)slot = (DWORD)(uintptr_t)replacement;
    record = &g_input_iat_hooks[g_input_iat_hook_count++];
    record->slot = slot;
    record->original = (DWORD)(uintptr_t)old_value;
    record->replacement = (DWORD)(uintptr_t)replacement;
    record->name = name;
    VirtualProtect(slot, sizeof(DWORD), old_protect, &restored_protect);
    return 1;
}

static int restore_input_iat_hooks(void) {
    int failed = 0;
    while (g_input_iat_hook_count > 0) {
        InputIatHookRecord *record =
            &g_input_iat_hooks[g_input_iat_hook_count - 1];
        DWORD old_protect;
        DWORD restored_protect;
        g_input_iat_hook_count--;
        if (*(DWORD *)record->slot != record->replacement) continue;
        if (!VirtualProtect(record->slot, sizeof(DWORD), PAGE_READWRITE,
                            &old_protect)) {
            failed = 1;
            continue;
        }
        *(DWORD *)record->slot = record->original;
        VirtualProtect(record->slot, sizeof(DWORD), old_protect,
                       &restored_protect);
    }
    return failed ? 0 : 1;
}

/* PeekMessageA and GetMessageTime must be hooked together (GetMessageA too
 * when imported); a partial set would let GetMessageTime report times for
 * messages the proxy never saw. */
static int install_input_iat_hooks(void) {
    HMODULE module = GetModuleHandleA(NULL);
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS32 nt;
    IMAGE_DATA_DIRECTORY directory;
    PIMAGE_IMPORT_DESCRIPTOR imports;
    BYTE *peek_slot = NULL, *get_slot = NULL, *time_slot = NULL;
    int failed = 0;

    if (!module) return 0;
    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return 0;
    imports = (PIMAGE_IMPORT_DESCRIPTOR)(base + directory.VirtualAddress);

    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)(base + imports->Name);
        DWORD *original_thunks;
        DWORD *iat_thunks;
        if (lstrcmpiA(dll_name, "user32.dll") != 0) continue;
        if (!imports->OriginalFirstThunk) continue;
        original_thunks = (DWORD *)(base + imports->OriginalFirstThunk);
        iat_thunks = (DWORD *)(base + imports->FirstThunk);
        for (; *original_thunks; original_thunks++, iat_thunks++) {
            IMAGE_IMPORT_BY_NAME *import_name;
            const char *name;
            if (IMAGE_SNAP_BY_ORDINAL32(*original_thunks)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + *original_thunks);
            name = (const char *)import_name->Name;
            if (lstrcmpA(name, "PeekMessageA") == 0) peek_slot = (BYTE *)iat_thunks;
            else if (lstrcmpA(name, "GetMessageA") == 0) get_slot = (BYTE *)iat_thunks;
            else if (lstrcmpA(name, "GetMessageTime") == 0) time_slot = (BYTE *)iat_thunks;
        }
    }
    if (!peek_slot || !time_slot) {
        log_text("input_fix=off\treason=imports_missing\n");
        return 0;
    }
    if (patch_input_iat_entry(peek_slot, "PeekMessageA",
                              (void *)(uintptr_t)hook_PeekMessageA,
                              (void **)&p_real_PeekMessageA) <= 0) failed = 1;
    if (!failed && get_slot &&
        patch_input_iat_entry(get_slot, "GetMessageA",
                              (void *)(uintptr_t)hook_GetMessageA,
                              (void **)&p_real_GetMessageA) <= 0) failed = 1;
    if (!failed &&
        patch_input_iat_entry(time_slot, "GetMessageTime",
                              (void *)(uintptr_t)hook_GetMessageTime,
                              (void **)&p_real_GetMessageTime) <= 0) failed = 1;
    if (failed) {
        restore_input_iat_hooks();
        log_text("input_fix=off\treason=iat_patch_failed\n");
        return 0;
    }
    {
        char buf[64];
        int written = snprintf(buf, sizeof(buf), "input_fix=on\thooks=%u\n",
                               g_input_iat_hook_count);
        if (written > 0) log_text(buf);
    }
    return 1;
}

/* The movie loops do not dispatch the Windows message queue.  A non-removing
 * probe is enough to keep USER32's ghost-window watchdog satisfied while
 * preserving the game's normal input ordering.  Only probe from the thread
 * that owns the controller's drawing window.  Keep this outside g_state_lock:
 * PeekMessage can enter USER32/window-manager code. */
static void maybe_pump_messages(HWND hwnd) {
    CompatThreadState *state;
    DWORD now;
    DWORD owner_thread;
    MSG message;

    /* Every frame, not throttled: a window drag should start promptly. */
    gt_scale_service_frame_input();
    if (!hwnd) return;
    owner_thread = GetWindowThreadProcessId(hwnd, NULL);
    if (!owner_thread || owner_thread != GetCurrentThreadId()) return;
    state = get_thread_state();
    if (!state) return;
    now = GetTickCount();
    if (state->message_pump_initialized &&
        (DWORD)(now - state->message_pump_last_tick) < 250u) return;
    state->message_pump_initialized = 1;
    state->message_pump_last_tick = now;
    PeekMessageA(&message, NULL, 0, 0, PM_NOREMOVE);
    if (g_trace_enabled &&
        InterlockedCompareExchange(&g_message_pump_logged, 1, 0) == 0) {
        char buf[128];
        int written = snprintf(buf, sizeof(buf),
                               "message_pump\ttick=%lu\thwnd=%08X\tthread=%lu\n",
                               (unsigned long)now,
                               (unsigned)(uintptr_t)hwnd,
                               (unsigned long)GetCurrentThreadId());
        if (written > 0) log_text(buf);
    }
}

typedef struct GdiBihSnapshot {
    LONG width;
    LONG height;
    WORD bit_count;
    int valid;
} GdiBihSnapshot;

static GdiBihSnapshot snapshot_bih(const BITMAPINFO *bmi) {
    GdiBihSnapshot result;
    BITMAPINFOHEADER header;
    memset(&result, 0, sizeof(result));
    if (copy_readable_memory(bmi, &header, sizeof(header)) != sizeof(header))
        return result;
    if (header.biSize < sizeof(BITMAPINFOHEADER)) return result;
    result.width = header.biWidth;
    result.height = header.biHeight;
    result.bit_count = header.biBitCount;
    result.valid = 1;
    return result;
}

static uint32_t gdi_caller_va(void) {
    return (uint32_t)(uintptr_t)__builtin_return_address(0);
}

static void log_gdi_call(const char *api, uint32_t caller, HDC hdc,
                         LONG dst_x, LONG dst_y, LONG dst_cx, LONG dst_cy,
                         LONG src_x, LONG src_y, LONG src_cx, LONG src_cy,
                         const BITMAPINFO *bmi, DWORD rop, uintptr_t result) {
    GdiBihSnapshot bih;
    char buf[512];
    int written;
    if (!g_gdi_trace_enabled || g_gdi_log == INVALID_HANDLE_VALUE) return;
    if (InterlockedCompareExchange(&g_gdi_logging, 1, 0) != 0) return;
    bih = snapshot_bih(bmi);
    written = snprintf(
        buf, sizeof(buf),
        "%lu\tapi=%s\tcaller=%08X\thdc=%08X\tdst=%ld,%ld,%ld,%ld\tsrc=%ld,%ld,%ld,%ld\tbih=%s,%ld,%ld,%u\trop=%08X\tresult=%08X\n",
        (unsigned long)GetTickCount(), api, (unsigned)caller,
        (unsigned)(uintptr_t)hdc, (long)dst_x, (long)dst_y, (long)dst_cx,
        (long)dst_cy, (long)src_x, (long)src_y, (long)src_cx, (long)src_cy,
        bih.valid ? "ok" : "na", (long)bih.width, (long)bih.height,
        (unsigned)bih.bit_count, (unsigned)rop, (unsigned)result);
    if (written > 0) gdi_log_text(buf);
    InterlockedExchange(&g_gdi_logging, 0);
}

static void log_gdi_install(const char *name, void *slot, void *original, void *replacement) {
    char buf[256];
    int written;
    if (g_gdi_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\tinstall\tapi=%s\tslot=%08X\toriginal=%08X\thook=%08X\n",
                       (unsigned long)GetTickCount(), name,
                       (unsigned)(uintptr_t)slot, (unsigned)(uintptr_t)original,
                       (unsigned)(uintptr_t)replacement);
    if (written > 0) gdi_log_text(buf);
}

static void log_gdi_protect_failure(const char *operation, const char *name,
                                    BYTE *slot, DWORD error) {
    char buf[256];
    int written;
    if (g_gdi_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu	gdi_hook_failure=VirtualProtect_%s\tapi=%s\tslot=%08X\terror=%lu\n",
                       (unsigned long)GetTickCount(), operation, name,
                       (unsigned)(uintptr_t)slot, (unsigned long)error);
    if (written > 0) gdi_log_text(buf);
}

static int gdi_trace_requested(void) {
    char value[16];
    DWORD length = GetEnvironmentVariableA("QTIM_GDI_TRACE", value, sizeof(value));
    if (length && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                   value[0] == 't' || value[0] == 'T')) return 1;
    return 0;
}

static int gdi_fix_requested(void) {
    char value[32];
    DWORD length = GetEnvironmentVariableA("QTIM_GDI_FIX", value, sizeof(value));
    if (!length || length >= sizeof(value)) return GDI_FIX_OFF;
    if (lstrcmpiA(value, "bmi_negative") == 0) return GDI_FIX_BMI_NEGATIVE;
    if (lstrcmpiA(value, "flip_rows") == 0) return GDI_FIX_FLIP_ROWS;
    return GDI_FIX_OFF;
}

static unsigned gdi_fix_callers_requested(void) {
    char value[128];
    DWORD length = GetEnvironmentVariableA("QTIM_GDI_FIX_CALLERS", value, sizeof(value));
    unsigned mask = GDI_FIX_CALLERS_DEFAULT;
    char *token;

    if (!length || length >= sizeof(value)) return mask;
    mask = 0;
    token = strtok(value, ",; \t\r\n");
    while (token) {
        char *end = NULL;
        unsigned long caller = strtoul(token, &end, 16);
        if (end != token && *end == '\0') {
            if (caller == 0x00407392ul) mask |= GDI_FIX_CALLER_407392;
            if (caller == 0x00409289ul) mask |= GDI_FIX_CALLER_409289;
        }
        token = strtok(NULL, ",; \t\r\n");
    }
    return mask;
}

static const char *gdi_fix_name(int mode) {
    switch (mode) {
        case GDI_FIX_BMI_NEGATIVE: return "bmi_negative";
        case GDI_FIX_FLIP_ROWS: return "flip_rows";
        default: return "off";
    }
}

static int gdi_fix_caller(uint32_t caller) {
    if (caller == 0x00407392u) return (g_gdi_fix_callers & GDI_FIX_CALLER_407392) != 0;
    if (caller == 0x00409289u) return (g_gdi_fix_callers & GDI_FIX_CALLER_409289) != 0;
    return 0;
}

static const char *gdi_fix_callers_name(void) {
    switch (g_gdi_fix_callers) {
        case 0: return "none";
        case GDI_FIX_CALLER_407392: return "00407392";
        case GDI_FIX_CALLER_409289: return "00409289";
        default: return "00407392,00409289";
    }
}

/* The two target call sites pass a 576x416, 8bpp BITMAPINFO with a palette.
 * Keep the copy bounded, while still allowing the usual DIB header variants.
 */
enum { GDI_FIX_BMI_STORAGE = 8192 };

static size_t gdi_fix_bmi_bytes(const BITMAPINFOHEADER *header) {
    size_t colors = 0;
    size_t header_bytes;
    if (!header || header->biSize < sizeof(BITMAPINFOHEADER) ||
        header->biSize > GDI_FIX_BMI_STORAGE) return 0;
    header_bytes = (size_t)header->biSize;
    if (header->biCompression == BI_BITFIELDS &&
        header->biSize == sizeof(BITMAPINFOHEADER)) {
        colors = 3;
    } else if (header->biBitCount <= 8) {
        colors = header->biClrUsed ? header->biClrUsed : (1u << header->biBitCount);
    }
    if (colors > (GDI_FIX_BMI_STORAGE - header_bytes) / sizeof(RGBQUAD)) return 0;
    return header_bytes + colors * sizeof(RGBQUAD);
}

static int gdi_fix_layout(const BITMAPINFOHEADER *header,
                          size_t *row_bytes, size_t *row_count,
                          size_t *total_bytes) {
    uint64_t width;
    uint64_t bits_per_row;
    uint64_t rows;
    uint64_t stride;
    LONG height;
    if (!header || !row_bytes || !row_count || !total_bytes ||
        header->biWidth <= 0 || header->biBitCount == 0) return 0;
    height = header->biHeight;
    rows = (height < 0) ? (uint64_t)(-(int64_t)height) : (uint64_t)height;
    if (rows == 0) return 0;
    width = (uint64_t)(uint32_t)header->biWidth;
    bits_per_row = width * (uint64_t)header->biBitCount;
    if (bits_per_row > UINT64_MAX - 31u) return 0;
    stride = ((bits_per_row + 31u) / 32u) * 4u;
    if (stride == 0 || stride > SIZE_MAX || rows > SIZE_MAX / stride) return 0;
    *row_bytes = (size_t)stride;
    *row_count = (size_t)rows;
    *total_bytes = (size_t)(stride * rows);
    return 1;
}

static void log_gdi_fix_mode(void) {
    char buf[128];
    int written;
    if (g_gdi_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf), "%lu\tgdi_fix=%s\tgdi_fix_callers=%s\n",
                       (unsigned long)GetTickCount(), gdi_fix_name(g_gdi_fix_mode),
                       gdi_fix_callers_name());
    if (written > 0) gdi_log_text(buf);
}

static void log_getobject_dibsection(HGDIOBJ object, int cb_buffer,
                                     LPVOID buffer, int result, uint32_t caller) {
    DIBSECTION dib;
    char buf[384];
    int written;

    if (!g_gdi_trace_enabled || g_gdi_log == INVALID_HANDLE_VALUE) return;
    if (result < (int)sizeof(DIBSECTION) ||
        copy_readable_memory(buffer, &dib, sizeof(dib)) != sizeof(dib)) return;
    written = snprintf(
        buf, sizeof(buf),
        "%lu\tapi=GetObjectA\tcaller=%08X\tobject=%08X\tcb=%d\tresult=%d\t"
        "dsBmih.biWidth=%ld\tdsBmih.biHeight=%ld\tdsBm.bmWidth=%ld\t"
        "dsBm.bmHeight=%ld\n",
        (unsigned long)GetTickCount(), (unsigned)caller,
        (unsigned)(uintptr_t)object, cb_buffer, result,
        (long)dib.dsBmih.biWidth, (long)dib.dsBmih.biHeight,
        (long)dib.dsBm.bmWidth, (long)dib.dsBm.bmHeight);
    if (written > 0) gdi_log_text(buf);
}

static int patch_gdi_iat_entry(BYTE *slot, const char *name, void *replacement,
                               void **original) {
    DWORD old_protect;
    DWORD restored_protect;
    void *old_value = (void *)(uintptr_t)(*(DWORD *)slot);
    GdiIatHookRecord *record;

    if (!old_value || old_value == replacement) return 0;
    if (g_gdi_iat_hook_count >= GDI_IAT_HOOK_CAPACITY) {
        log_gdi_protect_failure("capacity", name, slot, ERROR_INSUFFICIENT_BUFFER);
        return -1;
    }
    record = &g_gdi_iat_hooks[g_gdi_iat_hook_count];
    record->slot = slot;
    record->original = (DWORD)(uintptr_t)old_value;
    record->replacement = (DWORD)(uintptr_t)replacement;
    record->name = name;

    if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old_protect)) {
        log_gdi_protect_failure("make_writable", name, slot, GetLastError());
        return -1;
    }
    *(DWORD *)slot = (DWORD)(uintptr_t)replacement;
    if (!VirtualProtect(slot, sizeof(DWORD), old_protect, &restored_protect)) {
        log_gdi_protect_failure("restore_protection", name, slot, GetLastError());
        g_gdi_iat_hook_count++;
        if (original) *original = old_value;
        log_gdi_install(name, slot, old_value, replacement);
        return -1;
    }
    g_gdi_iat_hook_count++;
    if (original) *original = old_value;
    log_gdi_install(name, slot, old_value, replacement);
    return 1;
}

static int restore_gdi_iat_hooks(void) {
    int failed = 0;
    while (g_gdi_iat_hook_count > 0) {
        GdiIatHookRecord *record = &g_gdi_iat_hooks[g_gdi_iat_hook_count - 1];
        DWORD current = *(DWORD *)record->slot;
        DWORD old_protect;
        DWORD restored_protect;
        if (current != record->replacement) {
            char buf[256];
            int written = snprintf(buf, sizeof(buf),
                                   "%lu\trestore_skip\tapi=%s\tslot=%08X\tcurrent=%08X\texpected=%08X\n",
                                   (unsigned long)GetTickCount(), record->name,
                                   (unsigned)(uintptr_t)record->slot,
                                   (unsigned)current, (unsigned)record->replacement);
            if (written > 0) gdi_log_text(buf);
            g_gdi_iat_hook_count--;
            continue;
        }
        if (!VirtualProtect(record->slot, sizeof(DWORD), PAGE_READWRITE, &old_protect)) {
            log_gdi_protect_failure("detach_make_writable", record->name,
                                    record->slot, GetLastError());
            failed = 1;
            g_gdi_iat_hook_count--;
            continue;
        }
        *(DWORD *)record->slot = record->original;
        if (!VirtualProtect(record->slot, sizeof(DWORD), old_protect, &restored_protect)) {
            log_gdi_protect_failure("detach_restore_protection", record->name,
                                    record->slot, GetLastError());
            failed = 1;
        }
        g_gdi_iat_hook_count--;
    }
    return failed ? 0 : 1;
}

static int install_gdi_iat_hooks(void) {
    HMODULE module = GetModuleHandleA(NULL);
    BYTE *base = (BYTE *)module;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS32 nt;
    IMAGE_DATA_DIRECTORY directory;
    PIMAGE_IMPORT_DESCRIPTOR imports;
    unsigned installed = 0;
    int failed = 0;
    if (!module) return 0;
    dos = (PIMAGE_DOS_HEADER)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (PIMAGE_NT_HEADERS32)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return 0;
    imports = (PIMAGE_IMPORT_DESCRIPTOR)(base + directory.VirtualAddress);

    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)(base + imports->Name);
        DWORD *original_thunks;
        DWORD *iat_thunks;
        if (lstrcmpiA(dll_name, "gdi32.dll") != 0) continue;
        original_thunks = (DWORD *)(base + imports->OriginalFirstThunk);
        iat_thunks = (DWORD *)(base + imports->FirstThunk);
        if (!imports->OriginalFirstThunk) continue;
        for (; *original_thunks; original_thunks++, iat_thunks++) {
            IMAGE_IMPORT_BY_NAME *import_name;
            void *replacement = NULL;
            void **original = NULL;
            const char *name;
            if (IMAGE_SNAP_BY_ORDINAL32(*original_thunks)) continue;
            import_name = (IMAGE_IMPORT_BY_NAME *)(base + *original_thunks);
            name = (const char *)import_name->Name;
            if (lstrcmpA(name, "StretchDIBits") == 0) {
                replacement = (void *)(uintptr_t)trace_StretchDIBits;
                original = (void **)&p_real_StretchDIBits;
            } else if (lstrcmpA(name, "StretchBlt") == 0) {
                replacement = (void *)(uintptr_t)trace_StretchBlt;
                original = (void **)&p_real_StretchBlt;
            } else if (lstrcmpA(name, "BitBlt") == 0) {
                replacement = (void *)(uintptr_t)trace_BitBlt;
                original = (void **)&p_real_BitBlt;
            } else if (lstrcmpA(name, "CreateDIBSection") == 0) {
                replacement = (void *)(uintptr_t)trace_CreateDIBSection;
                original = (void **)&p_real_CreateDIBSection;
            } else if (lstrcmpA(name, "CreateCompatibleBitmap") == 0) {
                replacement = (void *)(uintptr_t)trace_CreateCompatibleBitmap;
                original = (void **)&p_real_CreateCompatibleBitmap;
            } else if (lstrcmpA(name, "SelectObject") == 0) {
                replacement = (void *)(uintptr_t)trace_SelectObject;
                original = (void **)&p_real_SelectObject;
            } else if (lstrcmpA(name, "SetDIBitsToDevice") == 0) {
                replacement = (void *)(uintptr_t)trace_SetDIBitsToDevice;
                original = (void **)&p_real_SetDIBitsToDevice;
            } else if (lstrcmpA(name, "GetObjectA") == 0) {
                replacement = (void *)(uintptr_t)trace_GetObjectA;
                original = (void **)&p_real_GetObjectA;
            }
            if (replacement) {
                int result = patch_gdi_iat_entry((BYTE *)iat_thunks, name,
                                                 replacement, original);
                if (result > 0) installed++;
                if (result < 0) failed = 1;
            }
        }
    }
    {
        char buf[128];
        int written = snprintf(buf, sizeof(buf), "%lu\tgdi_hooks_installed=%u\n",
                               (unsigned long)GetTickCount(), installed);
        if (written > 0) gdi_log_text(buf);
    }
    return failed ? 0 : 1;
}

static void append_ascii_preview(char *dst, size_t cap, const uint8_t *p, size_t n) {
    size_t len = strlen(dst);
    if (len >= cap) return;
    for (size_t i = 0; i < n && len + 2 < cap; i++) {
        uint8_t c = p[i];
        dst[len++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }
    dst[len] = 0;
}

static void append_hex_preview(char *dst, size_t cap, const uint8_t *p, size_t n) {
    size_t len = strlen(dst);
    if (len >= cap) return;
    for (size_t i = 0; i < n && len + 4 < cap; i++) {
        int written = snprintf(dst + len, cap - len, "%02X", (unsigned)p[i]);
        if (written <= 0) return;
        len += (size_t)written;
    }
}

static void append_arg_probe(char *dst, size_t cap, const char *name, uint32_t value) {
    uint8_t probe[192];
    size_t available;
    size_t len = strlen(dst);
    if (len >= cap) return;
    int n = snprintf(dst + len, cap - len, "\t%s=%08X", name, (unsigned)value);
    if (n <= 0) return;

    const uint8_t *p = (const uint8_t *)(uintptr_t)value;
    available = copy_readable_memory(p, probe, sizeof(probe));
    if (!available) return;

    len = strlen(dst);
    snprintf(dst + len, cap - len, " hex=");
    append_hex_preview(dst, cap, probe, available < 16 ? available : 16);

    len = strlen(dst);
    snprintf(dst + len, cap - len, " ascii=\"");
    append_ascii_preview(dst, cap, probe, available);
    len = strlen(dst);
    snprintf(dst + len, cap - len, "\"");

    /* Classic QuickTime APIs may pass Mac-style FSSpec-like data:
     * vRefNum(2), parID(4), name[64] Pascal string.  If it looks plausible,
     * include that name separately.
     */
    if (available >= 7) {
        uint8_t pas_len = probe[6];
        if (pas_len > 0 && pas_len <= 63 && available >= (size_t)7 + pas_len) {
            len = strlen(dst);
            snprintf(dst + len, cap - len, " pstr=\"");
            append_ascii_preview(dst, cap, probe + 7, pas_len);
            len = strlen(dst);
            snprintf(dst + len, cap - len, "\"");
        }
    }
}

static void log_selector_probe(uint32_t sel, uint32_t *f) {
    uint32_t frame[16];
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (copy_readable_memory(f, frame, sizeof(frame)) != sizeof(frame)) return;

    const char *name = NULL;
    switch (sel) {
        case 0x02: name = "CloseMovieFile"; break;
        case 0x07: name = "DisposeMovie"; break;
        case 0x2A: name = "NewMovieFromFile"; break;
        case 0x2C: name = "OpenMovieFile"; break;
        case 0x2F: name = "SetMovieActive"; break;
        case 0x31: name = "GoToBeginningOfMovie"; break;
        case 0x37: name = "DisposeMovieController"; break;
        case 0x38: name = "NewMovieController"; break;
        default:
            if (g_seen_selectors[sel]) return;
            g_seen_selectors[sel] = 1;
            name = "UnknownSelector";
            break;
    }

    char buf[2048];
    snprintf(
        buf,
        sizeof(buf),
        "%lu\tprobe_%s\tsel=0x%04X",
        (unsigned long)GetTickCount(),
        name,
        (unsigned)sel);

    append_arg_probe(buf, sizeof(buf), "arg0", frame[12]);
    append_arg_probe(buf, sizeof(buf), "arg1", frame[13]);
    append_arg_probe(buf, sizeof(buf), "arg2", frame[14]);
    append_arg_probe(buf, sizeof(buf), "arg3", frame[15]);

    size_t len = strlen(buf);
    if (len + 2 < sizeof(buf)) {
        buf[len++] = '\n';
        buf[len] = 0;
    }
    log_text(buf);
}

static void remember_pending_movie_path(uint32_t ptr_value) {
    CompatThreadState *state = get_thread_state();
    const char *p = (const char *)(uintptr_t)ptr_value;
    char path_probe[256];
    size_t path_bytes;
    if (!state) return;
    path_bytes = copy_readable_memory(p, path_probe, sizeof(path_probe) - 1);
    if (!path_bytes) return;
    path_probe[path_bytes] = 0;

    const char *movie = strstr(path_probe, "Movie\\");
    if (!movie) movie = strstr(path_probe, "movie\\");
    if (!movie) return;

    const char *end = strstr(movie, ".mov");
    if (!end) end = strstr(movie, ".MOV");
    if (!end) return;
    end += 4;

    size_t n = (size_t)(end - movie);
    if (n >= sizeof(state->pending_movie_path)) n = sizeof(state->pending_movie_path) - 1;
    memcpy(state->pending_movie_path, movie, n);
    state->pending_movie_path[n] = 0;

    char buf[256];
    int written = snprintf(buf, sizeof(buf), "%lu\tmap_pending\tpath=\"%s\"\n",
                           (unsigned long)GetTickCount(), state->pending_movie_path);
    if (written > 0) log_text(buf);
}

static void clear_pending_movie_state(CompatThreadState *state) {
    if (!state) return;
    state->pending_movie_path[0] = '\0';
    state->pending_file_handle = 0;
    state->pending_out_slot = 0;
    state->pending_close_seen = 0;
}

static void drop_pending_movie_state(CompatThreadState *state,
                                     uint32_t movie_handle,
                                     const char *reason) {
    char buf[320];
    int written;
    if (!state) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\tmap_pending_dropped\thandle=%08X\tpath=\"%s\"\treason=%s\n",
                       (unsigned long)GetTickCount(), (unsigned)movie_handle,
                       state->pending_movie_path, reason ? reason : "unknown");
    if (written > 0) log_text(buf);
    clear_pending_movie_state(state);
}

static void free_audio_chunk(AudioChunk *chunk) {
    if (!chunk) return;
    if (chunk->data) HeapFree(GetProcessHeap(), 0, chunk->data);
    HeapFree(GetProcessHeap(), 0, chunk);
}

static int unprepare_audio_chunk(HWAVEOUT audio_out, AudioChunk *chunk) {
    MMRESULT result;
    if (!chunk || !chunk->prepared) return 1;
    if (!audio_out) return 0;
    result = waveOutUnprepareHeader(audio_out, &chunk->header,
                                    sizeof(chunk->header));
    if (result != MMSYSERR_NOERROR) return 0;
    chunk->prepared = 0;
    return 1;
}

static void free_audio_chunks(MovieMap *movie) {
    AudioChunk **link;
    AudioChunk *chunk;
    if (!movie) return;
    link = &movie->audio_chunks;
    while (*link) {
        chunk = *link;
        if (chunk->prepared &&
            !unprepare_audio_chunk(movie->audio_out, chunk)) {
            link = &chunk->next;
            continue;
        }
        *link = chunk->next;
        free_audio_chunk(chunk);
    }
}

static void reap_audio_chunks(MovieMap *movie) {
    AudioChunk **link;
    AudioChunk *chunk;
    if (!movie || !movie->audio_out) return;
    link = &movie->audio_chunks;
    while (*link) {
        chunk = *link;
        if (!(chunk->header.dwFlags & WHDR_DONE)) {
            link = &chunk->next;
            continue;
        }
        if (chunk->prepared &&
            !unprepare_audio_chunk(movie->audio_out, chunk)) {
            link = &chunk->next;
            continue;
        }
        *link = chunk->next;
        free_audio_chunk(chunk);
    }
}

static int audio_teardown_run(AudioTeardown *teardown) {
    AudioChunk **link;
    AudioChunk *chunk;
    MMRESULT result;
    int complete = 1;

    if (!teardown) return 1;
    if (teardown->audio_out) {
        result = waveOutReset(teardown->audio_out);
        if (result != MMSYSERR_NOERROR) complete = 0;
        if (teardown->audio_prepared) {
            result = waveOutUnprepareHeader(teardown->audio_out,
                                             &teardown->audio_header,
                                             sizeof(teardown->audio_header));
            if (result == MMSYSERR_NOERROR) {
                teardown->audio_prepared = 0;
            } else {
                /* In particular, WAVERR_STILLPLAYING keeps the header and
                 * its backing buffer alive for the next retry. */
                complete = 0;
            }
        }
        link = &teardown->audio_chunks;
        while (*link) {
            chunk = *link;
            if (chunk->prepared &&
                !unprepare_audio_chunk(teardown->audio_out, chunk)) {
                complete = 0;
                link = &chunk->next;
                continue;
            }
            *link = chunk->next;
            free_audio_chunk(chunk);
        }
        if (complete && !teardown->audio_prepared &&
            !teardown->audio_chunks) {
            result = waveOutClose(teardown->audio_out);
            if (result == MMSYSERR_NOERROR) {
                teardown->audio_out = NULL;
            } else {
                /* Never discard a handle after a failed close. */
                complete = 0;
            }
        }
    }
    if (!teardown->audio_out && !teardown->audio_prepared &&
        !teardown->audio_chunks) {
        if (teardown->audio_buffer)
            HeapFree(GetProcessHeap(), 0, teardown->audio_buffer);
        teardown->audio_buffer = NULL;
        teardown->audio_bytes = 0;
    } else {
        complete = 0;
    }
    return complete;
}

static AudioTeardown *detach_movie_audio_state(MovieMap *movie) {
    AudioTeardown *teardown;
    if (!movie) return NULL;
    if (!movie->audio_out && !movie->audio_buffer && !movie->audio_chunks)
        return NULL;
    teardown = (AudioTeardown *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          sizeof(*teardown));
    if (!teardown) return NULL;
    teardown->audio_out = movie->audio_out;
    teardown->audio_header = movie->audio_header;
    teardown->audio_buffer = movie->audio_buffer;
    teardown->audio_bytes = movie->audio_bytes;
    teardown->audio_prepared = movie->audio_prepared;
    teardown->audio_chunks = movie->audio_chunks;
    movie->audio_out = NULL;
    memset(&movie->audio_header, 0, sizeof(movie->audio_header));
    movie->audio_buffer = NULL;
    movie->audio_bytes = 0;
    movie->audio_prepared = 0;
    movie->audio_streaming = 0;
    movie->audio_cursor = 0;
    movie->audio_last_movie_time = UINT32_MAX;
    movie->audio_chunks = NULL;
    return teardown;
}

static void queue_audio_teardown_locked(AudioTeardown *teardown) {
    if (!teardown) return;
    teardown->next = g_audio_teardown_queue;
    g_audio_teardown_queue = teardown;
}

static int queue_movie_audio_teardown_locked(MovieMap *movie) {
    AudioTeardown *teardown;
    if (!movie) return 1;
    teardown = detach_movie_audio_state(movie);
    if (!teardown && (movie->audio_out || movie->audio_buffer ||
                      movie->audio_chunks)) return 0;
    queue_audio_teardown_locked(teardown);
    return 1;
}

static void enqueue_audio_teardown(AudioTeardown *teardown) {
    if (!teardown) return;
    EnterCriticalSection(&g_state_lock);
    queue_audio_teardown_locked(teardown);
    LeaveCriticalSection(&g_state_lock);
}

static void drain_audio_teardowns(void) {
    AudioTeardown *list;
    AudioTeardown *next;
    if (!g_locks_ready) return;
    EnterCriticalSection(&g_state_lock);
    list = g_audio_teardown_queue;
    g_audio_teardown_queue = NULL;
    LeaveCriticalSection(&g_state_lock);
    while (list) {
        next = list->next;
        list->next = NULL;
        if (audio_teardown_run(list)) {
            HeapFree(GetProcessHeap(), 0, list);
        } else {
            enqueue_audio_teardown(list);
        }
        list = next;
    }
}

/* Called only after the dispatcher has released g_state_lock. */
static void stop_movie_audio(MovieMap *movie) {
    AudioTeardown *teardown;
    if (!movie) return;
    teardown = detach_movie_audio_state(movie);
    if (!teardown) return;
    if (audio_teardown_run(teardown))
        HeapFree(GetProcessHeap(), 0, teardown);
    else
        enqueue_audio_teardown(teardown);
}

static void reset_movie_audio_stream(MovieMap *movie) {
    MMRESULT result;
    if (!movie || !movie->audio_out) return;
    result = waveOutReset(movie->audio_out);
    if (result != MMSYSERR_NOERROR) return;
    free_audio_chunks(movie);
    if (movie->audio_chunks) return;
    movie->audio_cursor = 0;
    movie->audio_last_movie_time = UINT32_MAX;
}

static int release_movie(MovieMap *movie) {
    if (!movie) return 1;
    if (!queue_movie_audio_teardown_locked(movie)) return 0;
    if (movie->decoder) {
        movdec_close(movie->decoder);
        movie->decoder = NULL;
    }
    clear_movie_file_bindings_locked(movie->handle);
    memset(movie, 0, sizeof(*movie));
    return 1;
}

static MovieMap *find_movie(uint32_t handle) {
    int i;
    for (i = g_movie_count - 1; i >= 0; i--)
        if (g_movies[i].handle == handle) return &g_movies[i];
    return NULL;
}

static ControllerMap *find_controller(uint32_t handle) {
    for (int i = g_controller_count - 1; i >= 0; i--)
        if (g_controllers[i].handle == handle) return &g_controllers[i];
    return NULL;
}

static ControllerTombstone *find_controller_tombstone(uint32_t handle) {
    for (int i = g_controller_tombstone_count - 1; i >= 0; i--) {
        if (g_controller_tombstones[i].controller_handle == handle)
            return &g_controller_tombstones[i];
    }
    return NULL;
}

static int value_in_controller_args(uint32_t value, uint32_t arg0,
                                    uint32_t arg1, uint32_t arg2,
                                    uint32_t arg3) {
    return value == arg0 || value == arg1 || value == arg2 || value == arg3;
}

static int controller_binding_in_args_locked(uint32_t arg0, uint32_t arg1,
                                             uint32_t arg2, uint32_t arg3,
                                             uint32_t *controller_handle,
                                             uint32_t *movie_handle) {
    for (int i = g_controller_count - 1; i >= 0; i--) {
        ControllerMap *controller = &g_controllers[i];
        if (value_in_controller_args(controller->handle, arg0, arg1, arg2,
                                     arg3)) {
            if (controller_handle) *controller_handle = controller->handle;
            if (movie_handle) *movie_handle = controller->movie_handle;
            return 1;
        }
    }
    for (int i = g_controller_tombstone_count - 1; i >= 0; i--) {
        ControllerTombstone *tombstone = &g_controller_tombstones[i];
        if (value_in_controller_args(tombstone->controller_handle, arg0, arg1,
                                     arg2, arg3)) {
            if (controller_handle)
                *controller_handle = tombstone->controller_handle;
            if (movie_handle) *movie_handle = tombstone->movie_handle;
            return 1;
        }
    }
    {
        uint32_t args[4] = {arg0, arg1, arg2, arg3};
        for (int i = 0; i < 4; i++) {
            if (gt_controller_fake_handle_format(args[i])) {
                if (controller_handle) *controller_handle = args[i];
                if (movie_handle) *movie_handle = 0;
                return 1;
            }
        }
    }
    return 0;
}

static void remember_controller_tombstone_locked(
    const ControllerMap *controller) {
    if (!controller || !controller->handle ||
        g_controller_tombstone_count >= CONTROLLER_TOMBSTONE_CAPACITY)
        return;
    g_controller_tombstones[g_controller_tombstone_count].controller_handle =
        controller->handle;
    g_controller_tombstones[g_controller_tombstone_count].movie_handle =
        controller->movie_handle;
    g_controller_tombstone_count++;
}

static void clear_movie_tombstones_locked(uint32_t movie_handle) {
    for (int i = 0; i < g_controller_tombstone_count; i++) {
        if (g_controller_tombstones[i].movie_handle == movie_handle)
            g_controller_tombstones[i].movie_handle = 0;
    }
}

static void clear_movie_file_bindings_locked(uint32_t movie_handle) {
    int i;
    for (i = g_movie_file_binding_count - 1; i >= 0; i--) {
        if (g_movie_file_bindings[i].movie_handle == movie_handle) {
            if (i + 1 < g_movie_file_binding_count)
                memmove(&g_movie_file_bindings[i], &g_movie_file_bindings[i + 1],
                        (size_t)(g_movie_file_binding_count - i - 1) *
                            sizeof(g_movie_file_bindings[0]));
            g_movie_file_binding_count--;
        }
    }
}

static void bind_movie_file_locked(uint32_t file_handle, uint32_t movie_handle) {
    int i;
    if (!file_handle || !movie_handle) return;
    for (i = g_movie_file_binding_count - 1; i >= 0; i--) {
        if (g_movie_file_bindings[i].file_handle == file_handle) {
            g_movie_file_bindings[i].movie_handle = movie_handle;
            return;
        }
    }
    if (g_movie_file_binding_count >=
        (int)(sizeof(g_movie_file_bindings) / sizeof(g_movie_file_bindings[0]))) return;
    g_movie_file_bindings[g_movie_file_binding_count].file_handle = file_handle;
    g_movie_file_bindings[g_movie_file_binding_count].movie_handle = movie_handle;
    g_movie_file_binding_count++;
}

static uint32_t movie_for_file_locked(uint32_t file_handle) {
    for (int i = g_movie_file_binding_count - 1; i >= 0; i--)
        if (g_movie_file_bindings[i].file_handle == file_handle)
            return g_movie_file_bindings[i].movie_handle;
    return 0;
}

static ControllerMap *find_controller_for_movie(uint32_t movie_handle) {
    for (int i = g_controller_count - 1; i >= 0; i--)
        if (g_controllers[i].movie_handle == movie_handle)
            return &g_controllers[i];
    return NULL;
}

static ControllerMap *find_controller_in_args(uint32_t arg0, uint32_t arg1,
                                              uint32_t arg2, uint32_t arg3) {
    ControllerMap *controller = find_controller(arg0);
    if (controller) return controller;
    controller = find_controller(arg1);
    if (controller) return controller;
    controller = find_controller(arg2);
    if (controller) return controller;
    return find_controller(arg3);
}

static void shared_dispose_controller(uint32_t controller_handle,
                                      uint32_t movie_handle) {
    uint32_t generation;
    uint32_t index;
    if (!g_shared || g_shared->magic != GT_CONTROLLER_SHARED_MAGIC) return;
    gt_controller_shared_begin_write(g_shared);
    if (g_shared->controller_handle == controller_handle)
        g_shared->active = 0;
    generation = g_shared->journal_generation + 1u;
    if (!generation) generation = 1u;
    g_shared->journal_generation = generation;
    index = (generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY;
    g_shared->journal[index].generation = generation;
    g_shared->journal[index].event_type = GT_CONTROLLER_EVENT_CONTROLLER_DISPOSE;
    g_shared->journal[index].movie_handle = movie_handle;
    g_shared->journal[index].controller_handle = controller_handle;
    gt_controller_shared_end_write(g_shared);
}

static void release_controller_at_locked(int index, int stop_audio) {
    ControllerMap *controller;
    MovieMap *movie;
    if (index < 0 || index >= g_controller_count) return;
    controller = &g_controllers[index];
    movie = find_movie(controller->movie_handle);
    remember_controller_tombstone_locked(controller);
    if (stop_audio && movie) queue_movie_audio_teardown_locked(movie);
    shared_dispose_controller(controller->handle, controller->movie_handle);
    if (controller->last_dib) GlobalFree(controller->last_dib);
    if (index + 1 < g_controller_count) {
        memmove(controller, controller + 1,
                (size_t)(g_controller_count - index - 1) * sizeof(*controller));
    }
    g_controller_count--;
    memset(&g_controllers[g_controller_count], 0, sizeof(g_controllers[0]));
}

static void release_controller_handle_locked(uint32_t handle, int stop_audio) {
    for (int i = 0; i < g_controller_count; i++) {
        if (g_controllers[i].handle == handle) {
            release_controller_at_locked(i, stop_audio);
            return;
        }
    }
}

static void release_controllers_for_movie_locked(uint32_t movie_handle) {
    int first = 1;
    for (int i = g_controller_count - 1; i >= 0; i--) {
        if (g_controllers[i].movie_handle == movie_handle) {
            release_controller_at_locked(i, first);
            first = 0;
        }
    }
    /* Retain controller-only tombstones for late DisposeMovieController
     * calls, but never retain the old movie pointer as a binding key. */
    clear_movie_tombstones_locked(movie_handle);
    shared_publish_movie_event(GT_CONTROLLER_EVENT_MOVIE_UNBIND, movie_handle);
}

static ControllerMap *create_controller_locked(MovieMap *movie,
                                               uint32_t target_hwnd) {
    ControllerMap *controller;
    uint32_t handle;
    if (!movie || !movie->decoder || g_controller_count >=
        (int)(sizeof(g_controllers) / sizeof(g_controllers[0]))) return NULL;
    for (;;) {
        handle = g_next_fake_controller;
        g_next_fake_controller += 0x10000u;
        if (g_next_fake_controller < FAKE_CONTROLLER_BASE) return NULL;
        if (!find_controller(handle) && !find_controller_tombstone(handle))
            break;
        if (g_next_fake_controller == handle) return NULL;
    }
    controller = &g_controllers[g_controller_count++];
    memset(controller, 0, sizeof(*controller));
    controller->handle = handle;
    controller->movie_handle = movie->handle;
    controller->start_tick = GetTickCount();
    controller->last_time = 0;
    controller->last_frame = UINT32_MAX;
    controller->target_hwnd = (HWND)(uintptr_t)target_hwnd;
    controller->dst_x = 0;
    controller->dst_y = 0;
    controller->dst_cx = 576;
    controller->dst_cy = 416;
    controller->target_valid = 1;
    shared_publish_controller(controller, movie);
    return controller;
}

static void update_controller_target_locked(uint32_t movie_handle,
                                            uint32_t rect_ptr,
                                            uint32_t hdc_value,
                                            uint32_t hwnd_value) {
    RECT rect;
    int valid = 0;
    if (rect_ptr && copy_readable_memory((const void *)(uintptr_t)rect_ptr,
                                         &rect, sizeof(rect)) == sizeof(rect)) {
        if (rect.right > rect.left && rect.bottom > rect.top &&
            rect.right - rect.left <= 4096 && rect.bottom - rect.top <= 4096)
            valid = 1;
    }
    for (int i = g_controller_count - 1; i >= 0; i--) {
        ControllerMap *controller = &g_controllers[i];
        if (controller->movie_handle != movie_handle) continue;
        controller->target_hwnd = (HWND)(uintptr_t)hwnd_value;
        controller->target_hdc = (HDC)(uintptr_t)hdc_value;
        if (valid) {
            controller->dst_x = rect.left;
            controller->dst_y = rect.top;
            controller->dst_cx = rect.right - rect.left;
            controller->dst_cy = rect.bottom - rect.top;
            controller->target_valid = 1;
        }
    }
}

static void release_movie_handle_locked(uint32_t handle) {
    int i;
    for (i = 0; i < g_movie_count; i++) {
        if (g_movies[i].handle == handle) {
            release_controllers_for_movie_locked(handle);
            release_movie(&g_movies[i]);
            return;
        }
    }
}

static int open_movie_audio(MovieMap *movie) {
    const MovTrackInfo *audio;
    const uint8_t *source;
    uint32_t bytes = 0;
    WAVEFORMATEX format;
    MMRESULT result;
    uint32_t i;
    if (!movie || !movie->decoder || movie->audio_out) return movie &&
        movie->audio_out != NULL;
    audio = movdec_audio_info(movie->decoder);
    source = movdec_audio_bytes(movie->decoder, &bytes);
    if (!audio || !source || !bytes || !audio->sample_rate || !audio->channels)
        return 0;
    if (lstrcmpA(audio->codec, "raw ") != 0 &&
        lstrcmpA(audio->codec, "twos") != 0) return 0;

    movie->audio_buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!movie->audio_buffer) return 0;
    memcpy(movie->audio_buffer, source, bytes);
    if (lstrcmpA(audio->codec, "twos") == 0 && audio->bits_per_sample == 16) {
        for (i = 0; i + 1 < bytes; i += 2) {
            BYTE temp = movie->audio_buffer[i];
            movie->audio_buffer[i] = movie->audio_buffer[i + 1];
            movie->audio_buffer[i + 1] = temp;
        }
    }
    memset(&format, 0, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = audio->channels;
    format.nSamplesPerSec = audio->sample_rate;
    format.wBitsPerSample = audio->bits_per_sample ? audio->bits_per_sample : 8;
    format.nBlockAlign = (WORD)(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    result = waveOutOpen(&movie->audio_out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        HeapFree(GetProcessHeap(), 0, movie->audio_buffer);
        movie->audio_buffer = NULL;
        return 0;
    }
    movie->audio_bytes = bytes;
    return 1;
}

static void start_movie_audio(MovieMap *movie) {
    MMRESULT result;
    if (!movie || movie->audio_streaming || movie->audio_prepared) return;
    if (!open_movie_audio(movie)) return;
    memset(&movie->audio_header, 0, sizeof(movie->audio_header));
    movie->audio_header.lpData = (LPSTR)movie->audio_buffer;
    movie->audio_header.dwBufferLength = movie->audio_bytes;
    result = waveOutPrepareHeader(movie->audio_out, &movie->audio_header,
                                  sizeof(movie->audio_header));
    if (result != MMSYSERR_NOERROR) {
        stop_movie_audio(movie);
        return;
    }
    movie->audio_prepared = 1;
    result = waveOutWrite(movie->audio_out, &movie->audio_header,
                          sizeof(movie->audio_header));
    if (result != MMSYSERR_NOERROR) stop_movie_audio(movie);
}

/* GoToBeginning must rewind an already active real-time stream without
 * changing the movie's audio mode.  The full-buffer header is kept only for
 * this mode; picture mode uses the chunk queue below. */
static void restart_realtime_movie_audio(MovieMap *movie) {
    MMRESULT result;
    if (!movie || !movie->audio_out || !movie->audio_buffer ||
        !movie->audio_bytes) return;
    result = waveOutReset(movie->audio_out);
    if (result != MMSYSERR_NOERROR) return;
    if (movie->audio_prepared) {
        result = waveOutUnprepareHeader(movie->audio_out, &movie->audio_header,
                                         sizeof(movie->audio_header));
        if (result != MMSYSERR_NOERROR) return;
        movie->audio_prepared = 0;
    }
    free_audio_chunks(movie);
    if (movie->audio_chunks) return;
    movie->audio_streaming = 0;
    movie->audio_cursor = 0;
    movie->audio_last_movie_time = UINT32_MAX;
    memset(&movie->audio_header, 0, sizeof(movie->audio_header));
    movie->audio_header.lpData = (LPSTR)movie->audio_buffer;
    movie->audio_header.dwBufferLength = movie->audio_bytes;
    result = waveOutPrepareHeader(movie->audio_out, &movie->audio_header,
                                  sizeof(movie->audio_header));
    if (result != MMSYSERR_NOERROR) {
        stop_movie_audio(movie);
        return;
    }
    movie->audio_prepared = 1;
    result = waveOutWrite(movie->audio_out, &movie->audio_header,
                          sizeof(movie->audio_header));
    if (result != MMSYSERR_NOERROR) stop_movie_audio(movie);
}

static uint32_t movie_audio_target_bytes(const MovieMap *movie,
                                         uint32_t movie_time) {
    const MovTrackInfo *audio;
    uint32_t movie_scale;
    uint64_t audio_time;
    uint64_t target;
    if (!movie || !movie->decoder || !movie->audio_bytes) return 0;
    audio = movdec_audio_info(movie->decoder);
    movie_scale = movdec_movie_timescale(movie->decoder);
    if (!audio || !movie_scale) return 0;
    if (audio->duration && audio->timescale) {
        audio_time = (uint64_t)movie_time * audio->timescale / movie_scale;
        if (audio_time > audio->duration) audio_time = audio->duration;
        target = audio_time * movie->audio_bytes / audio->duration;
    } else if (audio->sample_rate && audio->channels &&
               audio->bits_per_sample) {
        target = (uint64_t)movie_time * audio->sample_rate *
                 audio->channels * audio->bits_per_sample / (8u * movie_scale);
    } else {
        target = 0;
    }
    if (target > movie->audio_bytes) target = movie->audio_bytes;
    return (uint32_t)target;
}

static void log_audio_stream(const MovieMap *movie, uint32_t movie_time,
                             uint32_t bytes) {
    char buf[320];
    int written;
    if (!movie || g_log == INVALID_HANDLE_VALUE) return;
    written = snprintf(buf, sizeof(buf),
                       "%lu\taudio_stream\thandle=%08X\ttime=%lu\t"
                       "bytes=%lu\tcursor=%lu\tpath=\"%s\"\n",
                       (unsigned long)GetTickCount(), (unsigned)movie->handle,
                       (unsigned long)movie_time, (unsigned long)bytes,
                       (unsigned long)movie->audio_cursor, movie->rel_path);
    if (written > 0) log_text(buf);
}

static void stream_movie_audio(MovieMap *movie, uint32_t movie_time) {
    uint32_t lookahead;
    uint32_t target;
    uint32_t queued = 0;
    const uint32_t chunk_limit = 16384u;
    if (!movie || !movie->decoder) return;
    if (!movie->audio_streaming) {
        if (!open_movie_audio(movie)) return;
        movie->audio_streaming = 1;
        movie->audio_cursor = 0;
        movie->audio_last_movie_time = UINT32_MAX;
    }
    reap_audio_chunks(movie);
    if (movie->audio_last_movie_time != UINT32_MAX &&
        movie_time < movie->audio_last_movie_time) {
        reset_movie_audio_stream(movie);
    }
    if (movie->audio_last_movie_time == movie_time) {
        log_audio_stream(movie, movie_time, 0);
        return;
    }
    lookahead = movdec_movie_timescale(movie->decoder) / 10u;
    if (lookahead == 0) lookahead = 1;
    if (movie_time > UINT32_MAX - lookahead)
        target = UINT32_MAX;
    else
        target = movie_time + lookahead;
    if (target > movdec_movie_duration(movie->decoder))
        target = movdec_movie_duration(movie->decoder);
    target = movie_audio_target_bytes(movie, target);
    while (movie->audio_cursor < target) {
        AudioChunk *chunk;
        uint32_t bytes = target - movie->audio_cursor;
        MMRESULT result;
        if (bytes > chunk_limit) bytes = chunk_limit;
        chunk = (AudioChunk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        sizeof(*chunk));
        if (!chunk) break;
        chunk->data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!chunk->data) {
            HeapFree(GetProcessHeap(), 0, chunk);
            break;
        }
        memcpy(chunk->data, movie->audio_buffer + movie->audio_cursor, bytes);
        chunk->header.lpData = (LPSTR)chunk->data;
        chunk->header.dwBufferLength = bytes;
        result = waveOutPrepareHeader(movie->audio_out, &chunk->header,
                                      sizeof(chunk->header));
        if (result != MMSYSERR_NOERROR) {
            HeapFree(GetProcessHeap(), 0, chunk->data);
            HeapFree(GetProcessHeap(), 0, chunk);
            break;
        }
        chunk->prepared = 1;
        result = waveOutWrite(movie->audio_out, &chunk->header,
                              sizeof(chunk->header));
        if (result != MMSYSERR_NOERROR) {
            if (!unprepare_audio_chunk(movie->audio_out, chunk)) {
                /* WAVERR_STILLPLAYING and every other unprepare failure keep
                 * the driver-owned header/data alive for reap/teardown. */
                chunk->next = movie->audio_chunks;
                movie->audio_chunks = chunk;
            } else {
                free_audio_chunk(chunk);
            }
            break;
        }
        chunk->next = movie->audio_chunks;
        movie->audio_chunks = chunk;
        movie->audio_cursor += bytes;
        queued += bytes;
    }
    movie->audio_last_movie_time = movie_time;
    log_audio_stream(movie, movie_time, queued);
}

static int is_known_cvid_path(const char *path) {
    const char *name;
    if (!path) return 0;
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    return lstrcmpiA(name, "Openmovi.mov") == 0 ||
           lstrcmpiA(name, "SIDE0.MOV") == 0 ||
           lstrcmpiA(name, "Side1.mov") == 0;
}

static void map_movie_handle_locked(uint32_t handle, const char *path) {
    int slot = -1;
    if (!path || !path[0]) return;
    if (!handle || (handle & 0x80000000u)) return;

    for (int i = 0; i < g_movie_count; i++) {
        if (g_movies[i].handle == handle) {
            if (lstrcmpiA(g_movies[i].rel_path, path) != 0) {
                release_controllers_for_movie_locked(handle);
                release_movie(&g_movies[i]);
                g_movies[i].handle = handle;
                lstrcpynA(g_movies[i].rel_path, path,
                          sizeof(g_movies[i].rel_path));
                shared_publish_movie_event(GT_CONTROLLER_EVENT_MOVIE_BIND,
                                           handle);

                char buf[256];
                int written = snprintf(buf, sizeof(buf),
                                       "%lu\tmap_movie_update\thandle=%08X\tpath=\"%s\"\n",
                                       (unsigned long)GetTickCount(), (unsigned)handle,
                                       path);
                if (written > 0) log_text(buf);
            } else if (!find_controller_for_movie(handle)) {
                /* A disposed movie can be reused with the same numeric
                 * handle. The bind event retires every old CMGR movie
                 * association before a possible new 0x38 create. */
                shared_publish_movie_event(GT_CONTROLLER_EVENT_MOVIE_UNBIND,
                                           handle);
                shared_publish_movie_event(GT_CONTROLLER_EVENT_MOVIE_BIND,
                                           handle);
            }
            clear_movie_tombstones_locked(handle);
            if (!g_movies[i].decoder && !is_known_cvid_path(g_movies[i].rel_path))
                g_movies[i].decoder = movdec_open(g_movies[i].rel_path);
            if (g_movies[i].decoder &&
                lstrcmpA(movdec_codec(g_movies[i].decoder), "smc ") != 0) {
                movdec_close(g_movies[i].decoder);
                g_movies[i].decoder = NULL;
            }
            if (!g_movies[i].decoder) log_event("cvid_fallback", 0x2A, handle, 0, 0);
            return;
        }
    }
    for (int i = 0; i < g_movie_count; i++) {
        if (!g_movies[i].handle) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_movie_count >= (int)(sizeof(g_movies) / sizeof(g_movies[0]))) return;
        slot = g_movie_count++;
    }

    shared_publish_movie_event(GT_CONTROLLER_EVENT_MOVIE_BIND, handle);

    g_movies[slot].handle = handle;
    lstrcpynA(g_movies[slot].rel_path, path,
              (int)sizeof(g_movies[slot].rel_path));
    g_movies[slot].decoder = is_known_cvid_path(g_movies[slot].rel_path) ?
        NULL : movdec_open(g_movies[slot].rel_path);
    if (g_movies[slot].decoder &&
        lstrcmpA(movdec_codec(g_movies[slot].decoder), "smc ") != 0) {
        movdec_close(g_movies[slot].decoder);
        g_movies[slot].decoder = NULL;
    }
    if (!g_movies[slot].decoder)
        log_event("cvid_fallback", 0x2A, handle, 0, 0);
    g_movies[slot].audio_last_movie_time = UINT32_MAX;

    char buf[256];
    {
        const MovTrackInfo *video = g_movies[slot].decoder ?
            movdec_video_info(g_movies[slot].decoder) : NULL;
        const MovTrackInfo *audio = g_movies[slot].decoder ?
            movdec_audio_info(g_movies[slot].decoder) : NULL;
        int written = snprintf(
            buf, sizeof(buf),
            "%lu\tmap_movie\thandle=%08X\tpath=\"%s\"\tvideo_frames=%lu\t"
            "audio_duration=%lu\taudio_timescale=%lu\taudio_samples=%lu\n",
            (unsigned long)GetTickCount(), (unsigned)handle, path,
            (unsigned long)(video ? video->sample_count : 0),
            (unsigned long)(audio ? audio->duration : 0),
            (unsigned long)(audio ? audio->timescale : 0),
            (unsigned long)(audio ? audio->sample_count : 0));
        if (written > 0) log_text(buf);
    }
}

static const char *path_for_movie(uint32_t handle) {
    for (int i = g_movie_count - 1; i >= 0; i--) {
        if (g_movies[i].handle == handle) return g_movies[i].rel_path;
    }
    return NULL;
}

static uint32_t selftest_dispatch(uint32_t sel, uint32_t arg0, uint32_t arg1,
                                  uint32_t arg2, uint32_t arg3,
                                  uint32_t *result) {
    uint32_t frame[16] = {0};
    uint32_t handled;
    frame[4] = sel;
    frame[12] = arg0;
    frame[13] = arg1;
    frame[14] = arg2;
    frame[15] = arg3;
    handled = compat_dispatch(frame);
    if (result) *result = frame[7];
    return handled;
}

static void reset_selftest_state_locked(void) {
    int i;
    for (i = 0; i < g_movie_count; i++) {
        if (g_movies[i].handle)
            release_controllers_for_movie_locked(g_movies[i].handle);
        release_movie(&g_movies[i]);
    }
    while (g_controller_count > 0)
        release_controller_at_locked(0, 1);
    memset(g_movies, 0, sizeof(g_movies));
    memset(g_controllers, 0, sizeof(g_controllers));
    memset(g_controller_tombstones, 0, sizeof(g_controller_tombstones));
    memset(g_movie_file_bindings, 0, sizeof(g_movie_file_bindings));
    g_movie_count = 0;
    g_controller_count = 0;
    g_controller_tombstone_count = 0;
    g_movie_file_binding_count = 0;
}

static int run_controller_reuse_selftest(void) {
    const uint32_t failed_movie = 0x04FC63D0u;
    const uint32_t failed_file = 0x02A1193Cu;
    const uint32_t reuse_movie = 0x03C84C08u;
    const uint32_t old_file = 0x00001234u;
    const uint32_t new_file = 0x00005678u;
    const uint32_t survivor_movie_a = 0x05112233u;
    const uint32_t survivor_movie_b = 0x05112244u;
    const uint32_t survivor_file = 0x00009ABCu;
    const char *smc_path = "Movie\\HMZK\\HMZK_06.mov";
    const char *pending_path = "Movie\\GM-C\\GM-C_22.mov";
    char failed_path[] = "C:\\GT\\Movie\\GM-C\\GM-C_22.mov";
    char reuse_path[] = "Movie\\__qtim_selftest_new__.mov";
    uint32_t failed_slot = 0xB7040000u;
    uint32_t reuse_slot = reuse_movie;
    uint32_t survivor_slot = survivor_movie_b;
    uint32_t failed_controller = 0;
    uint32_t result = 0;
    int passed = 1;

    /* Failed 0x2A: a pending B path must not retag the live A movie when the
     * game skips CloseMovieFile and continues with stale Movie* data. */
    EnterCriticalSection(&g_state_lock);
    reset_selftest_state_locked();
    map_movie_handle_locked(failed_movie, smc_path);
    MovieMap *failed_map = find_movie(failed_movie);
    ControllerMap *failed_controller_map = failed_map ?
        create_controller_locked(failed_map, 0) : NULL;
    if (!failed_map || !failed_map->decoder || !failed_controller_map) {
        passed = 0;
    } else {
        failed_map->audio_mode = AUDIO_MODE_PICTURE;
        failed_controller = failed_controller_map->handle;
    }
    LeaveCriticalSection(&g_state_lock);

    selftest_dispatch(0x2C, (uint32_t)(uintptr_t)failed_path, 0, 0, 0,
                      NULL);
    selftest_dispatch(0x2A, (uint32_t)(uintptr_t)&failed_slot, failed_file,
                      0, 0, NULL);
    if (selftest_dispatch(0x31, failed_movie, 0, 0, 0, NULL) != 1)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    failed_map = find_movie(failed_movie);
    if (!failed_map || lstrcmpiA(failed_map->rel_path, smc_path) != 0 ||
        !find_controller_for_movie(failed_movie))
        passed = 0;
    LeaveCriticalSection(&g_state_lock);
    uint32_t handled_36 = selftest_dispatch(0x36, failed_movie, 0, 0,
                                            failed_controller, &result);
    uint32_t decoder_duration_36 = 0;
    EnterCriticalSection(&g_state_lock);
    failed_map = find_movie(failed_movie);
    if (!failed_map || !failed_map->decoder) {
        passed = 0;
    } else {
        decoder_duration_36 = movdec_movie_duration(failed_map->decoder);
    }
    LeaveCriticalSection(&g_state_lock);
    if (handled_36 != 1 || result > decoder_duration_36)
        passed = 0;

    EnterCriticalSection(&g_state_lock);
    reset_selftest_state_locked();
    LeaveCriticalSection(&g_state_lock);
    drain_audio_teardowns();

    /* Existing numeric-handle reuse: CloseMovieFile is present before the
     * next 0x2A, and the new mapping is accepted only after its own close. */
    EnterCriticalSection(&g_state_lock);
    g_movie_count = 1;
    g_movies[0].handle = reuse_movie;
    lstrcpynA(g_movies[0].rel_path, "Movie\\__qtim_selftest_old__.mov",
              sizeof(g_movies[0].rel_path));
    g_controller_count = 1;
    g_controllers[0].handle = 0x80C6FFFFu;
    g_controllers[0].movie_handle = reuse_movie;
    bind_movie_file_locked(old_file, reuse_movie);
    LeaveCriticalSection(&g_state_lock);
    selftest_dispatch(0x02, old_file, 0, 0, 0, NULL);
    EnterCriticalSection(&g_state_lock);
    if (find_movie(reuse_movie) || find_controller_for_movie(reuse_movie) ||
        movie_for_file_locked(old_file))
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    selftest_dispatch(0x2C, (uint32_t)(uintptr_t)reuse_path, 0, 0, 0, NULL);
    selftest_dispatch(0x2A, (uint32_t)(uintptr_t)&reuse_slot, new_file,
                      0, 0, NULL);
    selftest_dispatch(0x02, new_file, 0, 0, 0, NULL);
    if (selftest_dispatch(0x31, reuse_movie, 0, 0, 0, NULL) != 0)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    MovieMap *reuse_map = find_movie(reuse_movie);
    if (!reuse_map || lstrcmpiA(reuse_map->rel_path, reuse_path) != 0 ||
        movie_for_file_locked(new_file) != 0)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    EnterCriticalSection(&g_state_lock);
    reset_selftest_state_locked();
    LeaveCriticalSection(&g_state_lock);
    drain_audio_teardowns();

    /* A repeated file handle must not release unrelated live movies.  The
     * 0x02-before-0x31 sequence maps B without creating a file binding;
     * repeating the same file value later must leave both A and B alive. */
    EnterCriticalSection(&g_state_lock);
    map_movie_handle_locked(survivor_movie_a, smc_path);
    MovieMap *survivor_a = find_movie(survivor_movie_a);
    ControllerMap *survivor_a_controller = survivor_a ?
        create_controller_locked(survivor_a, 0) : NULL;
    if (!survivor_a || !survivor_a->decoder || !survivor_a_controller)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    selftest_dispatch(0x2C, (uint32_t)(uintptr_t)failed_path, 0, 0, 0,
                      NULL);
    selftest_dispatch(0x2A, (uint32_t)(uintptr_t)&survivor_slot,
                      survivor_file, 0, 0, NULL);
    selftest_dispatch(0x02, survivor_file, 0, 0, 0, NULL);
    if (selftest_dispatch(0x31, survivor_movie_b, 0, 0, 0, NULL) != 1)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    MovieMap *survivor_b = find_movie(survivor_movie_b);
    ControllerMap *survivor_b_controller = survivor_b ?
        create_controller_locked(survivor_b, 0) : NULL;
    if (!survivor_b || !survivor_b->decoder || !survivor_b_controller ||
        !find_movie(survivor_movie_a) ||
        !find_controller_for_movie(survivor_movie_a) ||
        movie_for_file_locked(survivor_file) != 0)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    selftest_dispatch(0x2C, (uint32_t)(uintptr_t)failed_path, 0, 0, 0,
                      NULL);
    selftest_dispatch(0x2A, (uint32_t)(uintptr_t)&survivor_slot,
                      survivor_file, 0, 0, NULL);
    selftest_dispatch(0x02, survivor_file, 0, 0, 0, NULL);
    EnterCriticalSection(&g_state_lock);
    if (!find_movie(survivor_movie_a) ||
        !find_controller_for_movie(survivor_movie_a) ||
        !find_movie(survivor_movie_b) ||
        !find_controller_for_movie(survivor_movie_b) ||
        movie_for_file_locked(survivor_file) != 0)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    EnterCriticalSection(&g_state_lock);
    reset_selftest_state_locked();
    LeaveCriticalSection(&g_state_lock);
    drain_audio_teardowns();

    /* A cvid movie has no proxy decoder, but its local map still has to be
     * retired on DisposeMovie before QuickTime reuses the numeric handle for
     * an SMC movie. */
    EnterCriticalSection(&g_state_lock);
    map_movie_handle_locked(failed_movie, "Movie\\Openmovi.mov");
    failed_map = find_movie(failed_movie);
    if (!failed_map || failed_map->decoder)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);
    if (selftest_dispatch(0x07, failed_movie, 0, 0, 0, NULL) != 0)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    if (find_movie(failed_movie))
        passed = 0;
    LeaveCriticalSection(&g_state_lock);
    failed_slot = failed_movie;
    selftest_dispatch(0x2C, (uint32_t)(uintptr_t)failed_path, 0, 0, 0,
                      NULL);
    selftest_dispatch(0x2A, (uint32_t)(uintptr_t)&failed_slot, failed_file,
                      0, 0, NULL);
    selftest_dispatch(0x02, failed_file, 0, 0, 0, NULL);
    if (selftest_dispatch(0x31, failed_movie, 0, 0, 0, NULL) != 1)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    failed_map = find_movie(failed_movie);
    if (!failed_map || lstrcmpiA(failed_map->rel_path, pending_path) != 0 ||
        !failed_map->decoder)
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    EnterCriticalSection(&g_state_lock);
    reset_selftest_state_locked();
    LeaveCriticalSection(&g_state_lock);
    drain_audio_teardowns();

    /* DisposeMovie must pass through after local SMC cleanup, while a fake
     * controller value in its arguments remains a locally handled sentinel. */
    EnterCriticalSection(&g_state_lock);
    map_movie_handle_locked(failed_movie, smc_path);
    failed_map = find_movie(failed_movie);
    failed_controller_map = failed_map ? create_controller_locked(failed_map, 0) : NULL;
    if (!failed_map || !failed_map->decoder || !failed_controller_map)
        passed = 0;
    else
        failed_map->audio_mode = AUDIO_MODE_PICTURE;
    LeaveCriticalSection(&g_state_lock);
    if (selftest_dispatch(0x07, failed_movie, 0x12345678u, 0, 0, NULL) != 0)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    if (find_movie(failed_movie) || find_controller_for_movie(failed_movie))
        passed = 0;
    LeaveCriticalSection(&g_state_lock);

    EnterCriticalSection(&g_state_lock);
    map_movie_handle_locked(failed_movie, smc_path);
    failed_map = find_movie(failed_movie);
    failed_controller_map = failed_map ? create_controller_locked(failed_map, 0) : NULL;
    if (!failed_map || !failed_map->decoder || !failed_controller_map)
        passed = 0;
    else
        failed_map->audio_mode = AUDIO_MODE_PICTURE;
    failed_controller = failed_controller_map ? failed_controller_map->handle : 0;
    LeaveCriticalSection(&g_state_lock);
    if (selftest_dispatch(0x07, failed_movie, failed_controller, 0, 0,
                          NULL) != 1)
        passed = 0;
    EnterCriticalSection(&g_state_lock);
    if (find_movie(failed_movie) || find_controller_for_movie(failed_movie))
        passed = 0;
    reset_selftest_state_locked();
    LeaveCriticalSection(&g_state_lock);
    drain_audio_teardowns();
    return passed;
}

static HGLOBAL load_bmp_as_dib(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = GetFileSize(h, NULL);
    if (size <= 14 || size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return NULL;
    }

    BYTE header[14];
    DWORD got = 0;
    if (!ReadFile(h, header, 14, &got, NULL) || got != 14 || header[0] != 'B' || header[1] != 'M') {
        CloseHandle(h);
        return NULL;
    }

    DWORD dib_size = size - 14;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, dib_size);
    if (!hg) {
        CloseHandle(h);
        return NULL;
    }

    BYTE *dst = (BYTE *)GlobalLock(hg);
    if (!dst) {
        GlobalFree(hg);
        CloseHandle(h);
        return NULL;
    }

    DWORD read_total = 0;
    while (read_total < dib_size) {
        DWORD chunk = 0;
        DWORD want = dib_size - read_total;
        if (want > 65536) want = 65536;
        if (!ReadFile(h, dst + read_total, want, &chunk, NULL) || chunk == 0) break;
        read_total += chunk;
    }

    GlobalUnlock(hg);
    CloseHandle(h);

    if (read_total != dib_size) {
        GlobalFree(hg);
        return NULL;
    }
    return hg;
}

static void build_cache_dir(const char *movie_path, char *out, size_t out_size) {
    const char *rel = movie_path;
    if (lstrlenA(rel) > 6 && _strnicmp(rel, "Movie\\", 6) == 0) rel += 6;

    snprintf(out, out_size, "MovieCache\\%s", rel);

    char *last_dot = NULL;
    for (char *p = out; *p; p++) {
        if (*p == '.') last_dot = p;
    }
    if (last_dot) *last_dot = '\0';
}

static void log_cache_miss_once(const char *movie_path, const char *reason) {
    if (!movie_path || !movie_path[0]) return;
    if (g_log == INVALID_HANDLE_VALUE) return;

    for (int i = 0; i < g_cache_miss_log_count; i++) {
        if (lstrcmpiA(g_cache_miss_logs[i].rel_path, movie_path) == 0 &&
            lstrcmpiA(g_cache_miss_logs[i].reason, reason) == 0) {
            return;
        }
    }

    if (g_cache_miss_log_count < (int)(sizeof(g_cache_miss_logs) / sizeof(g_cache_miss_logs[0]))) {
        lstrcpynA(g_cache_miss_logs[g_cache_miss_log_count].rel_path, movie_path,
                  sizeof(g_cache_miss_logs[g_cache_miss_log_count].rel_path));
        lstrcpynA(g_cache_miss_logs[g_cache_miss_log_count].reason, reason,
                  sizeof(g_cache_miss_logs[g_cache_miss_log_count].reason));
        g_cache_miss_log_count++;
    }

    char cache_dir[CACHE_PATH_MAX];
    build_cache_dir(movie_path, cache_dir, sizeof(cache_dir));

    char buf[320];
    int written = snprintf(buf, sizeof(buf),
                           "%lu\tcache_miss\tpath=\"%s\"\tcache=\"%s\"\treason=\"%s\"\n",
                           (unsigned long)GetTickCount(), movie_path, cache_dir, reason);
    if (written > 0) log_text(buf);
}

static unsigned count_cache_frames_for_path(const char *movie_path) {
    for (int i = 0; i < g_frame_cache_count; i++) {
        if (lstrcmpiA(g_frame_caches[i].rel_path, movie_path) == 0) {
            return g_frame_caches[i].frame_count;
        }
    }

    char cache_dir[CACHE_PATH_MAX];
    build_cache_dir(movie_path, cache_dir, sizeof(cache_dir));
    if (lstrlenA(cache_dir) > CACHE_PATH_MAX - 32) return 0;

    unsigned count = 0;
    for (unsigned i = 1; i < 10000; i++) {
        char bmp_path[CACHE_PATH_MAX];
        wsprintfA(bmp_path, "%s\\frame_%04u.bmp", cache_dir, i);
        if (GetFileAttributesA(bmp_path) == INVALID_FILE_ATTRIBUTES) break;
        count = i;
    }

    if (g_frame_cache_count < (int)(sizeof(g_frame_caches) / sizeof(g_frame_caches[0]))) {
        lstrcpynA(g_frame_caches[g_frame_cache_count].rel_path, movie_path,
                  sizeof(g_frame_caches[g_frame_cache_count].rel_path));
        g_frame_caches[g_frame_cache_count].frame_count = count;
        g_frame_cache_count++;
    }
    return count;
}

static HGLOBAL make_decoded_dib(MovieMap *movie, int32_t movie_time) {
    const MovTrackInfo *video;
    const uint8_t *pixels, *palette;
    uint32_t frame = 0;
    DWORD stride, image_size, total;
    HGLOBAL dib;
    BYTE *p;
    BITMAPINFOHEADER *header;
    RGBQUAD *colors;
    uint32_t y, x;

    if (!movie || !movie->decoder) return NULL;
    video = movdec_video_info(movie->decoder);
    pixels = movdec_frame(movie->decoder, movie_time, &frame);
    palette = movdec_palette_rgb(movie->decoder);
    if (!video || !pixels || !palette) return NULL;
    stride = (video->width + 3u) & ~3u;
    image_size = stride * video->height;
    total = sizeof(BITMAPINFOHEADER) + 256u * sizeof(RGBQUAD) + image_size;
    dib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    if (!dib) return NULL;
    p = (BYTE *)GlobalLock(dib);
    if (!p) { GlobalFree(dib); return NULL; }
    header = (BITMAPINFOHEADER *)p;
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = (LONG)video->width;
    header->biHeight = (LONG)video->height;
    header->biPlanes = 1;
    header->biBitCount = 8;
    header->biCompression = BI_RGB;
    header->biSizeImage = image_size;
    colors = (RGBQUAD *)(p + sizeof(BITMAPINFOHEADER));
    for (x = 0; x < 256; x++) {
        colors[x].rgbRed = palette[x * 3 + 0];
        colors[x].rgbGreen = palette[x * 3 + 1];
        colors[x].rgbBlue = palette[x * 3 + 2];
    }
    for (y = 0; y < video->height; y++) {
        memcpy(p + sizeof(BITMAPINFOHEADER) + 256u * sizeof(RGBQUAD) +
                   (video->height - 1u - y) * stride,
               pixels + y * video->width, video->width);
    }
    GlobalUnlock(dib);
    if (g_log != INVALID_HANDLE_VALUE) {
        char buf[256];
        int written = snprintf(buf, sizeof(buf),
                               "%lu\tdecoded_PicToDIB\thandle=%08X\ttime=%08X\tframe=%lu\tpath=\"%s\"\n",
                               (unsigned long)GetTickCount(),
                               (unsigned)movie->handle, (unsigned)movie_time,
                               (unsigned long)frame, movie->rel_path);
        if (written > 0) log_text(buf);
    }
    return dib;
}

static uint32_t controller_time_locked(const ControllerMap *controller,
                                       const MovieMap *movie) {
    uint32_t scale;
    uint32_t duration;
    uint32_t elapsed;
    uint64_t time;
    if (!controller || !movie || !movie->decoder) return 0;
    scale = movdec_movie_timescale(movie->decoder);
    duration = movdec_movie_duration(movie->decoder);
    if (!scale || !duration) return 0;
    elapsed = GetTickCount() - controller->start_tick;
    time = (uint64_t)elapsed * scale / 1000u;
    if (time >= duration) return duration;
    return (uint32_t)time;
}

static void draw_controller_frame_locked(ControllerMap *controller,
                                         MovieMap *movie) {
    const MovTrackInfo *video;
    uint32_t movie_time;
    uint32_t frame;
    int new_frame;
    HGLOBAL dib;
    BYTE *p;
    BITMAPINFO *bmi;
    HDC dc = NULL;
    HWND hwnd = NULL;
    int release_dc = 0;

    if (!controller || !movie || !movie->decoder) return;
    video = movdec_video_info(movie->decoder);
    if (!video) return;
    movie_time = controller_time_locked(controller, movie);
    frame = movdec_frame_index_for_time(movie->decoder, (int32_t)movie_time);
    new_frame = controller->last_dib == NULL || controller->last_frame != frame;
    if (new_frame) {
        dib = make_decoded_dib(movie, (int32_t)movie_time);
        if (dib) {
            if (controller->last_dib) GlobalFree(controller->last_dib);
            controller->last_dib = dib;
            controller->last_frame = frame;
        }
    }

    controller->last_time = movie_time;
    if (controller->last_dib) {
        p = (BYTE *)GlobalLock(controller->last_dib);
        if (p) {
            bmi = (BITMAPINFO *)(void *)p;
            if (controller->target_hwnd &&
                IsWindow(controller->target_hwnd)) {
                hwnd = controller->target_hwnd;
            } else {
                hwnd = FindWindowA(NULL, "Gundam Tactics");
                if (hwnd) controller->target_hwnd = hwnd;
            }
            if (hwnd) {
                dc = gt_scale_get_dc(hwnd);
                if (dc) release_dc = 1;
            }
            if (!dc) dc = controller->target_hdc;
            if (dc) {
                StretchDIBits(
                    dc, controller->dst_x, controller->dst_y,
                    controller->dst_cx, controller->dst_cy,
                    0, 0, (int)video->width, (int)video->height,
                    p + sizeof(BITMAPINFOHEADER) +
                        256u * sizeof(RGBQUAD), bmi, DIB_RGB_COLORS,
                    SRCCOPY);
            }
            GlobalUnlock(controller->last_dib);
            if (release_dc) gt_scale_release_dc(hwnd, dc);
        }
    }

    if (new_frame && controller->last_frame != UINT32_MAX)
        log_controller_line("controller_frame", controller, movie_time, frame);
    if (movie_time >= movdec_movie_duration(movie->decoder) &&
        !controller->done_logged) {
        controller->done_logged = 1;
        log_controller_line("controller_done", controller, movie_time, frame);
    }
    shared_refresh_controller(controller, movie);
}

static HGLOBAL make_black_dib(unsigned width, unsigned height) {
    uint64_t stride64, image64, total64;
    DWORD stride, image_size, total;
    HGLOBAL dib;
    uint8_t *p;
    BITMAPINFOHEADER *header;
    if (!width || !height || width > 8192 || height > 8192) {
        width = 64; height = 64;
    }
    stride64 = ((uint64_t)width + 3u) & ~3u;
    image64 = stride64 * height;
    total64 = sizeof(BITMAPINFOHEADER) + 256u * sizeof(RGBQUAD) + image64;
    if (stride64 > UINT32_MAX || image64 > UINT32_MAX || total64 > UINT32_MAX)
        return NULL;
    stride = (DWORD)stride64;
    image_size = (DWORD)image64;
    total = (DWORD)total64;
    dib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    if (!dib) return NULL;
    p = (uint8_t *)GlobalLock(dib);
    if (!p) { GlobalFree(dib); return NULL; }
    header = (BITMAPINFOHEADER *)p;
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = (LONG)width;
    header->biHeight = (LONG)height;
    header->biPlanes = 1;
    header->biBitCount = 8;
    header->biCompression = BI_RGB;
    header->biSizeImage = image_size;
    (void)stride;
    GlobalUnlock(dib);
    return dib;
}

static HGLOBAL make_black_dib_for_handle(uint32_t handle) {
    MovieMap *movie = find_movie(handle);
    const MovTrackInfo *video = movie && movie->decoder ?
        movdec_video_info(movie->decoder) : NULL;
    return make_black_dib(video ? video->width : 64,
                          video ? video->height : 64);
}

static HGLOBAL make_cached_dib(uint32_t handle, int32_t movie_time) {
    MovieMap *movie = find_movie(handle);
    if (movie && movie->decoder &&
        lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0)
        return make_decoded_dib(movie, movie_time);

    const char *path = path_for_movie(handle);
    if (!path) return NULL;

    unsigned frame_count = count_cache_frames_for_path(path);
    if (!frame_count) {
        log_cache_miss_once(path, "no_frames");
        return NULL;
    }

    unsigned frame = movie_time < 0 ? 1u : (unsigned)movie_time + 1u;
    if (movie && movie->decoder)
        frame = movdec_frame_index_for_time(movie->decoder, movie_time) + 1u;
    if (frame > frame_count) frame = frame_count;
    char cache_dir[CACHE_PATH_MAX];
    char bmp_path[CACHE_PATH_MAX];
    build_cache_dir(path, cache_dir, sizeof(cache_dir));
    if (lstrlenA(cache_dir) > CACHE_PATH_MAX - 32) return NULL;
    wsprintfA(bmp_path, "%s\\frame_%04u.bmp", cache_dir, frame);

    HGLOBAL dib = load_bmp_as_dib(bmp_path);
    if (!dib) {
        log_cache_miss_once(path, "load_failed");
    }
    if (dib) {
        static uint32_t last_cache_log_handle = 0;
        static int32_t last_cache_log_time = INT32_MIN;
        static char last_cache_log_path[128];
        if (last_cache_log_handle != handle ||
            last_cache_log_time != movie_time ||
            lstrcmpiA(last_cache_log_path, path) != 0) {
            last_cache_log_handle = handle;
            last_cache_log_time = movie_time;
            lstrcpynA(last_cache_log_path, path, sizeof(last_cache_log_path));

            char buf[256];
            int written = snprintf(buf, sizeof(buf),
                                   "%lu\tcache_PicToDIB\thandle=%08X\ttime=%08X\tpath=\"%s\"\tbmp=\"%s\"\tret=%08X\n",
                                   (unsigned long)GetTickCount(),
                                   (unsigned)handle,
                                   (unsigned)movie_time,
                                   path,
                                   bmp_path,
                                   (unsigned)(uintptr_t)dib);
            if (written > 0) log_text(buf);
        }
    }
    return dib;
}

/* Frame layout after pushfl; pushal in proxy_EntryPoint:
 *   f[0]=edi f[1]=esi f[2]=ebp f[3]=esp_snapshot
 *   f[4]=ebx f[5]=edx f[6]=ecx f[7]=eax
 *   f[8]=eflags f[9]=dispatcher_ra f[10]=saved_thunk_ebx
 *   f[11]=thunk_caller_ra f[12...]=QuickTime stack args
 *
 * The game thunks look like:
 *   push ebx
 *   mov  bx, selector
 *   call [dispatcher]
 *   pop  ebx
 *   ret
 *
 * So the actual QuickTime arguments are two DWORDs past the dispatcher's
 * immediate return address.
 *
 * Return 1 if handled and f[7] contains the caller's EAX result.
 * Return 0 to tail-call the real QuickTime dispatcher.  In particular,
 * NewMovieFromFile (0x2A) must stay on the tail-call path: calling the real
 * dispatcher after restoring the saved registers would shift its stack
 * arguments by the thunk return address.
 */

static uint32_t handled_result(uint32_t *f, uint32_t result) {
    f[7] = result;
    return 1;
}

uint32_t __cdecl compat_dispatch(uint32_t *f) {
    uint32_t frame[16];
    uint32_t sel;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t bound_controller = 0;
    uint32_t bound_movie = 0;
    int bound;

    if (copy_readable_memory(f, frame, sizeof(frame)) != sizeof(frame)) return 0;
    sel = frame[4] & 0xFFFFu;
    arg0 = frame[12];
    arg1 = frame[13];
    arg2 = frame[14];
    arg3 = frame[15];

    log_selector_probe(sel, frame);
    CompatThreadState *state = get_thread_state();
    if (g_selftest_requested && g_mci_trace_enabled &&
        !g_mci_selftest_done) {
        g_mci_selftest_done = 1;
        if (g_log != INVALID_HANDLE_VALUE)
            log_text(run_mci_hook_selftest() ?
                     "mci_hook_selftest=PASS\n" :
                     "mci_hook_selftest=FAIL\n");
    }
    if (g_selftest_requested && !g_input_selftest_done) {
        g_input_selftest_done = 1;
        if (g_log != INVALID_HANDLE_VALUE) {
            log_text(run_input_time_selftest() ?
                     "input_time_selftest=PASS\n" :
                     "input_time_selftest=FAIL\n");
            log_text(gt_scale_selftest() ? "display_scale_selftest=PASS\n" :
                                           "display_scale_selftest=FAIL\n");
        }
    }
    if (g_selftest_requested && !g_selftest_done && !g_selftest_running) {
        g_selftest_done = 1;
        g_selftest_running = 1;
        int passed = run_controller_reuse_selftest();
        g_selftest_running = 0;
        if (g_log != INVALID_HANDLE_VALUE)
            log_text(passed ? "controller_reuse_selftest=PASS\n"
                            : "controller_reuse_selftest=FAIL\n");
    }

    if (sel == 0x2C) {
        clear_pending_movie_state(state);
        remember_pending_movie_path(arg0);
    }

    if (sel == 0x2A && state) {
        /* This is deliberately pre-call state only.  0x2A remains a
         * pass-through selector; the stock dispatcher writes the Movie* into
         * this slot after returning. */
        state->pending_out_slot = arg0;
        state->pending_file_handle = arg1;
        state->pending_close_seen = 0;
    }

    if (sel == 0x31 || sel == 0x38) {
        /* 0x2A returns an OSErr on this title.  Associate the path from
         * 0x2C with the concrete Movie handle supplied by the next call only
         * after the successful 0x2A -> 0x02 sequence has been observed. */
        if (state && (state->pending_movie_path[0] ||
                      state->pending_out_slot != 0)) {
            uint32_t pending_file_handle = state->pending_file_handle;
            uint32_t slot_value = 0;
            const char *drop_reason = NULL;
            int slot_readable = 0;

            if (!state->pending_close_seen) {
                drop_reason = "no_close";
            } else {
                slot_readable = state->pending_out_slot &&
                    copy_readable_memory(
                        (const void *)(uintptr_t)state->pending_out_slot,
                        &slot_value, sizeof(slot_value)) == sizeof(slot_value);
                if (!state->pending_movie_path[0] || !slot_readable ||
                    slot_value != arg0)
                    drop_reason = "slot_mismatch";
            }

            if (!drop_reason) {
                EnterCriticalSection(&g_state_lock);
                MovieMap *live_movie = find_movie(arg0);
                if (live_movie &&
                    lstrcmpiA(live_movie->rel_path,
                              state->pending_movie_path) != 0) {
                    /* A failed 0x2A can be followed by stale game data.  Do
                     * not retag a live movie, especially one with a fake
                     * controller, with the new pending path. */
                    drop_reason = "live_handle";
                } else {
                    map_movie_handle_locked(arg0, state->pending_movie_path);
                    bind_movie_file_locked(pending_file_handle, arg0);
                }
                LeaveCriticalSection(&g_state_lock);
            }

            if (drop_reason)
                drop_pending_movie_state(state, arg0, drop_reason);
            else
                clear_pending_movie_state(state);
        }
    }

    if (sel == 0x38) {
        ControllerMap *controller = NULL;
        EnterCriticalSection(&g_state_lock);
        MovieMap *movie = find_movie(arg0);
        if (movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0) {
            controller = create_controller_locked(movie, arg3);
            if (controller) {
                log_controller_line("controller_emulated", controller, 0, 0);
                log_controller_result(controller, 0x38, controller->movie_handle,
                                      controller->handle, controller->handle);
                LeaveCriticalSection(&g_state_lock);
                return handled_result(f, controller->handle);
            }
            /* A decoder-backed SMC movie must never fall through to the
             * original controller with a partially emulated state. */
            LeaveCriticalSection(&g_state_lock);
            return handled_result(f, 0);
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x31) {
        MovieMap *audio_movie = NULL;
        int restart_audio = 0;
        int reset_audio = 0;
        EnterCriticalSection(&g_state_lock);
        MovieMap *movie = find_movie(arg0);
        if (movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0) {
            reset_movie_audio_position_locked(movie);
            shared_reset_movie_clock(arg0);
            audio_movie = movie;
            if (movie->audio_mode == AUDIO_MODE_REALTIME)
                restart_audio = 1;
            else if (movie->audio_mode == AUDIO_MODE_PICTURE &&
                     movie->audio_streaming)
                reset_audio = 1;
            LeaveCriticalSection(&g_state_lock);
            if (restart_audio) restart_realtime_movie_audio(audio_movie);
            if (reset_audio) reset_movie_audio_stream(audio_movie);
            drain_audio_teardowns();
            return handled_result(f, 0);
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x2F) {
        EnterCriticalSection(&g_state_lock);
        MovieMap *movie = find_movie(arg0);
        if (movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0) {
            LeaveCriticalSection(&g_state_lock);
            return handled_result(f, 0);
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x32) {
        EnterCriticalSection(&g_state_lock);
        MovieMap *movie = find_movie(arg0);
        bound = controller_binding_in_args_locked(arg0, arg1, arg2, arg3,
                                                  &bound_controller,
                                                  &bound_movie);
        if ((movie && movie->decoder &&
             lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0) || bound) {
            update_controller_target_locked(arg0, arg1, arg2, arg3);
            LeaveCriticalSection(&g_state_lock);
            return handled_result(f, 0);
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x36 || sel == 0x06 || sel == 0x12) {
        uint32_t result = 0;
        uint32_t result_movie_handle = 0;
        uint32_t result_controller_handle = 0;
        HWND pump_hwnd = NULL;
        int handled = 0;
        int start_audio = 0;
        MovieMap *audio_movie = NULL;
        EnterCriticalSection(&g_state_lock);
        ControllerMap *controller =
            find_controller_in_args(arg0, arg1, arg2, arg3);
        MovieMap *movie = controller ? find_movie(controller->movie_handle) :
            find_movie(arg0);
        if (!controller && movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0)
            controller = find_controller_for_movie(movie->handle);
        if (movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0) {
            if (sel == 0x12) {
                result = movdec_movie_duration(movie->decoder);
                handled = 1;
            } else if (controller) {
                /* The controller clock and controller rendering are always
                 * real-time driven.  Audio mode is deliberately independent:
                 * a later GetMoviePict may switch only the audio path. */
                if (sel == 0x36) {
                    if (movie->audio_mode == AUDIO_MODE_UNSET)
                        set_movie_audio_mode_locked(movie, AUDIO_MODE_REALTIME,
                                                    "first_0x36_idle");
                    if (movie->audio_mode == AUDIO_MODE_REALTIME)
                        start_audio = 1;
                    audio_movie = movie;
                }
                draw_controller_frame_locked(controller, movie);
                result = controller->last_time;
                handled = 1;
            }
            result_movie_handle = movie->handle;
            result_controller_handle = controller ? controller->handle : 0;
        }
        if (handled) {
            pump_hwnd = controller ? controller->target_hwnd : NULL;
            log_controller_result(controller, sel, result_movie_handle,
                                  result_controller_handle, result);
            LeaveCriticalSection(&g_state_lock);
            if (sel == 0x36) maybe_pump_messages(pump_hwnd);
            if (start_audio) start_movie_audio(audio_movie);
            drain_audio_teardowns();
            return handled_result(f, result);
        }
        bound = controller_binding_in_args_locked(arg0, arg1, arg2, arg3,
                                                  &bound_controller,
                                                  &bound_movie);
        LeaveCriticalSection(&g_state_lock);
        if (bound) {
            log_controller_unhandled(sel, arg0, arg1, arg2, arg3,
                                     bound_controller, bound_movie);
            return handled_result(f, 0);
        }
    }

    if (sel == 0x37) {
        EnterCriticalSection(&g_state_lock);
        bound = controller_binding_in_args_locked(arg0, arg1, arg2, arg3,
                                                  &bound_controller,
                                                  &bound_movie);
        if (bound) {
            ControllerMap *controller = find_controller(bound_controller);
            log_controller_result(controller, 0x37, bound_movie,
                                  bound_controller, 0);
            if (controller)
                release_controller_handle_locked(bound_controller, 1);
            LeaveCriticalSection(&g_state_lock);
            drain_audio_teardowns();
            return handled_result(f, 0);
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x07) {
        EnterCriticalSection(&g_state_lock);
        bound = controller_binding_in_args_locked(arg0, arg1, arg2, arg3,
                                                  &bound_controller,
                                                  &bound_movie);
        MovieMap *movie = find_movie(arg0);
        /* Retire every locally tracked Movie, including cvid movies whose
         * decoder is NULL.  Their map/file binding must not block a later
         * QuickTime reuse of the numeric handle for an SMC movie. */
        if (movie || bound) {
            if (movie)
                release_movie_handle_locked(arg0);
            else if (bound)
                release_controller_handle_locked(bound_controller, 1);
            LeaveCriticalSection(&g_state_lock);
            drain_audio_teardowns();
            /* DisposeMovie's arg0 is a real Movie handle.  The proxy state is
             * retired locally, then the original runtime must dispose its
             * Movie object too.  A fake controller value in any argument is
             * the one exception: never pass that sentinel to QuickTime. */
            if (bound) return handled_result(f, 0);
            return 0;
        }
        LeaveCriticalSection(&g_state_lock);
    }

    if (sel == 0x02) {
        uint32_t movie_handle;
        EnterCriticalSection(&g_state_lock);
        movie_handle = movie_for_file_locked(arg0);
        if (movie_handle) release_movie_handle_locked(movie_handle);
        if (state && state->pending_file_handle == arg0) {
            state->pending_close_seen = 1;
            state->pending_file_handle = 0;
        }
        LeaveCriticalSection(&g_state_lock);
        drain_audio_teardowns();
        /* CloseMovieFile itself remains a stock QuickTime call. The local
         * cleanup above only retires any proxy binding associated with it. */
    }

    if (sel == 0x14) {
        FakePicture *picture = (FakePicture *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*picture));
        MovieMap *audio_movie = NULL;
        HWND pump_hwnd = NULL;
        int stream_audio = 0;
        if (!picture) return handled_result(f, 0);
        picture->magic = FAKE_PICTURE_MAGIC;
        picture->handle = arg0;
        picture->movie_time = (int32_t)arg1;
        EnterCriticalSection(&g_state_lock);
        MovieMap *movie = find_movie(arg0);
        ControllerMap *controller = movie ? find_controller_for_movie(arg0) : NULL;
        pump_hwnd = controller ? controller->target_hwnd : NULL;
        if (movie && movie->decoder &&
            lstrcmpA(movdec_codec(movie->decoder), "smc ") == 0 &&
            !controller) {
            /* v1.0.3 did not enter the picture-time audio path while an
             * emulated controller was alive.  Keep real-time waveOut audio
             * untouched in that case; resetting/unpreparing it while the
             * dispatcher lock is held can stall the game in WinMM. */
            if (movie->audio_mode == AUDIO_MODE_UNSET) {
                set_movie_audio_mode_locked(movie, AUDIO_MODE_PICTURE,
                                            "first_GetMoviePict");
            }
            movie->last_picture_time = arg1;
            audio_movie = movie;
            stream_audio = 1;
        }
        LeaveCriticalSection(&g_state_lock);
        maybe_pump_messages(pump_hwnd);
        if (stream_audio) stream_movie_audio(audio_movie, arg1);
        drain_audio_teardowns();
        log_event("fake_GetMoviePict", sel, arg0, arg1,
                  (uint32_t)(uintptr_t)picture);
        return handled_result(f, (uint32_t)(uintptr_t)picture);
    }

    if (sel == 0x2E || sel == 0x08) {
        FakePicture picture_snapshot;
        if (copy_readable_memory((void *)(uintptr_t)arg0, &picture_snapshot,
                                 sizeof(picture_snapshot)) != sizeof(picture_snapshot))
            return 0;
        if (picture_snapshot.magic != FAKE_PICTURE_MAGIC) return 0;
        FakePicture *picture = (FakePicture *)(uintptr_t)arg0;
        if (sel == 0x2E) {
            HGLOBAL dib;
            EnterCriticalSection(&g_state_lock);
            dib = make_cached_dib(picture_snapshot.handle, picture_snapshot.movie_time);
            if (!dib) dib = make_black_dib_for_handle(picture_snapshot.handle);
            LeaveCriticalSection(&g_state_lock);
            log_event("fake_PicToDIB", sel, picture_snapshot.handle,
                      (uint32_t)picture_snapshot.movie_time,
                      (uint32_t)(uintptr_t)dib);
            return handled_result(f, (uint32_t)(uintptr_t)dib);
        }
        picture->magic = 0;
        HeapFree(GetProcessHeap(), 0, picture);
        return handled_result(f, 0);
    }

    EnterCriticalSection(&g_state_lock);
    bound = controller_binding_in_args_locked(arg0, arg1, arg2, arg3,
                                              &bound_controller,
                                              &bound_movie);
    LeaveCriticalSection(&g_state_lock);
    if (bound) {
        log_controller_unhandled(sel, arg0, arg1, arg2, arg3,
                                 bound_controller, bound_movie);
        return handled_result(f, 0);
    }
    return 0;
}

static int WINAPI trace_StretchDIBits(HDC hdc, int x_dest, int y_dest,
                                      int w_dest, int h_dest, int x_src, int y_src,
                                      int w_src, int h_src, const VOID *bits,
                                      const BITMAPINFO *bmi, UINT usage, DWORD rop) {
    uint32_t caller = gdi_caller_va();
    const VOID *call_bits = bits;
    const BITMAPINFO *call_bmi = bmi;
    BYTE bmi_storage[GDI_FIX_BMI_STORAGE];
    BITMAPINFOHEADER bmi_header;
    BYTE *flipped_bits = NULL;
    BYTE *source_bits = NULL;
    size_t bmi_bytes = 0;
    size_t row_bytes = 0;
    size_t row_count = 0;
    size_t total_bytes = 0;

    if (g_gdi_fix_mode != GDI_FIX_OFF && gdi_fix_caller(caller) && bmi &&
        copy_readable_memory(bmi, &bmi_header, sizeof(bmi_header)) ==
            sizeof(bmi_header)) {
        bmi_bytes = gdi_fix_bmi_bytes(&bmi_header);
        if (bmi_bytes && bmi_bytes <= sizeof(bmi_storage) &&
            copy_readable_memory(bmi, bmi_storage, bmi_bytes) == bmi_bytes) {
            call_bmi = (const BITMAPINFO *)(const void *)bmi_storage;
            if (g_gdi_fix_mode == GDI_FIX_BMI_NEGATIVE) {
                ((BITMAPINFO *)(void *)bmi_storage)->bmiHeader.biHeight = -416;
            } else if (gdi_fix_layout(&bmi_header, &row_bytes, &row_count,
                                      &total_bytes) &&
                       total_bytes <= 64u * 1024u * 1024u && bits) {
                source_bits = (BYTE *)HeapAlloc(GetProcessHeap(), 0, total_bytes);
                if (source_bits &&
                    copy_readable_memory(bits, source_bits, total_bytes) == total_bytes) {
                    flipped_bits = (BYTE *)HeapAlloc(GetProcessHeap(), 0, total_bytes);
                }
                if (source_bits && flipped_bits) {
                    size_t row;
                    for (row = 0; row < row_count; row++) {
                        memcpy(flipped_bits + row * row_bytes,
                               source_bits + (row_count - 1 - row) * row_bytes,
                               row_bytes);
                    }
                    ((BITMAPINFO *)(void *)bmi_storage)->bmiHeader.biHeight =
                        (bmi_header.biHeight < 0) ? -bmi_header.biHeight : bmi_header.biHeight;
                    call_bits = flipped_bits;
                }
            }
        }
    }

    int result = p_real_StretchDIBits ?
        p_real_StretchDIBits(hdc, x_dest, y_dest, w_dest, h_dest, x_src, y_src,
                             w_src, h_src, call_bits, call_bmi, usage, rop) : 0;
    log_gdi_call("StretchDIBits", caller, hdc,
                 x_dest, y_dest, w_dest, h_dest,
                 x_src, y_src, w_src, h_src, call_bmi, rop, (uintptr_t)(uint32_t)result);
    if (flipped_bits) HeapFree(GetProcessHeap(), 0, flipped_bits);
    if (source_bits) HeapFree(GetProcessHeap(), 0, source_bits);
    return result;
}

static BOOL WINAPI trace_StretchBlt(HDC hdc, int x_dest, int y_dest,
                                    int w_dest, int h_dest, HDC src_hdc,
                                    int x_src, int y_src, int w_src, int h_src,
                                    DWORD rop) {
    BOOL result = p_real_StretchBlt ?
        p_real_StretchBlt(hdc, x_dest, y_dest, w_dest, h_dest, src_hdc,
                          x_src, y_src, w_src, h_src, rop) : FALSE;
    log_gdi_call("StretchBlt", gdi_caller_va(), hdc,
                 x_dest, y_dest, w_dest, h_dest,
                 x_src, y_src, w_src, h_src, NULL, rop, (uintptr_t)result);
    return result;
}

static BOOL WINAPI trace_BitBlt(HDC hdc, int x_dest, int y_dest,
                                int width, int height, HDC src_hdc,
                                int x_src, int y_src, DWORD rop) {
    BOOL result = p_real_BitBlt ?
        p_real_BitBlt(hdc, x_dest, y_dest, width, height, src_hdc,
                      x_src, y_src, rop) : FALSE;
    log_gdi_call("BitBlt", gdi_caller_va(), hdc,
                 x_dest, y_dest, width, height,
                 x_src, y_src, 0, 0, NULL, rop, (uintptr_t)result);
    return result;
}

static HBITMAP WINAPI trace_CreateDIBSection(HDC hdc, const BITMAPINFO *bmi,
                                             UINT usage, VOID **bits,
                                             HANDLE section, DWORD offset) {
    HBITMAP result = p_real_CreateDIBSection ?
        p_real_CreateDIBSection(hdc, bmi, usage, bits, section, offset) : NULL;
    log_gdi_call("CreateDIBSection", gdi_caller_va(), hdc,
                 0, 0, 0, 0, 0, 0, 0, 0, bmi, 0, (uintptr_t)result);
    return result;
}

static HBITMAP WINAPI trace_CreateCompatibleBitmap(HDC hdc, int width, int height) {
    HBITMAP result = p_real_CreateCompatibleBitmap ?
        p_real_CreateCompatibleBitmap(hdc, width, height) : NULL;
    log_gdi_call("CreateCompatibleBitmap", gdi_caller_va(), hdc,
                 0, 0, width, height, 0, 0, 0, 0, NULL, 0, (uintptr_t)result);
    return result;
}

static HGDIOBJ WINAPI trace_SelectObject(HDC hdc, HGDIOBJ object) {
    HGDIOBJ result = p_real_SelectObject ? p_real_SelectObject(hdc, object) : NULL;
    log_gdi_call("SelectObject", gdi_caller_va(), hdc,
                 0, 0, 0, 0, 0, 0, 0, 0, NULL, 0, (uintptr_t)result);
    return result;
}

static int WINAPI trace_SetDIBitsToDevice(HDC hdc, int x_dest, int y_dest,
                                          DWORD width, DWORD height, int x_src,
                                          int y_src, UINT start_scan, UINT lines,
                                          const VOID *bits, const BITMAPINFO *bmi,
                                          UINT usage) {
    int result = p_real_SetDIBitsToDevice ?
        p_real_SetDIBitsToDevice(hdc, x_dest, y_dest, width, height, x_src, y_src,
                                 start_scan, lines, bits, bmi, usage) : 0;
    log_gdi_call("SetDIBitsToDevice", gdi_caller_va(), hdc,
                 x_dest, y_dest, (LONG)width, (LONG)height,
                 x_src, y_src, 0, (LONG)lines, bmi, 0, (uintptr_t)(uint32_t)result);
    return result;
}

static int WINAPI trace_GetObjectA(HGDIOBJ object, int cb_buffer, LPVOID buffer) {
    uint32_t caller = gdi_caller_va();
    int result = p_real_GetObjectA ?
        p_real_GetObjectA(object, cb_buffer, buffer) : 0;
    log_getobject_dibsection(object, cb_buffer, buffer, result, caller);
    return result;
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
        "jmp   *_p_EntryPoint           \n\t"
        ::: "memory"
    );
}

#define FWD(name) \
__attribute__((naked)) void proxy_##name(void) { \
    __asm__ volatile ("jmp *_p_" #name "\n\t" ::: "memory"); \
}

FWD(Flip16)
FWD(Flip16Many)
FWD(Flip32)
FWD(Flip32Many)
FWD(FreeMemory)
FWD(GetMemory)
FWD(ReallocateMemory)
FWD(VidWindowHook)

#define ORD(N) \
__attribute__((naked)) void proxy_ord_##N(void) { \
    __asm__ volatile ("jmp *_p_ord_" #N "\n\t" ::: "memory"); \
}

ORD(505) ORD(506) ORD(507) ORD(508) ORD(509)
ORD(510) ORD(511) ORD(512) ORD(513) ORD(514)
ORD(515) ORD(516) ORD(517) ORD(518) ORD(519)
ORD(520) ORD(521) ORD(522) ORD(523) ORD(524)
ORD(525) ORD(526) ORD(527) ORD(528) ORD(529)

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
    DWORD length = GetEnvironmentVariableA("QTIM_COMPAT_TRACE", value, sizeof(value));
    if (length && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                   value[0] == 't' || value[0] == 'T')) return 1;
    if (!build_trace_ini_path(self, ini_path, sizeof(ini_path))) return 0;
    return GetPrivateProfileIntA("trace", "enabled", 0, ini_path) != 0;
}

static int mci_trace_requested(HMODULE self) {
    char value[16];
    char ini_path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("QTIM_MCI_TRACE", value,
                                           sizeof(value));
    if (length && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                   value[0] == 't' || value[0] == 'T')) return 1;
    if (!build_trace_ini_path(self, ini_path, sizeof(ini_path))) return 0;
    return GetPrivateProfileIntA("trace", "mci", 0, ini_path) != 0;
}

/* On by default; QTIM_INPUT_FIX=0 or [input] click_fix=0 restores the
 * original click timing. */
static int input_fix_requested(HMODULE self) {
    char value[16];
    char ini_path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("QTIM_INPUT_FIX", value,
                                           sizeof(value));
    if (length && length < sizeof(value))
        return !(value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
                 value[0] == 'f' || value[0] == 'F');
    if (!build_trace_ini_path(self, ini_path, sizeof(ini_path))) return 1;
    return GetPrivateProfileIntA("input", "click_fix", 1, ini_path) != 0;
}

/* On by default; QTIM_MIDI_LOOP_FIX=0 or [audio] midi_loop_fix=0 lets the
 * BGM play its silent tail as Windows 11 does without the patch. */
static int midi_loop_fix_requested(HMODULE self) {
    char value[16];
    char ini_path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("QTIM_MIDI_LOOP_FIX", value,
                                           sizeof(value));
    if (length && length < sizeof(value))
        return !(value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
                 value[0] == 'f' || value[0] == 'F');
    if (!build_trace_ini_path(self, ini_path, sizeof(ini_path))) return 1;
    return GetPrivateProfileIntA("audio", "midi_loop_fix", 1, ini_path) != 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_tls_index = TlsAlloc();
        if (g_tls_index == TLS_OUT_OF_INDEXES) return FALSE;
        g_selftest_requested = GetEnvironmentVariableA(
            "QTIM_COMPAT_SELFTEST", NULL, 0) != 0;
        InitializeCriticalSection(&g_state_lock);
        InitializeCriticalSection(&g_log_lock);
        g_locks_ready = 1;
        g_shared = gt_controller_shared_open(&g_shared_mapping);
        g_trace_enabled = trace_requested(self);
        g_mci_trace_enabled = g_trace_enabled && mci_trace_requested(self);
        g_gdi_trace_enabled = gdi_trace_requested();
        g_gdi_fix_mode = gdi_fix_requested();
        g_gdi_fix_callers = gdi_fix_callers_requested();
        if (g_trace_enabled) {
            g_log = CreateFileA(
                "qtim_compat_trace.log",
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
        }
        if (g_gdi_trace_enabled) {
            g_gdi_log = CreateFileA(
                "qtim_gdi_trace.log",
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
        }

        g_real = LoadLibraryA("QTIM32R.DLL");
        if (!g_real) {
            log_text("FATAL LoadLibraryA(QTIM32R.DLL) failed\n");
            return FALSE;
        }

        p_EntryPoint       = GetProcAddress(g_real, "_EntryPoint");
        p_Flip16           = GetProcAddress(g_real, "Flip16");
        p_Flip16Many       = GetProcAddress(g_real, "Flip16Many");
        p_Flip32           = GetProcAddress(g_real, "Flip32");
        p_Flip32Many       = GetProcAddress(g_real, "Flip32Many");
        p_FreeMemory       = GetProcAddress(g_real, "FreeMemory");
        p_GetMemory        = GetProcAddress(g_real, "GetMemory");
        p_ReallocateMemory = GetProcAddress(g_real, "ReallocateMemory");
        p_VidWindowHook    = GetProcAddress(g_real, "VidWindowHook");

#define RESOLVE_ORD(N) p_ord_##N = GetProcAddress(g_real, (LPCSTR)(uintptr_t)N)
        RESOLVE_ORD(505); RESOLVE_ORD(506); RESOLVE_ORD(507); RESOLVE_ORD(508); RESOLVE_ORD(509);
        RESOLVE_ORD(510); RESOLVE_ORD(511); RESOLVE_ORD(512); RESOLVE_ORD(513); RESOLVE_ORD(514);
        RESOLVE_ORD(515); RESOLVE_ORD(516); RESOLVE_ORD(517); RESOLVE_ORD(518); RESOLVE_ORD(519);
        RESOLVE_ORD(520); RESOLVE_ORD(521); RESOLVE_ORD(522); RESOLVE_ORD(523); RESOLVE_ORD(524);
        RESOLVE_ORD(525); RESOLVE_ORD(526); RESOLVE_ORD(527); RESOLVE_ORD(528); RESOLVE_ORD(529);
#undef RESOLVE_ORD

        if (!p_EntryPoint) {
            log_text("FATAL _EntryPoint missing\n");
            return FALSE;
        }
        if (g_gdi_trace_enabled || g_gdi_fix_mode != GDI_FIX_OFF) {
            log_gdi_fix_mode();
            if (!install_gdi_iat_hooks()) {
                log_text("FATAL GDI IAT hook installation failed\n");
                return FALSE;
            }
        }
        g_self_module = self;
        g_midi_loop_fix_enabled = midi_loop_fix_requested(self);
        if (g_mci_trace_enabled || g_midi_loop_fix_enabled) {
            if (!install_mci_iat_hooks()) {
                if (g_mci_trace_enabled) {
                    log_text("FATAL MCI IAT hook installation failed\n");
                    return FALSE;
                }
                g_midi_loop_fix_enabled = 0;
                log_text("midi_loop_fix=off\treason=iat_patch_failed\n");
            } else if (!p_real_mciSendCommandA) {
                g_midi_loop_fix_enabled = 0;
                log_text("midi_loop_fix=off\treason=import_missing\n");
            } else {
                log_text(g_midi_loop_fix_enabled ? "midi_loop_fix=on\n" :
                         "midi_loop_fix=off\treason=disabled\n");
            }
        } else {
            log_text("midi_loop_fix=off\treason=disabled\n");
        }
        if (input_fix_requested(self))
            g_input_fix_enabled = install_input_iat_hooks();
        else
            log_text("input_fix=off\treason=disabled\n");
        {
            char ini_path[MAX_PATH];
            gt_scale_attach(build_trace_ini_path(self, ini_path,
                                                 sizeof(ini_path))
                                ? ini_path : NULL,
                            g_real, log_text);
        }
        log_text(g_trace_enabled ? "qtim_compat_proxy attached\n" : "qtim_compat_proxy attached (trace off)\n");
    } else if (reason == DLL_THREAD_DETACH) {
        free_thread_state();
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Scaling hooks chain on top of the input hooks: undo them first. */
        gt_scale_detach(reserved != NULL);
        if (g_input_iat_hook_count && !restore_input_iat_hooks()) {
            log_text("FATAL input IAT hook restoration failed\n");
        }
        if (g_mci_iat_hook_count && !restore_mci_iat_hooks()) {
            log_text("FATAL MCI IAT hook restoration failed\n");
        }
        if (g_gdi_iat_hook_count && !restore_gdi_iat_hooks()) {
            log_text("FATAL GDI IAT hook restoration failed\n");
        }
        if (g_locks_ready) EnterCriticalSection(&g_state_lock);
        while (g_controller_count > 0)
            release_controller_at_locked(g_controller_count - 1, 0);
        for (int i = 0; i < g_movie_count; i++) release_movie(&g_movies[i]);
        g_movie_count = 0;
        if (g_locks_ready) LeaveCriticalSection(&g_state_lock);
        for (int retry = 0; retry < 3; retry++) drain_audio_teardowns();
        gt_controller_shared_close(&g_shared_mapping, &g_shared);
        if (g_log != INVALID_HANDLE_VALUE) {
            log_text("qtim_compat_proxy detached\n");
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
        if (g_gdi_log != INVALID_HANDLE_VALUE) {
            gdi_log_text("qtim_gdi_trace detached\n");
            CloseHandle(g_gdi_log);
            g_gdi_log = INVALID_HANDLE_VALUE;
        }
        if (g_real) {
            FreeLibrary(g_real);
            g_real = NULL;
        }
        free_thread_state();
        if (g_tls_index != TLS_OUT_OF_INDEXES) {
            TlsFree(g_tls_index);
            g_tls_index = TLS_OUT_OF_INDEXES;
        }
        if (g_locks_ready) {
            DeleteCriticalSection(&g_log_lock);
            DeleteCriticalSection(&g_state_lock);
            g_locks_ready = 0;
        }
    }
    return TRUE;
}

