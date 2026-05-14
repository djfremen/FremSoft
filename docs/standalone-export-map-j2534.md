# FremSoft — standalone implementation map for j2534_interface.dll

The 13 SAE J2534 PassThru* exports we need to satisfy when there is no
real `j2534_interface.dll` behind us. Companion to
`standalone-export-map.md` (which covers the 29 D-PDU exports of
CSTech2Win.dll).

The two APIs share the same backend (chipsoft adapter), the same wire
recording format, and the same core scheduler. Only the wrapping
function signatures differ.

References:
- SAE J2534-1 (2002) function reference + `PASSTHRU_MSG` layout
- Our own RE in `Chipsoft_RE/shim/j2534/src/j2534.h` for the struct
  ABI we already pinned down (`PASSTHRU_MSG` 4152 bytes, `SCONFIG`,
  `SBYTE_ARRAY`, IOCTL IDs, error codes)
- The `.def` file at `shim/j2534/src/j2534_interface.def`

## Categorization

| # | Export | Category | What it returns / does in standalone mode |
|---|---|---|---|
| 1 | `PassThruOpen` | STATEFUL | Allocate a virtual DeviceID, return via out-param. We expose **one** virtual device. Return STATUS_NOERROR. |
| 2 | `PassThruClose` | STATEFUL | Mark device closed. Return STATUS_NOERROR. |
| 3 | `PassThruConnect` | STATEFUL | Allocate a ChannelID per (DeviceID, ProtocolID). Remember protocol + baud + flags. Return STATUS_NOERROR. |
| 4 | `PassThruDisconnect` | STATEFUL | Mark channel closed; drop any queued messages for it. Return STATUS_NOERROR. |
| 5 | `PassThruReadMsgs` | **DATA PATH** | Pop next due message(s) from the per-Channel queue, write into caller's `PASSTHRU_MSG[]`, set `*pNumMsgs` to count actually returned. Return STATUS_NOERROR if any, ERR_BUFFER_EMPTY (0x10) if none ready before Timeout expires. |
| 6 | `PassThruWriteMsgs` | **DATA PATH** | The big one. Decode each `PASSTHRU_MSG.Data` (first 4 bytes = CAN-ID for ISO15765, rest = UDS payload). For each msg: look up in recording, schedule responses on the per-Channel queue with `due = now + delay_ms`. If no match: log to `fremsoft_unknown_<ts>.log` and schedule a `7F <SID> 11` NRC reply at `now + 50ms`. Return STATUS_NOERROR. |
| 7 | `PassThruStartPeriodicMsg` | STATEFUL | Allocate a periodic-msg ID, remember the schedule. (For MVP: don't actually fire the periodics — most diag flows don't use them. Tech2 / Trionic mostly use TesterPresent broadcasts which fire from their own thread.) Return STATUS_NOERROR. |
| 8 | `PassThruStopPeriodicMsg` | STATEFUL | Drop the periodic. Return STATUS_NOERROR. |
| 9 | `PassThruStartMsgFilter` | STATEFUL | Allocate a FilterID, remember filter type + mask + pattern. (Filters are advisory in FremSoft — we schedule whatever the recording says, regardless of filter — but we keep them so `PassThruStopMsgFilter` can find them.) Return STATUS_NOERROR. |
| 10 | `PassThruStopMsgFilter` | STATEFUL | Drop the filter. Return STATUS_NOERROR. |
| 11 | `PassThruSetProgrammingVoltage` | TRIVIAL | Ignore. Return STATUS_NOERROR. (No real Vpp pin to drive.) |
| 12 | `PassThruReadVersion` | DISCOVERY | Fill the three out-strings: `pFirmwareVersion="FremSoft v0.1.0"`, `pDllVersion="FremSoft j2534 v0.1.0"`, `pApiVersion="04.04"`. Return STATUS_NOERROR. |
| 13 | `PassThruGetLastError` | DISCOVERY | Copy our last-error string into the caller's buffer. We mostly set "No error." Return STATUS_NOERROR. |
| 14 | `PassThruIoctl` | MIXED | IOCTL ID dispatch: `GET_CONFIG`/`SET_CONFIG` → store/return SCONFIG values; `READ_VBATT` → return 12000 mV; `CLEAR_TX_BUFFER`/`CLEAR_RX_BUFFER`/`CLEAR_PERIODIC_MSGS`/`CLEAR_MSG_FILTERS` → clear our per-Channel state; `FAST_INIT`/`FIVE_BAUD_INIT` → return canned response. Most others → return STATUS_NOERROR quietly. |

(Note: 14 numbered items, 13 unique exports — `PassThruIoctl` was missing from my earlier 13-count by oversight. The DLL exports 14.)

## Categories summary

- **TRIVIAL (1)**: `PassThruSetProgrammingVoltage` — no Vpp to drive in software.
- **STATEFUL (8)**: handle/state bookkeeping. PassThruOpen/Close,
  PassThruConnect/Disconnect, PassThruStart/StopPeriodicMsg,
  PassThruStart/StopMsgFilter.
- **DISCOVERY (2)**: PassThruReadVersion, PassThruGetLastError.
- **MIXED (1)**: PassThruIoctl — dispatch on IoCtl ID.
- **DATA PATH (2)**: the actual replayer.
  - `PassThruWriteMsgs` → look up + schedule events
  - `PassThruReadMsgs` → return next due event(s)

## Compared to D-PDU (CSTech2Win)

| Aspect | D-PDU (CSTech2Win) | J2534 (j2534_interface) |
|---|---|---|
| Total exports | 29 | 14 |
| Data path fns | 3 (Start/Get/RegisterCallback) | 2 (Write/Read) |
| Async delivery | Callback-driven via `PDURegisterEventCallback` | Poll-only — `PassThruReadMsgs` is the only delivery path |
| Per-msg timing | `Timestamp` field in `PDU_RESULT_DATA` | `Timestamp` field in `PASSTHRU_MSG` |
| ISO-TP framing | Done by client (we see raw multi-frames) | Done by adapter (we see assembled UDS) |
| Channel concept | "ComLogicalLink" (CLL) per protocol/ECU | "Channel" per (Device, Protocol) |

Both APIs are call-and-poll. J2534 is simpler because there's no
async callback surface — every message arrives via `PassThruReadMsgs`
polling.

## Recording format reuse

Same `recording_t` struct as the D-PDU side (one JSON file, indexed by
`(can_id, uds_request_bytes)`). The two implementations share
`recording.c` directly.

## Scheduler reuse

Same `scheduler_t`. The J2534 side just doesn't use the
`scheduler_set_callback()` mechanism — it polls in `PassThruReadMsgs`
instead. The scheduler's `pop_due` is the same primitive.

## NRC byte format (correction applies to both APIs)

When responding with NegativeResponse for an unknown request, the
correct format is:

```
7F <original_SID> 0x11
```

NOT just `7F 11`. The middle byte echoes the SID Tech2/Trionic sent.
For example:

  Request: `1A 90`        →   NRC: `7F 1A 11`
  Request: `27 01`        →   NRC: `7F 27 11`
  Request: `A9 81 12`     →   NRC: `7F A9 11`

This is fixed in `standalone-export-map.md` too.

## Build wiring

`shim/j2534/scripts/build_msvc.bat` adds the shared FremSoft sources:

```bat
cl ... ^
   src\dllmain.c src\log.c src\wrappers.c src\forwarders.c ^
   ..\..\fremsoft\src\fremsoft_j2534.c ^
   ..\..\fremsoft\src\recording.c ^
   ..\..\fremsoft\src\scheduler.c ^
   ..\..\fremsoft\src\unknown_log.c ^
   /I ..\..\fremsoft\src ^
   /I ..\..\fremsoft\src\vendor ^
   ...
```

`fremsoft_j2534.c` is the J2534 wrapper layer — same shape as
`fremsoft.c` but with PassThru* signatures.

## Out of scope for L1 MVP (J2534 side)

- Periodic-message simulation (we honor the API but don't auto-fire).
- Filter enforcement (filters are advisory; recording dictates output).
- Real timing accuracy of `PASSTHRU_MSG.Timestamp` — we set it to
  monotonic microseconds since adapter open, not synced to a real-bus
  clock.
