/* Replica of gundam.exe's click gate (e.g. FUN_004185c0 / FUN_00403640):
 *   loop { flag=0; PeekMessage(PM_REMOVE) one message; Dispatch (WndProc sets flag on WM_LBUTTONDOWN);
 *          if (GetMessageTime() > last) { last = GetMessageTime(); if (msg != WM_MOUSEMOVE && flag) ACCEPT; } }
 * A WM_LBUTTONDOWN whose message time equals the time of the previously retrieved message
 * (e.g. a WM_MOUSEMOVE in the same GetTickCount tick) is silently dropped.
 * Usage: click_gate_test.exe            -> interactive: click in the window, stats in title + log
 *        --proxy <QTIM32.dll>           -> load the patch proxy first (its input-time hooks apply here too)
 *        click_gate_test.exe --synth N  -> SendInput move+click N times, print stats and exit
 */
#include <windows.h>
#include <stdio.h>

static volatile int g_down;
static int g_total, g_accept, g_drop_same, g_drop_other;
static FILE *g_log;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN: g_down = 1; SetCapture(h); break;
    case WM_LBUTTONUP: ReleaseCapture(); break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}


int main(int argc, char **argv) {
    int synth_n = 0;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--synth") && a + 1 < argc) synth_n = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--proxy") && a + 1 < argc) {
            /* load the QTIM32 proxy so it hooks this EXE's user32 imports like gundam.exe's */
            if (!LoadLibraryExA(argv[++a], NULL, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                printf("LoadLibrary(%s) failed: %lu\n", argv[a], GetLastError());
                return 2;
            }
        }
    }
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
    wc.lpszClassName = "ClickGateTest";
    RegisterClassA(&wc);
    HWND h = CreateWindowA("ClickGateTest", "click gate test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           200, 200, 640, 480, 0, 0, wc.hInstance, 0);
    g_log = fopen("click_gate_log.txt", "w");
    LONG last = GetMessageTime(), prev_time = 0; UINT prev_msg = 0;
    DWORD synth_next = GetTickCount() + 300; int synth_i = 0, synth_phase = 0;
    RECT wr; GetWindowRect(h, &wr);
    for (;;) {
        MSG msg = {0};
        g_down = 0;
        BOOL got = PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE);
        if (got) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        LONG mt = GetMessageTime();
        if (g_down) {
            g_total++;
            int accepted = (mt > last);
            if (accepted) g_accept++;
            else if (mt == prev_time) g_drop_same++;
            else g_drop_other++;
            if (g_log) fprintf(g_log, "click t=%ld last=%ld prev_msg=0x%03x dt_prev=%ld %s\n", mt, last,
                               prev_msg, mt - prev_time, accepted ? "ACCEPT" : "DROP");
            char title[160];
            sprintf(title, "clicks=%d accepted=%d dropped=%d (same-tick=%d)", g_total, g_accept,
                    g_drop_same + g_drop_other, g_drop_same);
            SetWindowTextA(h, title);
        }
        if (mt > last) last = mt;        /* game updates the stamp for every newer message, incl. WM_MOUSEMOVE */
        if (got) { prev_time = msg.time; prev_msg = msg.message; }

        if (synth_n < 0 && (LONG)(GetTickCount() - synth_next) >= 0) break;
        if (synth_n > 0) {               /* drive SendInput from the same thread, between loop iterations */
            DWORD now = GetTickCount();
            if ((LONG)(now - synth_next) >= 0) {
                int cx = (wr.left + wr.right) / 2, cy = (wr.top + wr.bottom) / 2;
                INPUT in = {0}; in.type = INPUT_MOUSE;
                if (synth_phase == 0) {
                    SetForegroundWindow(h); SetCursorPos(cx + (synth_i & 1), cy);
                    synth_phase = 1; synth_next = now + 50;
                } else if (synth_phase == 1) {
                    POINT pt; GetCursorPos(&pt);
                    if (WindowFromPoint(pt) != h) { synth_phase = 0; synth_next = now + 500; continue; } /* never click elsewhere */
                    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                    { int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                      int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                      in.mi.dx = (LONG)(((cx + 3 + (synth_i & 1) * 3) - vx) * 65535LL / (vw - 1));
                      in.mi.dy = (LONG)((cy - vy) * 65535LL / (vh - 1)); }
                    SendInput(1, &in, sizeof in);
                    synth_phase = 2; synth_next = now + ((synth_i % 4) ? 0 : 20); /* mostly same-tick move->press */
                } else if (synth_phase == 2) {
                    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1, &in, sizeof in);
                    in.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &in, sizeof in);
                    synth_phase = 0; synth_next = now + 60;
                    if (++synth_i >= synth_n) { synth_next = now + 300; synth_n = -synth_n; }
                }
            }
        }
    }
    printf("clicks=%d accepted=%d dropped_same_tick=%d dropped_other=%d\n", g_total, g_accept, g_drop_same,
           g_drop_other);
    if (g_log) fclose(g_log);
    return 0;
}
