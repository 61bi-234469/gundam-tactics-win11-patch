/* The menu bar's tools.  See menu_tools.h.
 *
 * shell32, ole32, gdiplus and advapi32 are loaded on the worker thread when a
 * tool is used, so the proxy's import table and its load inside the game stay
 * as they were. */
#include "menu_tools.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GUIDE_URL L"https://61bi-234469.github.io/gundam-tactics-win11-patch/"
#define RELEASES_URL \
    L"https://github.com/61bi-234469/gundam-tactics-win11-patch/releases"

enum { PATH_CAP = 1024 };

typedef struct ToolJob {
    HMODULE self; /* the reference that keeps the proxy loaded, or NULL */
    HWND notify;
    int kind; /* -1: screenshot, else a GtToolsTarget */
    int width, height;
    void *bits;
    wchar_t ini[PATH_CAP];
} ToolJob;

static void notify(const ToolJob *job, GtToolsNotice what) {
    if (what != GT_NOTICE_NONE && job->notify && IsWindow(job->notify))
        PostMessageW(job->notify, GT_TOOLS_NOTICE, (WPARAM)what, 0);
}

/* ---- paths ---- */

typedef HRESULT (WINAPI *PFN_SHGetFolderPathW)(HWND, int, HANDLE, DWORD,
                                               LPWSTR);
#ifndef CSIDL_MYPICTURES
#define CSIDL_MYPICTURES 0x0027
#endif

/* Pictures\GundamTactics, created if missing. */
static int shot_dir(wchar_t *out, size_t cap) {
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    PFN_SHGetFolderPathW get = shell ?
        (PFN_SHGetFolderPathW)(void *)GetProcAddress(shell, "SHGetFolderPathW")
        : NULL;
    wchar_t base[MAX_PATH];
    int ok = 0;
    if (get && SUCCEEDED(get(NULL, CSIDL_MYPICTURES, NULL, 0, base)) &&
        wcslen(base) + 16 < cap) {
        wcscpy(out, base);
        wcscat(out, L"\\GundamTactics");
        CreateDirectoryW(out, NULL);
        ok = GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
    }
    if (shell) FreeLibrary(shell);
    return ok;
}

/* dir\GT_YYYYMMDD_HHMMSS[_n].ext, reserved by creating it empty (CREATE_NEW),
 * so two screenshots taken within a second never pick the same name.  The
 * caller overwrites it, or deletes it on failure. */
static int shot_name(const wchar_t *dir, const wchar_t *ext, wchar_t *out,
                     size_t cap) {
    SYSTEMTIME t;
    int n;
    GetLocalTime(&t);
    for (n = 1; n < 100; n++) {
        wchar_t suffix[8] = L"";
        if (n > 1) wsprintfW(suffix, L"_%d", n);
        if (wcslen(dir) + 40 >= cap) return 0;
        wsprintfW(out, L"%s\\GT_%04u%02u%02u_%02u%02u%02u%s.%s", dir,
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  suffix, ext);
        HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
            return 1;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) return 0;
    }
    return 0;
}

/* The folder part of path (no trailing backslash). */
static void dir_of(wchar_t *path) {
    wchar_t *sep = wcsrchr(path, L'\\');
    if (sep) *sep = L'\0';
}

/* gundam.exe has no manifest, so its writes under Program Files go to
 * %LOCALAPPDATA%\VirtualStore and it reads them back from there; Notepad
 * and Explorer are not virtualized. */
static int process_virtualized(void) {
    typedef BOOL (WINAPI *PFN_OpenProcessToken)(HANDLE, DWORD, PHANDLE);
    typedef BOOL (WINAPI *PFN_GetTokenInformation)(HANDLE, int, LPVOID, DWORD,
                                                   PDWORD);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    PFN_OpenProcessToken open_token = adv ?
        (PFN_OpenProcessToken)(void *)GetProcAddress(adv, "OpenProcessToken")
        : NULL;
    PFN_GetTokenInformation info = adv ?
        (PFN_GetTokenInformation)(void *)GetProcAddress(adv,
                                                        "GetTokenInformation")
        : NULL;
    HANDLE token = NULL;
    DWORD virt = 0, got = 0;
    if (open_token && info &&
        open_token(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        /* 24: TokenVirtualizationEnabled */
        if (!info(token, 24, &virt, sizeof(virt), &got)) virt = 0;
        CloseHandle(token);
    }
    if (adv) FreeLibrary(adv);
    return virt != 0;
}

/* The VirtualStore copy of path, if it exists. */
static int virtual_store_path(const wchar_t *path, wchar_t *out, size_t cap) {
    wchar_t local[MAX_PATH];
    DWORD got;
    if (wcslen(path) < 3 || path[1] != L':' || path[2] != L'\\') return 0;
    got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!got || got >= MAX_PATH ||
        wcslen(local) + wcslen(path) + 16 >= cap)
        return 0;
    wcscpy(out, local);
    wcscat(out, L"\\VirtualStore");
    wcscat(out, path + 2); /* drop "C:" */
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* The INI as the user can edit it.  In a virtualized process the first
 * write to a protected file makes Windows copy it to the VirtualStore (where
 * the game reads it from then on); opening it for writing alone does not
 * (checked on Windows 11 with an unmanifested 32-bit test program).  A
 * zero-byte write triggers the copy and changes no content; a file in a
 * writable folder is left where it is. */
static void editable_ini(const wchar_t *path, wchar_t *out, size_t cap) {
    wcscpy(out, path);
    if (!process_virtualized()) return;
    if (!virtual_store_path(path, out, cap)) {
        HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD done = 0;
            BYTE none = 0;
            WriteFile(f, &none, 0, &done, NULL);
            CloseHandle(f);
        }
        if (!virtual_store_path(path, out, cap)) wcscpy(out, path);
    }
}

/* ---- the INI template ---- */

/* Comments only (every setting commented out at its default), so creating
 * it changes nothing.  UTF-8 without a BOM: the INI functions read the ASCII
 * keys, and a BOM would become part of the first section name. */
static const char k_ini_template[] =
    "; ガンダムタクティクス Win11 互換パッチの設定ファイル\r\n"
    "; 行頭の ; を消すと有効になります。変更はゲームを次に起動したときに反映されます。\r\n"
    "; メニューバーで選んだ設定は、このファイルの [display] に書き込まれます。\r\n"
    ";\r\n"
    "[display]\r\n"
    "; 起動したときの大きさ: auto (画面に収まる最大の整数倍) / 1〜8 (倍率) / off (パッチ前と同じ元の大きさ。メニューバーもなし)\r\n"
    ";scale=auto\r\n"
    "; 拡大の補間: auto / nearest (くっきり) / smooth (なめらか)\r\n"
    ";filter=auto\r\n"
    "; 起動したときから全画面にする: 0 / 1\r\n"
    ";fullscreen=0\r\n"
    "; 画面のフェードの長さ (ミリ秒、0 で元の速さ、最大 2000)\r\n"
    ";fade_ms=50\r\n"
    "; メニューバー: 1 (表示) / 0 (隠す。タイトルバーの右クリックで戻せます)\r\n"
    ";menu=1\r\n"
    "; ウィンドウの位置と大きさを記憶する: 1 / 0 (記憶は下の [window] に書かれます)\r\n"
    ";remember_window=1\r\n"
    "\r\n"
    "[game]\r\n"
    "; ゲームの同時起動: 0 (原作どおり 1 つだけ) / 1 (許可する。セーブデータは共通です)\r\n"
    ";multi_instance=0\r\n";

static int write_template(const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, k_ini_template, sizeof(k_ini_template) - 1, &done,
                   NULL) && done == sizeof(k_ini_template) - 1;
    CloseHandle(f);
    return ok;
}

/* ---- PNG / BMP ---- */

typedef struct GdipStartupInput {
    UINT32 version;
    void *debug_callback;
    BOOL suppress_background_thread;
    BOOL suppress_external_codecs;
} GdipStartupInput;
typedef int (WINAPI *PFN_GdiplusStartup)(ULONG_PTR *, const GdipStartupInput *,
                                         void *);
typedef void (WINAPI *PFN_GdiplusShutdown)(ULONG_PTR);
typedef int (WINAPI *PFN_GdipCreateBitmapFromScan0)(INT, INT, INT, INT, BYTE *,
                                                    void **);
typedef int (WINAPI *PFN_GdipSaveImageToFile)(void *, const WCHAR *,
                                              const CLSID *, const void *);
typedef int (WINAPI *PFN_GdipDisposeImage)(void *);

static int save_png(const wchar_t *path, BYTE *bits, int w, int h) {
    /* image/png encoder */
    static const CLSID png = {0x557CF406, 0x1A04, 0x11D3,
                              {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    HMODULE gdip = LoadLibraryW(L"gdiplus.dll");
    PFN_GdiplusStartup startup;
    PFN_GdiplusShutdown shutdown;
    PFN_GdipCreateBitmapFromScan0 create;
    PFN_GdipSaveImageToFile save;
    PFN_GdipDisposeImage dispose;
    GdipStartupInput in = {1, NULL, FALSE, FALSE};
    ULONG_PTR token = 0;
    void *image = NULL;
    int ok = 0;
    if (!gdip) return 0;
    startup = (PFN_GdiplusStartup)(void *)GetProcAddress(gdip, "GdiplusStartup");
    shutdown = (PFN_GdiplusShutdown)(void *)GetProcAddress(gdip,
                                                           "GdiplusShutdown");
    create = (PFN_GdipCreateBitmapFromScan0)(void *)GetProcAddress(
        gdip, "GdipCreateBitmapFromScan0");
    save = (PFN_GdipSaveImageToFile)(void *)GetProcAddress(
        gdip, "GdipSaveImageToFile");
    dispose = (PFN_GdipDisposeImage)(void *)GetProcAddress(gdip,
                                                           "GdipDisposeImage");
    if (startup && shutdown && create && save && dispose &&
        startup(&token, &in, NULL) == 0) {
        /* 0x00022009: PixelFormat32bppRGB (the X byte is ignored) */
        if (create(w, h, w * 4, 0x00022009, bits, &image) == 0 && image) {
            ok = save(image, path, &png, NULL) == 0;
            dispose(image);
        }
        shutdown(token);
    }
    FreeLibrary(gdip);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* 24bpp bottom-up BMP from top-down BGRX. */
static int save_bmp(const wchar_t *path, const BYTE *bits, int w, int h) {
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    DWORD row = ((DWORD)w * 3 + 3) & ~3u;
    BYTE *line = (BYTE *)calloc(1, row);
    HANDLE f;
    DWORD done;
    int ok = 1, y, x;
    if (!line) return 0;
    /* The name was reserved by shot_name. */
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        free(line);
        DeleteFileW(path);
        return 0;
    }
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + row * (DWORD)h;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = row * (DWORD)h;
    ok = WriteFile(f, &fh, sizeof(fh), &done, NULL) &&
         WriteFile(f, &ih, sizeof(ih), &done, NULL);
    for (y = h - 1; ok && y >= 0; y--) {
        const BYTE *src = bits + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            line[x * 3] = src[x * 4];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 2];
        }
        ok = WriteFile(f, line, row, &done, NULL) && done == row;
    }
    CloseHandle(f);
    free(line);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* ---- the worker ---- */

typedef HRESULT (WINAPI *PFN_CoInitializeEx)(LPVOID, DWORD);
typedef void (WINAPI *PFN_CoUninitialize)(void);
typedef HINSTANCE (WINAPI *PFN_ShellExecuteW)(HWND, LPCWSTR, LPCWSTR, LPCWSTR,
                                              LPCWSTR, INT);

static int shell_open(const wchar_t *file, const wchar_t *params) {
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    HMODULE shell = LoadLibraryW(L"shell32.dll");
    PFN_CoInitializeEx co_init = ole ?
        (PFN_CoInitializeEx)(void *)GetProcAddress(ole, "CoInitializeEx") : NULL;
    PFN_CoUninitialize co_uninit = ole ?
        (PFN_CoUninitialize)(void *)GetProcAddress(ole, "CoUninitialize") : NULL;
    PFN_ShellExecuteW exec = shell ?
        (PFN_ShellExecuteW)(void *)GetProcAddress(shell, "ShellExecuteW") : NULL;
    HRESULT hr = E_FAIL;
    int ok = 0;
    if (co_init)
        /* COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE */
        hr = co_init(NULL, 0x2 | 0x4);
    if (exec)
        ok = (INT_PTR)exec(NULL, L"open", file, params, NULL, SW_SHOWNORMAL) >
             32;
    if (SUCCEEDED(hr) && co_uninit) co_uninit();
    if (shell) FreeLibrary(shell);
    if (ole) FreeLibrary(ole);
    return ok;
}

static GtToolsNotice run_screenshot(const ToolJob *job) {
    wchar_t dir[PATH_CAP], path[PATH_CAP];
    if (!shot_dir(dir, PATH_CAP)) return GT_NOTICE_SHOT_FAILED;
    /* A failed save of any kind removes the name shot_name reserved. */
    if (shot_name(dir, L"png", path, PATH_CAP)) {
        if (save_png(path, (BYTE *)job->bits, job->width, job->height))
            return GT_NOTICE_SHOT_SAVED;
        DeleteFileW(path);
    }
    if (shot_name(dir, L"bmp", path, PATH_CAP)) {
        if (save_bmp(path, (const BYTE *)job->bits, job->width, job->height))
            return GT_NOTICE_SHOT_SAVED;
        DeleteFileW(path);
    }
    return GT_NOTICE_SHOT_FAILED;
}

static GtToolsNotice run_open(const ToolJob *job) {
    wchar_t path[PATH_CAP], alt[PATH_CAP];
    switch (job->kind) {
    case GT_OPEN_SHOT_DIR:
        if (!shot_dir(path, PATH_CAP)) return GT_NOTICE_OPEN_FAILED;
        return shell_open(path, NULL) ? GT_NOTICE_NONE : GT_NOTICE_OPEN_FAILED;
    case GT_OPEN_SAVE_DIR: {
        /* As the game reads it (FUN_00408060): Gundam.ini in the Windows
         * folder, default C:\G-TACT. */
        char ansi[MAX_PATH];
        GetPrivateProfileStringA("Gundam", "DataDir", "C:\\G-TACT", ansi,
                                 sizeof(ansi), "Gundam.ini");
        if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, path, PATH_CAP))
            return GT_NOTICE_OPEN_FAILED;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            return GT_NOTICE_NO_SAVE_DIR;
        return shell_open(path, NULL) ? GT_NOTICE_NONE : GT_NOTICE_OPEN_FAILED;
    }
    case GT_OPEN_GAME_DIR:
        if (!job->ini[0]) return GT_NOTICE_OPEN_FAILED;
        wcscpy(path, job->ini);
        dir_of(path);
        return shell_open(path, NULL) ? GT_NOTICE_NONE : GT_NOTICE_OPEN_FAILED;
    case GT_OPEN_INI:
        if (!job->ini[0]) return GT_NOTICE_OPEN_FAILED;
        wcscpy(path, job->ini);
        /* Created here (this process is the virtualized one), so a
         * Program Files copy lands where the game reads it. */
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            write_template(path);
        editable_ini(path, alt, PATH_CAP);
        wcscpy(path, alt);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            return GT_NOTICE_OPEN_FAILED;
        return shell_open(L"notepad.exe", path) ? GT_NOTICE_NONE
                                                : GT_NOTICE_OPEN_FAILED;
    default:
        return GT_NOTICE_OPEN_FAILED;
    }
}

static DWORD WINAPI tool_thread(LPVOID param) {
    ToolJob *job = (ToolJob *)param;
    HMODULE self = job->self; /* taken by start_job, released here */
    GtToolsNotice result;
    result = job->kind < 0 ? run_screenshot(job) : run_open(job);
    notify(job, result);
    if (job->bits) VirtualFree(job->bits, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, job);
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

static int start_job(ToolJob *job) {
    HANDLE th;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCWSTR)(void *)tool_thread, &job->self))
        job->self = NULL;
    th = CreateThread(NULL, 0, tool_thread, job, 0, NULL);
    if (!th) {
        if (job->self) FreeLibrary(job->self);
        if (job->bits) VirtualFree(job->bits, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, job);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

static ToolJob *new_job(HWND notify_wnd, int kind) {
    ToolJob *job = (ToolJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        sizeof(ToolJob));
    if (job) {
        job->notify = notify_wnd;
        job->kind = kind;
    }
    return job;
}

int gt_tools_screenshot(HWND notify_wnd, const void *bits, int width,
                        int height) {
    ToolJob *job;
    size_t bytes = (size_t)width * height * 4;
    if (!bits || width <= 0 || height <= 0) return 0;
    job = new_job(notify_wnd, -1);
    if (!job) return 0;
    job->bits = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (!job->bits) {
        HeapFree(GetProcessHeap(), 0, job);
        return 0;
    }
    memcpy(job->bits, bits, bytes);
    job->width = width;
    job->height = height;
    return start_job(job);
}

int gt_tools_open(HWND notify_wnd, GtToolsTarget target, const char *ini_path) {
    ToolJob *job = new_job(notify_wnd, (int)target);
    if (!job) return 0;
    if (ini_path && ini_path[0] &&
        !MultiByteToWideChar(CP_ACP, 0, ini_path, -1, job->ini, PATH_CAP))
        job->ini[0] = L'\0';
    return start_job(job);
}

/* ---- the about window ---- */

#define REPO_URL L"https://github.com/61bi-234469/gundam-tactics-win11-patch"
#define ABOUT_CLASS L"GtWin11PatchAbout"

enum {
    ABOUT_W = 470, ABOUT_H = 236, /* client area, 96 DPI units */
    ID_OPEN_GUIDE = 101, ID_OPEN_RELEASES = 102, ID_OPEN_REPO = 103
};

typedef struct AboutJob {
    HMODULE self;
    HWND game;
} AboutJob;

static HWND volatile g_about;
static volatile LONG g_about_running;
static HFONT g_about_font, g_about_bold;

static HWND about_child(HWND parent, const wchar_t *cls, const wchar_t *text,
                        DWORD style, int x, int y, int w, int h, int id,
                        HFONT font) {
    HWND c = CreateWindowExW(
        lstrcmpW(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text,
        WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
        (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    return c;
}

/* A label, a read-only (selectable, copyable) field and an Open button. */
static void about_link(HWND hwnd, int y, const wchar_t *label,
                       const wchar_t *value, int id) {
    about_child(hwnd, L"STATIC", label, SS_LEFT, 16, y + 3, 84, 18, -1,
                g_about_font);
    about_child(hwnd, L"EDIT", value,
                ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, 100, y, 280, 22,
                -1, g_about_font);
    about_child(hwnd, L"BUTTON", L"開く", BS_PUSHBUTTON | WS_TABSTOP, 388,
                y - 1, 66, 24, id, g_about_font);
}

static void about_create_controls(HWND hwnd) {
    about_child(hwnd, L"STATIC", L"ガンダムタクティクス MOBILITY FLEET0079",
                SS_LEFT, 16, 14, 440, 22, -1, g_about_bold);
    about_child(hwnd, L"STATIC",
                L"Windows 11 互換パッチ  " GT_PATCH_VERSION_W, SS_LEFT, 16,
                40, 440, 20, -1, g_about_font);
    about_child(hwnd, L"STATIC",
                L"ファンによる非公式のパッチです。原作の権利者・販売元とは"
                L"関係ありません。",
                SS_LEFT, 16, 64, 440, 20, -1, g_about_font);
    about_link(hwnd, 100, L"配布ページ", RELEASES_URL, ID_OPEN_RELEASES);
    about_link(hwnd, 132, L"ソースコード", REPO_URL, ID_OPEN_REPO);
    about_link(hwnd, 164, L"攻略データ", GUIDE_URL, ID_OPEN_GUIDE);
    about_child(hwnd, L"BUTTON", L"閉じる",
                BS_DEFPUSHBUTTON | WS_TABSTOP, 364, 198, 90, 28, IDOK,
                g_about_font);
}

static LRESULT CALLBACK about_proc(HWND hwnd, UINT msg, WPARAM wp,
                                   LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        about_create_controls(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        case ID_OPEN_GUIDE:
            shell_open(GUIDE_URL, NULL);
            return 0;
        case ID_OPEN_RELEASES:
            shell_open(RELEASES_URL, NULL);
            return 0;
        case ID_OPEN_REPO:
            shell_open(REPO_URL, NULL);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void about_fonts(void) {
    NONCLIENTMETRICSW ncm;
    LOGFONTW lf;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -12;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpyW(lf.lfFaceName, L"MS UI Gothic");
    }
    g_about_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = lf.lfHeight * 5 / 4;
    g_about_bold = CreateFontIndirectW(&lf);
}

static DWORD WINAPI about_thread(LPVOID param) {
    AboutJob *job = (AboutJob *)param;
    HMODULE self = job->self; /* taken by gt_tools_about, released here */
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    RECT r, game;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    MSG msg;
    HWND hwnd;
    int x, y;

    about_fonts();
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = about_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(0x66)); /* the game's */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = ABOUT_CLASS;
    RegisterClassW(&wc);
    SetRect(&r, 0, 0, ABOUT_W, ABOUT_H);
    AdjustWindowRectEx(&r, style, FALSE, 0);
    x = CW_USEDEFAULT;
    y = CW_USEDEFAULT;
    if (job->game && GetWindowRect(job->game, &game)) {
        x = game.left + ((game.right - game.left) - (r.right - r.left)) / 2;
        y = game.top + ((game.bottom - game.top) - (r.bottom - r.top)) / 2;
    }
    /* Not owned by the game's window: an owner on another thread would join
     * the two threads' input. */
    hwnd = CreateWindowExW(0, ABOUT_CLASS,
                           L"バージョン情報 - Win11 互換パッチ", style, x, y,
                           r.right - r.left, r.bottom - r.top, NULL, NULL,
                           inst, job);
    if (hwnd) {
        g_about = hwnd;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd, &msg)) continue; /* Tab, Enter, Esc */
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_about = NULL;
    }
    UnregisterClassW(ABOUT_CLASS, inst);
    if (g_about_font) DeleteObject(g_about_font);
    if (g_about_bold) DeleteObject(g_about_bold);
    g_about_font = g_about_bold = NULL;
    HeapFree(GetProcessHeap(), 0, job);
    InterlockedExchange(&g_about_running, 0);
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}

int gt_tools_about(HWND game) {
    AboutJob *job;
    HANDLE th;
    if (InterlockedCompareExchange(&g_about_running, 1, 0) != 0) {
        HWND open = g_about;
        if (open && IsWindow(open)) {
            /* Async: a synchronous ShowWindow on another thread's window
             * would wait for that thread from the game's thread. */
            if (IsIconic(open)) ShowWindowAsync(open, SW_RESTORE);
            SetForegroundWindow(open);
        }
        return 1;
    }
    job = (AboutJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(AboutJob));
    if (!job) {
        InterlockedExchange(&g_about_running, 0);
        return 0;
    }
    job->game = game;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCWSTR)(void *)about_thread, &job->self))
        job->self = NULL;
    th = CreateThread(NULL, 0, about_thread, job, 0, NULL);
    if (!th) {
        if (job->self) FreeLibrary(job->self);
        HeapFree(GetProcessHeap(), 0, job);
        InterlockedExchange(&g_about_running, 0);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

int gt_tools_selftest(void) {
    wchar_t path[64] = L"C:\\Games\\GT\\qtim_compat.ini";
    int ok = 1;
    dir_of(path);
    ok &= wcscmp(path, L"C:\\Games\\GT") == 0;
    ok &= k_ini_template[0] == ';' &&
          strstr(k_ini_template, "\r\n[display]\r\n") != NULL;
    return ok;
}
