#!/usr/bin/env python3
"""Build the unofficial 1005.0 experiment offline. Never accesses a monitor."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
import firmware_container as fmt


def apply_delta(body, delta):
    fmt.require(fmt.sha(body) == delta["base_body_sha256"], "Unexpected base application hash")
    changed = bytearray(body)
    previous_end = 0
    for entry in sorted(delta["changes"], key=lambda x: x["offset"]):
        offset = entry["offset"]
        before, after = bytes.fromhex(entry["before"]), bytes.fromhex(entry["after"])
        fmt.require(len(before) == len(after), "Patch changes application length")
        fmt.require(offset >= previous_end and offset + len(before) <= len(body), "Overlapping or invalid patch range")
        fmt.require(offset + int(delta["runtime_base"], 16) == int(entry["address"], 16), "Runtime address mismatch")
        fmt.require(body[offset:offset + len(before)] == before, "Patch-site bytes mismatch")
        changed[offset:offset + len(after)] = after
        previous_end = offset + len(before)
    fmt.require(fmt.sha(changed) == delta["patched_body_sha256"], "Patched application hash mismatch")
    return bytes(changed)


def build(stock):
    fmt.require(fmt.sha(stock) == fmt.STOCK_SHA256, "Only the pinned official 1004.5 BIN is supported")
    delta = json.loads((ROOT / "patches/1005.0-experimental.1.json").read_text())
    _, body, _ = fmt.inspect(stock)
    patched = apply_delta(body, delta)
    candidate = fmt.repack(stock, patched)
    report, expanded, _ = fmt.inspect(candidate)
    fmt.require(expanded == patched, "Final container decompression mismatch")
    fmt.require(candidate[:0x10000] == stock[:0x10000], "Bootstrap changed")
    fmt.require(candidate[0x10080:0x20080] == stock[0x10080:0x20080], "PM51 changed")
    report.update({"build": delta["name"], "source_sha256": fmt.sha(stock),
                   "hardware_tested": False, "changes": delta["changes"]})
    return candidate, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_bin", type=Path)
    parser.add_argument("output_bin", type=Path)
    args = parser.parse_args()
    report_path = args.output_bin.with_suffix(args.output_bin.suffix + ".json")
    fmt.require(not args.output_bin.exists() and not report_path.exists(), "Refusing to overwrite output")
    candidate, report = build(args.stock_bin.read_bytes())
    args.output_bin.write_bytes(candidate)
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "changes"}, indent=2))


if __name__ == "__main__":
    main()
