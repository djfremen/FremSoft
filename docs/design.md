# FremSoft — design

**Status:** Day-1 design doc, no code committed yet.

FremSoft is a behavior mode added to the existing `CSTech2Win.dll`
shim (built today at `Chipsoft_RE/shim/cstech2win/`). When the mode
flag is set to `playback`, the shim stops forwarding to the real
Chipsoft DLL and instead serves responses from a pre-recorded session.

Tech2Win can't tell the difference (that's the point).

## Why this is worth building

Three layers of value, in order of priority:

1. **Replay.** Demo Tech2Win against a "ghost car" without an actual
   ECM. Useful for development, dogfooding, training, and field repair
   tests where you want to show "this is what the menu would look like
   on a real car."
2. **Probe capture.** When Tech2Win sends a request that's not in our
   recording, we log it and return a polite NRC. The log of unknowns
   becomes a protocol map — every menu Tech2Win can possibly invoke,
   discovered by letting Tech2Win itself walk us through them.
3. **Module emulation.** Once we know every request, we build per-module
   responders (fake CIM, fake BCM, fake ECM) that produce internally
   consistent state. Tech2Win drives a complete software-only SAAB.

## Mode switch

The CSTech2Win shim picks its mode at `DllMain DLL_PROCESS_ATTACH` from
the registry:

```
HKLM\SOFTWARE\OpenSAAB\Collector
    Mode               REG_SZ   "passthrough" | "record" | "playback"
    PlaybackRecording  REG_SZ   path to .json file
```

Defaults: `Mode=record` (today's behavior — forward to real DLL +
write shim log to `%TEMP%`). Setting `Mode=playback` selects the
FremSoft replayer; `PlaybackRecording` points at the recording.

The shim still loads the real `CSTech2Win_real.dll` at attach time so
`passthrough` and `record` work normally. In `playback` mode, the real
DLL is loaded but never called — kept around so we could fall back to
passthrough mid-session if ever useful.

## Recording format

Indexed JSON, lookup-friendly. One file per recording.

```json
{
  "recording_id": "2026-05-13-check-codes",
  "captured_at": "2026-05-13T11:48:26-07:00",
  "vehicle_profile": "SAAB 9-3 (bench, 2017)",
  "source_log": "cstech2win_shim_20260513-114826.log",
  "exchanges": [
    {
      "tx": {"can_id": "0x0241", "uds": "20"},
      "rx": [
        {"can_id": "0x0641", "uds": "60", "delay_ms": 119}
      ]
    },
    {
      "tx": {"can_id": "0x0241", "uds": "A9 81 12"},
      "rx": [
        {"can_id": "0x054B", "uds": "81 b8 32 45 13 00 00 00", "delay_ms": 87},
        {"can_id": "0x054B", "uds": "81 00 00 00 12 00 00 00", "delay_ms": 13}
      ]
    }
  ]
}
```

Lookup key for the runtime is `(can_id, uds_bytes)`. The replayer
returns the ordered `rx` list with the `delay_ms` between each. If a
TX appears multiple times in the source log with different responses,
the converter picks the first one (with a note in the log) — multi-
response support is a later enhancement.

## Replayer logic

State: `Map<(uint32 can_id, bytes uds), Queue<Response>>` loaded once
at attach.

For each `PDUStartComPrimitive` (Tech2Win sends a UDS request):

1. Decode the request to `(can_id, uds_bytes)`.
2. Look up in the map.
3. If found: pop the next `Response`, schedule callbacks via
   `PDURegisterEventCallback` to deliver the bytes after `delay_ms`.
4. If not found: append to `%TEMP%\fremsoft_unknown_<ts>.log` AND
   schedule a `7F 11 ServiceNotSupported` reply after 50 ms so
   Tech2Win moves on cleanly.

For each `PDUGetEventItem` (Tech2Win polls for replies): drain the
queued callbacks. Synchronous return path is just status.

## Threading

Tech2Win expects `PDURegisterEventCallback` to fire on a separate
thread. The replayer maintains a single delivery thread that consumes
a priority queue of `(scheduled_time, response_bytes, callback_ptr)`.
`delay_ms` from the recording is added to "now" at the moment of the
TX call, then the delivery thread sleeps until that time and fires.

For the MVP this is "good enough." If we hit timing accuracy issues
we'll switch to multimedia timers.

## Live activity log (decoder-friendly)

When FremSoft is active (playback or standalone mode), it ALSO writes
its full activity to `%TEMP%\fremsoft_<wall_ms>.log` in the **same
line format** the existing CSTech2Win shim uses:

```
<ms>|<tid>|HEX|REQ-PDU|len=<n>|<chipsoft 4-byte header><uds bytes>
<ms>|<tid>|HEX|RSP-UDS|len=<n>|<chipsoft 4-byte header><uds bytes>
```

This means the same `fremsoft-decoder.exe` (PyInstaller-bundled
scapy-based dissector that ships in the OpenSAAB Collector) tail-pipes
this file with no special-casing. The Collector's tray already
recognises `fremsoft_*` as a third log family alongside
`cstech2win_shim_*` and `j2534_shim_*`.

Practical effect: launch Tech2Win against FremSoft, open the tray's
"Open decoded console (scapy)" menu item, and watch decoded UDS
service names scroll past as Tech2Win exercises the API — even
though no real ECM is present.

## Unknown-request log format

```
ms_since_attach | wall_clock_ms | tid | UNKNOWN | can_id=0x0241 | uds=AA 01 0F
```

Same line shape as the existing shim log so the same decoders work.
The point of this file is for me (or a script) to read it after a
session and learn "Tech2Win tried these requests but we had no
recorded response for them." Each one becomes a candidate for the
next bench capture.

## What this needs from the shim that doesn't exist yet

- **Mode-switch read** at attach time (registry).
- **Recording loader** (parse JSON, build the lookup map).
- **Replayer state machine** (queue + delivery thread).
- **PDUStartComPrimitive override** in playback mode to consult the
  map instead of forwarding.
- **PDURegisterEventCallback override** to remember Tech2Win's
  callback pointer so the delivery thread can fire it.
- **Unknown-request logger** (mostly the same as the existing
  `shim_log` but with a different filename + tag).

## What does NOT need to change

- The forwarder stubs for the 22+ exports we don't instrument.
- The wrappers for `PDUSetComParam`, `PDUIoCtl`, `PDUConstruct`,
  `PDUDestruct` — these still return success in playback mode (we
  pretend the channel/device is configured fine).
- The auto-generated `cstech2win.def` + naked-jmp forwarders.

## Out of scope for the L1 MVP

- Multi-recording library (one recording at a time).
- Adaptive matching (e.g. partial UDS match if exact bytes differ).
- OpenSAAB-catalog synthesis fallback.
- Full TX-side state machine (channel state, filter state). We assume
  Tech2Win does its own bookkeeping and just needs the bytes back.
- Recording capture from a non-shim source (slcan, USBPcap, etc.).

## Build/install plan

The MVP ships as a behavior change inside the existing CSTech2Win
shim. No new DLL, no installer changes. To switch a Collector install
into playback mode:

```
reg add HKLM\SOFTWARE\OpenSAAB\Collector /v Mode /t REG_SZ /d playback /f
reg add HKLM\SOFTWARE\OpenSAAB\Collector /v PlaybackRecording /t REG_SZ /d "C:\path\to\recording.json" /f
```

Then restart Tech2Win. Switching back is `/d record /f`.

A future iteration adds tray-app UI to flip modes without `reg add`.

## Test plan for L1

1. Build the recording converter on Mac (Python).
2. Convert today's `cstech2win_shim_20260513-114826.log` (Check Codes)
   to `recordings/2026-05-13-check-codes.json`.
3. Patch the C shim with playback mode, build on EliteBook.
4. Install on EliteBook (or the second laptop with Chipsoft) with
   `Mode=playback` + recording path.
5. Launch Tech2Win, open the Check Codes menu.
6. **Expected:** the same DTCs displayed as in the original capture
   (e.g. `B3832-45` on doors, etc.). No real ECM connected.
7. If Tech2Win sends anything outside the recording, check
   `%TEMP%\fremsoft_unknown_*.log` for the protocol-map gold.

## Naming

"FremSoft" — Fremen + soft, also a pun on Chipsoft. Lives at
`Chipsoft_RE/fremsoft/` since it's a downstream of the existing
Chipsoft RE work.
