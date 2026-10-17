/* Observe the real 32-bit MCI sequencer's outgoing short MIDI messages.
 * Hooks only this process's mciseq.dll IAT, retains/forwards every message,
 * and restores the slots before exit. Does not load or patch the game.
 * Build: gcc -m32 -O2 -Wall -Wextra mci_restart_probe.c -lwinmm -o mci_restart_probe.exe
 * Usage: mci_restart_probe.exe path.mid [quick|natural] [from|reopen]
 * Default: play 1s, seek to end, then use game's SEEK_TO_START + PLAY path.
 * natural: wait for the real end (up to 240s), then use that same path.
 * from: add MCI_FROM with dwFrom=0 to the replay as a separate experiment.
 * reopen: close/reopen the same MIDI before replay, as a separate experiment.
 * MCI length/position retain the driver's default time units, not milliseconds.
 * Output CSV is stdout; audio is played through the normal MIDI device.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef MMRESULT (WINAPI *ShortFn)(HMIDIOUT, DWORD);
typedef MMRESULT (WINAPI *LongFn)(HMIDIOUT, LPMIDIHDR, UINT);
typedef struct Row { int phase; double ms; DWORD message; MMRESULT result; } Row;
static ShortFn real_short;
static LongFn real_long;
typedef struct LongRow { int phase; double ms; DWORD size; BYTE data[512]; } LongRow;
static LongRow long_rows[16384];
static unsigned long_count;
static volatile LONG phase;
static LARGE_INTEGER frequency, origin;
static CRITICAL_SECTION lock;
static Row rows[16384];
static unsigned row_count;

static MMRESULT WINAPI traced_short(HMIDIOUT device, DWORD message) {
    LARGE_INTEGER now;
    int current = phase;
    QueryPerformanceCounter(&now);
    MMRESULT result = real_short(device, message);
    EnterCriticalSection(&lock);
    if (row_count < sizeof(rows)/sizeof(rows[0])) {
        Row *r = &rows[row_count++];
        r->phase = current;
        r->ms = (now.QuadPart - origin.QuadPart) * 1000.0 / frequency.QuadPart;
        r->message = message;
        r->result = result;
    }
    LeaveCriticalSection(&lock);
    return result;
}

static MMRESULT WINAPI traced_long(HMIDIOUT device, LPMIDIHDR header, UINT size) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&lock);
    if (long_count < sizeof(long_rows)/sizeof(long_rows[0])) {
        LongRow *r = &long_rows[long_count++];
        r->phase = phase;
        r->ms = (now.QuadPart - origin.QuadPart) * 1000.0 / frequency.QuadPart;
        r->size = header->dwBufferLength;
        memcpy(r->data, header->lpData, r->size < sizeof(r->data) ? r->size : sizeof(r->data));
    }
    LeaveCriticalSection(&lock);
    return real_long(device, header, size);
}

static void **find_slot(HMODULE module, const char *wanted) {
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    if (!rva) return NULL;
    for (; desc->Name; desc++) {
        if (!desc->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *name = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (!strcmp((char *)name->Name, wanted))
                return (void **)&slots->u1.Function;
        }
    }
    return NULL;
}

static int write_slot(void **slot, void *value) {
    DWORD old, ignored;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return 0;
    *slot = value;
    return VirtualProtect(slot, sizeof(*slot), old, &ignored) != 0;
}

static int command(MCIDEVICEID device, UINT cmd, DWORD_PTR flags, DWORD_PTR param) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    MCIERROR error = mciSendCommandA(device, cmd, flags, param);
    QueryPerformanceCounter(&t1);
    printf("# command,phase=%ld,cmd=%04x,flags=%08lx,error=%lu,at_ms=%.3f,took_ms=%.3f\n",
           phase, cmd, (unsigned long)flags, (unsigned long)error,
           (t0.QuadPart - origin.QuadPart) * 1000.0 / frequency.QuadPart,
           (t1.QuadPart - t0.QuadPart) * 1000.0 / frequency.QuadPart);
    return error == 0;
}

/* Timing of mciseq's other imports, to see where MCI_OPEN spends its time. */
static double qpc_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - origin.QuadPart) * 1000.0 / frequency.QuadPart;
}
#define TIMED_LOG(name, t0, extra) \
    printf("# import,%s,at_ms=%.3f,took_ms=%.3f,%s\n", name, t0, qpc_ms() - t0, extra)
typedef MMRESULT (WINAPI *MidiOpenFn)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *MidiCloseFn)(HMIDIOUT);
typedef MMRESULT (WINAPI *MidiCacheFn)(HMIDIOUT, UINT, LPWORD, UINT);
typedef VOID (WINAPI *SleepFn)(DWORD);
typedef HMMIO (WINAPI *MmioOpenFn)(LPWSTR, LPMMIOINFO, DWORD);
typedef UINT (WINAPI *TaskCreateFn)(LPVOID, HANDLE *, DWORD_PTR);
static MidiOpenFn real_mopen; static MidiCloseFn real_mclose;
static MidiCacheFn real_mcache, real_mdrum; static SleepFn real_sleep;
static MmioOpenFn real_mmio; static TaskCreateFn real_task;
static MMRESULT WINAPI t_mopen(LPHMIDIOUT h, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD f) {
    double t0 = qpc_ms(); MMRESULT r = real_mopen(h, id, cb, inst, f);
    char x[48]; snprintf(x, sizeof x, "dev=%d,ret=%u", (int)id, r); TIMED_LOG("midiOutOpen", t0, x); return r; }
static MMRESULT WINAPI t_mclose(HMIDIOUT h) {
    double t0 = qpc_ms(); MMRESULT r = real_mclose(h); TIMED_LOG("midiOutClose", t0, ""); return r; }
static MMRESULT WINAPI t_mcache(HMIDIOUT h, UINT a, LPWORD b, UINT c) {
    double t0 = qpc_ms(); MMRESULT r = real_mcache(h, a, b, c); TIMED_LOG("midiOutCachePatches", t0, ""); return r; }
static MMRESULT WINAPI t_mdrum(HMIDIOUT h, UINT a, LPWORD b, UINT c) {
    double t0 = qpc_ms(); MMRESULT r = real_mdrum(h, a, b, c); TIMED_LOG("midiOutCacheDrumPatches", t0, ""); return r; }
static int fast_sleep;
static volatile LONG sleep_calls;
static VOID WINAPI t_sleep(DWORD ms) {
    InterlockedIncrement(&sleep_calls);
    if (fast_sleep && ms == 4) { real_sleep(0); return; }
    double t0 = qpc_ms(); real_sleep(ms); char x[32]; snprintf(x, sizeof x, "arg=%lu", ms);
    if (ms && !fast_sleep) TIMED_LOG("Sleep", t0, x); }
static HMMIO WINAPI t_mmio(LPWSTR n, LPMMIOINFO i, DWORD f) {
    double t0 = qpc_ms(); HMMIO r = real_mmio(n, i, f); TIMED_LOG("mmioOpenW", t0, ""); return r; }
static UINT WINAPI t_task(LPVOID a, HANDLE *b, DWORD_PTR c) {
    double t0 = qpc_ms(); UINT r = real_task(a, b, c); TIMED_LOG("mmTaskCreate", t0, ""); return r; }

static void **timing_slots[7];
static void *timing_reals[7];
static MCIERROR spare_error;
static HANDLE spare_opened_event, worker_exit_event;

/* Opens the spare, then stays alive until the probe ends when keep_worker
 * is set: devices opened by a thread that exits are closed with it. */
static int keep_worker;
static DWORD WINAPI open_spare_thread(LPVOID param) {
    spare_error = mciSendCommandA(0, MCI_OPEN,
                                  MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS,
                                  (DWORD_PTR)param);
    SetEvent(spare_opened_event);
    if (keep_worker) WaitForSingleObject(worker_exit_event, INFINITE);
    return 0;
}

static void position(MCIDEVICEID device, const char *label) {
    MCI_STATUS_PARMS status = {0};
    status.dwItem = MCI_STATUS_POSITION;
    MCIERROR error = mciSendCommandA(device, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&status);
    printf("# position,%s,value=%lu,error=%lu\n", label,
           (unsigned long)status.dwReturn, (unsigned long)error);
}

int main(int argc, char **argv) {
    char module_path[MAX_PATH];
    MCI_OPEN_PARMSA open = {0};
    MCI_OPEN_PARMSA spare = {0};
    int spare_opened = 0;
    HANDLE worker = NULL;
    MCI_PLAY_PARMS play = {0};
    MCI_SEEK_PARMS seek = {0};
    MCI_STATUS_PARMS status = {0};
    int ok = 0, opened = 0;
    void **slot = NULL;
    void **long_slot = NULL;
    HMODULE seq = NULL;
    if (argc < 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&lock);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&origin);
    GetSystemDirectoryA(module_path, MAX_PATH);
    strcat(module_path, "\\mciseq.dll");
    seq = LoadLibraryA(module_path);
    if (!seq) { printf("# load_failed,%lu\n", GetLastError()); goto done; }
    open.lpstrDeviceType = "sequencer";
    open.lpstrElementName = argv[1];
    if (!command(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT, (DWORD_PTR)&open)) goto done;
    opened = 1;
    printf("# device_id,open,%u\n", (unsigned)open.wDeviceID);
    slot = find_slot(seq, "midiOutShortMsg");
    if (!slot) { printf("# no_midi_slot\n"); goto done; }
    real_short = (ShortFn)*slot;
    if (!write_slot(slot, (void *)traced_short)) goto done;
    long_slot = find_slot(seq, "midiOutLongMsg");
    if (!long_slot) goto done;
    real_long = (LongFn)*long_slot;
    if (!write_slot(long_slot, (void *)traced_long)) goto done;
    fast_sleep = getenv("PROBE_FAST_SLEEP") != NULL;
    if (getenv("PROBE_IMPORT_TIMING") || fast_sleep) {
        static const char *names[] = {"midiOutOpen", "midiOutClose", "midiOutCachePatches",
                                      "midiOutCacheDrumPatches", "Sleep", "mmioOpenW",
                                      "mmTaskCreate"};
        void *hooks[] = {(void *)t_mopen, (void *)t_mclose, (void *)t_mcache,
                         (void *)t_mdrum, (void *)t_sleep, (void *)t_mmio, (void *)t_task};
        void **reals[] = {(void **)&real_mopen, (void **)&real_mclose, (void **)&real_mcache,
                          (void **)&real_mdrum, (void **)&real_sleep, (void **)&real_mmio,
                          (void **)&real_task};
        for (int i = 0; i < 7; i++) {
            timing_slots[i] = find_slot(seq, names[i]);
            if (!timing_slots[i]) continue;
            *reals[i] = *timing_slots[i];
            timing_reals[i] = *timing_slots[i];
            if (!write_slot(timing_slots[i], hooks[i])) timing_slots[i] = NULL;
        }
    }
    status.dwItem = MCI_STATUS_LENGTH;
    if (!command(open.wDeviceID, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&status)) goto done;
    printf("# length_default_units,%lu\n", (unsigned long)status.dwReturn);
    phase = 1;
    if (!command(open.wDeviceID, MCI_PLAY, 0, (DWORD_PTR)&play)) goto done;
    if (argc > 3 && (!strcmp(argv[3], "spare") || !strcmp(argv[3], "sparethread") ||
                     !strcmp(argv[3], "sparekeep"))) {
        /* A second device on the same file, opened while the first plays;
         * sparethread opens it on a worker thread that then exits,
         * sparekeep on one that stays alive. */
        spare.lpstrDeviceType = "sequencer";
        spare.lpstrElementName = argv[1];
        spare.lpstrAlias = "probe_spare";
        if (strcmp(argv[3], "spare")) {
            keep_worker = !strcmp(argv[3], "sparekeep");
            spare_opened_event = CreateEventA(NULL, TRUE, FALSE, NULL);
            worker_exit_event = CreateEventA(NULL, TRUE, FALSE, NULL);
            worker = CreateThread(NULL, 0, open_spare_thread, &spare, 0, NULL);
            if (!worker) goto done;
            WaitForSingleObject(spare_opened_event, INFINITE);
            if (!keep_worker) WaitForSingleObject(worker, INFINITE);
            printf("# spare_thread_open,keep=%d,error=%lu\n", keep_worker,
                   (unsigned long)spare_error);
            if (spare_error) goto done;
        } else if (!command(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_OPEN_ALIAS,
                            (DWORD_PTR)&spare)) goto done;
        spare_opened = 1;
        printf("# device_id,spare,%u\n", (unsigned)spare.wDeviceID);
    }
    if (argc > 2 && !strcmp(argv[2], "natural")) {
        DWORD start = GetTickCount();
        do {
            Sleep(100);
            status.dwItem = MCI_STATUS_MODE;
            if (mciSendCommandA(open.wDeviceID, MCI_STATUS, MCI_STATUS_ITEM,
                                (DWORD_PTR)&status)) goto done;
        } while (status.dwReturn != MCI_MODE_STOP && GetTickCount() - start < 240000);
        if (status.dwReturn != MCI_MODE_STOP) goto done;
    } else {
        Sleep(1100);
        phase = 0;
        if (!command(open.wDeviceID, MCI_SEEK, MCI_SEEK_TO_END, (DWORD_PTR)&seek)) goto done;
    }
    position(open.wDeviceID, "before_rewind");
    phase = 2;
    if (argc > 3 && !strcmp(argv[3], "stopfirst") &&
        !command(open.wDeviceID, MCI_STOP, 0, 0)) goto done;
    if (!command(open.wDeviceID, MCI_SEEK, MCI_SEEK_TO_START, (DWORD_PTR)&seek)) goto done;
    position(open.wDeviceID, "after_rewind");
    if (argc > 3 && !strcmp(argv[3], "port")) {
        MCI_SEQ_SET_PARMS set = {0};
        set.dwPort = MIDI_MAPPER;
        if (!command(open.wDeviceID, MCI_SET, MCI_SEQ_SET_PORT, (DWORD_PTR)&set)) goto done;
    }
    if (argc > 3 && !strcmp(argv[3], "fmt")) {
        MCI_SET_PARMS set = {0};
        set.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
        if (!command(open.wDeviceID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&set)) goto done;
        set.dwTimeFormat = MCI_SEQ_FORMAT_SONGPTR;
        if (!command(open.wDeviceID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&set)) goto done;
    }
    if (spare_opened) {
        if (!command(open.wDeviceID, MCI_CLOSE, 0, 0)) goto done;
        open.wDeviceID = spare.wDeviceID;
        spare_opened = 0;
    }
    if (argc > 3 && !strcmp(argv[3], "reopen")) {
        if (!command(open.wDeviceID, MCI_CLOSE, 0, 0)) goto done;
        opened = 0;
        if (!command(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT, (DWORD_PTR)&open)) goto done;
        opened = 1;
        printf("# device_id,reopen,%u\n", (unsigned)open.wDeviceID);
    }
    if (!command(open.wDeviceID, MCI_PLAY,
                 argc > 3 && !strcmp(argv[3], "from") ? MCI_FROM : 0,
                 (DWORD_PTR)&play)) goto done;
    Sleep(1100);
    ok = 1;
done:
    phase = 0;
    if (spare_opened) command(spare.wDeviceID, MCI_CLOSE, 0, 0);
    if (opened) command(open.wDeviceID, MCI_CLOSE, 0, 0);
    if (worker) {
        SetEvent(worker_exit_event);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    }
    if (slot && real_short && !write_slot(slot, (void *)real_short)) {
        printf("# restore_failed\n"); ok = 0;
    }
    if (long_slot && real_long && !write_slot(long_slot, (void *)real_long)) ok = 0;
    for (int i = 0; i < 7; i++)
        if (timing_slots[i] && !write_slot(timing_slots[i], timing_reals[i])) ok = 0;
    printf("phase,absolute_ms,message,result\n");
    for (unsigned i = 0; i < row_count; i++)
        printf("%d,%.3f,%06lx,%u\n", rows[i].phase, rows[i].ms,
               (unsigned long)(rows[i].message & 0xFFFFFF), rows[i].result);
    for (unsigned i = 0; i < long_count; i++) {
        LongRow *r = &long_rows[i];
        printf("# long,%d,%.3f,%lu,", r->phase, r->ms, (unsigned long)r->size);
        for (unsigned j = 0; j < r->size && j < sizeof(r->data); j++) printf("%02x", r->data[j]);
        printf("\n");
    }
    if (row_count == sizeof(rows)/sizeof(rows[0]) ||
        long_count == sizeof(long_rows)/sizeof(long_rows[0])) ok = 0;
    printf("# sleep_calls,%ld\n", sleep_calls);
    printf("# result,%s,rows=%u,long_packets=%u\n", ok ? "PASS" : "FAIL", row_count, long_count);
    if (seq) FreeLibrary(seq);
    DeleteCriticalSection(&lock);
    return ok ? 0 : 1;
}
