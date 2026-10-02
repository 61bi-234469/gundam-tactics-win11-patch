#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
/* Time gundam.exe's per-tick BGM poll (MCI_STATUS mode, FUN_00421600) while a MIDI plays.
   Run with and without __COMPAT_LAYER to compare compatibility modes.
   Every line also goes to mci_status_cost.log next to the exe, flushed at once.
   With no arguments it plays run\GT01\Sound\M04GM.MID (path relative to the exe), so the exe can be
   started by double-click with compatibility settings from its Properties dialog. */
static FILE *logf;
static void out(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout);
  if (logf) { va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap); fflush(logf); }
}
static LONG WINAPI crash(EXCEPTION_POINTERS *x) {
  out("crash code=0x%08lx addr=%p\n", (unsigned long)x->ExceptionRecord->ExceptionCode,
      x->ExceptionRecord->ExceptionAddress);
  return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD WINAPI watchdog(LPVOID p) {
  DWORD start = GetTickCount();
  (void)p;
  for (;;) { Sleep(5000); out("still running %lu s\n", (unsigned long)((GetTickCount() - start) / 1000)); }
  return 0;
}
int main(int argc, char **argv) {
  static char dir[MAX_PATH], defpath[MAX_PATH], logpath[MAX_PATH];
  const char *path;
  int n = argc > 2 ? atoi(argv[2]) : 300;
  LARGE_INTEGER f, t0, t1;
  MCI_OPEN_PARMSA op = {0};
  MCI_PLAY_PARMS pp = {0};
  MCI_STATUS_PARMS st = {0};
  MCIERROR e;
  MCIDEVICEID id;
  DWORD v;
  int i;
  char *layer = getenv("__COMPAT_LAYER");
  GetModuleFileNameA(NULL, dir, MAX_PATH);
  *strrchr(dir, '\\') = 0;
  snprintf(defpath, MAX_PATH, "%s\\..\\..\\..\\run\\GT01\\Sound\\M04GM.MID", dir);
  snprintf(logpath, MAX_PATH, "%s\\mci_status_cost.log", dir);
  path = argc > 1 ? argv[1] : defpath;
  logf = fopen(logpath, "a");
  v = GetVersion();
  out("__COMPAT_LAYER=%s GetVersion=%lu.%lu build=%lu\n", layer && *layer ? layer : "(none)",
      (unsigned long)(v & 0xFF), (unsigned long)((v >> 8) & 0xFF), (unsigned long)(v >> 16));
  SetUnhandledExceptionFilter(crash);
  CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
  out("path=%s exists=%d\n", path, GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
  op.lpstrDeviceType = "sequencer";
  op.lpstrElementName = path;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  e = mciSendCommandA(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT, (DWORD_PTR)&op);
  QueryPerformanceCounter(&t1);
  out("open err=%lu id=%u %.1f ms\n", (unsigned long)e, (unsigned)op.wDeviceID,
      (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
  if (e) return 1;
  id = op.wDeviceID;
  e = mciSendCommandA(id, MCI_PLAY, 0, (DWORD_PTR)&pp);
  out("play err=%lu\n", (unsigned long)e);
  Sleep(1000);
  st.dwItem = MCI_STATUS_MODE;
  QueryPerformanceCounter(&t0);
  for (i = 0; i < n; i++) mciSendCommandA(id, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&st);
  QueryPerformanceCounter(&t1);
  out("status mode x%d: %.3f ms/call, mode=%lu\n", n,
      (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / n, (unsigned long)st.dwReturn);
  mciSendCommandA(id, MCI_CLOSE, 0, 0);
  out("done\n");
  return 0;
}
