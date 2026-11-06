#!/usr/bin/env python3
"""Offline container checks; supply the exact official 1004.5 BIN separately."""
import argparse
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import firmware_container as fmt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    args = parser.parse_args()
    stock = args.stock.read_bytes()
    results = []
    expected = {
        "main_crc32": "9A36F923", "inner_crc16": "95D5",
        "outer_crc16": "BBB7", "file_crc32": "90B3850E",
        "service_sum16": "48D0",
        "container_sha256": fmt.STOCK_SHA256,
        "app_sha256": "43d99a1e97a11735a95b5547fa0cbe297100753481c453876d970d747bc442d4",
    }
    report, app, _ = fmt.inspect(stock)
    assert all(report[key] == value for key, value in expected.items())
    results.append("official image: four independent stored CRC fields and service sum match")
    exact = fmt.repack(stock, app, reuse_original_stream=True)
    assert exact == stock
    results.append("same-stream reconstruction is byte-for-byte identical to the official SHA256")
    recompressed = fmt.repack(stock, app)
    report2, extracted2, _ = fmt.inspect(recompressed)
    assert extracted2 == app
    assert recompressed == fmt.repack(stock, app)
    assert recompressed[:fmt.HEADER] == stock[:fmt.HEADER]
    assert recompressed[0x10080:fmt.MAIN] == stock[0x10080:fmt.MAIN]
    results.append("recompression: deterministic in this zlib runtime; exact application, bootstrap and PM51 preserved")
    # A one-byte synthetic mutation checks that all CRCs depend on new content.
    changed = bytearray(app)
    changed[0] ^= 1
    _, changed_readback, _ = fmt.inspect(fmt.repack(stock, bytes(changed)))
    assert changed_readback == changed
    results.append("synthetic one-byte body mutation survives roundtrip exactly (memory-only, not a firmware candidate)")

    def reject(data, expected_error):
        try:
            fmt.inspect(bytes(data))
        except ValueError as exc:
            assert expected_error in str(exc), (expected_error, str(exc))
        else:
            raise AssertionError("corruption unexpectedly accepted")

    bad = bytearray(stock)
    bad[fmt.STREAM + 42] ^= 1
    reject(bad, "Main CRC32")
    results.append("compressed payload corruption rejected")
    bad = bytearray(stock)
    bad[0x500] ^= 1
    reject(bad, "Whole-container CRC32")
    results.append("bootstrap corruption rejected")
    # Recompute enclosing CRCs to prove the embedded CRC16 is checked itself.
    main_end = fmt.u32(stock, fmt.HEADER + 12)
    bad = bytearray(stock)
    bad[main_end - 2] ^= 1
    struct.pack_into("<I", bad, fmt.HEADER + 0x14, fmt.crc(bad[fmt.MAIN:main_end], 32))
    struct.pack_into(">I", bad, len(bad) - 4, fmt.crc(bad[:-4], 32))
    reject(bad, "Inner CRC16")
    results.append("inner CRC16 corruption rejected despite valid enclosing CRC32s")
    bad = bytearray(stock)
    bad[-6] ^= 1
    struct.pack_into(">I", bad, len(bad) - 4, fmt.crc(bad[:-4], 32))
    reject(bad, "Outer CRC16")
    results.append("outer CRC16 corruption rejected despite valid final CRC32")
    try:
        fmt.repack(stock[:-1], app)
    except ValueError as exc:
        assert "exact supported official" in str(exc)
    else:
        raise AssertionError("wrong base accepted")
    results.append("repacker rejects any base other than the pinned official image")
    print(json.dumps({
        "passed": len(results), "checks": results,
        "stock": report, "recompressed_unchanged_application": report2,
        "hardware_tested": False,
    }, indent=2))


if __name__ == "__main__":
    main()
