#!/usr/bin/env python3
"""Offline checks; supply the exact stock BIN separately (not redistributed)."""
import importlib.util
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build
fmt = build.fmt


def main():
    stock = Path(sys.argv[1]).read_bytes()
    _, body, _ = fmt.inspect(stock)
    checks = []
    assert fmt.repack(stock, body, reuse_original_stream=True) == stock
    checks.append("byte-exact stock reconstruction, including all four CRC fields")
    candidate, report = build.build(stock)
    assert candidate == build.build(stock)[0]
    checks.append("deterministic candidate rebuild within this zlib runtime")
    _, patched, _ = fmt.inspect(candidate)
    delta = json.loads((ROOT / "patches/1005.0-experimental.1.json").read_text())
    allowed = set()
    reverse = bytearray(patched)
    for change in delta["changes"]:
        offset = change["offset"]
        before = bytes.fromhex(change["before"])
        allowed.update(range(offset, offset + len(before)))
        reverse[offset:offset + len(before)] = before
    actual = {i for i, (a, b) in enumerate(zip(body, patched)) if a != b}
    assert actual <= allowed and bytes(reverse) == body
    checks.append("application delta is confined to hook, stub and version; exact reverse delta restores original")
    assert candidate[:0x10000] == stock[:0x10000]
    assert candidate[0x10080:0x20080] == stock[0x10080:0x20080]
    old_head, new_head = bytearray(stock[0x20080:0x23880]), bytearray(candidate[0x20080:0x23880])
    old_head[0x114a:0x114e] = new_head[0x114a:0x114e]
    assert old_head == new_head
    checks.append("bootstrap and PM51 unchanged; main bootstrap changes only its compressed-length field")
    def reject(data, message):
        try:
            fmt.inspect(bytes(data))
        except ValueError as exc:
            assert message in str(exc)
        else:
            raise AssertionError("corruption accepted")
    for offset, message in [(fmt.STREAM + 42, "Main CRC32"), (0x500, "Whole-container CRC32")]:
        bad = bytearray(candidate); bad[offset] ^= 1; reject(bad, message)
    checks.append("compressed-payload and bootstrap corruptions rejected")
    end = fmt.u32(candidate, 0x1000c)
    bad = bytearray(candidate); bad[end - 2] ^= 1
    struct.pack_into("<I", bad, 0x10014, fmt.crc(bad[fmt.MAIN:end], 32))
    struct.pack_into(">I", bad, len(bad) - 4, fmt.crc(bad[:-4], 32))
    reject(bad, "Inner CRC16")
    bad = bytearray(candidate); bad[-6] ^= 1
    struct.pack_into(">I", bad, len(bad) - 4, fmt.crc(bad[:-4], 32))
    reject(bad, "Outer CRC16")
    checks.append("both CRC16 corruptions rejected even after repairing enclosing CRC32s")
    try:
        build.build(stock[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError("wrong base accepted")
    checks.append("wrong base rejected before patching")
    print(json.dumps({"passed": len(checks), "checks": checks,
                      "candidate_sha256": fmt.sha(candidate), "hardware_tested": False}, indent=2))


if __name__ == "__main__":
    main()
