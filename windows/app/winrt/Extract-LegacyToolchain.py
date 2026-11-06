#!/usr/bin/env python3
"""Extract already-downloaded VS 2012 MSI payloads as data, without installation.

Requires Windows CPython with _msi, and expand.exe from SystemRoot. MSI tables
are opened READONLY: no MSI action, registration, downloaded binary or extracted
program is executed. Verify the provenance of the source ISO separately.
Directory-table paths are relocated beneath output-root; installer properties
and custom actions are intentionally not evaluated.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid


DEFAULT_PACKAGES = (
    "vc_compilercore/vc_compilercore.msi",
    "vc_compilercoreres/vc_compilercoreres.msi",
    "vc_librarycore/vc_librarycore.msi",
    "Windows_SDK/Windows Software Development Kit for Metro style Apps-x86_en-us.msi",
    "win8express/core/win8express_coremsi.msi",
    "vcRuntimeMinimum_x86/vc_runtimeMinimum_x86.msi",
)
MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_PACKAGE_FILES = 100000


def io_path(path):
    """Support the legacy tree's long paths without changing logical paths."""
    text = str(path)
    if text.startswith("\\\\?\\"):
        return Path(text)
    if text.startswith("\\\\"):
        return Path("\\\\?\\UNC\\" + text[2:])
    return Path("\\\\?\\" + text)


def no_reparse(path):
    for part in reversed((path,) + tuple(path.parents)):
        try:
            info = io_path(part).lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("Reparse points are not supported: " + str(part))


def root_path(value, create=False):
    path = Path(os.path.abspath(value))
    no_reparse(path)
    if create:
        io_path(path).mkdir(parents=True, exist_ok=True)
    if not io_path(path).is_dir():
        raise ValueError("Expected an existing directory: " + str(path))
    return path


def under(path, root):
    path.relative_to(root)
    no_reparse(path)
    return path


def safe_name(value):
    if (not value or len(value) > 255 or value in (".", "..")
            or value[-1] in (".", " ") or any(ord(c) < 32 for c in value)
            or re.search(r'[\\/:<>"|?*]', value)):
        raise ValueError("Unsafe data filename: " + repr(value))
    stem = value.split(".", 1)[0].casefold()
    if stem in {"con", "prn", "aux", "nul"} or re.fullmatch(r"(?:com|lpt)[1-9]", stem):
        raise ValueError("Reserved data filename: " + repr(value))
    return value


def regular_file(path):
    no_reparse(path)
    info = io_path(path).stat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("Expected a regular file: " + str(path))
    return info


def digest(path):
    result = hashlib.sha256()
    with io_path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def rows(database, table, msi_module):
    # Table names are fixed internal constants, never CLI/metadata text.
    view = database.OpenView("SELECT * FROM `" + table + "`")
    try:
        view.Execute(None)
        names_record = view.GetColumnInfo(msi_module.MSICOLINFO_NAMES)
        names = [names_record.GetString(i) for i in range(1, names_record.GetFieldCount() + 1)]
        result = []
        while True:
            record = view.Fetch()
            if record is None:
                return result
            if len(result) >= MAX_PACKAGE_FILES:
                raise ValueError("MSI table exceeds its row limit")
            result.append({name: record.GetString(i + 1) for i, name in enumerate(names)})
    finally:
        view.Close()


class Extractor:
    def __init__(self, packages, target, cache, expand, msi_module):
        self.packages, self.target, self.cache = packages, target, cache
        self.expand, self.msi = expand, msi_module
        self.cabinets = {}

    def run_expand(self, arguments):
        result = subprocess.run(
            [str(self.expand)] + arguments, capture_output=True, text=True,
            errors="replace", timeout=300, creationflags=subprocess.CREATE_NO_WINDOW,
        )
        if result.returncode:
            raise RuntimeError("expand.exe failed: " + result.stdout + result.stderr)
        return result.stdout

    def cabinet_files(self, cabinet):
        under(cabinet, self.packages)
        regular_file(cabinet)
        if cabinet in self.cabinets:
            return self.cabinets[cabinet]
        listing = self.run_expand(["-D", str(cabinet)])
        prefix = str(cabinet).casefold() + ": "
        names = []
        for line in listing.splitlines():
            position = line.casefold().find(prefix)
            if position >= 0:
                names.append(safe_name(line[position + len(prefix):]))
        if not names or len(names) > MAX_PACKAGE_FILES:
            raise RuntimeError("No supported cabinet listing or too many entries: " + str(cabinet))
        if len(set(name.casefold() for name in names)) != len(names):
            raise ValueError("Duplicate cabinet filename")
        # Same relative-path key as the original local extractor, so a rerun
        # validates and reuses that cache without rewriting existing files.
        key = hashlib.sha256(str(cabinet.relative_to(self.packages)).encode()).hexdigest()[:24]
        output = under(self.cache / key, self.cache)
        marker = output / ".complete"
        if not io_path(marker).exists():
            if io_path(output).exists():
                raise RuntimeError("Incomplete cache directory requires inspection: " + str(output))
            io_path(output).mkdir()
            self.run_expand(["-R", str(cabinet), "-F:*", str(output)])
            for name in names:
                regular_file(under(output / name, self.cache))
            with io_path(marker).open("x", encoding="utf-8") as stream:
                stream.write("data-only extraction\n")
                stream.flush()
                os.fsync(stream.fileno())
        regular_file(marker)
        for name in names:
            regular_file(under(output / name, self.cache))
        self.cabinets[cabinet] = output
        return output

    def copy_new_or_verify(self, source, destination, expected_bytes):
        under(destination, self.target)
        if regular_file(source).st_size != expected_bytes:
            raise RuntimeError("File table size mismatch: " + source.name)
        source_hash = digest(source)
        if io_path(destination).exists():
            if regular_file(destination).st_size != expected_bytes or digest(destination) != source_hash:
                raise RuntimeError("Conflicting extracted file: " + str(destination))
            return False
        io_path(destination.parent).mkdir(parents=True, exist_ok=True)
        under(destination.parent, self.target)
        temporary = under(destination.parent / (".extract-" + uuid.uuid4().hex + ".tmp"), self.target)
        temporary_created = False
        try:
            with io_path(source).open("rb") as src, io_path(temporary).open("xb") as dst:
                temporary_created = True
                shutil.copyfileobj(src, dst, 1024 * 1024)
                dst.flush()
                os.fsync(dst.fileno())
            if regular_file(temporary).st_size != expected_bytes or digest(temporary) != source_hash:
                raise RuntimeError("Copied payload failed verification")
            # On Windows rename fails if destination already exists: never replace.
            os.rename(io_path(temporary), io_path(destination))
            return True
        finally:
            if temporary_created and io_path(temporary).exists():
                under(temporary, self.target)
                io_path(temporary).unlink()

    def extract(self, requested):
        relative = Path(requested)
        if relative.is_absolute() or not relative.parts:
            raise ValueError("MSI paths must be relative to packages-root")
        for part in relative.parts:
            safe_name(part)
        msi = under(self.packages / relative, self.packages)
        if msi.suffix.casefold() != ".msi":
            raise ValueError("Expected an MSI filename")
        regular_file(msi)
        database = self.msi.OpenDatabase(str(msi), self.msi.MSIDBOPEN_READONLY)
        directories = {row["Directory"]: row for row in rows(database, "Directory", self.msi)}
        components = {row["Component"]: row["Directory_"] for row in rows(database, "Component", self.msi)}
        media = sorted(rows(database, "Media", self.msi), key=lambda row: int(row["LastSequence"]))
        resolved, resolving = {}, set()

        def directory(key):
            if key in resolved:
                return resolved[key]
            if key in resolving or len(resolving) >= 128:
                raise ValueError("Cyclic or excessive MSI Directory hierarchy")
            resolving.add(key)
            row = directories[key]
            path = self.target
            if row["Directory_Parent"]:
                path = directory(row["Directory_Parent"])
                name = row["DefaultDir"].split(":", 1)[0].split("|")[-1]
                if name != ".":
                    path /= safe_name(name)
            resolved[key] = under(path, self.target)
            resolving.remove(key)
            return resolved[key]

        copied, verified = 0, 0
        for row in rows(database, "File", self.msi):
            size, sequence = int(row["FileSize"]), int(row["Sequence"])
            if not 0 <= size <= MAX_FILE_BYTES or sequence <= 0:
                raise ValueError("Invalid MSI File size or sequence")
            entry = next((item for item in media if sequence <= int(item["LastSequence"])), None)
            if entry is None or not entry["Cabinet"] or entry["Cabinet"].startswith("#"):
                raise ValueError("Only external cabinet payloads are supported; do not execute MSI")
            source_dir = self.cabinet_files(msi.parent / safe_name(entry["Cabinet"]))
            source = under(source_dir / safe_name(row["File"]), self.cache)
            destination = directory(components[row["Component_"]]) / safe_name(row["FileName"].split("|")[-1])
            if self.copy_new_or_verify(source, destination, size):
                copied += 1
            else:
                verified += 1
        return {"msi": relative.as_posix(), "files": copied + verified,
                "created": copied, "existing_verified": verified}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages-root", required=True, help="Existing ISO-extracted packages directory")
    parser.add_argument("--output-root", required=True, help="Relocated compiler/SDK tree, preferably a short path")
    parser.add_argument("--cache-root", required=True, help="External cabinet data cache")
    parser.add_argument("--msi", action="append", help="Optional relative MSI path; repeat to replace the six defaults")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Windows CPython with _msi is required")
    try:
        import _msi
    except ImportError as error:
        raise RuntimeError("This CPython runtime does not provide the Windows _msi module") from error
    packages = root_path(args.packages_root)
    target = root_path(args.output_root, create=True)
    cache = root_path(args.cache_root, create=True)
    for first, second in ((packages, target), (packages, cache), (target, cache)):
        common = Path(os.path.commonpath((first, second))) if first.drive.casefold() == second.drive.casefold() else None
        if common in (first, second):
            raise ValueError("Packages, output and cache directories must not overlap")
    system_root = os.environ.get("SystemRoot")
    if not system_root:
        raise RuntimeError("SystemRoot is not set")
    expand = Path(system_root) / "System32" / "expand.exe"
    regular_file(expand)
    extractor = Extractor(packages, target, cache, expand, _msi)
    reports = [extractor.extract(requested) for requested in (args.msi or DEFAULT_PACKAGES)]
    print(json.dumps({"data_only": True, "packages": reports,
                      "created": sum(report["created"] for report in reports),
                      "existing_verified": sum(report["existing_verified"] for report in reports)}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("Extraction failed: " + str(error), file=sys.stderr)
        sys.exit(1)
