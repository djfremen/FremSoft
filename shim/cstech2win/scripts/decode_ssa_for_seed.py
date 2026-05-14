#!/usr/bin/env python3
"""
decode_ssa_for_seed.py — decode a 714-byte SSA card and find the key for a
specific captured `$27 0B` seed.

Background (see HANDOFF.md):
  - The SSA card stores up to ~12 (status, algo, seed, key) SKA tuples.
  - When the engine ECM responds to `$27 0B` with a seed, that seed is one of
    the stored tuples — the matching `key` field is the answer the ECM is
    waiting for.
  - If the seed isn't in the stored tuples (rare), we can fall back to
    recomputing keys for each (algo present, target_seed) pair via
    security_calc.py — that algorithm is the RE'd transform, validated 12/12
    against `~/Desktop/tis2web_logs/ground_truth.md`.

Usage:
  python decode_ssa_for_seed.py <ssa.bin> [--seed 0xC4DC]

Layout (verified against bojer_ssa.bin, HWKID Q000000010):
  offset 0x00:    `B1` magic (1B)
  offset 0x01:    `00` (1B)
  offset 0x02-0x0B: HWKID (10 ASCII bytes, e.g. "Q000000010")
  offset 0x0C-0x0D: SSA version (2B BE, e.g. 0x12EF)
  offset 0x0E-0x0F: free-shots / status (2B)
  offset 0x10-0x13: padding 0xFF
  offset 0x14:    VIN tuples block (variable, up to 4 VINs × ~36 bytes incl.
                  17B VIN + null + 8B SecurityCode)
  offset 0x132:   SKA tuples block — 12 entries × 8 bytes:
                    status(2B) | algo(2B BE) | seed(2B BE) | key(2B BE)
                    (e.g. for tuple #0: 00 00 03 66 39 49 82 49 = OK,
                     algo=0x0366, seed=0x3949, key=0x8249)
"""
import argparse
import struct
import sys
from pathlib import Path

try:
    sys.path.insert(0, str(Path(__file__).resolve().parents[4]
                          / "saab_security_project"
                          / "SAABSecurityAccess"
                          / "python_server"))
    import security_calc
except ImportError:
    security_calc = None


SKA_OFFSET = 0x132
SKA_COUNT = 12
SKA_ENTRY_SIZE = 8


def parse_ssa(data: bytes) -> dict:
    if len(data) != 714:
        print(f"WARN: expected 714 bytes, got {len(data)}", file=sys.stderr)

    hwkid = data[2:12].decode("ascii", errors="replace").rstrip("\x00")
    version = struct.unpack(">H", data[0x0C:0x0E])[0]
    free_shots = struct.unpack(">H", data[0x0E:0x10])[0]

    tuples = []
    for i in range(SKA_COUNT):
        off = SKA_OFFSET + i * SKA_ENTRY_SIZE
        status, algo, seed, key = struct.unpack(">HHHH", data[off:off + SKA_ENTRY_SIZE])
        tuples.append({
            "index": i,
            "offset": off,
            "status": status,
            "algo": algo,
            "seed": seed,
            "key": key,
            "blank": (status == 0xFFFF and algo == 0xFFFF
                      and seed == 0xFFFF and key == 0xFFFF),
        })
    return {
        "hwkid": hwkid,
        "version": version,
        "free_shots": free_shots,
        "tuples": tuples,
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("ssa", help="Path to 714-byte SSA capture")
    ap.add_argument("--seed", default=None,
                    help="Target seed (hex, e.g. 0xC4DC). If present in the "
                         "tuples, prints the stored key. Otherwise computes "
                         "candidate keys via security_calc.py.")
    args = ap.parse_args()

    data = Path(args.ssa).read_bytes()
    info = parse_ssa(data)

    print(f"SSA file:    {args.ssa}")
    print(f"HWKID:       {info['hwkid']}")
    print(f"Version:     0x{info['version']:04X} ({info['version']})")
    print(f"Free shots:  0x{info['free_shots']:04X}")
    print()
    print("SKA tuples:")
    for t in info["tuples"]:
        if t["blank"]:
            print(f"  #{t['index']:02d} @ 0x{t['offset']:03X}  -- blank --")
        else:
            print(f"  #{t['index']:02d} @ 0x{t['offset']:03X}  "
                  f"status=0x{t['status']:04X}  algo=0x{t['algo']:04X}  "
                  f"seed=0x{t['seed']:04X}  key=0x{t['key']:04X}")

    if not args.seed:
        return 0

    target = int(args.seed, 0)
    print()
    print(f"Looking up seed 0x{target:04X}...")

    matches = [t for t in info["tuples"]
               if not t["blank"] and t["seed"] == target]
    if matches:
        print()
        for m in matches:
            print(f"  ✓ FOUND in tuple #{m['index']:02d}: "
                  f"algo=0x{m['algo']:04X}, key=0x{m['key']:04X}")
            print(f"    (no algorithm computation needed — key is stored in card)")
        return 0

    print(f"  Seed 0x{target:04X} not in stored tuples.")
    if security_calc is None:
        print("  security_calc.py not importable; cannot compute candidates.", file=sys.stderr)
        return 1

    algos = sorted({t["algo"] for t in info["tuples"] if not t["blank"]})
    if not algos:
        print("  No populated tuples to derive candidate algos from.", file=sys.stderr)
        return 1

    print(f"  Computing candidate keys via security_calc for {len(algos)} algo(s) seen in this card:")
    for algo in algos:
        try:
            k = security_calc.get_key_from_seed(target, algo)
            print(f"    algo=0x{algo:04X}  key=0x{k:04X}")
        except Exception as e:
            print(f"    algo=0x{algo:04X}  ERROR: {e}", file=sys.stderr)

    print()
    print("  Note: the actual algo for this seed is whichever is stored in the "
          "ECU's matching tuple. If your captured seed isn't in the card, the "
          "ECU is using a tuple that wasn't filled by the SAS server — pull a "
          "fresh SSA after pairing this VIN.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
