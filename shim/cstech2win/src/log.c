// CSTech2Win shim — logging.
//
// Pipe-delimited log: <timestamp_ms>|<tid>|<event>|<detail>
// Lock-protected so multiple threads (event callbacks, the API thread) don't
// interleave. Buffered, but flushed after every line for crash-safety.

#include "shim.h"
#include <stdarg.h>
#include <time.h>

static FILE*           g_log_fp = NULL;
static CRITICAL_SECTION g_log_lock;
static int             g_log_initialized = 0;

static void open_log_file(void) {
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        // fall back to current working dir
        path[0] = '.';
        path[1] = '\0';
        n = 1;
    }
    // append filename
    char fullpath[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(fullpath, MAX_PATH,
             "%s\\cstech2win_shim_%04d%02d%02d-%02d%02d%02d.log",
             path,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    g_log_fp = fopen(fullpath, "wb");
    if (g_log_fp) {
        // unbuffered — every write hits disk so a crash doesn't lose context
        setvbuf(g_log_fp, NULL, _IONBF, 0);
        fprintf(g_log_fp,
                "# cstech2win shim log\n"
                "# format: ms_since_attach | tid | event | function | detail\n"
                "# log file: %s\n",
                fullpath);
    }
}

void shim_log_init(void) {
    if (g_log_initialized) return;
    InitializeCriticalSection(&g_log_lock);
    open_log_file();
    g_log_initialized = 1;
}

static uint64_t now_ms(void) {
    static uint64_t t0 = 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t /= 10000; // 100ns ticks → ms
    if (t0 == 0) t0 = t;
    return t - t0;
}

void shim_log(const char* fmt, ...) {
    if (!g_log_fp) return;
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log_fp, "%llu|%lu|", (unsigned long long)now_ms(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_log_fp);
    LeaveCriticalSection(&g_log_lock);
}

void shim_log_hex(const char* tag, const void* buf, size_t len) {
    if (!g_log_fp) return;
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log_fp, "%llu|%lu|HEX  |%s|len=%zu|",
            (unsigned long long)now_ms(), GetCurrentThreadId(), tag, len);
    const unsigned char* p = (const unsigned char*)buf;
    for (size_t i = 0; i < len; i++) {
        fprintf(g_log_fp, "%02X", p[i]);
        if (i + 1 < len) fputc(' ', g_log_fp);
    }
    fputc('\n', g_log_fp);
    LeaveCriticalSection(&g_log_lock);
}
