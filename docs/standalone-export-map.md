# FremSoft — standalone implementation map for CSTech2Win.dll

The 29 PDU* exports we need to implement when there is no real
`CSTech2Win.dll` behind us. Categorized by what each function actually
needs to do once the real DLL is gone.

References:
- ISO 22900-2 D-PDU API spec (function signatures + return codes)
- Our own RE in `Chipsoft_RE/shim/cstech2win/src/wrappers.c` for the
  `PDU_EVENT_ITEM` (20-byte), `PDU_RESULT_DATA` (44-byte), and
  `T_PDU_PARAM` (16-byte) ABIs
- The `.def` file at `shim/cstech2win/src/cstech2win.def`

## Categorization

| # | Export | Category | What it returns / does in standalone mode |
|---|---|---|---|
| 1 | `PDUCancelComPrimitive` | TRIVIAL | Find hCop in our pending queue, drop it. Return 0. |
| 2 | `PDUConnect` | STATEFUL | Mark module hMod as connected. Return 0. |
| 3 | `PDUConstruct` | STATEFUL | Read `OptionStr` (config XML), allocate FremSoft state, return 0. Call once at API init. |
| 4 | `PDUCreateComLogicalLink` | STATEFUL | Allocate a CLL slot, return its handle via out-param. We expose **one virtual link per CAN-ID family** (engine $0241 / OBD $07E0 / etc.) so Tech2Win sees the right channel topology. |
| 5 | `PDUDestroyComLogicalLink` | STATEFUL | Free the CLL slot. Return 0. |
| 6 | `PDUDestroyItem` | TRIVIAL | Free the event-item allocation we handed back from `PDUGetEventItem`. Return 0. |
| 7 | `PDUDestruct` | TRIVIAL | Free FremSoft state. Return 0. |
| 8 | `PDUDisconnect` | STATEFUL | Mark module disconnected. Return 0. |
| 9 | `PDUGetComParam` | STATEFUL | Return the value Tech2Win previously set via `PDUSetComParam` (we keep a small param dictionary per CLL). |
| 10 | `PDUGetConflictingResources` | DISCOVERY | Return empty list — we own everything, nothing conflicts. |
| 11 | `PDUGetEventItem` | **DATA PATH** | Pop the next event from the per-CLL queue if one is due (timestamp ≤ now). Return PDU_ERROR_EVENT_QUEUE_EMPTY (0x18) if empty. |
| 12 | `PDUGetLastError` | DISCOVERY | Return our last-error code (we always set 0 unless Tech2Win sent something genuinely malformed). |
| 13 | `PDUGetModuleIds` | DISCOVERY | Return one fake module: `{ "ChipsoftEmu", id=0x0001, vendor="FremSoft" }`. Tech2Win iterates these to find what's available. |
| 14 | `PDUGetObjectId` | DISCOVERY | Map the OBD/Tech2 object names ("MVCI_PROTOCOL_ISO_15765") to numeric IDs from a hardcoded table. |
| 15 | `PDUGetResourceIds` | DISCOVERY | Return one resource (the simulated CAN bus). |
| 16 | `PDUGetResourceStatus` | DISCOVERY | Always return `PDU_RSTATUS_AVAIL` (idle/available). |
| 17 | `PDUGetStatus` | DISCOVERY | Return status flags for the requested handle. CLL: `PDU_CLLST_OFFLINE` until first `PDUConnect`, then `PDU_CLLST_ONLINE`. |
| 18 | `PDUGetTimestamp` | DISCOVERY | Return monotonic microseconds since `PDUConstruct`. |
| 19 | `PDUGetUniqueRespIdTable` | STATEFUL | Return whatever Tech2 set via `PDUSetUniqueRespIdTable` (or empty). |
| 20 | `PDUGetVersion` | DISCOVERY | Return `{ MVCI_PART1_STANDARD_VERSION=0x00010000, MVCI_PART2_STANDARD_VERSION=0x00010000, HwSerialNumber="FREMSOFT-ALPHA-0001", HwName="FremSoft Virtual MVCI", HwVendor="OpenSAAB", HwVersion=0x00000100, FwName="FremSoft", FwVendor="OpenSAAB", FwVersion=0x00000100, VendorName="OpenSAAB" }` — fully canned. |
| 21 | `PDUIoCtl` | MIXED | IoCtl ID dispatch: `PDU_IOCTL_RESET` (0x0001) → return 0. `PDU_IOCTL_CLEAR_TX_QUEUE/RX_QUEUE` (0x000C/0x000D) → drain our queues. `PDU_IOCTL_READ_VBATT` → return 12000 mV. Most others → return 0 quietly. |
| 22 | `PDULockResource` | TRIVIAL | Always return 0 — only one Tech2Win runs anyway. |
| 23 | `PDUModuleConnect` | STATEFUL | Mark module as live in our state. Return 0. |
| 24 | `PDUModuleDisconnect` | STATEFUL | Mark module disconnected. Return 0. |
| 25 | `PDURegisterEventCallback` | **DATA PATH** | Store the function pointer. Our delivery thread will invoke it for any event whose due-time has arrived. Return 0. |
| 26 | `PDUSetComParam` | STATEFUL | Store the param value in our per-CLL dictionary. Return 0. |
| 27 | `PDUSetUniqueRespIdTable` | STATEFUL | Store the response-ID table for later `Get`. Return 0. |
| 28 | `PDUStartComPrimitive` | **DATA PATH** | The big one. Decode `pCoPData` to `(can_id, uds_bytes)`. Look up in the recording. Allocate event-item(s) + result-data(s) + pDataBytes copies. Schedule them on the per-CLL queue with `due = now + delay_ms`. If no match: log to `fremsoft_unknown_<ts>.log` and schedule a `7F 11` NRC reply at `now + 50ms`. Return the `hCop` handle via out-param + 0 status. |
| 29 | `PDUUnlockResource` | TRIVIAL | Return 0. |

## Categories summary

- **TRIVIAL (5)**: stub, return 0, no state. PDUCancelComPrimitive,
  PDUDestroyItem, PDUDestruct, PDULockResource, PDUUnlockResource.
- **STATEFUL (10)**: keep simple bookkeeping (handles, params, flags)
  but no real wire activity. PDUConnect, PDUConstruct,
  PDUCreate/DestroyComLogicalLink, PDUDisconnect, PDUGet/SetComParam,
  PDUGet/SetUniqueRespIdTable, PDUModuleConnect/Disconnect.
- **DISCOVERY (8)**: return canned identity / status data.
  PDUGetConflictingResources, PDUGetLastError, PDUGetModuleIds,
  PDUGetObjectId, PDUGetResourceIds, PDUGetResourceStatus,
  PDUGetStatus, PDUGetTimestamp, PDUGetVersion.
- **MIXED (1)**: PDUIoCtl — dispatch on IoCtl ID.
- **DATA PATH (3)**: the actual replayer.
  - `PDUStartComPrimitive` → look up + schedule events
  - `PDUGetEventItem` → return next due event
  - `PDURegisterEventCallback` → fire scheduled events to Tech2Win

## Threading model

- **Main thread**: serves all the API calls (Tech2Win is single-threaded
  for its diag dispatcher, by observation).
- **Delivery thread**: one background thread per process. Sleeps until
  the soonest scheduled event's due-time, then either:
  - If a callback is registered (via `PDURegisterEventCallback`): fire
    the callback synchronously.
  - Else: just leave the event in the queue for the next
    `PDUGetEventItem` call.

## Scheduled-event format

```c
typedef struct {
    UNUM64           due_us;       // monotonic timestamp (microseconds since attach)
    UNUM32           h_mod;
    UNUM32           h_cll;
    UNUM32           h_cop;        // matches the hCop returned by StartComPrimitive
    void*            cop_tag;      // caller's tag (Tech2 keeps these per CLL)
    UNUM32           item_type;    // 0x1300 PDU_IT_RESULT (most common)
    UNUM32           num_data;     // pDataBytes length
    UNUM8*           data_bytes;   // owned by us, freed in PDUDestroyItem
} sched_event_t;
```

## Recording lookup contract

Recording loaded once at `PDUConstruct`:

```
typedef struct {
    UNUM32           can_id;
    UNUM32           uds_len;
    UNUM8*           uds_bytes;  // request
    UNUM32           num_responses;
    sched_response_t* responses; // ordered list with delay_ms
} recorded_exchange_t;
```

Indexed by `(can_id, fnv1a(uds_bytes))` for O(1) lookup. Linear
collision chain — recordings are small (today's check_codes JSON has
58 keys), no need for fancy hash strategies.

## Out-of-recording behavior

When `PDUStartComPrimitive` looks up a `(can_id, uds_bytes)` pair that
isn't in the recording:

1. Append one line to `%TEMP%\fremsoft_unknown_<wall_ms>.log`:
   ```
   <ms_since_attach>|<wall_ms>|<tid>|UNKNOWN|can_id=0x%04X|uds=<hex bytes>
   ```
2. Schedule a single PDU_IT_RESULT event at `now + 50 ms` carrying a
   UDS NegativeResponse: `7F <original_SID> 0x11` (ServiceNotSupported).
   The middle byte ECHOES the SID Tech2 sent — `7F 11` alone is not
   valid UDS. Examples:

       Request 1A 90      → NRC 7F 1A 11
       Request 27 01      → NRC 7F 27 11
       Request A9 81 12   → NRC 7F A9 11

   Tech2Win sees a clean per-ECU rejection and moves on to the next
   step in the menu (or displays "ECU does not support this function"
   for interactive ones). Much cleaner than a PDU_IT_ERROR event,
   which signals a transport-stack failure rather than an ECU response.

This unknown log IS the protocol-map. Each line is a Tech2 capability
we hadn't recorded yet — a candidate for the next bench capture.

## What this gives us

When Tech2Win runs against FremSoft + the today's check_codes recording:

- It sees the same 17 ECUs respond with the same DTCs.
- It can scroll, filter, save, print as if connected to the real bench.
- Any menu Tech2Win lets the user click that we DIDN'T record gets
  logged to the unknown file. Walking every menu in Tech2Win → walking
  the entire Tech2 capability set, observed.

## Out of scope for L1 MVP

- Multi-recording switching at runtime.
- OpenSAAB-catalog synthesis fallback for unknown requests
  (just log + NRC for now).
- Full PDU_IT_STATUS / PDU_IT_INFO event types — we only emit
  PDU_IT_RESULT (0x1300) and PDU_IT_ERROR (0x1304).
- TimestampFlags / pExtraInfo / RxFlag fields in PDU_RESULT_DATA —
  we set these to zero/null.
- Periodic message simulation.
