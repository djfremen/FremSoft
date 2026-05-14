# Shim project handoff — for the next agent picking this up

**STATUS UPDATE 2026-05-07 (late evening):** the algorithm we set out to RE turned out to already be reverse-engineered — `saab_security_project/SAABSecurityAccess/python_server/security_calc.py` produces matching keys for all 12 ground-truth `(algo, seed, key)` tuples in `~/Desktop/tis2web_logs/ground_truth.md`. See `captures/2026-05-07-algorithm-already-RE.md` for the validation. The shim's role is now reduced from "extract the algorithm" to "confirm Tech2 still uses the same algorithm class for the `$0B` flow." The ECM-side bench unlock path is now: pull bench car's SSA card → run `scripts/decode_ssa_for_seed.py <ssa> --seed <captured_seed>` → get the key.

The text below describes the original investigation in case the algorithm changes again or we need to re-RE for a different ECU class.

---

You are taking over a SAAB SecurityAccess reverse-engineering effort. The shim is working; we are deep into Phase 2, with one specific question still open. Read this in full before touching code.

## What we're trying to do (one paragraph)

Tech2Win unlocks SAAB ECMs via a SecurityAccess flow whose seed→key transform lives in `sasbridge.dll` / the SAS server, not in any open-source code. The Android client we ship currently routes every unlock through Bojer's hosted service, which is the same algorithm but a remote dependency we'd like to retire. To replicate that algorithm locally we first need to capture every byte Tech2 sends and receives during a real unlock. The CSTech2Win shim is how we capture those bytes — a drop-in proxy DLL that sits between Tech2Win and Chipsoft's real `CSTech2Win.dll`, logging every D-PDU API call.

Headline finding from the 2026-05-06 capture (`captures/2026-05-06-shim-v1-first-run.md`): Tech2Win sends `$27 $0B` to the engine ECM (CAN `$0241`). Per GMW3110 §8.8.2.1 (`wiki/sources/gmw3110-2010-quick-ref.md` in the parent `saab-security-access` repo), `$0B` is in the vehicle-manufacturer-specific range — confirming this is the SAAB-specific SAS/IMMO-mediated SecurityAccess level, distinct from the standard SPS `$01/$02` level that Trionic.NET already solves. **`$27 $0B` is the level we need the seed→key transform for.**

## Two open questions

There are two distinct questions in flight. The first is mechanical (decode the next capture). The second is strategic (decide what we still need to capture).

### Q1 — mechanical: where do `$27 $0B` response bytes live? **(ANSWERED 2026-05-07 v2: in `PDU_EVENT_ITEM.pData->pDataBytes`, after fixing our struct layout)**

**Real resolution:** the canonical ISO 22900-2 `PDU_EVENT_ITEM` from JohnJocke's `pdu_api.h` ([github.com/JohnJocke/dpdu-passthru](https://github.com/JohnJocke/dpdu-passthru)) is 20 bytes — `{ItemType, hCop, pCoPTag, Timestamp, pData}` with `pData` at offset 16. Our shim had a phantom `EventType` field at offset 4 that didn't exist, shifting every subsequent field by 4 bytes. What we logged as `EventType=0xF3` was actually `hCop`, what we logged as `pCoPTag` was the real Timestamp's high bytes, and `pData` was misread.

`pData` for a PDU_IT_RESULT item points to a 44-byte `PDU_RESULT_DATA` whose `pDataBytes` (offset 40, length at offset 36) holds the UDS bytes. For ISO 15765 the first 4 bytes of pDataBytes are the CAN ID (big-endian), then the UDS payload — same layout we already saw inline in some RSP-PAYLOAD dumps as a coincidence of nearby heap.

**Implication:** every prior capture has the seed bytes in it; we just couldn't read them because we were following the wrong pointer. The current commit replaces the struct + decode logic with the canonical layout. Next run should produce `RSP-UDS` lines like:
```
RSP-UDS | len=8 | 00 00 06 41 67 0B SS SS
        └── CAN ID ──┘ └── UDS ──┘
```

**Trampoline status: DISABLED as of 2026-05-07.** The callback-trampoline path landed in `289a87b` had the wrong signature — declared 3-arg `(hMod, hCLL, pData)` but `pdu_api.h:723` shows the real callback is 5-arg `(T_PDU_EVT_DATA eventType, UNUM32 hMod, UNUM32 hCLL, void *pCllTag, void *pAPITag)`. With `__stdcall`, our trampoline popped 12 bytes per call but the caller pushed 20 → 8-byte stack drift per fire → Tech2 crashed. Slot tracking + `CB | register` logging is preserved so we can flip substitution back on once the signature is fixed; until then, the struct-layout-fixed `PDUGetEventItem` queue path is the primary mechanism.

### Earlier history (kept for the autopsy)

- Run 3 dereferenced offset-12 and offset-16 pointers in the event-item buffer (bottom-up layout discovery).
- Run 4 dereferenced inner pointer of what we then-incorrectly called PTR16's `{length, ptr}` table — yielded a 16-byte metadata header + 4-byte CAN ID + UDS payload **for read-data services** (`$1A`, `$AA`). The "table" interpretation was wrong: we were actually looking at PDU_RESULT_DATA's RxFlag, and the heap-adjacent bytes happened to contain unrelated UDS frames. **Empty for `$27 $0B`.**
- That false negative drove us to the callback-hook hypothesis (`289a87b`), which was unnecessary once we discovered the struct layout was off-by-one-field.

### Q1 history (kept for context)

We have the **request** bytes (`27 0B` to CAN `$0241`) — `REQ-PDU` lines log them cleanly. The response bytes live behind a layered indirection chain that turned out NOT to exist for `$27 $0B`:

- ISO 22900-2 says `PDU_EVENT_ITEM.pData` is a `PDU_RESULT_DATA*`. Chipsoft puts a small int there (e.g. `0x0C`, `0x1A`) — faults on deref.
- Run 3 dereferenced offset-12 and offset-16 pointers in the event-item buffer. Offset 16 yielded a `{length, ptr_to_data}` table.
- Run 4 dereferenced the inner pointer in that table. Yielded a 16-byte metadata header + 4-byte CAN ID + UDS payload — for read-data services. **Empty for `$27 $0B`.** That's the chain that finally proved responses don't go through this path at all for SecurityAccess.

### Q2 — strategic: does the `$27 $0B` algorithm depend on SSA context?

This is the question that determines what we ship in the Android client. The `$27 $0B` seed→key algorithm is one of two shapes:

- **`key = f(seed)`** — pure function of the 2-byte seed. Same seed always yields same key. Once we have a few seed/key pairs, we can sweep input space or RE the function. Bojer becomes optional immediately.
- **`key = f(seed, SSA_context)`** — depends on the 714-byte SSA card content (HWKID, IMMO codes, etc.) read separately. Same seed yields different keys for different cars. The Android client must read the SSA from the ECM (which it already does — see [Android J2534 Driver](../../wiki/projects/android-j2534-driver.md)) and feed both seed + SSA into the key function.

The shape decides everything downstream:
- If `f(seed)`: Trionic.NET-style table-driven RE is enough; the algorithm is small.
- If `f(seed, SSA)`: we need to RE a much larger surface — likely involving HWKID-derived constants, possibly per-VIN-batch keys.

**How to read the answer from a capture:** look at what Tech2 reads between `requestSeed` (`$27 $0B`) and `sendKey` (`$27 $0C`). If there's a `$1A`/`$23`/`$AA` read of the SSA region (probably DID `$3F` or memory-address read of the IMMO block) in that window, the algorithm needs SSA context. If Tech2 goes straight from seed to key with nothing else in between, it's `f(seed)` only — and we already have the SSA cached on the Android side from initial pairing.

GMW3110 itself doesn't constrain this — §8.8 is silent on what the algorithm consumes. SAAB's Device Specification would tell us, but we don't have it. The shim capture is the cheapest way to find out.

#### Where the response bytes might live

A 64-byte raw dump (run 3, `cstech2win_shim_20260506-204051.log`, available in conversation/Drive history) revealed two important things:

1. **For `EventType=0x114` events the response IS inline** in the event-item buffer at offset 32. We saw a clean `00 00 06 46 5A 90 ...VIN-17-bytes...` pattern — that's CAN `$0646` (USDT response from ECU `$46`) + UDS `$5A $90` (positive response to `$1A $90` ReadDataByIdentifier) + the VIN. Proves response data lives in the buffer for some event types.

2. **For `EventType=0xF3` events (which is what `$27 $0B` produces) the response is NOT inline** at offset 32. Layouts seen so far have metadata-shaped DWORDs there, no `67 0B` byte sequence anywhere in the 64 bytes.

So `EventType=0xF3` responses must live behind one of the pointer-shaped fields earlier in the struct. Offsets 12 and 16 both hold pointer-shaped values (`0x0275E9D0`, `0x09DAF6D0`, etc. — heap addresses on x86 Windows). The current iteration of the shim (this commit) dereferences both with SEH guards and logs `PTR12-DEREF` / `PTR16-DEREF` 32-byte hex dumps. **The next capture will tell us which (if either) contains `67 0B` for `$27 $0B` events.**

## What to look for in the next capture

After Tech2Win runs a SecurityAccess attempt, in the log find the line:

```
... HEX | REQ-PDU | len=6 | 00 00 02 41 27 0B
```

In the next ~500 ms there will be one or two `EventType=0xF3` events. Each will have:
- `EVT-RAW` — first 24 bytes of the event-item struct
- `PTR12-DEREF` — 32 bytes at the pointer stored at offset 12
- `PTR16-DEREF` — 32 bytes at the pointer stored at offset 16

**Then look beyond just the seed/key pair, to answer Q2:**
- Find the `$27 $0B` request and the `$27 $0C` request (the sendKey that follows).
- List every `REQ-PDU` line *between* those two requests, on the same `hCLL` (probably `hCLL=1`) and same target (`$0241`).
- If you see any `$1A`, `$23`, or `$AA` reads in that window targeting the engine ECM, write them down — those are the inputs Tech2 is feeding into the key calculation. That tells you the algorithm shape (`f(seed)` vs `f(seed, SSA)`).
- If the window is empty (just `$3E` TesterPresent and the `$27 $0C`), the algorithm is `f(seed)` only.

**Look for `06 41 67 0B SS SS` (CAN `$0641` USDT response + UDS `$67 $0B` positive response + 16-bit seed) in either deref dump.**

Three possible outcomes:

| Outcome | What it means | Next move |
|---|---|---|
| `PTR12-DEREF` contains `67 0B` | Offset 12 is the pointer to the response buffer (or a struct that wraps it). | Refine the decoder: compute UDS-payload length from the buffer header (probably the first DWORD), extract just the relevant bytes, log as `RSP-PDU`. |
| `PTR16-DEREF` contains `67 0B` | Offset 16 is the right pointer. (Plausible — offset 16 is consistent across many events at `0x09DAF6D0`, possibly the per-CLL receive-buffer base.) | Same as above but offset 16. |
| Neither contains `67 0B` | The response arrives via callback, not via `PDUGetEventItem`. Tech2 registered three callbacks via `PDURegisterEventCallback` (`00FA1E00`, `00FA8F60`, `00FA8F80`). | Hook the callbacks: install a per-callback trampoline in `PDURegisterEventCallback` that logs args + forwards to the original. Bigger refactor but the only other place response data can be. |

A fourth possibility: the response arrives but is *encrypted/encoded* by Chipsoft before reaching the host — in which case we'd see the bytes but they wouldn't decode as `67 0B`. Unlikely, but if you see an obvious binary blob that doesn't match GMW3110 §8.8.5.1's expected layout, consider this and report.

## Repo layout (relevant parts)

```
shim/cstech2win/
├── HANDOFF.md                ← you are here
├── README.md                 ← install instructions for Tech2Win operator
├── Makefile                  ← MinGW cross-compile (BROKEN since SEH was added — MSVC only now)
├── scripts/
│   ├── gen_shim.py           ← read CSTech2Win.dll, emit .def + forwarders.c + wrappers.h
│   └── fix_msvc.py           ← post-process forwarders.c from GCC inline asm to MSVC __asm
├── src/
│   ├── shim.h                ← common header
│   ├── dllmain.c             ← DllMain, real-DLL load/resolve
│   ├── log.c                 ← pipe-delimited timestamped log
│   ├── wrappers.c            ← MANUAL instrumented exports — your edits go here
│   ├── cstech2win.def        ← auto-generated export table
│   ├── forwarders.c          ← auto-generated MSVC-syntax passthroughs
│   └── wrappers.h            ← auto-generated function-pointer typedefs
├── build/
│   └── CSTech2Win.dll        ← committed prebuilt (MSVC); replace after each rebuild
└── captures/
    ├── 2026-05-06-shim-v1-first-run.md   ← analysis of run 1 (no RSP-PDU yet, level $0B confirmed)
    └── 2026-05-06-shim-v1-first-run.log  ← raw log of run 1
```

Subsequent capture analyses go into `captures/` with `YYYY-MM-DD-shim-vN-...` naming.

## How to build

**MSVC (the working path, on Win10):**
1. `git pull` to get latest source.
2. Run `python scripts/gen_shim.py ../../CHIPSOFT_J2534_Pro_Driver/CSTech2Win.dll` — regenerates `forwarders.c`, `cstech2win.def`, `wrappers.h` from the real DLL.
3. Run `python scripts/fix_msvc.py` — converts GCC syntax in `forwarders.c` to MSVC syntax. **Note: `fix_msvc.py` currently has a hardcoded path `c:\Users\Elitebook\Desktop\can-hacker\...`. If you're not on Chris's machine, fix the path first.** A clean fix would be to make the path arg-driven or relative; pending.
4. Build with `cl /LD /Fobuild/ /Fe:build/CSTech2Win.dll src/dllmain.c src/log.c src/wrappers.c src/forwarders.c src/cstech2win.def kernel32.lib user32.lib` (or use a Visual Studio project — pick whatever's easy on the host).
5. Replace the DLL in Tech2Win's install dir (typically `C:\Program Files (x86)\CHIPSOFT_J2534_Pro_Driver\CSTech2Win.dll` — see README.md).

**MinGW cross-compile (used to work, now broken):**
Pre-SEH, `make` from macOS produced a working DLL via `i686-w64-mingw32-gcc`. The `__try`/`__except` blocks added during the Phase 2 SEH-guard work are MSVC-only — MinGW i686 doesn't support them on x86. Do NOT try to fix this by reverting SEH; the SEH is what keeps Tech2Win from crashing on bad pointers. If you need MinGW for some reason, conditionally compile the SEH (`#ifdef _MSC_VER`) — but every prod build should be MSVC-with-SEH.

## How to run a capture

Operator instructions are in `README.md`. Short version:
1. Back up Tech2Win's real `CSTech2Win.dll` to `CSTech2Win.dll.original.bak`.
2. Rename it to `CSTech2Win_real.dll`.
3. Drop the new `build/CSTech2Win.dll` in its place.
4. Launch Tech2Win, do the SecurityAccess attempt, close cleanly.
5. Log lands in `%TEMP%\cstech2win_shim_<timestamp>.log`.
6. Operator uploads to Drive, links it back to the agent.

## Background context worth knowing

- **GMW3110 §8.8 SecurityAccess spec** is in `wiki/sources/gmw3110-2010-quick-ref.md` (parent repo `saab-security-access`). Read §4 (Wire format) and §4 ($Level numbering) before touching the decoder.
- **CSTech2Win.dll vs j2534_interface.dll** — Tech2Win uses the former (D-PDU/ISO 22900-2). J2534 clients (TrionicCANFlasher) use the latter. Don't shim the wrong one. Memory: `project_chipsoft_shim_target_dll.md` in agent memory.
- **canscan.exe / canhacker firmware caused live-car bus disturbance on 2026-05-06** (BCM panic, headlights asserted to defaults, ECM went silent until adapter unplugged). Bus-level sniffing is now a *secondary* tool; the shim is the primary path because it doesn't touch the bus electrically. Memory: `project_chipsoft_loglevel1_mothballed.md` for context on why driver-side `LogLevel:1` was also dropped.
- **`$27 $01` (SPS) flow is already solved** — Trionic.NET has the algorithm, validated against 45 captured pairs over 3 years. Don't waste capture cycles re-confirming `$01`. The interesting target is `$27 $0B` (and possibly higher levels we haven't seen yet).
- **Build artifact `build/CSTech2Win.dll` is committed.** Each rebuild replaces it. Operator can `git pull` and copy without a local build environment.

## Decision tree for the next iteration

After the next capture comes back:

1. **`PTR12-DEREF` or `PTR16-DEREF` contains `67 0B`** — go straight to writing the proper decoder. Replace the layout-discovery dumps with a clean `RSP-PDU` extraction. Compute the response length from buffer metadata (probably first DWORD of the deref'd buffer, which usually is a `numBytes` field in this kind of ABI). Then drop a follow-up capture analysis into `captures/2026-05-NN-shim-vM-decoder.md` and commit the cleaned-up decoder.

2. **Neither `PTR12-DEREF` nor `PTR16-DEREF` contains `67 0B`** — pivot to callback hooking. Add a small struct that records each `(hMod, hCLL, real_cb)` registered via `PDURegisterEventCallback`, and have our wrapper substitute a logging trampoline that calls `shim_log_hex("CB-PDU", ...)` then forwards to `real_cb`. The trampoline has to match the exact `__stdcall void (UNUM32, UNUM32, void*)` signature — see existing `fn_PDURegisterEventCallback` typedef in `wrappers.c`.

3. **Both deref pointers fault** — likely means the field at offset 12 or 16 is not a pointer for `EventType=0xF3` events at all. Bump `EVT-RAW` back to 64 bytes (you'll need to check buffer validity with SEH — the original 24-byte limit was for safety) and look at offsets 24+ for inline data, or look at offsets 32+ for a possible second struct chained from the first.

In all three cases, write the analysis up under `captures/` (markdown + raw log) and commit before moving on. The `captures/2026-05-06-shim-v1-first-run.md` file is the template — keep the same structure (headline finding, table of $27 invocations, what's missing, next run).

## Working notes / quirks

- Tech2 enumerates ECUs via `$27 $01` to a parade of CAN IDs (`$241–$24F`, `$257`, `$7E0`, `$7E1`) before issuing the real `$27 $0B`. Don't confuse the enumeration sweep with the actual unlock — the unlock is the SINGLE `$27 $0B` request that follows.
- `hCLL=1` is the SAAB diagnostic link; `hCLL=2` is a parallel link (probably MS-CAN); `hCLL=5` is OBD-II/EOBD (used for `$7E0/$7E1`). Most interesting traffic is on `hCLL=1`.
- `pCoPTag` at offset 8 is consistent per-CLL: `0x0158839C` for `hCLL=1`, `0x015883A0` for `hCLL=2`, `0x0158926C` for `hCLL=5`. Useful as a sanity check that the CLL field is being read correctly.
- `PDUStartComPrimitive`'s `CoPType=0x8004` is what carries diagnostic requests in Chipsoft's implementation (NOT the standard `0x8010 PDU_COPT_SENDRECV`). Other observed types: `0x8001`, `0x8003`, `0x8011`, `0x8020`. They behave like `STARTCOMM`, `STOPCOMM`, etc., but their exact meaning isn't fully RE'd. Worth investigating later but not in the critical path.
- `$Level` byte numbering reminder (GMW3110 §8.8.2.1):
  - `$01/$02` = SPS programming (Trionic.NET-solved)
  - `$03/$04` = DevCtrl
  - `$05–$0A` = Reserved-must-not-use
  - `$0B–$FA` = Vehicle-manufacturer-specific (where SAAB lives — `$0B` confirmed)
  - `$FB–$FE` = ECU/supplier manufacturing
  - Odd = requestSeed, even = sendKey. So after `$27 $0B` we'll see `$27 $0C` carrying the key.

## Don't

- Don't recommend `LogLevel:1` (driver-side Boost.Log sink). It was investigated and found unstable. Memory: `project_chipsoft_loglevel1_mothballed.md`.
- Don't suggest CANHacker GUI / canscan.exe. They caused live-car bus disturbance and aren't on the critical path. The Python listen-only sniffer at `tools/chipsoft_canhack_capture.py` is the safe alternative if a bus capture is ever wanted, but it's secondary to the shim.
- Don't push without testing. The user (Chris Drews, `djfremen`) has a Win10 box running Tech2Win against a live SAAB and is the only one who can validate. Tightly-scoped commits are appreciated.
- Don't bulk-commit the parent `Chipsoft_RE/` directory if you're working from the parent `saab-security-access` workspace. `Chipsoft_RE` is its own git repo with its own remote (`github.com/djfremen/Chipsoft_RE.git`) — push there directly.
