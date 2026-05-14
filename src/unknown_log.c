// FremSoft — unknown-request logger.
//
// Whenever PDUStartComPrimitive looks up a (can_id, uds_bytes) pair that
// isn't in the recording, we append one line to
// %TEMP%\fremsoft_unknown_<wall_ms>.log so the operator can later review
// every request Tech2Win sent that we didn't have a recorded response
// for. Each unknown is a Tech2 capability we hadn't catalogued yet —
// the protocol-map gold.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

extern void shim_log(const char* fmt, ...);

static FILE*           g_fp = NULL;
static CRITICAL_SECTION g_lock;
static int             g_init = 0;

static uint64_t wall_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000;
}

void unknown_log_init(void) {
    if (g_init) return;
    InitializeCriticalSection(&g_lock);
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { dir[0] = '.'; dir[1] = 0; }
    char path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(path, MAX_PATH,
             "%s\\fremsoft_unknown_%04d%02d%02d-%02d%02d%02d.log",
             dir, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    g_fp = fopen(path, "wb");
    if (g_fp) {
        setvbuf(g_fp, NULL, _IONBF, 0);
        fprintf(g_fp,
                "# fremsoft unknown-request log\n"
                "# format: ms_since_attach | wall_clock_ms | tid | UNKNOWN | can_id=0xNNNN | uds=NN NN ...\n"
                "# log file: %s\n", path);
        shim_log("INIT |unknown_log|opened %s", path);
    } else {
        shim_log("WARN |unknown_log|fopen failed for %s", path);
    }
    g_init = 1;
}

static uint64_t since_attach_ms(void) {
    static uint64_t t0 = 0;
    uint64_t t = wall_ms();
    if (t0 == 0) t0 = t;
    return t - t0;
}

void unknown_log_record(uint32_t can_id, const uint8_t* uds, uint32_t uds_len) {
    if (!g_init) unknown_log_init();
    if (!g_fp) return;
    EnterCriticalSection(&g_lock);
    fprintf(g_fp, "%llu|%llu|%lu|UNKNOWN|can_id=0x%04X|uds=",
            (unsigned long long)since_attach_ms(),
            (unsigned long long)wall_ms(),
            GetCurrentThreadId(), can_id);
    for (uint32_t i = 0; i < uds_len; i++) {
        fprintf(g_fp, "%02X", uds[i]);
        if (i + 1 < uds_len) fputc(' ', g_fp);
    }
    fputc('\n', g_fp);
    LeaveCriticalSection(&g_lock);
}

void unknown_log_shutdown(void) {
    if (!g_init) return;
    EnterCriticalSection(&g_lock);
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_init = 0;
}
