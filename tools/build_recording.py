#!/usr/bin/env python3
"""Convert a CSTech2Win shim log into a FremSoft playback recording.

Input:  cstech2win_shim_<ts>.log  — the format produced by the shim
        committed at Chipsoft_RE/shim/cstech2win/.
Output: recordings/<recording_id>.json  — the format documented in
        Chipsoft_RE/fremsoft/docs/design.md.

The converter:
  - Parses each "REQ-PDU" / "RSP-UDS" line out of the shim log.
  - Strips the chipsoft 4-byte header (00 00 <can_id_hi> <can_id_lo>)
    to get the (can_id, uds_bytes) pair Tech2Win actually exchanges.
  - Pairs each TX with the RX frames that follow before the next TX
    (or until a wall-clock gap larger than --max-gap-ms, default 3000).
  - Computes per-RX delay_ms relative to the preceding frame.
  - Indexes by (can_id, uds_bytes); if the same TX appears multiple
    times with different responses, keeps the FIRST and warns.

Usage:
    python3 build_recording.py <shim.log> [--output recordings/foo.json]
                               [--vehicle "SAAB 9-3 (bench, 2017)"]
                               [--id 2026-05-13-check-codes]
                               [--max-gap-ms 3000]

Pure stdlib, no scapy / no third-party deps. Safe to run on any host.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

# Shim-log line: <ms>|<tid>|HEX|<REQ-PDU|RSP-UDS>|len=<n>|<hex bytes>
LINE_RE = re.compile(
    r"^(?P<ms>\d+)\|(?P<tid>\d+)\|HEX\s*\|"
    r"(?P<kind>REQ-PDU|RSP-UDS)\|len=(?P<len>\d+)\|(?P<hex>.+)$"
)


def parse_log(path: Path):
    """Yield (ms, direction, can_id, uds_bytes) tuples from the shim log."""
    for line in path.read_text(errors="replace").splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        try:
            raw = bytes.fromhex(m.group("hex").replace(" ", ""))
        except ValueError:
            continue
        if len(raw) < 5:
            continue
        # bytes[0:2] = chipsoft channel/source (typically 00 00).
        # bytes[2:4] = CAN ID big-endian.
        # bytes[4:]  = UDS payload (ISO-TP PCI already stripped).
        can_id = (raw[2] << 8) | raw[3]
        uds = raw[4:]
        direction = "tx" if m.group("kind") == "REQ-PDU" else "rx"
        yield int(m.group("ms")), direction, can_id, uds


def hexbytes(b: bytes) -> str:
    return " ".join(f"{x:02x}" for x in b)


def pair_exchanges(frames, max_gap_ms: int):
    """Group frames into (TX → list of RX) exchanges.

    A TX is paired with all RX frames that arrive before the NEXT TX,
    capped by max_gap_ms (so a long idle doesn't accumulate everything
    into one bucket). Floating RX frames before the first TX are dropped.
    """
    exchanges = []
    current = None
    last_ms = None
    for ms, direction, can_id, uds in frames:
        if direction == "tx":
            if current:
                exchanges.append(current)
            current = {
                "tx_ms": ms,
                "tx_can_id": can_id,
                "tx_uds": uds,
                "rx_list": [],
                "last_seen_ms": ms,
            }
            last_ms = ms
        else:  # rx
            if current is None:
                continue
            if ms - current["last_seen_ms"] > max_gap_ms:
                exchanges.append(current)
                current = None
                continue
            delay = ms - current["last_seen_ms"]
            current["rx_list"].append({
                "can_id": can_id,
                "uds": uds,
                "delay_ms": delay,
            })
            current["last_seen_ms"] = ms
    if current:
        exchanges.append(current)
    return exchanges


def index_by_request(exchanges):
    """Build the lookup map keyed by (can_id, uds_bytes).

    On collision (same TX seen again later with different RX), keep
    the first observed pairing and emit a warning to stderr.
    """
    seen = {}
    collisions = 0
    out = []
    for ex in exchanges:
        key = (ex["tx_can_id"], bytes(ex["tx_uds"]))
        if key in seen:
            prev = seen[key]
            if [r["uds"] for r in prev] != [r["uds"] for r in ex["rx_list"]]:
                collisions += 1
                if collisions <= 5:
                    print(
                        f"  warn: duplicate TX ${ex['tx_can_id']:04X} "
                        f"{hexbytes(bytes(ex['tx_uds'])[:4])}… seen with "
                        f"different RX; keeping first.",
                        file=sys.stderr,
                    )
            continue
        seen[key] = ex["rx_list"]
        out.append({
            "tx": {
                "can_id": f"0x{ex['tx_can_id']:04X}",
                "uds": hexbytes(bytes(ex["tx_uds"])),
            },
            "rx": [
                {
                    "can_id": f"0x{r['can_id']:04X}",
                    "uds": hexbytes(bytes(r["uds"])),
                    "delay_ms": int(r["delay_ms"]),
                }
                for r in ex["rx_list"]
            ],
        })
    if collisions:
        print(f"  ({collisions} duplicate TX collisions total — kept first observed RX for each)",
              file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shim_log", help="Path to cstech2win_shim_*.log")
    ap.add_argument("--output", "-o", default=None,
                    help="Output JSON path (default: recordings/<id>.json)")
    ap.add_argument("--id", default=None,
                    help="Recording ID (default: derived from log filename)")
    ap.add_argument("--vehicle", default="SAAB 9-3 (bench, unspecified MY)",
                    help="vehicle_profile string for the recording metadata")
    ap.add_argument("--max-gap-ms", type=int, default=3000,
                    help="Stop accumulating RX into a TX bucket after this gap")
    args = ap.parse_args()

    log_path = Path(args.shim_log)
    if not log_path.exists():
        sys.exit(f"shim log not found: {log_path}")

    rec_id = args.id or log_path.stem.replace("cstech2win_shim_", "")

    print(f"Parsing {log_path.name}…")
    frames = list(parse_log(log_path))
    n_tx = sum(1 for _, d, *_ in frames if d == "tx")
    n_rx = sum(1 for _, d, *_ in frames if d == "rx")
    print(f"  {len(frames)} frames ({n_tx} TX, {n_rx} RX)")

    print(f"Pairing exchanges (max_gap_ms={args.max_gap_ms})…")
    exchanges = pair_exchanges(frames, args.max_gap_ms)
    print(f"  {len(exchanges)} exchanges before dedup")

    print("Indexing by (can_id, uds)…")
    indexed = index_by_request(exchanges)
    print(f"  {len(indexed)} unique (can_id, uds) keys after dedup")

    out = {
        "recording_id": rec_id,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "vehicle_profile": args.vehicle,
        "source_log": log_path.name,
        "exchanges": indexed,
    }

    out_path = Path(args.output) if args.output else (
        Path(__file__).parent.parent / "recordings" / f"{rec_id}.json"
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out, indent=2))
    print(f"\nWrote {out_path}  ({out_path.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
