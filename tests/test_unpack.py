#!/usr/bin/env python3
"""Exercise the public CLI with supplied stock/candidate BINs; no hardware I/O."""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "firmware_container.py"


def run(*arguments):
    return subprocess.run([sys.executable, str(TOOL), *map(str, arguments)],
                          text=True, capture_output=True, check=False)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    checks = []
    stock = args.stock.read_bytes()
    with tempfile.TemporaryDirectory(prefix="samsung-unpack-test-") as temporary:
        base = Path(temporary)
        for label, image in (("stock", args.stock), ("candidate", args.candidate)):
            data = image.read_bytes()
            destination = base / label
            result = run("unpack", image, destination)
            assert result.returncode == 0, result.stderr
            manifest = json.loads((destination / "manifest.json").read_text())
            assert json.loads(result.stdout) == manifest
            assert manifest["container"]["container_sha256"] == digest(data)
            assert set(path.name for path in destination.iterdir()) == {
                "application.bin", "application.deflate", "manifest.json"}
            for name, metadata in manifest["files"].items():
                content = (destination / name).read_bytes()
                assert len(content) == metadata["length"]
                assert digest(content) == metadata["sha256"]
            compressed = (destination / "application.deflate").read_bytes()
            app = (destination / "application.bin").read_bytes()
            assert data[0x23880:0x23880 + len(compressed)] == compressed
            assert zlib.decompress(compressed, -15) == app
            assert digest(app) == manifest["container"]["app_sha256"]
            assert manifest["files"]["application.bin"]["runtime_base"] == 0x203800
            checks.append(f"{label}: CLI extracts original stream, verified body and matching manifest")

        rebuilt = base / "rebuilt-stock.bin"
        result = run("repack", args.stock, base / "stock" / "application.bin",
                     rebuilt, "--reuse-original-stream")
        assert result.returncode == 0, result.stderr
        assert rebuilt.read_bytes() == stock
        checks.append("unpacked stock body repacks byte-for-byte with --reuse-original-stream")

        corrupted = bytearray(stock)
        corrupted[0x23880 + 42] ^= 1
        corrupt_path = base / "corrupt.bin"
        corrupt_path.write_bytes(corrupted)
        rejected_destination = base / "corrupt-output"
        result = run("unpack", corrupt_path, rejected_destination)
        assert result.returncode != 0 and "Main CRC32 mismatch" in result.stderr
        assert not rejected_destination.exists()
        checks.append("corrupt input rejected before creating the destination")

        existing = base / "existing"
        existing.mkdir()
        sentinel = existing / "application.bin"
        sentinel.write_bytes(b"must remain unchanged")
        result = run("unpack", args.stock, existing)
        assert result.returncode != 0 and "FileExistsError" in result.stderr
        assert sentinel.read_bytes() == b"must remain unchanged"
        assert list(existing.iterdir()) == [sentinel]
        checks.append("existing destination rejected without overwriting any file")

        empty = base / "existing-empty"
        empty.mkdir()
        result = run("unpack", args.stock, empty)
        assert result.returncode != 0 and not list(empty.iterdir())
        checks.append("existing empty directory is also rejected")
    print(json.dumps({"passed": len(checks), "checks": checks,
                      "hardware_tested": False}, indent=2))


if __name__ == "__main__":
    main()
