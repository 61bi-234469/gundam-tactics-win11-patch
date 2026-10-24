/* Isolate the proxy's movie-audio teardown, without loading the game.
 * Includes the production implementation, so open/write/detach/teardown are
 * the proxy's own functions.  Run each case in its own process:
 *   stable        raw WinMM baseline (header kept in place: reset/unprepare/close)
 *   detached      device torn down while the track is still queued
 *   detached_done device torn down after the track finished (WHDR_DONE)
 *   restart       GoToBeginning restart (restart_realtime_movie_audio), then teardown
 *   slot_reuse    next movie opens in the same g_movies slot before the old
 *                 teardown runs; the old teardown must not touch the new header
 *   paused        device paused (window move/size loop), then torn down
 *   open_fail     waveOutOpen fails: nothing may be kept or counted
 *   queued        teardown through the queue and drain_audio_teardowns
 * Uses silent PCM only.  Exit 0 = pass, 1 = defect reproduced, 3 = setup or
 * API failure, 4 = completion wait timed out.  Run with a time limit.
 * Build from the repository root with 32-bit MinGW:
 * gcc -m32 -O2 -Wall -Wextra -static-libgcc -o <output.exe>
 *   work/tools/wave_header_lifetime_probe.c work/qtim_proxy/movdec.c
 *   work/qtim_proxy/display_scale.c work/qtim_proxy/menu_tools.c
 *   work/qtim_proxy/startup_diag.c -luser32 -lgdi32 -lwinmm
 */
#include "../qtim_proxy/qtim_compat_proxy.c"

enum { PCM_BYTES = 11025 * 5 }; /* 5 s at 11025 Hz, 8-bit mono */
static BYTE g_pcm[PCM_BYTES];

static int checked(const char *step, MMRESULT result) {
    printf("%s=%u\n", step, result);
    return result == MMSYSERR_NOERROR;
}

static void pcm_format(WAVEFORMATEX *format) {
    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = 1;
    format->nSamplesPerSec = 11025;
    format->wBitsPerSample = 8;
    format->nBlockAlign = 1;
    format->nAvgBytesPerSec = 11025;
}

/* Opens and writes the whole track as start_movie_audio does.  With hold
 * the device is paused before the write, so the track stays queued
 * (WHDR_INQUEUE) until the teardown however slow the machine is. */
static int start_slot(MovieMap *movie, int hold) {
    WAVEFORMATEX format;
    pcm_format(&format);
    if (!open_movie_audio_device(movie, g_pcm, PCM_BYTES, &format, 0)) {
        puts("open_movie_audio_device=0");
        return 0;
    }
    if (hold && !checked("pause", waveOutPause(movie->audio_out))) return 0;
    write_full_movie_audio(movie);
    if (!movie->audio_full || !movie->audio_full->prepared) {
        puts("write_full_movie_audio failed");
        return 0;
    }
    printf("submitted=%p flags=0x%lx\n", (void *)&movie->audio_full->header,
           (unsigned long)movie->audio_full->header.dwFlags);
    return 1;
}

/* Runs a detached teardown up to 3 more times; 1 when it released everything. */
static int finish(AudioTeardown *teardown) {
    int complete = audio_teardown_run(teardown);
    printf("teardown_complete=%d\n", complete);
    for (int i = 0; !complete && i < 3; i++) {
        Sleep(20);
        complete = audio_teardown_run(teardown);
        printf("retry=%d complete=%d\n", i + 1, complete);
    }
    if (!complete) {
        printf("teardown_stuck handle_retained=%d full_prepared=%d\n",
               teardown->audio_out != NULL,
               teardown->audio_full && teardown->audio_full->prepared);
        return 0;
    }
    HeapFree(GetProcessHeap(), 0, teardown);
    return 1;
}

/* A teardown under test must find the track still queued. */
static int still_queued(const MovieMap *movie) {
    if (movie->audio_full && (movie->audio_full->header.dwFlags & WHDR_INQUEUE))
        return 1;
    puts("not_queued_before_detach=1");
    return 0;
}

static int counters_balanced(void) {
    printf("opened=%ld closed=%ld pending=%ld\n", (long)g_audio_devices_opened,
           (long)g_audio_devices_closed, (long)g_audio_teardown_pending);
    return g_audio_devices_opened == g_audio_devices_closed &&
           g_audio_teardown_pending == 0;
}

static int run_stable(void) {
    WAVEFORMATEX format;
    HWAVEOUT out;
    WAVEHDR header;
    pcm_format(&format);
    if (!checked("open", waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0,
                                     CALLBACK_NULL))) return 3;
    if (!checked("pause", waveOutPause(out))) return 3;
    memset(&header, 0, sizeof(header));
    header.lpData = (LPSTR)g_pcm;
    header.dwBufferLength = PCM_BYTES;
    if (!checked("prepare", waveOutPrepareHeader(out, &header,
                                                 sizeof(header)))) return 3;
    if (!checked("write", waveOutWrite(out, &header, sizeof(header)))) return 3;
    if (!checked("reset", waveOutReset(out))) return 3;
    if (!checked("unprepare", waveOutUnprepareHeader(out, &header,
                                                     sizeof(header)))) return 3;
    if (!checked("close", waveOutClose(out))) return 3;
    puts("stable_complete=1");
    return 0;
}

int main(int argc, char **argv) {
    MovieMap *movie = &g_movies[0];
    const char *mode = argc == 2 ? argv[1] : "";
    AudioTeardown *teardown;
    setvbuf(stdout, NULL, _IONBF, 0);
    memset(g_pcm, 0x80, sizeof(g_pcm));
    InitializeCriticalSection(&g_state_lock);
    InitializeCriticalSection(&g_log_lock);
    g_locks_ready = 1;
    g_movie_count = 1;

    if (!strcmp(mode, "stable")) return run_stable();

    if (!strcmp(mode, "open_fail")) {
        WAVEFORMATEX format;
        pcm_format(&format);
        format.nChannels = 0; /* rejected by waveOutOpen */
        if (open_movie_audio_device(movie, g_pcm, PCM_BYTES, &format, 0)) {
            puts("unexpected_open=1");
            return 3;
        }
        printf("audio_out=%p audio_full=%p\n", (void *)movie->audio_out,
               (void *)movie->audio_full);
        if (movie->audio_out || movie->audio_full) return 1;
        return counters_balanced() && g_audio_devices_opened == 0 ? 0 : 1;
    }

    if (!start_slot(movie, strcmp(mode, "detached_done") != 0)) return 3;

    if (!strcmp(mode, "detached_done")) {
        DWORD deadline = GetTickCount() + 10000;
        while (!(movie->audio_full->header.dwFlags & WHDR_DONE)) {
            if ((LONG)(GetTickCount() - deadline) >= 0) return 4;
            Sleep(10);
        }
    } else if (!strcmp(mode, "restart")) {
        Sleep(100);
        restart_realtime_movie_audio(movie);
        /* The restart writes to the paused device, so it stays queued. */
        if (!movie->audio_full || !movie->audio_full->prepared ||
            !(movie->audio_full->header.dwFlags & WHDR_INQUEUE)) {
            puts("restart_not_requeued=1");
            return 1;
        }
        printf("restarted flags=0x%lx\n",
               (unsigned long)movie->audio_full->header.dwFlags);
    } else if (!strcmp(mode, "paused")) {
        /* As in a window move/size loop: the device is held paused. */
        movie->audio_paused = 1;
    } else if (!strcmp(mode, "slot_reuse")) {
        AudioFull *old_full = movie->audio_full;
        AudioFull *new_full;
        if (!still_queued(movie)) return 3;
        teardown = detach_movie_audio_state(movie);
        if (!teardown || teardown->audio_full != old_full) return 1;
        /* The next movie takes the same slot before the old teardown runs. */
        memset(movie, 0, sizeof(*movie));
        if (!start_slot(movie, 1)) return 3;
        new_full = movie->audio_full;
        printf("old_header=%p new_header=%p\n", (void *)&old_full->header,
               (void *)&new_full->header);
        if (&old_full->header == &new_full->header) return 1;
        if (!finish(teardown)) return 1;
        printf("new_flags_after_old_teardown=0x%lx\n",
               (unsigned long)new_full->header.dwFlags);
        if (!new_full->prepared || !(new_full->header.dwFlags & WHDR_INQUEUE))
            return 1;
        if (!still_queued(movie)) return 3;
        teardown = detach_movie_audio_state(movie);
        if (!teardown || !finish(teardown)) return 1;
        return counters_balanced() ? 0 : 1;
    } else if (!strcmp(mode, "queued")) {
        if (!still_queued(movie)) return 3;
        EnterCriticalSection(&g_state_lock);
        if (!queue_movie_audio_teardown_locked(movie)) {
            LeaveCriticalSection(&g_state_lock);
            return 1;
        }
        LeaveCriticalSection(&g_state_lock);
        if (movie->audio_out || movie->audio_full) return 1;
        drain_audio_teardowns();
        return counters_balanced() ? 0 : 1;
    } else if (strcmp(mode, "detached") != 0) {
        return 2;
    }

    printf("before_detach flags=0x%lx\n",
           (unsigned long)movie->audio_full->header.dwFlags);
    /* Every case but detached_done must tear down a still-queued track. */
    if (strcmp(mode, "detached_done") != 0 && !still_queued(movie)) return 3;
    teardown = detach_movie_audio_state(movie);
    if (!teardown) return 3;
    if (movie->audio_out || movie->audio_full || movie->audio_paused) return 1;
    if (!finish(teardown)) return 1;
    return counters_balanced() ? 0 : 1;
}
