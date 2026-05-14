// CSTech2Win shim — DLL entry point and lifecycle.
//
// On DLL_PROCESS_ATTACH:
//   1. Open the log file in %TEMP%\cstech2win_shim.log (or fallback to CWD).
//   2. LoadLibrary("CSTech2Win_real.dll") from the same directory as our DLL.
//      We deliberately do NOT call LoadLibrary("CSTech2Win.dll") — that would
//      recurse into ourselves. The real DLL must be renamed before install.
//   3. Resolve all 29 D-PDU exports.
//
// On DLL_PROCESS_DETACH: close handles cleanly.

#include "shim.h"
#include "wrappers.h"
#include "../../../src/fremsoft.h"

HMODULE g_hRealDll = NULL;

// Implemented in forwarders.c (auto-generated)
extern void resolve_passthrough_exports(HMODULE hReal);

// Read the FremSoft mode from HKLM\SOFTWARE\OpenSAAB\Collector\Mode.
// Defaults to RECORD (today's behavior) if the key isn't set.
static fremsoft_mode_t read_mode_from_registry(void) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\OpenSAAB\\Collector", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return FREMSOFT_MODE_RECORD;
    }
    char buf[32] = {0};
    DWORD type = 0, sz = sizeof(buf) - 1;
    LONG rc = RegQueryValueExA(key, "Mode", NULL, &type, (BYTE*)buf, &sz);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return FREMSOFT_MODE_RECORD;
    if (_stricmp(buf, "passthrough") == 0) return FREMSOFT_MODE_PASSTHROUGH;
    if (_stricmp(buf, "record")      == 0) return FREMSOFT_MODE_RECORD;
    if (_stricmp(buf, "playback")    == 0) return FREMSOFT_MODE_PLAYBACK;
    if (_stricmp(buf, "standalone")  == 0) return FREMSOFT_MODE_STANDALONE;
    return FREMSOFT_MODE_RECORD;
}

static void load_real_dll(HINSTANCE hSelf) {
    char self_path[MAX_PATH] = {0};
    GetModuleFileNameA((HMODULE)hSelf, self_path, MAX_PATH);
    // Strip filename, leaving directory + trailing backslash
    char* last_slash = strrchr(self_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';

    char real_path[MAX_PATH];
    snprintf(real_path, MAX_PATH, "%sCSTech2Win_real.dll", self_path);

    g_hRealDll = LoadLibraryA(real_path);
    if (!g_hRealDll) {
        DWORD err = GetLastError();
        shim_log("FATAL|LoadLibrary failed for '%s' (err=%lu)", real_path, err);
        // We can't recover. Subsequent forwarder calls will crash; that's OK
        // because Tech2Win is broken anyway without the real DLL.
        return;
    }
    shim_log("INIT |loaded real DLL from '%s'", real_path);

    resolve_passthrough_exports(g_hRealDll);
    resolve_instrumented_exports(g_hRealDll);
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hInstance);
            shim_log_init();
            shim_log("INIT |DLL_PROCESS_ATTACH pid=%lu", GetCurrentProcessId());
            fremsoft_mode_t mode = read_mode_from_registry();
            shim_log("INIT |fremsoft mode=%d (0=passthrough 1=record 2=playback 3=standalone)",
                     (int)mode);
            // Standalone mode skips real-DLL load entirely.
            if (mode != FREMSOFT_MODE_STANDALONE) {
                load_real_dll(hInstance);
            }
            // Initialise FremSoft for playback / standalone — opens the
            // recording, the activity log, the unknown-request log, and
            // starts the delivery thread.
            fremsoft_init(mode);
            break;
        }
        case DLL_PROCESS_DETACH:
            shim_log("EXIT |DLL_PROCESS_DETACH pid=%lu", GetCurrentProcessId());
            fremsoft_shutdown();
            if (g_hRealDll) {
                FreeLibrary(g_hRealDll);
                g_hRealDll = NULL;
            }
            break;
    }
    return TRUE;
}
