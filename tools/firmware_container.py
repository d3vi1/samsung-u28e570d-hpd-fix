#!/usr/bin/env python3
"""Offline Samsung m-E5728UMDB 1004.5 container verifier/unpacker/repacker.

No monitor communication. This validates container integrity, not executable
behavior or hardware safety. CRCs/layout are matched against the stock image.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from pathlib import Path

STOCK_SHA256 = "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b"
HEADER = 0x10000
MAIN = 0x20080
LOAD = 0x200000
STREAM = 0x23880
COMPRESS_MAGIC = 0x211C7
COMPRESS_LENGTH = 0x211CA
OUTER_MARKER = bytes.fromhex("55 aa ab cd 00 00")
APP_LENGTH = 0x137910
# Do not extend into a new 64 KiB region beyond that occupied by stock 1004.5.
# This is a conservative packaging bound, NOT a physical-flash ownership proof.
MAX_CONTAINER = 0x90000


def _table(width: int, poly: int) -> list[int]:
    mask = (1 << width) - 1
    result = []
    for b in range(256):
        value = b << (width - 8)
        for _ in range(8):
            value = ((value << 1) ^ (poly if value & (1 << (width - 1)) else 0)) & mask
        result.append(value)
    return result


TABLE16 = _table(16, 0x8005)
TABLE32 = _table(32, 0x04C11DB7)


def crc(data: bytes | bytearray, width: int) -> int:
    """MSB-first, initial zero, no reflection and no final XOR."""
    table = TABLE16 if width == 16 else TABLE32
    mask = (1 << width) - 1
    value = 0
    for b in data:
        value = ((value << 8) & mask) ^ table[(value >> (width - 8)) ^ b]
    return value


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def u32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect(data: bytes) -> tuple[dict, bytes, bytes]:
    require(STREAM + 24 < len(data) <= MAX_CONTAINER, "Unexpected container size")
    require(u32(data, HEADER) == MAIN, "Unexpected main-image start")
    require(u32(data, HEADER + 4) == LOAD, "Unexpected main load address")
    main_end = u32(data, HEADER + 12)
    require(STREAM + 6 < main_end < len(data), "Invalid main-image end")
    require(u32(data, HEADER + 8) == LOAD + main_end - MAIN, "Main RAM end mismatch")
    crc_end = u32(data, HEADER + 0x20)
    require(crc_end + 4 == len(data), "Whole-container CRC offset/length mismatch")
    require(data[COMPRESS_MAGIC:COMPRESS_MAGIC + 3] == bytes.fromhex("be ad de"), "Unexpected compression magic")
    require(u32(data, COMPRESS_LENGTH) == main_end - MAIN, "Embedded compressed length mismatch")
    require(u32(data, COMPRESS_LENGTH + 4) == STREAM - MAIN, "Uncompressed head size mismatch")
    require(data[HEADER + 0x10:HEADER + 0x14] == bytes(4), "Unexpected header flags")
    require(u32(data, HEADER + 0x24) == HEADER, "Unexpected CRC16 coverage start")
    require(u32(data, HEADER + 0x14) == crc(data[MAIN:main_end], 32), "Main CRC32 mismatch")
    require(struct.unpack_from(">I", data, crc_end)[0] == crc(data[:crc_end], 32), "Whole-container CRC32 mismatch")
    zeroed = bytearray(data)
    zeroed[HEADER + 0x14:HEADER + 0x18] = bytes(4)
    inner_crc_offset = main_end - 2
    require(struct.unpack_from(">H", data, inner_crc_offset)[0] == crc(zeroed[HEADER:inner_crc_offset], 16), "Inner CRC16 mismatch")
    aligned = (main_end + 7) & ~7
    require(data[main_end:aligned] == bytes(aligned - main_end), "Unexpected alignment padding")
    require(data[aligned:aligned + 6] == OUTER_MARKER, "Outer marker mismatch")
    require(aligned + 8 == crc_end, "Outer trailer size mismatch")
    require(struct.unpack_from(">H", data, aligned + 6)[0] == crc(zeroed[HEADER:aligned + 6], 16), "Outer CRC16 mismatch")
    decompressor = zlib.decompressobj(-15)
    compressed = data[STREAM:main_end - 6]
    app = decompressor.decompress(compressed) + decompressor.flush()
    require(decompressor.eof and not decompressor.unused_data and not decompressor.unconsumed_tail, "Invalid raw-DEFLATE stream boundary")
    require(len(app) == APP_LENGTH == u32(data, main_end - 6), "Expanded application size mismatch")
    return {
        "container_sha256": sha(data), "container_length": len(data),
        "main_end": main_end, "crc32_offset": crc_end,
        "compressed_length": len(compressed), "app_length": len(app),
        "app_sha256": sha(app), "main_crc32": f"{u32(data, HEADER + 0x14):08X}",
        "inner_crc16": data[inner_crc_offset:inner_crc_offset + 2].hex().upper(),
        "outer_crc16": data[aligned + 6:aligned + 8].hex().upper(),
        "file_crc32": data[crc_end:].hex().upper(),
        "service_sum16": f"{sum(data) & 0xffff:04X}",
        "validation_scope": "offline container integrity only; no execution or hardware qualification",
        "zlib_runtime": zlib.ZLIB_RUNTIME_VERSION,
    }, app, compressed


def repack(stock: bytes, app: bytes, *, reuse_original_stream: bool = False) -> bytes:
    require(sha(stock) == STOCK_SHA256, "Base is not the exact supported official 1004.5 image")
    _, old_app, old_stream = inspect(stock)
    require(len(app) == len(old_app), "Application length changes are unsupported")
    if reuse_original_stream:
        require(app == old_app, "Cannot reuse the original stream for a changed body")
        stream = old_stream
    else:
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
        stream = compressor.compress(app) + compressor.flush(zlib.Z_FINISH)
    data = bytearray(stock[:STREAM]) + stream + struct.pack("<I", len(app))
    main_end = len(data) + 2
    aligned = (main_end + 7) & ~7
    crc_end = aligned + 8
    require(crc_end + 4 <= MAX_CONTAINER, "Repacked image crosses the conservative 0x90000 bound")
    struct.pack_into("<I", data, HEADER + 8, LOAD + main_end - MAIN)
    struct.pack_into("<I", data, HEADER + 12, main_end)
    struct.pack_into("<I", data, HEADER + 0x14, 0)
    struct.pack_into("<I", data, HEADER + 0x20, crc_end)
    struct.pack_into("<I", data, COMPRESS_LENGTH, main_end - MAIN)
    data += struct.pack(">H", crc(data[HEADER:], 16))
    data += bytes(aligned - main_end) + OUTER_MARKER
    data += struct.pack(">H", crc(data[HEADER:], 16))
    struct.pack_into("<I", data, HEADER + 0x14, crc(data[MAIN:main_end], 32))
    data += struct.pack(">I", crc(data, 32))
    result = bytes(data)
    _, verified_app, _ = inspect(result)
    require(verified_app == app, "Repacked body differs from the input application")
    return result


def unpack(data: bytes, output_directory: Path) -> dict:
    """Validate before creating a new directory; never overwrite an output."""
    report, app, compressed = inspect(data)
    manifest = {
        "schema_version": 1,
        "container": report,
        "files": {
            "application.bin": {
                "description": "Verified expanded application body",
                "length": len(app), "sha256": sha(app),
                "runtime_base": LOAD + STREAM - MAIN,
            },
            "application.deflate": {
                "description": "Original raw-DEFLATE stream without its six-byte trailer",
                "length": len(compressed), "sha256": sha(compressed),
                "container_offset": STREAM,
            },
        },
    }
    # mkdir is the exclusive destination claim, including existing empty
    # directories, files and symlinks. A missing parent must be created by
    # the caller; never silently reuse an existing destination.
    output_directory.mkdir()
    for name, content in (("application.bin", app), ("application.deflate", compressed)):
        with (output_directory / name).open("xb") as output:
            output.write(content)
    # Write the manifest last so it is present only after both artifacts were
    # written. A filesystem failure may leave a partial new directory.
    with (output_directory / "manifest.json").open("x", encoding="utf-8") as output:
        output.write(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("inspect")
    check.add_argument("image", type=Path)
    extract = sub.add_parser("unpack", help="Validate and extract into a new directory")
    extract.add_argument("image", type=Path)
    extract.add_argument("output_directory", type=Path)
    pack = sub.add_parser("repack")
    pack.add_argument("stock", type=Path)
    pack.add_argument("application", type=Path)
    pack.add_argument("output", type=Path)
    pack.add_argument("--reuse-original-stream", action="store_true", help="Exact stock reconstruction check; only works with unchanged application")
    args = parser.parse_args()
    if args.command == "unpack":
        print(json.dumps(unpack(args.image.read_bytes(), args.output_directory), indent=2))
        return
    if args.command == "inspect":
        data = args.image.read_bytes()
    else:
        require(not args.output.exists(), "Refusing to overwrite existing output")
        data = repack(args.stock.read_bytes(), args.application.read_bytes(), reuse_original_stream=args.reuse_original_stream)
        args.output.write_bytes(data)
    print(json.dumps(inspect(data)[0], indent=2))


if __name__ == "__main__":
    main()
