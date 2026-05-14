# FremSoft

**FremSoft is a playback / standalone mode for the `CSTech2Win.dll` shim.** When the registry mode is set to `playback`, the shim stops forwarding to the real Chipsoft DLL and serves UDS responses from a pre-recorded JSON session. Tech2Win cannot tell the difference — that's the point.

Name: **Fremen + soft**, with a pun on Chipsoft.

## Three layers of value

1. **Replay.** Drive Tech2Win against a ghost car. No ECM connected. Useful for development, dogfooding, training, field repair walk-throughs.
2. **Probe capture.** Tech2Win sends an unrecorded request → FremSoft logs `(can_id, uds_bytes)` to `%TEMP%\fremsoft_unknown_<ts>.log` and replies with `7F 11 ServiceNotSupported` so Tech2Win moves on. The unknown log becomes a self-discovered protocol map.
3. **Module emulation.** Once every Tech2Win request is known, build per-module responders (fake CIM, fake BCM, fake ECM) that produce internally consistent state. Tech2Win drives a complete software-only SAAB.

## How it switches mode

The shim DLL reads `HKLM\SOFTWARE\OpenSAAB\Collector` at `DLL_PROCESS_ATTACH`:

| Mode | Behavior |
|---|---|
| `passthrough` | Forward every call to the real DLL; no logging. |
| `record` (default) | Forward + write a shim log to `%TEMP%\cstech2win_shim_*.log`. |
| `playback` | Serve responses from `PlaybackRecording` JSON; real DLL is loaded but never called. |
| `standalone` | Serve from recording; do not load the real DLL at all. |

```
reg add HKLM\SOFTWARE\OpenSAAB\Collector /v Mode /t REG_SZ /d playback /f
reg add HKLM\SOFTWARE\OpenSAAB\Collector /v PlaybackRecording /t REG_SZ /d "C:\path\to\recording.json" /f
```

Helper `.cmd`s ship in `installer/`:

- `installer/enable-playback.cmd` — UAC self-elevate; set `Mode=playback`.
- `installer/enable-record.cmd` — UAC self-elevate; set `Mode=record`.

## Layout

```
FremSoft/
├── src/                 # core engine: fremsoft.c/h, recording.c/h, scheduler.c/h, unknown_log.c
│   └── vendor/jsmn.h    # header-only JSON parser (MIT)
├── docs/                # design.md + standalone export maps for CSTech2Win and J2534
├── recordings/          # captured JSON sessions; 2026-05-13-check-codes.json is the seed
├── tools/               # build_recording.py — Mac-side log → JSON converter
├── shim/
│   └── cstech2win/      # playback-capable CSTech2Win.dll shim
│       ├── src/
│       ├── scripts/build_msvc.bat   # Win10 MSVC build
│       └── Makefile                  # macOS/Linux MinGW cross-build
└── installer/           # enable-playback.cmd, enable-record.cmd
```

## Recording format

Indexed JSON, lookup key `(can_id, uds_bytes)`:

```json
{
  "recording_id": "2026-05-13-check-codes",
  "captured_at": "2026-05-13T11:48:26-07:00",
  "vehicle_profile": "SAAB 9-3 (bench, 2017)",
  "source_log": "cstech2win_shim_20260513-114826.log",
  "exchanges": [
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

Converter: `tools/build_recording.py` ingests CSTech2Win shim logs and emits the JSON.

## Live activity log

When FremSoft is active (`playback` or `standalone`), it also writes `%TEMP%\fremsoft_<wall_ms>.log` in the same line format as the existing CSTech2Win shim log, so the bundled scapy decoder ingests it with no special-casing.

## L1 test plan

1. Build the playback shim on Win10 (`shim/cstech2win/scripts/build_msvc.bat`).
2. Install on a Chipsoft-equipped laptop alongside the genuine `CSTech2Win.dll` (rename real → `CSTech2Win_real.dll`, drop the FremSoft build alongside).
3. `installer\enable-playback.cmd` → set `Mode=playback` + point at `recordings/2026-05-13-check-codes.json`.
4. Launch Tech2Win, open Check Codes.
5. **Expected:** the same DTCs displayed as in the original capture (e.g. `B3832-45` on doors). No real ECM connected.
6. Inspect `%TEMP%\fremsoft_unknown_*.log` for any out-of-recording requests — each one is a protocol-map data point.

## Relationship to sibling projects

- [OpenSAAB-Collector](https://github.com/djfremen/OpenSAAB-Collector) — captures shim logs in `record` mode and uploads them to a central server. Pairs with FremSoft (Collector captures, FremSoft replays).
- [Chipsoft_RE](https://github.com/djfremen/Chipsoft_RE) — reverse-engineering corpus that produced the CSTech2Win shim scaffolding FremSoft extends.
- [OpenSAAB](https://github.com/djfremen/OpenSAAB) — Apache-licensed protocol catalog whose YAMLs FremSoft can eventually synthesize answers from for L2 (no recording required).

## License

Apache 2.0 — see `LICENSE`.
