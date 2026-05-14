# CSTech2Win shim — synthetic mode sketch

**Goal:** run Tech2Win without a chipsoft, without a bench, without Bojer. The
shim intercepts every D-PDU call as it does today, but **fabricates plausible
responses** instead of forwarding to the real DLL. Tech2Win runs the complete
workflow believing the bus answered. We log every command.

**First slice:** read-only services that don't need SecurityAccess unlock —
VIN read, ECU presence probe, ECU ID DIDs, software version, hardware version.
Once that works, layer on `$27 01`/`$27 0B` seeds, then `$27 02`/`$27 0C` key
acks, then `$3D` writebacks.

## Activation

Environment variable:
```
CSTECH2WIN_SHIM_MODE=synthetic   # default = "passthrough"
CSTECH2WIN_SHIM_REPLAY=<path>    # optional: replay from a recorded shim log
```

Read once in `DllMain`. No CLI args; Tech2Win launches the DLL with its own
process args.

## File layout

```
src/
├── shim.h                     ← shared types (existing)
├── dllmain.c                  ← entry point; reads env vars; loads real DLL
│                                 only when mode=passthrough
├── log.c                      ← shim logger (existing)
├── wrappers.c                 ← per-function wrappers (existing, modify)
├── forwarders.c               ← bulk passthrough for the 22 non-instrumented
│                                 PDU* exports (existing — also needs synthetic stubs)
└── synthetic/
    ├── synthetic.h            ← public synthetic-mode API
    ├── synthetic_engine.c     ← state machine + event queue
    ├── synthetic_responses.c  ← UDS request → response table
    └── synthetic_iso_tp.c     ← multi-frame UDS reassembly helper
```

## Key data structures

```c
// One entry per outstanding ComPrimitive — Tech2Win will poll PDUGetEventItem
// until each one yields its result.
typedef struct {
    UNUM32 hCoP;            // ComPrimitive handle returned by PDUStartComPrimitive
    UNUM32 hCLL;            // owning ComLogicalLink
    UNUM8  uds_request[8];  // captured request bytes (CAN-ID + UDS)
    size_t req_len;
    UNUM8  uds_response[256]; // synthesized response bytes (UDS payload)
    size_t resp_len;
    int    dispensed;       // 1 once PDUGetEventItem has handed it to Tech2Win
} pending_event_t;

// Global queue, fixed-size (Tech2Win is single-threaded enough)
#define MAX_PENDING 64
static pending_event_t g_pending[MAX_PENDING];
static int g_pending_count = 0;

// Mode flag set in DllMain
typedef enum { MODE_PASSTHROUGH=0, MODE_SYNTHETIC=1, MODE_REPLAY=2 } shim_mode_t;
static shim_mode_t g_mode = MODE_PASSTHROUGH;
```

## Wrapper changes (PDUStartComPrimitive)

```c
T_PDU_ERROR PDUAPI PDUStartComPrimitive(
    UNUM32 hMod, UNUM32 hCLL, UNUM32 CoPType, UNUM32 CoPDataSize,
    UNUM8* pCoPData, void* pCopCtrlData, void* pCoPTag, UNUM32* phCoP)
{
    shim_log_call("PDUStartComPrimitive", hMod, hCLL, CoPType, CoPDataSize, pCoPData);

    if (g_mode == MODE_PASSTHROUGH) {
        T_PDU_ERROR r = ((fn_PDUStartComPrimitive)g_real_PDUStartComPrimitive)(
            hMod, hCLL, CoPType, CoPDataSize, pCoPData, pCopCtrlData, pCoPTag, phCoP);
        shim_log_ret("PDUStartComPrimitive", r, *phCoP);
        return r;
    }

    // --- synthetic / replay mode ---
    // Allocate a fake hCoP, synthesize the expected response, queue it.
    UNUM32 fake_hcop = synthetic_alloc_hcop();
    *phCoP = fake_hcop;

    pending_event_t* ev = synthetic_queue_alloc();
    ev->hCoP = fake_hcop;
    ev->hCLL = hCLL;
    memcpy(ev->uds_request, pCoPData, min(CoPDataSize, sizeof(ev->uds_request)));
    ev->req_len = CoPDataSize;

    // The synthesis function looks at the captured request bytes
    // and fills uds_response[]/resp_len based on the (CAN-ID, UDS service, sub).
    synthetic_generate_response(ev);

    shim_log("SYNTH | hCoP=0x%08X | resp=%s", fake_hcop, hex(ev->uds_response, ev->resp_len));
    return PDU_STATUS_NOERROR;
}
```

## Wrapper changes (PDUGetEventItem)

```c
T_PDU_ERROR PDUAPI PDUGetEventItem(UNUM32 hMod, UNUM32 hCLL, void** pEventItem) {
    shim_log("CALL |PDUGetEventItem|hMod=0x%X hCLL=0x%X", hMod, hCLL);

    if (g_mode == MODE_PASSTHROUGH) {
        T_PDU_ERROR r = ((fn_PDUGetEventItem)g_real_PDUGetEventItem)(hMod, hCLL, pEventItem);
        shim_log_event_item(*pEventItem);
        return r;
    }

    // --- synthetic mode: dispense next queued event for this hCLL ---
    pending_event_t* ev = synthetic_queue_next_for_cll(hCLL);
    if (ev == NULL) {
        *pEventItem = NULL;
        return PDU_STATUS_NOERROR;  // "no events ready" — caller polls again
    }

    // Build an ISO 22900-2 PDU_EVENT_ITEM with embedded PDU_RESULT_DATA.
    // The real layout is complex; we allocate from a static pool and free
    // when the caller is done. For the first slice we tag it as
    // PDU_EVT_DATA_AVAILABLE with the synthesized UDS payload.
    *pEventItem = synthetic_build_event_item(ev);
    ev->dispensed = 1;

    shim_log("DISP | hCoP=0x%08X | uds=%s", ev->hCoP, hex(ev->uds_response, ev->resp_len));
    return PDU_STATUS_NOERROR;
}
```

## Response synthesis — first slice (no-access reads)

A small switch in `synthetic_responses.c`. The request `pCoPData` carries the
UDS bytes as captured — we already know the format from existing shim logs.

```c
void synthetic_generate_response(pending_event_t* ev) {
    // Layout per existing shim observation:
    //   pCoPData[0..3]   = CAN ID (big-endian)
    //   pCoPData[4]      = UDS SID
    //   pCoPData[5..]    = UDS sub + data
    UNUM32 can_id = (ev->uds_request[0]<<24) | (ev->uds_request[1]<<16)
                  | (ev->uds_request[2]<<8)  |  ev->uds_request[3];
    UNUM8  sid    = ev->uds_request[4];
    UNUM8  sub    = ev->req_len > 5 ? ev->uds_request[5] : 0;

    // Reply CAN ID convention: 0x024X → 0x064X; 0x07E0 → 0x07E8 etc.
    UNUM32 reply_id = canonical_reply_id(can_id);

    switch (sid) {
    case 0x1A:  // ReadDataByLocalId — the no-access reads
        switch (sub) {
        case 0x90: emit_vin_response(ev, reply_id);          return;  // VIN read
        case 0x9A: emit_presence_response(ev, reply_id);     return;  // ECU present?
        case 0x80: emit_calibration_id(ev, reply_id);        return;
        case 0x81: emit_software_version(ev, reply_id);      return;
        case 0x82: emit_hardware_version(ev, reply_id);      return;
        case 0x3F: emit_vin_response(ev, reply_id);          return;  // VIN alt
        // ...add more $1A DIDs as we encounter them
        default:   emit_nrc(ev, reply_id, sid, 0x31);        return;  // RequestOutOfRange
        }

    case 0xAA:  // DynamicallyDefineDID — Tech2Win uses this as preamble
        emit_positive(ev, reply_id, sid, sub, NULL, 0);      return;

    case 0xAE:  // ResponseOnEvent
        emit_positive(ev, reply_id, sid, sub, NULL, 0);      return;

    case 0x3E:  // TesterPresent
        if (sub == 0x80) { ev->resp_len = 0; return; }  // suppress = no reply
        emit_positive(ev, reply_id, sid, sub, NULL, 0);      return;

    // Later slices fill these in:
    case 0x27: /* SecurityAccess — TODO */                   return;
    case 0x3D: /* WriteMemoryByAddress — TODO */             return;
    case 0x18: /* ReadDtcInformation — TODO */               return;
    default:
        emit_nrc(ev, reply_id, sid, 0x11);  // ServiceNotSupported
    }
}
```

## Canned values for the no-access slice

```c
// Bench profiles. Keyed by which (CAN-ID-via-hCLL or VIN-context) we're
// currently emulating. v0.1: hardcode the 2017 bench car.
#define BENCH_VIN_2017  "YS3FD49YX41012017"

static void emit_vin_response(pending_event_t* ev, UNUM32 reply_id) {
    // UDS layer: 0x5A 0x90 + 17 ASCII bytes. D-PDU API hides ISO-TP from the
    // caller; we just provide the full UDS payload.
    UNUM8 p[64];
    p[0]=0x5A; p[1]=0x90;
    memcpy(p+2, BENCH_VIN_2017, 17);
    pack_response(ev, reply_id, p, 19);
}

static void emit_presence_response(pending_event_t* ev, UNUM32 reply_id) {
    // 0x5A 0x9A + 2 bytes (module class, presence bits)
    UNUM8 p[] = {0x5A, 0x9A, 0x03, 0x04};
    pack_response(ev, reply_id, p, 4);
}

static void emit_software_version(pending_event_t* ev, UNUM32 reply_id) {
    const char* sw = "P12X-7654321";
    UNUM8 p[32];
    p[0]=0x5A; p[1]=0x81;
    memcpy(p+2, sw, strlen(sw));
    pack_response(ev, reply_id, p, 2 + strlen(sw));
}
// ... more emit_*  helpers as needed
```

## What this unlocks for the no-access slice

Tech2Win running in synthetic mode can complete every read-only workflow with
no bench, no chipsoft, no Bojer. The shim logger captures:

- Every UDS request Tech2Win issues, in order, with timing
- Every PDU-level call (Construct, Connect, IoCtl, SetComParam) it makes
- The complete workflow definition for "identify car + read all public info"

That output is the workflow JSON for our API server.

## What the no-access slice does NOT cover

- `$27 0B`/`$27 0C` SecurityAccess — needs deterministic seed/key pair to make
  Tech2Win believe the unlock succeeded. Implementable in slice 2 using the
  known bench seed `0xC4DC` + `security_calc` key `0x4EED`.
- `$3D` writebacks to the SSA region — Tech2Win sends these only after a
  successful `$27 0C`. Implementable in slice 2.
- Multi-frame request bodies (e.g. SSA region writes carrying 714 bytes) —
  D-PDU likely passes them in one buffer, but we need to verify.

## Estimated effort

- **No-access slice (this sketch):** 2 days
  - Day 1: synthetic_engine.c (event queue + hCoP allocator), wrap PDUStart/
    PDUGet, hardcode 4-5 emit_* functions
  - Day 2: build on Windows, drop in beside CSTech2Win.dll, run Tech2Win,
    iterate until "identify car" workflow completes without error
- **SecurityAccess slice:** +1 day (add 0x27 cases, use security_calc key)
- **Writeback slice:** +1 day (add 0x3D, accept the big buffer, ack)
- **Replay mode (read responses from a recorded log):** +1 day on top of all
  of the above — drop-in replacement for the hardcoded response table

Total ~5 days for the full synthetic + replay system. The no-access slice
alone delivers the workflow capture for ECM info workflows on day 2.

## Testing strategy

1. Build, drop next to real DLL with `CSTECH2WIN_SHIM_MODE=synthetic`
2. Launch Tech2Win, point at SAAB, hit "Identify Vehicle"
3. Tech2Win runs ECM presence probe → expects responses → gets ours → identifies as SAAB
4. Click "Read VIN" → Tech2Win sends `$1A 90` → shim returns canned VIN bytes → Tech2Win displays
5. The shim log under `Chipsoft_RE/shim/cstech2win/captures/raw/cstech2win_shim_*.log` shows the full sequence
6. Pipe that log through `Chipsoft_RE/workflows/scripts/parse_shim_to_timeline.py` → workflow JSON

## Next concrete step

If you want to start: I write the synthetic_engine.c + a minimal
synthetic_responses.c with VIN + presence + DIDs, and update `wrappers.c` to
branch on `g_mode`. Build on the EliteBook (you already have MSVC setup since
the existing shim builds there).

Quick reality check on Windows-side: this needs to compile against the same
MSVC + WDK setup as the existing shim. Looking at `scripts/build_msvc.bat` to
make sure synthetic mode integrates with the existing build.
