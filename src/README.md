# FremSoft — C source skeleton

Builds linked into the existing CSTech2Win shim at
`shim/cstech2win/`. When the registry mode is `playback` or
`standalone`, the shim's wrappers route to `fremsoft_*` instead of
forwarding to the real DLL.

## Files

| File | Status | What |
|---|---|---|
| `fremsoft.h` | ✅ | Public header — modes + entry points + categorized PDU* function declarations |
| `recording.h` | ✅ | Recording loader API — `recording_load()`, `recording_lookup()` |
| `scheduler.h` | ✅ | Event scheduler API — push/pop_due + delivery thread |
| `fremsoft.c` | ⏳ tomorrow | Implements the categorized PDU* fns + mode dispatch |
| `recording.c` | ⏳ tomorrow | JSON parsing (jsmn) + `(can_id, fnv1a(uds))` hashmap |
| `scheduler.c` | ⏳ tomorrow | Per-CLL min-heap + delivery thread |
| `unknown_log.c` | ⏳ tomorrow | `%TEMP%\fremsoft_unknown_<ts>.log` writer |
| `vendor/jsmn.h` | ⏳ tomorrow | Header-only JSON parser (4 KB, MIT) |

## Build wiring (tomorrow)

The MSVC `build_msvc.bat` for the shim adds:

```bat
cl ... ^
   src\dllmain.c src\log.c src\wrappers.c src\forwarders.c ^
   ..\..\fremsoft\src\fremsoft.c ^
   ..\..\fremsoft\src\recording.c ^
   ..\..\fremsoft\src\scheduler.c ^
   ..\..\fremsoft\src\unknown_log.c ^
   /I ..\..\fremsoft\src ^
   /I ..\..\fremsoft\src\vendor ^
   ...
```

`dllmain.c` adds at attach:

```c
#include "fremsoft.h"
fremsoft_mode_t mode = read_mode_from_registry();  // default RECORD
if (mode >= FREMSOFT_MODE_PLAYBACK) fremsoft_init(mode);
```

`wrappers.c` adds the dispatch:

```c
T_PDU_ERROR PDUAPI PDUStartComPrimitive(...) {
    if (fremsoft_get_mode() >= FREMSOFT_MODE_PLAYBACK) {
        return fremsoft_PDUStartComPrimitive(...);
    }
    // Existing forward-and-log path stays for RECORD / PASSTHROUGH.
    ...
}
```

Same shape for `PDUGetEventItem`, `PDURegisterEventCallback`,
`PDUDestroyItem`. The remaining 25 exports get a single dispatch line
each, falling through to the real DLL for RECORD/PASSTHROUGH and the
fremsoft stubs for PLAYBACK/STANDALONE.

## Runtime configuration

```
HKLM\SOFTWARE\OpenSAAB\Collector
    Mode                REG_SZ   "passthrough" | "record" | "playback" | "standalone"
    PlaybackRecording   REG_SZ   "C:\path\to\fremsoft\recordings\<id>.json"
```

Set via `reg add` for now; tray-app UI for it later.
