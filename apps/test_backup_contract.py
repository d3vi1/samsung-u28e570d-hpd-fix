#!/usr/bin/env python3
"""Offline shared-backup contract checks using real Linux/Windows emitters.

Run: python3 apps/test_backup_contract.py
Requires Python's standard library and an existing C++14 compiler for Windows
portable-core cases. No driver, device, real firmware, or private identifier is
used. All raw fixtures are generated in a temporary directory and removed.
Optionally set MONITORFW_MAC_BACKUP to an absolute, external mock bundle directory
to check its schema and Linux/Windows ingestion. The optional case otherwise skips.
"""
from __future__ import annotations

import copy
from datetime import datetime
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = json.loads((ROOT / "apps/backup.schema.json").read_text())
PROFILES = json.loads((ROOT / "apps/flash-profiles.json").read_text())["profiles"]
CAPACITY = 2 * 1024 * 1024
LEAF = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


class ContractError(ValueError):
    pass


def check(ok, message):
    if not ok:
        raise ContractError(message)


def hash_bytes(data):
    return hashlib.sha256(data).hexdigest()


def validate_schema(value, schema=SCHEMA, path="$"):
    """Evaluate every assertion keyword in this repo's current JSON Schema.

    This is intentionally not a general JSON Schema implementation. Unknown
    keywords fail rather than silently weakening future contract checks.
    """
    supported = {"$schema", "$id", "title", "description", "type", "required",
                 "properties", "const", "enum", "minimum", "maximum", "minLength",
                 "pattern", "format", "minItems", "maxItems", "items"}
    check(not set(schema) - supported, f"{path}: unsupported schema keywords {set(schema) - supported}")
    kind = schema.get("type")
    matches = {
        "object": isinstance(value, dict), "array": isinstance(value, list),
        "string": isinstance(value, str), "boolean": type(value) is bool,
        "integer": (type(value) is int or type(value) is float and value.is_integer()),
    }
    if kind:
        check(kind in matches and matches[kind], f"{path}: expected {kind}")
    if "const" in schema:
        expected = schema["const"]
        check(value == expected and (type(value) is bool) == (type(expected) is bool), f"{path}: wrong constant")
    if "enum" in schema:
        check(value in schema["enum"], f"{path}: value outside enum")
    for key, compare in (("minimum", lambda a, b: a >= b), ("maximum", lambda a, b: a <= b)):
        if key in schema:
            check(type(value) in (int, float) and compare(value, schema[key]), f"{path}: violates {key}")
    if "minLength" in schema:
        check(isinstance(value, str) and len(value) >= schema["minLength"], f"{path}: string too short")
    if "pattern" in schema:
        check(isinstance(value, str) and re.search(schema["pattern"], value) is not None, f"{path}: pattern mismatch")
    if "format" in schema:
        check(schema["format"] == "date-time", f"{path}: unsupported format")
        check(isinstance(value, str) and re.fullmatch(r"\d{4}-\d\d-\d\d[Tt]\d\d:\d\d:\d\d(?:\.\d+)?(?:[Zz]|[+-](?:[01]\d|2[0-3]):[0-5]\d)", value),
              f"{path}: invalid date-time")
        try:
            parsed = datetime.fromisoformat(value.replace("t", "T").replace("z", "+00:00").replace("Z", "+00:00"))
        except ValueError as error:
            raise ContractError(f"{path}: invalid date-time") from error
        check(parsed.tzinfo is not None, f"{path}: missing time zone")
    for key, compare in (("minItems", lambda a, b: a >= b), ("maxItems", lambda a, b: a <= b)):
        if key in schema:
            check(isinstance(value, list) and compare(len(value), schema[key]), f"{path}: violates {key}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            check(key in value, f"{path}: missing required {key}")
        for key, child in schema.get("properties", {}).items():
            if key in value:
                validate_schema(value[key], child, path + "." + key)
    if "items" in schema:
        check(isinstance(value, list), f"{path}: expected array items")
        for index, child in enumerate(value):
            validate_schema(child, schema["items"], f"{path}[{index}]")


def read_artifact(directory, leaf):
    check(isinstance(leaf, str) and LEAF.fullmatch(leaf), "Unsafe artifact reference")
    path = directory / leaf
    check(not path.is_symlink() and path.is_file(), "Artifact is missing or is a symlink: " + leaf)
    return path.read_bytes()


def validate_bundle(directory):
    manifest = json.loads((directory / "backup.json").read_text())
    validate_schema(manifest)
    records = manifest["reads"]
    check(records[0]["file"] != records[1]["file"], "Two independent files are required")
    reads = []
    for record in records:
        data = read_artifact(directory, record["file"])
        check(len(data) == CAPACITY == record["bytes"], "Full image length mismatch")
        check(hash_bytes(data) == record["sha256"], "Image SHA256 mismatch")
        reads.append(data)
    check(reads[0] == reads[1] and manifest["reads_equal"] is True, "Independent full reads differ")
    flash = manifest["flash"]
    sfdp = read_artifact(directory, flash["sfdp_file"])
    check(len(sfdp) == flash["sfdp_bytes"] == 64 and hash_bytes(sfdp) == flash["sfdp_sha256"], "SFDP evidence mismatch")
    matched = [profile for profile in PROFILES if sfdp.startswith(bytes.fromhex(profile["sfdp_known_prefix_hex"]))]
    check(len(matched) == 1, "Unknown SFDP profile")
    profile = matched[0]
    check(flash["status_before"] == flash["status_after"] == profile["expected_idle_status"], "Status/profile mismatch")
    return manifest, reads[0]


CPP_HELPER = r'''
#include "mock_flash.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace monitorfw;
using namespace monitorfw::testing;
Bytes readBounded(const std::string& path,std::uint32_t maximum) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw Error("Cannot open mock artifact");
    const auto size=input.tellg();
    if(size<0 || std::uint64_t(size)>maximum)throw Error("Mock artifact exceeds bound");
    Bytes result(static_cast<std::size_t>(size)); input.seekg(0);
    if(!result.empty())input.read(reinterpret_cast<char*>(result.data()),result.size());
    if(!input)throw Error("Short mock artifact read");
    return result;
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="load") {
            const std::string directory=argv[2];
            const auto raw=readBounded(directory+"/backup.json",1024*1024);
            unsigned reads=0;
            const auto evidence=loadBackupEvidence(std::string(raw.begin(),raw.end()),[&](const std::string& leaf,std::uint32_t maximum){
                ++reads; return readBounded(directory+"/"+leaf,maximum);
            });
            if(!evidence.artifactsValidated() || reads!=3)throw Error("All three evidence files must be validated");
            std::cout<<evidence.imageSha256<<"\n"; return 0;
        }
        if(argc==3 && std::string(argv[1])=="parse") {
            std::ifstream input(argv[2],std::ios::binary);
            if(!input) throw Error("Cannot read mock manifest");
            const std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
            const auto evidence=parseBackupManifest(text);
            std::cout<<evidence.imageSha256<<"\n"; return 0;
        }
        if(argc!=4 || std::string(argv[1])!="emit") throw Error("Expected emit DIR legacy|jv, parse FILE or load DIR");
        Bytes image(SpiCapacity); for(std::size_t i=0;i<image.size();++i)image[i]=std::uint8_t(i);
        MockFlash device(image); MemoryStore store;
        if(std::string(argv[3])=="jv") {
            device.flash.sfdp={0x53,0x46,0x44,0x50,5,1,0,0xff,0,5,1,16,0x80,0,0,0xff};
            device.flash.sfdp.resize(64,0xa5); // Explicitly synthetic remaining bytes.
            device.flash.status={0,2,0x60}; device.flash.profile="SYNTHETIC-JV-2M";
        } else if(std::string(argv[3])!="legacy") throw Error("Unknown mock profile");
        Session session(device,store,device.identity);
        session.backupTwice();
        if(!session.finishBackup().success) throw Error("Mock backup did not finish");
        for(const auto& item:store.files) {
            std::ofstream output(std::string(argv[2])+"/"+item.first,std::ios::binary);
            output.write(reinterpret_cast<const char*>(item.second.data()),item.second.size());
            if(!output) throw Error("Cannot save mock artifact");
        }
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 1;}
}
'''


class BackupContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="monitorfw-contract-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        sys.path.insert(0, str(ROOT / "linux"))
        cls.addClassCleanup(lambda: sys.path.remove(str(ROOT / "linux")))
        cls.linux = importlib.import_module("mstar")
        cls.linux_tests = importlib.import_module("test_mstar")
        cls.fixtures = {}
        for variant in ("legacy", "jv"):
            name = "EF4015-2MiB-legacy-SR0000-SFDP1.0" if variant == "legacy" else cls.linux.DEFAULT_FLASH_PROFILE
            profile = cls.linux_tests.dataclasses_replace(cls.linux_tests.profile(), flash_profile=name)
            bridge = cls.linux.Bridge(cls.linux_tests.WireTransport(legacy=variant == "legacy"), delay=lambda _: None, expected_profile=name)
            args = types.SimpleNamespace(output=cls.directory / ("linux-" + variant), device=profile.transport_locator,
                                         state_directory=cls.directory / "unused-state")
            with patch.object(cls.linux, "Session", cls.linux_tests.BackupAndCliTests().session(bridge)), \
                 patch.object(cls.linux, "LinuxI2C", side_effect=AssertionError("Real adapter opened")):
                cls.linux.backup(args, profile)
            cls.fixtures["linux-" + variant] = args.output
        command = shlex.split(os.environ.get("CXX", ""))
        if not command:
            compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            command = [compiler] if compiler else []
        cls.helper = None
        if command:
            source = cls.directory / "contract-helper.cpp"
            source.write_text(CPP_HELPER)
            cls.helper = cls.directory / "contract-helper"
            subprocess.run(command + ["-std=c++14", "-O0", "-I", str(ROOT / "windows/app/include"),
                           "-I", str(ROOT / "windows/app/tests"), str(source),
                           *[str(ROOT / "windows/app/src" / name) for name in ("core.cpp", "package.cpp", "sha256.cpp")],
                           "-o", str(cls.helper)], check=True, capture_output=True, text=True)
            for variant in ("legacy", "jv"):
                destination = cls.directory / ("windows-" + variant)
                destination.mkdir()
                subprocess.run([str(cls.helper), "emit", str(destination), variant], check=True, capture_output=True, text=True)
                cls.fixtures["windows-" + variant] = destination

    def copied(self, name):
        temporary = tempfile.TemporaryDirectory(prefix="monitorfw-case-")
        self.addCleanup(temporary.cleanup)
        destination = Path(temporary.name) / "bundle"
        shutil.copytree(self.fixtures[name], destination)
        return destination

    def require_windows(self):
        if self.helper is None:
            self.skipTest("No existing C++14 compiler; Windows portable-core cases were not run")

    def linux_profile(self, manifest):
        id_ = manifest["identity"]
        name = "EF4015-2MiB-legacy-SR0000-SFDP1.0" if manifest["flash"]["status_before"] == "0000" else self.linux.DEFAULT_FLASH_PROFILE
        return self.linux.Profile(manifest["model"], id_["edid_serial"], id_["edid_sha256"], "synthetic-only",
                                  id_["manufacturer"], id_["product_code"], id_["transport_locator"], name)

    def windows_parse(self, manifest):
        self.require_windows()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            path.write_text(json.dumps(manifest))
            return subprocess.run([str(self.helper), "parse", str(path)], capture_output=True, text=True)

    def windows_load(self, directory):
        self.require_windows()
        return subprocess.run([str(self.helper), "load", str(directory)], capture_output=True, text=True)

    def test_linux_emitters_satisfy_schema_and_artifact_semantics(self):
        for variant in ("legacy", "jv"):
            with self.subTest(variant=variant):
                validate_bundle(self.fixtures["linux-" + variant])

    def test_windows_emitters_satisfy_schema_and_artifact_semantics(self):
        self.require_windows()
        for variant in ("legacy", "jv"):
            with self.subTest(variant=variant):
                validate_bundle(self.fixtures["windows-" + variant])

    def test_linux_consumes_windows_backup_artifacts(self):
        self.require_windows()
        for variant in ("legacy", "jv"):
            with self.subTest(variant=variant):
                directory = self.fixtures["windows-" + variant]
                manifest, expected = validate_bundle(directory)
                actual, _ = self.linux.load_backup(directory, self.linux_profile(manifest))
                self.assertEqual(actual, expected)

    def test_windows_parses_linux_manifests(self):
        for variant in ("legacy", "jv"):
            with self.subTest(variant=variant):
                manifest = json.loads((self.fixtures["linux-" + variant] / "backup.json").read_text())
                result = self.windows_parse(manifest)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_windows_consumes_actual_linux_backup_artifacts(self):
        for variant in ("legacy", "jv"):
            with self.subTest(variant=variant):
                directory = self.fixtures["linux-" + variant]
                manifest, _ = validate_bundle(directory)
                result = self.windows_load(directory)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), manifest["reads"][0]["sha256"])

    def test_windows_bundle_reader_rejects_missing_corrupt_or_truncated_evidence(self):
        for variant in ("legacy", "jv"):
            for leaf in ("backup-read-1.bin", "backup-read-2.bin", "flash-sfdp.bin"):
                for mutation in ("missing", "corrupt", "truncated"):
                    with self.subTest(variant=variant, leaf=leaf, mutation=mutation):
                        directory = self.copied("linux-" + variant)
                        path = directory / leaf
                        if mutation == "missing":
                            path.unlink()
                        else:
                            data = bytearray(path.read_bytes())
                            if mutation == "corrupt": data[-1] ^= 1
                            else: data.pop()
                            path.write_bytes(data)
                        self.assertNotEqual(self.windows_load(directory).returncode, 0,
                                            "Windows accepted damaged evidence: " + leaf)

    def test_distinct_safe_noncanonical_filenames_remain_compatible(self):
        directory = self.copied("linux-legacy")
        manifest = json.loads((directory / "backup.json").read_text())
        for index, record in enumerate(manifest["reads"]):
            renamed = f"portable-read-{index + 1}.bin"
            (directory / record["file"]).rename(directory / renamed)
            record["file"] = renamed
        renamed = "portable-sfdp64.bin"
        (directory / manifest["flash"]["sfdp_file"]).rename(directory / renamed)
        manifest["flash"]["sfdp_file"] = renamed
        (directory / "backup.json").write_text(json.dumps(manifest))
        _, expected = validate_bundle(directory)
        actual, _ = self.linux.load_backup(directory, self.linux_profile(manifest))
        self.assertEqual(actual, expected)
        result = self.windows_load(directory)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_optional_external_mac_mock_bundle(self):
        selected = os.environ.get("MONITORFW_MAC_BACKUP")
        if not selected:
            self.skipTest("MONITORFW_MAC_BACKUP not set; optional external Mac bundle was not checked")
        directory = Path(selected)
        self.assertTrue(directory.is_absolute() and directory.is_dir(), "MONITORFW_MAC_BACKUP must select an existing absolute bundle directory")
        manifest, expected = validate_bundle(directory)
        self.assertIs(manifest.get("simulated"), True, "This optional fixture must be a declared mock export")
        actual, _ = self.linux.load_backup(directory, self.linux_profile(manifest))
        self.assertEqual(actual, expected)
        result = self.windows_load(directory)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_extra_evidence_fields_do_not_break_either_consumer(self):
        self.require_windows()
        directory = self.copied("windows-legacy")
        manifest = json.loads((directory / "backup.json").read_text())
        manifest["extra_platform_evidence"] = {"synthetic": True}
        manifest["identity"]["extra_field"] = "synthetic"
        manifest["reads"][0]["extra_field"] = 42
        (directory / "backup.json").write_text(json.dumps(manifest))
        validate_bundle(directory)
        self.linux.load_backup(directory, self.linux_profile(manifest))
        self.assertEqual(self.windows_parse(manifest).returncode, 0)

    def test_semantics_recompute_images_and_sfdp(self):
        self.require_windows()
        for leaf in ("backup-read-1.bin", "backup-read-2.bin", "flash-sfdp.bin"):
            with self.subTest(leaf=leaf):
                directory = self.copied("windows-legacy")
                path = directory / leaf
                data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
                with self.assertRaises(ContractError):
                    validate_bundle(directory)

    def test_semantics_rejects_equal_hash_claims_with_unequal_bytes(self):
        self.require_windows()
        directory = self.copied("windows-legacy")
        manifest = json.loads((directory / "backup.json").read_text())
        path = directory / manifest["reads"][1]["file"]
        data = bytearray(path.read_bytes()); data[0] ^= 1; path.write_bytes(data)
        manifest["reads"][1]["sha256"] = hash_bytes(data)
        (directory / "backup.json").write_text(json.dumps(manifest))
        validate_schema(manifest)  # Schema alone cannot enforce byte equality.
        with self.assertRaisesRegex(ContractError, "Independent full reads differ"):
            validate_bundle(directory)

    def test_semantics_rejects_alias_reads_and_status_profile_mismatch(self):
        self.require_windows()
        for change in ("alias", "status"):
            with self.subTest(change=change):
                directory = self.copied("windows-legacy")
                manifest = json.loads((directory / "backup.json").read_text())
                if change == "alias":
                    manifest["reads"][1]["file"] = manifest["reads"][0]["file"]
                else:
                    manifest["flash"]["status_before"] = manifest["flash"]["status_after"] = "000260"
                (directory / "backup.json").write_text(json.dumps(manifest))
                validate_schema(manifest)
                with self.assertRaises(ContractError):
                    validate_bundle(directory)

    def test_schema_rejects_unsafe_references_boolean_product_and_invalid_date(self):
        self.require_windows()
        original, _ = validate_bundle(self.fixtures["windows-legacy"])
        for key, value in (("unsafe-read", "../outside.bin"), ("unsafe-sfdp", "/outside.bin"),
                           ("product", True), ("date", "not-a-date"),
                           ("date-offset", "2026-01-02T00:00:00+00:60"), ("eeprom", True)):
            with self.subTest(key=key):
                manifest = copy.deepcopy(original)
                if key == "unsafe-read": manifest["reads"][0]["file"] = value
                elif key == "unsafe-sfdp": manifest["flash"]["sfdp_file"] = value
                elif key == "product": manifest["identity"]["product_code"] = value
                elif key.startswith("date"): manifest["created_utc"] = value
                else: manifest["coding_eeprom"]["available"] = value
                with self.assertRaises(ContractError):
                    validate_schema(manifest)

    def test_linux_consumer_rejects_missing_or_corrupted_sfdp_file(self):
        self.require_windows()
        for change in ("missing", "corrupt"):
            with self.subTest(change=change):
                directory = self.copied("windows-legacy")
                manifest = json.loads((directory / "backup.json").read_text())
                path = directory / manifest["flash"]["sfdp_file"]
                if change == "missing": path.unlink()
                else:
                    data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
                with self.assertRaises((self.linux.MstarError, FileNotFoundError)):
                    self.linux.load_backup(directory, self.linux_profile(manifest))

    def test_windows_consumer_rejects_missing_required_manifest_fields(self):
        self.require_windows()
        original, _ = validate_bundle(self.fixtures["windows-legacy"])
        fields = (("created_utc",), ("coding_eeprom",), ("identity", "ambiguous"),
                  ("identity", "ambiguity_reason"), ("flash", "sfdp_file"),
                  ("flash", "programming_chunk_size"))
        for keys in fields:
            with self.subTest(field=".".join(keys)):
                manifest = copy.deepcopy(original)
                parent = manifest
                for key in keys[:-1]: parent = parent[key]
                del parent[keys[-1]]
                with self.assertRaises(ContractError):
                    validate_schema(manifest)
                self.assertNotEqual(self.windows_parse(manifest).returncode, 0,
                                    "Windows accepted a manifest missing required " + ".".join(keys))

    def test_linux_consumer_rejects_missing_required_manifest_fields(self):
        fields = (("created_utc",), ("coding_eeprom",), ("identity", "ambiguous"),
                  ("identity", "ambiguity_reason"), ("flash", "sfdp_file"),
                  ("flash", "programming_chunk_size"))
        for keys in fields:
            with self.subTest(field=".".join(keys)):
                directory = self.copied("linux-legacy")
                manifest = json.loads((directory / "backup.json").read_text())
                profile = self.linux_profile(manifest)
                parent = manifest
                for key in keys[:-1]: parent = parent[key]
                del parent[keys[-1]]
                (directory / "backup.json").write_text(json.dumps(manifest))
                with self.assertRaises(ContractError):
                    validate_schema(manifest)
                with self.assertRaises((self.linux.MstarError, KeyError, ValueError)):
                    self.linux.load_backup(directory, profile)


if __name__ == "__main__":
    unittest.main(verbosity=2)
