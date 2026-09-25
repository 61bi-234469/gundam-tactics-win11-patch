#ifndef GT_CONTROLLER_SHARED_H
#define GT_CONTROLLER_SHARED_H

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GT_CONTROLLER_SHARED_NAME_PREFIX "Local\\GundamTacticsQTIMController_"
#define GT_CONTROLLER_SHARED_MAGIC ((uint32_t)0x47544332u) /* "GTC2" */
#define GT_CONTROLLER_FAKE_HANDLE_BASE ((uint32_t)0x8088FFFFu)

/* Fake controllers advance by 0x10000, preserving the low 16-bit value
 * rejected by the original Component Manager. Movie handles are ordinary
 * QuickTime pointers and must never be classified by this predicate. */
static __inline int gt_controller_fake_handle_format(uint32_t value) {
    return value >= GT_CONTROLLER_FAKE_HANDLE_BASE &&
           (value & 0xFFFFu) == 0xFFFFu;
}

/* The journal is deliberately larger than the QTIM controller tombstone
 * table. A movie binding can generate bind/unbind plus controller create and
 * dispose events, so CMGR can reconstruct every fake handle even when its
 * first selector arrives after the controller has already been disposed. */
#define GT_CONTROLLER_JOURNAL_CAPACITY ((uint32_t)131072u)

enum {
    GT_CONTROLLER_EVENT_NONE = 0,
    GT_CONTROLLER_EVENT_MOVIE_BIND = 1,
    GT_CONTROLLER_EVENT_MOVIE_UNBIND = 2,
    GT_CONTROLLER_EVENT_CONTROLLER_CREATE = 3,
    GT_CONTROLLER_EVENT_CONTROLLER_DISPOSE = 4
};

typedef struct GtControllerJournalEntry {
    uint32_t generation;
    uint32_t event_type;
    uint32_t movie_handle;
    uint32_t controller_handle;
} GtControllerJournalEntry;

/* QTIM32.DLL and CMGR32.DLL are separate modules, but both dispatchers run
 * inside the same game process. This mapping is only the clock/journal
 * hand-off for the emulated SMC controller; movie/frame ownership remains in
 * qtim_compat_proxy.c. The sequence is a process-local seqlock: QTIM writes
 * the complete state and one journal entry between odd/even values, while
 * CMGR accepts only an even, unchanged sequence. */
typedef struct GtControllerSharedState {
    uint32_t magic;
    uint32_t owner_pid;
    volatile LONG sequence;
    uint32_t movie_handle;
    uint32_t controller_handle;
    uint32_t start_tick;
    uint32_t timescale;
    uint32_t duration;
    LONG active;
    uint32_t journal_generation;
    GtControllerJournalEntry journal[GT_CONTROLLER_JOURNAL_CAPACITY];
} GtControllerSharedState;

typedef struct GtControllerSharedSnapshot {
    uint32_t magic;
    uint32_t owner_pid;
    uint32_t movie_handle;
    uint32_t controller_handle;
    uint32_t start_tick;
    uint32_t timescale;
    uint32_t duration;
    LONG active;
    uint32_t journal_generation;
} GtControllerSharedSnapshot;

static __inline int gt_controller_shared_name(char *name, size_t name_size) {
    int written;
    if (!name || name_size == 0) return 0;
    written = snprintf(name, name_size, "%s%lu",
                       GT_CONTROLLER_SHARED_NAME_PREFIX,
                       (unsigned long)GetCurrentProcessId());
    return written >= 0 && (size_t)written < name_size;
}

static __inline GtControllerSharedState *gt_controller_shared_open(
    HANDLE *mapping_out) {
    HANDLE mapping;
    GtControllerSharedState *state;
    char name[96];
    DWORD owner_pid = GetCurrentProcessId();

    if (!gt_controller_shared_name(name, sizeof(name))) return NULL;
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                 0, sizeof(GtControllerSharedState), name);
    if (!mapping) return NULL;
    state = (GtControllerSharedState *)MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(GtControllerSharedState));
    if (!state) {
        CloseHandle(mapping);
        return NULL;
    }
    if (state->magic != GT_CONTROLLER_SHARED_MAGIC ||
        state->owner_pid != owner_pid) {
        memset(state, 0, sizeof(*state));
        state->magic = GT_CONTROLLER_SHARED_MAGIC;
        state->owner_pid = owner_pid;
    }
    if (mapping_out) *mapping_out = mapping;
    return state;
}

static __inline void gt_controller_shared_close(
    HANDLE *mapping, GtControllerSharedState **state) {
    if (state && *state) {
        UnmapViewOfFile(*state);
        *state = NULL;
    }
    if (mapping && *mapping) {
        CloseHandle(*mapping);
        *mapping = NULL;
    }
}

static __inline void gt_controller_shared_begin_write(
    GtControllerSharedState *state) {
    if (!state) return;
    InterlockedIncrement(&state->sequence);
    MemoryBarrier();
}

static __inline void gt_controller_shared_end_write(
    GtControllerSharedState *state) {
    if (!state) return;
    MemoryBarrier();
    InterlockedIncrement(&state->sequence);
}

static __inline int gt_controller_shared_read_snapshot(
    GtControllerSharedState *state, GtControllerSharedSnapshot *snapshot) {
    LONG before;
    LONG after;
    int tries;
    if (!state || !snapshot) return 0;
    for (tries = 0; tries < 32; tries++) {
        before = InterlockedCompareExchange(&state->sequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        snapshot->magic = state->magic;
        snapshot->owner_pid = state->owner_pid;
        snapshot->movie_handle = state->movie_handle;
        snapshot->controller_handle = state->controller_handle;
        snapshot->start_tick = state->start_tick;
        snapshot->timescale = state->timescale;
        snapshot->duration = state->duration;
        snapshot->active = state->active;
        snapshot->journal_generation = state->journal_generation;
        MemoryBarrier();
        after = InterlockedCompareExchange(&state->sequence, 0, 0);
        if (before == after && !(after & 1) &&
            snapshot->magic == GT_CONTROLLER_SHARED_MAGIC &&
            snapshot->owner_pid == GetCurrentProcessId()) return 1;
    }
    return 0;
}

static __inline int gt_controller_shared_read_event(
    GtControllerSharedState *state, uint32_t generation,
    GtControllerJournalEntry *entry) {
    LONG before;
    LONG after;
    uint32_t index;
    int tries;
    if (!state || !entry || generation == 0) return 0;
    index = (generation - 1u) % GT_CONTROLLER_JOURNAL_CAPACITY;
    for (tries = 0; tries < 32; tries++) {
        before = InterlockedCompareExchange(&state->sequence, 0, 0);
        if (before & 1) continue;
        MemoryBarrier();
        *entry = state->journal[index];
        MemoryBarrier();
        after = InterlockedCompareExchange(&state->sequence, 0, 0);
        if (before == after && !(after & 1) &&
            state->magic == GT_CONTROLLER_SHARED_MAGIC &&
            state->owner_pid == GetCurrentProcessId() &&
            entry->generation == generation) return 1;
    }
    return 0;
}

#endif
