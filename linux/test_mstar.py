#!/usr/bin/env python3
"""Offline Linux CLI/protocol/fault tests. No adapters or native I2C are used."""
import contextlib
import copy
import io
import json
from pathlib import Path
import shutil
import tempfile
import types
import unittest
from unittest.mock import patch
import stat

import mstar


def image():
    return bytes(range(256)) * (mstar.CAPACITY // 256)


def edid():
    data = bytearray(128)
    data[:8] = bytes.fromhex("00ffffffffffff00")
    vendor = (19 << 10) | (1 << 5) | 13  # SAM, synthetic descriptor only.
    data[8:10] = vendor.to_bytes(2, "big")
    data[10:12] = (1234).to_bytes(2, "little")
    data[54:72] = b"\0\0\0\xff\0SYNTHETIC01\n "
    data[72:90] = b"\0\0\0\xfc\0Test display\n"
    data[127] = -sum(data) & 255
    return bytes(data)


def profile():
    return mstar.Profile("Synthetic test", "SYNTHETIC01", mstar.sha(edid()), "test",
                         "SAM", 1234, "/dev/i2c-999")


class WireTransport:
    """Independent byte-level bridge simulation; no filesystem/device operations."""
    capabilities = mstar.TransportCapabilities(True, True, 256, 37)
    def __init__(self, source=None, legacy=False):
        self.image = bytearray(source if source is not None else image())
        self.calls = []
        self.command = None
        self.read_enabled = False
        self.entered = False
        self.sr1 = 0
        self.sr2 = 0 if legacy else 2
        self.legacy = legacy
        self.sfdp = mstar.FLASH_PROFILES["EF4015-2MiB-legacy-SR0000-SFDP1.0"]["sfdp"] if legacy else mstar.SFDP
        self.short_read = False
        self.fail_write = False
        self.edid_offset = 0
        self.pages = []

    def write(self, address, data):
        self.calls.append(("write", address, bytes(data)))
        if address == 0x50:
            assert len(data) == 1
            self.edid_offset = data[0]
            return len(data)
        assert address == 0x49
        if data == b"MSTAR":
            self.entered = True
            return len(data)
        assert self.entered
        if data == b"\x24":
            self.entered = False
            return len(data)
        if data == b"\x12":
            self.command = None
            self.read_enabled = False
            return len(data)
        if self.fail_write:
            raise OSError("Injected transport failure")
        if data == b"\x11":
            self.read_enabled = True
            return len(data)
        assert data[0] == 0x10
        self.command = data[1:]
        opcode = self.command[0]
        assert not (self.legacy and opcode == 0x15), "Legacy transport must NEVER receive status opcode 0x15"
        if opcode == 0x06:
            self.sr1 = 2
        elif opcode == 0x04:
            self.sr1 = 0
        elif opcode in (0x20, 0x02):
            assert self.sr1 == 2
            address = int.from_bytes(self.command[1:4], "big")
            if opcode == 0x20:
                assert address % 4096 == 0
                self.image[address:address + 4096] = b"\xff" * 4096
            else:
                payload = self.command[4:]
                assert len(payload) == 32 and address % 32 == 0
                self.pages.append((address, payload))
                for i, value in enumerate(payload):
                    self.image[address + i] &= value
            self.sr1 = 0
        return len(data)

    def read(self, address, count):
        self.calls.append(("read", address, count))
        if address == 0x50:
            return edid()[self.edid_offset:self.edid_offset + count]
        assert address == 0x49 and self.entered and self.read_enabled
        opcode = self.command[0]
        if opcode == 0x03:
            start = int.from_bytes(self.command[1:4], "big")
            result = bytes(self.image[start:start + count])
        else:
            result = {0x9F: mstar.JEDEC, 0x05: bytes([self.sr1]), 0x35: bytes([self.sr2]),
                      0x15: b"\x60", 0x5A: self.sfdp + bytes(48)}[opcode][:count]
        return result[:-1] if self.short_read else result


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.wire = WireTransport()
        self.bridge = mstar.Bridge(self.wire, delay=lambda _: None)
        self.bridge.enter()

    def test_separate_stop_delimited_spi_read(self):
        self.wire.calls.clear()
        self.assertEqual(self.bridge.spi_read(b"\x9f", 3), mstar.JEDEC)
        self.assertEqual(self.wire.calls, [("write", 0x49, b"\x10\x9f"),
                                         ("write", 0x49, b"\x11"), ("read", 0x49, 3), ("write", 0x49, b"\x12")])

    def test_short_response_rejected_and_chip_select_released(self):
        self.wire.short_read = True
        with self.assertRaises(mstar.MstarError):
            self.bridge.spi_read(b"\x9f", 3)
        self.assertEqual(self.wire.calls[-1], ("write", 0x49, b"\x12"))

    def test_failed_command_still_releases(self):
        self.wire.fail_write = True
        with self.assertRaises(OSError):
            self.bridge.spi_read(b"\x9f", 3)
        self.assertEqual(self.wire.calls[-1], ("write", 0x49, b"\x12"))

    def test_reject_unreviewed_status_without_status_write(self):
        self.wire.sr2 = 0
        with self.assertRaises(mstar.MstarError):
            self.bridge.profile()
        self.assertFalse(any(call[0] == "write" and call[2][:2] == b"\x10\x01" for call in self.wire.calls))

    def test_sector_uses_32_byte_programs_and_readback(self):
        target = bytes(reversed(range(256))) * 16
        self.bridge.write_sector(0x20000, target)
        self.assertEqual(bytes(self.wire.image[0x20000:0x21000]), target)
        self.assertEqual(len(self.wire.pages), 128)
        self.assertTrue(all(len(payload) == 32 for _, payload in self.wire.pages))
        self.assertEqual(self.bridge.status(), mstar.STATUS)

    def test_illegal_mutations_rejected(self):
        for command in (b"\xc7", b"\x01\0", b"\x02\0\0\0" + bytes(256)):
            with self.assertRaises(mstar.MstarError):
                self.bridge.spi_write(command)

    def test_edid_read_and_identity(self):
        captured = self.bridge.read_edid()
        fields = mstar.decode_edid(captured)
        self.assertEqual(fields["serial"], "SYNTHETIC01")
        self.assertEqual(fields["manufacturer"], "SAM")
        self.assertEqual(fields["product_code"], 1234)

    def test_legacy_sfdp_selects_only_two_status_registers(self):
        wire = WireTransport(legacy=True)
        bridge = mstar.Bridge(wire, delay=lambda _: None,
                              expected_profile="EF4015-2MiB-legacy-SR0000-SFDP1.0")
        bridge.enter()
        evidence = bridge.profile()
        self.assertEqual(evidence["status"], "0000")
        self.assertEqual(bridge.status(), b"\0\0")
        bridge.write_sector(0x20000, b"L" * mstar.SECTOR)
        bridge.write_disable()
        self.assertEqual(bridge.profile()["status"], "0000")
        with self.assertRaises(mstar.MstarError):
            bridge.spi_read(b"\x15", 1)
        self.assertNotIn(("write", 0x49, b"\x10\x15"), wire.calls)

    def test_wrong_variant_rejected_before_status_opcode_15(self):
        wire = WireTransport(legacy=True)
        bridge = mstar.Bridge(wire, delay=lambda _: None, expected_profile=mstar.DEFAULT_FLASH_PROFILE)
        bridge.enter()
        with self.assertRaises(mstar.MstarError):
            bridge.profile()
        self.assertNotIn(("write", 0x49, b"\x10\x15"), wire.calls)


class TransitionTests(unittest.TestCase):
    def setUp(self):
        self.source = image()
        target = bytearray(self.source)
        target[mstar.HEADER:mstar.HEADER + 32] = b"A" * 32
        target[0x20000:0x20020] = b"B" * 32
        self.plan = mstar.make_plan(self.source, bytes(target))
        self.state = mstar.Transition()

    def test_header_last_and_complete_target(self):
        self.assertEqual(self.plan.sectors, (0x20000, mstar.HEADER))
        device = mstar.SimulatedFlash(self.source)
        mstar.execute(device, self.plan, self.state)
        self.assertTrue(self.state.target_verified and self.state.reset_allowed)
        self.assertEqual(bytes(device.image), self.plan.target)

    def test_dual_full_preflight_and_final(self):
        class Counting(mstar.SimulatedFlash):
            full_reads = 0
            def read(self, address, length, check=lambda: None):
                self.full_reads += int(length == mstar.CAPACITY)
                return super().read(address, length, check)
        device = Counting(self.source)
        mstar.execute(device, self.plan, self.state)
        self.assertEqual(device.full_reads, 4)

    def test_source_mismatch_never_erases_or_allows_reset(self):
        changed = bytearray(self.source)
        changed[-1] ^= 1
        device = mstar.SimulatedFlash(bytes(changed))
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, self.plan, self.state)
        self.assertEqual(device.calls, [])
        self.assertFalse(self.state.reset_allowed)

    def test_erase_then_failure_restores_touched_sector(self):
        device = mstar.SimulatedFlash(self.source, fail_after_erase=0x20000)
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, self.plan, self.state)
        self.assertEqual(bytes(device.image), self.source)
        self.assertTrue(self.state.rollback_verified and self.state.reset_allowed)

    def test_failed_restoration_withholds_reset(self):
        device = mstar.SimulatedFlash(self.source, fail_after_erase=0x20000, fail_restore=True)
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, self.plan, self.state)
        self.assertFalse(self.state.reset_allowed)
        self.assertFalse(self.state.rollback_verified)
        self.assertTrue(self.state.rollback_errors)

    def test_cancellation_between_sectors_restores_source(self):
        device = mstar.SimulatedFlash(self.source)
        def cancellation():
            if any(call[0] == "erase" for call in device.calls):
                raise mstar.Cancelled("cancel")
        with self.assertRaises(mstar.Cancelled):
            mstar.execute(device, self.plan, self.state, cancellation)
        self.assertEqual(self.state.touched, [0x20000])
        self.assertEqual(bytes(device.image), self.source)
        self.assertTrue(self.state.reset_allowed)

    def test_final_read_failure_forces_restoration(self):
        class FailingFinal(mstar.SimulatedFlash):
            full_reads = 0
            def read(self, address, length, check=lambda: None):
                self.full_reads += int(length == mstar.CAPACITY)
                result = super().read(address, length, check)
                if length == mstar.CAPACITY and self.full_reads == 3:
                    return bytes([result[0] ^ 1]) + result[1:]
                return result
        device = FailingFinal(self.source)
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, self.plan, self.state)
        self.assertTrue(self.state.rollback_verified)
        self.assertEqual(bytes(device.image), self.source)

    def test_logging_failure_does_not_interrupt_restoration(self):
        device = mstar.SimulatedFlash(self.source, fail_after_erase=0x20000)
        def bad_log(_):
            raise OSError("disk failure")
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, self.plan, self.state, log=bad_log)
        self.assertTrue(self.state.rollback_verified)
        self.assertTrue(self.state.log_errors)

    def test_plan_mutation_and_direction(self):
        bad = dataclasses_replace(self.plan, sectors=(mstar.HEADER,))
        device = mstar.SimulatedFlash(self.source)
        with self.assertRaises(mstar.MstarError):
            mstar.execute(device, bad, self.state)
        self.assertFalse(device.calls)
        reverse = mstar.make_plan(self.plan.target, self.source)
        self.assertEqual(reverse.sectors[-1], mstar.HEADER)


def dataclasses_replace(value, **kwargs):
    import dataclasses
    return dataclasses.replace(value, **kwargs)


class BackupAndCliTests(unittest.TestCase):
    def session(self, bridge):
        class FakeSession:
            def __init__(self, *_, **_kwargs):
                self.bridge = bridge
                self.evidence = mstar.decode_edid(edid())
            def __enter__(self):
                return self
            def __exit__(self, *_):
                return None
            def finish(self, allowed):
                assert allowed
                self.bridge.exit()
                return True
        return FakeSession

    def test_backup_schema_dual_pass_and_same_unit_guard(self):
        wire = WireTransport()
        bridge = mstar.Bridge(wire, delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            args = types.SimpleNamespace(output=Path(temporary) / "backup", device="/dev/i2c-999", state_directory=Path(temporary))
            with patch.object(mstar, "Session", self.session(bridge)):
                result = mstar.backup(args, profile())
            self.assertTrue(result["success"] and result["resetSent"])
            data, manifest = mstar.load_backup(args.output, profile())
            self.assertEqual(data, image())
            self.assertEqual(manifest["schema"], "monitorfw.backup.v1")
            self.assertTrue(manifest["reads_equal"])
            self.assertFalse(manifest["coding_eeprom"]["available"])
            self.assertTrue(manifest["identity"]["ambiguous"])
            self.assertIs(type(manifest["identity"]["product_code"]), int)
            self.assertEqual(manifest["flash"]["sfdp_bytes"], 64)
            self.assertEqual(manifest["flash"]["sfdp_file"], "flash-sfdp.bin")
            self.assertEqual(manifest["flash"]["page_size"], 256)
            self.assertEqual(manifest["flash"]["programming_chunk_size"], 32)
            self.assertEqual(mstar.sha((args.output / "flash-sfdp.bin").read_bytes()), manifest["flash"]["sfdp_sha256"])
            # Other platform writers may include more evidence fields.
            manifest["identity"]["platform_evidence"] = {"synthetic": True}
            manifest["reads"][0]["extra_evidence"] = "synthetic"
            manifest["flash"]["extra_evidence"] = {"source_platform": "synthetic"}
            (args.output / "backup.json").write_text(json.dumps(manifest))
            self.assertEqual(mstar.load_backup(args.output, profile())[0], data)
            other = dataclasses_replace(profile(), transport_locator="/dev/i2c-998")
            with self.assertRaises(mstar.MstarError):
                mstar.load_backup(args.output, other)

    def test_backup_sfdp_file_is_required_and_rehashed(self):
        bridge = mstar.Bridge(WireTransport(), delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            args = types.SimpleNamespace(output=base / "backup", device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, profile())
            for change in ("missing", "corrupt", "short", "long", "unknown-profile", "other-profile"):
                with self.subTest(change=change):
                    directory = base / change
                    shutil.copytree(args.output, directory)
                    manifest_path = directory / "backup.json"
                    manifest = json.loads(manifest_path.read_text())
                    path = directory / manifest["flash"]["sfdp_file"]
                    data = path.read_bytes()
                    if change == "missing":
                        path.unlink()
                    else:
                        if change == "corrupt":
                            data = data[:-1] + bytes([data[-1] ^ 1])
                        elif change == "short":
                            data = data[:-1]
                        elif change == "long":
                            data += b"\0"
                        elif change == "unknown-profile":
                            data = b"BAD!" + data[4:]
                        else:
                            other = mstar.FLASH_PROFILES["EF4015-2MiB-legacy-SR0000-SFDP1.0"]["sfdp"]
                            data = other + data[len(other):]
                        path.write_bytes(data)
                        # A self-consistent hash cannot authorize a wrong length
                        # or an unknown/different physical flash profile.
                        if change != "corrupt":
                            manifest["flash"]["sfdp_sha256"] = mstar.sha(data)
                            manifest_path.write_text(json.dumps(manifest))
                    with self.assertRaises(mstar.MstarError):
                        mstar.load_backup(directory, profile())

    def test_backup_sfdp_reference_is_a_safe_regular_local_file(self):
        bridge = mstar.Bridge(WireTransport(), delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            args = types.SimpleNamespace(output=base / "backup", device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, profile())
            manifest_path = args.output / "backup.json"
            manifest = json.loads(manifest_path.read_text())
            for leaf in (None, "../outside.bin", "/outside.bin", "subdir/file.bin", "..", "a\\b.bin"):
                with self.subTest(leaf=leaf):
                    manifest["flash"]["sfdp_file"] = leaf
                    manifest_path.write_text(json.dumps(manifest))
                    with self.assertRaises(mstar.MstarError):
                        mstar.load_backup(args.output, profile())
            # Writers use the canonical leaf, while readers accept another
            # safe filename declared by a compatible platform's manifest.
            renamed = args.output / "platform-sfdp.bin"
            (args.output / "flash-sfdp.bin").rename(renamed)
            manifest["flash"]["sfdp_file"] = renamed.name
            manifest_path.write_text(json.dumps(manifest))
            self.assertEqual(mstar.load_backup(args.output, profile())[0], image())
            link = args.output / "linked-sfdp.bin"
            link.symlink_to(renamed.name)
            manifest["flash"]["sfdp_file"] = link.name
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(mstar.MstarError, "regular local file"):
                mstar.load_backup(args.output, profile())

    def test_backup_requires_every_shared_schema_field(self):
        bridge = mstar.Bridge(WireTransport(), delay=lambda _: None)
        schema = json.loads((Path(mstar.__file__).resolve().parents[1] / "apps" / "backup.schema.json").read_text())
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            args = types.SimpleNamespace(output=base / "backup", device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, profile())
            path = args.output / "backup.json"
            original = json.loads(path.read_text())
            def required_paths(rule, value, prefix=()):
                if isinstance(value, dict):
                    for key in rule.get("required", []):
                        yield prefix + (key,)
                    for key, child in rule.get("properties", {}).items():
                        if key in value:
                            yield from required_paths(child, value[key], prefix + (key,))
                elif isinstance(value, list) and "items" in rule:
                    for index, item in enumerate(value):
                        yield from required_paths(rule["items"], item, prefix + (index,))
            for keys in required_paths(schema, original):
                with self.subTest(missing=keys):
                    manifest = copy.deepcopy(original)
                    parent = manifest
                    for key in keys[:-1]:
                        parent = parent[key]
                    del parent[keys[-1]]
                    path.write_text(json.dumps(manifest))
                    with self.assertRaisesRegex(mstar.MstarError, "missing required"):
                        mstar.load_backup(args.output, profile())

    def test_backup_rejects_wrong_schema_types_and_values(self):
        bridge = mstar.Bridge(WireTransport(), delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            args = types.SimpleNamespace(output=base / "backup", device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, profile())
            path = args.output / "backup.json"
            original = json.loads(path.read_text())
            changes = [
                (("schema",), "other"), (("image_kind",), "eeprom"), (("model",), ""),
                (("created_utc",), 123), (("created_utc",), "not-a-date"),
                (("created_utc",), "2026-02-30T12:00:00Z"), (("created_utc",), "2026-11-06T12:00:00"),
                (("created_utc",), "2026-11-06T12:00:00+00:99"),
                (("identity",), []), (("identity", "manufacturer"), "SAM\n"),
                (("identity", "product_code"), True), (("identity", "product_code"), 65536),
                (("identity", "edid_serial"), 1), (("identity", "edid_sha256"), "bad"),
                (("identity", "transport_locator"), ""), (("identity", "ambiguous"), 1),
                (("identity", "ambiguity_reason"), None), (("flash",), []),
                (("flash", "jedec"), "ffffff"), (("flash", "capacity"), 1),
                (("flash", "sfdp_sha256"), 123), (("flash", "sfdp_bytes"), 16),
                (("flash", "sector_size"), 8192), (("flash", "page_size"), 32),
                (("flash", "programming_chunk_size"), 256), (("flash", "status_before"), "02"),
                (("flash", "status_after"), None), (("reads",), []), (("reads",), {}),
                (("reads", 0, "bytes"), 128), (("reads", 0, "sha256"), "bad"),
                (("reads", 1, "file"), "../outside.bin"), (("reads_equal",), 1),
                (("reads_equal",), False), (("coding_eeprom",), False),
                (("coding_eeprom", "available"), True), (("coding_eeprom", "available"), 0),
                (("coding_eeprom", "reason"), ""), (("simulated",), "yes"),
            ]
            for keys, value in changes:
                with self.subTest(field=keys, value=value):
                    manifest = copy.deepcopy(original)
                    parent = manifest
                    for key in keys[:-1]:
                        parent = parent[key]
                    parent[keys[-1]] = value
                    path.write_text(json.dumps(manifest))
                    with self.assertRaises(mstar.MstarError):
                        mstar.load_backup(args.output, profile())

    def test_backup_accepts_distinct_local_read_names_and_rejects_aliases(self):
        bridge = mstar.Bridge(WireTransport(), delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            args = types.SimpleNamespace(output=base / "backup", device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, profile())
            path = args.output / "backup.json"
            manifest = json.loads(path.read_text())
            for index, record in enumerate(manifest["reads"], 1):
                leaf = f"other-platform-read-{index}.bin"
                (args.output / record["file"]).rename(args.output / leaf)
                record["file"] = leaf
                record["platform_evidence"] = {"synthetic": True}
            manifest["platform_evidence"] = {"extra": [1, 2, 3]}
            path.write_text(json.dumps(manifest))
            self.assertEqual(mstar.load_backup(args.output, profile())[0], image())
            manifest["reads"][1]["file"] = manifest["reads"][0]["file"]
            path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(mstar.MstarError, "two distinct"):
                mstar.load_backup(args.output, profile())

    def test_mismatched_backup_passes_are_retained(self):
        class DifferentSecond(mstar.Bridge):
            full_reads = 0
            def read(self, address, length, check=lambda: None):
                result = super().read(address, length, check)
                if length == mstar.CAPACITY:
                    self.full_reads += 1
                    if self.full_reads == 2:
                        return bytes([result[0] ^ 1]) + result[1:]
                return result
        bridge = DifferentSecond(WireTransport(), delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            args = types.SimpleNamespace(output=Path(temporary) / "backup", device="/dev/i2c-999", state_directory=Path(temporary))
            with patch.object(mstar, "Session", self.session(bridge)):
                with self.assertRaises(mstar.MstarError):
                    mstar.backup(args, profile())
            manifest = json.loads((args.output / "backup-invalid.json").read_text())
            self.assertFalse(manifest["reads_equal"])
            self.assertEqual(len(manifest["reads"]), 2)
            self.assertFalse((args.output / "backup.json").exists())
            self.assertTrue((args.output / "backup-read-1.bin").exists() and (args.output / "backup-read-2.bin").exists())
            with self.assertRaises(FileNotFoundError):
                mstar.load_backup(args.output, profile())

    def test_legacy_backup_schema_and_profile_load(self):
        legacy_name = "EF4015-2MiB-legacy-SR0000-SFDP1.0"
        legacy_profile = dataclasses_replace(profile(), flash_profile=legacy_name)
        bridge = mstar.Bridge(WireTransport(legacy=True), delay=lambda _: None, expected_profile=legacy_name)
        with tempfile.TemporaryDirectory() as temporary:
            args = types.SimpleNamespace(output=Path(temporary) / "backup", device="/dev/i2c-999", state_directory=Path(temporary))
            with patch.object(mstar, "Session", self.session(bridge)):
                mstar.backup(args, legacy_profile)
            _, manifest = mstar.load_backup(args.output, legacy_profile)
            self.assertEqual(manifest["flash"]["status_before"], "0000")
            self.assertEqual(manifest["flash"]["status_after"], "0000")
            for name, spec in mstar.FLASH_PROFILES.items():
                path = Path(temporary) / "profile.json"
                path.write_text(json.dumps({"schema": "monitorfw.profile.v1", "identity": {
                    **profile().identity(), "model": profile().model, "firmware": "test"},
                    "flash": {"profile": name, "capacity": mstar.CAPACITY, "jedec": "ef4015",
                              "status": spec["status"].hex(), "sfdpPrefix": spec["sfdp"].hex()}}))
                self.assertEqual(mstar.Profile.load(path).flash_profile, name)

    def test_offline_cli_never_constructs_live_transport(self):
        with patch.object(mstar, "LinuxI2C", side_effect=AssertionError("hardware opened")):
            for args in (["list"], ["drivers"], ["eeprom"]):
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(mstar.main(args), 0)

    def test_unsupported_eeprom_explains_limit(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            mstar.main(["eeprom"])
        self.assertFalse(json.loads(output.getvalue())["supported"])

    def test_existing_destination_refused_before_isp(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = types.SimpleNamespace(output=Path(temporary), device="/dev/i2c-999", state_directory=Path(temporary))
            with patch.object(mstar, "Session", side_effect=AssertionError("ISP accessed")):
                with self.assertRaises(FileExistsError):
                    mstar.backup(args, profile())

    def test_verify_keeps_two_mismatching_passes(self):
        wire = WireTransport()
        bridge = mstar.Bridge(wire, delay=lambda _: None)
        with tempfile.TemporaryDirectory() as temporary:
            expected = bytearray(image())
            expected[0] ^= 1
            selected = Path(temporary) / "expected.bin"
            selected.write_bytes(expected)
            args = types.SimpleNamespace(input=selected, input_sha256=mstar.sha(expected),
                                         output=Path(temporary) / "verify", device="/dev/i2c-999", state_directory=Path(temporary))
            with patch.object(mstar, "Session", self.session(bridge)):
                with self.assertRaises(mstar.MstarError):
                    mstar.verify_image(args, profile())
            self.assertEqual((args.output / "read1.bin").read_bytes(), image())
            self.assertEqual((args.output / "read2.bin").read_bytes(), image())
            self.assertFalse(json.loads((args.output / "result.json").read_text())["success"])


class NativeAndSessionTests(unittest.TestCase):
    def test_read_operations_withhold_reset_and_marker_on_failure(self):
        for operation in ("backup", "verify"):
            for failure in ("first-read", "unequal-reads", "final-profile", "cancellation"):
                with self.subTest(operation=operation, failure=failure), tempfile.TemporaryDirectory() as temporary:
                    wire = self.transport()
                    class FaultBridge(mstar.Bridge):
                        def __init__(self, *args, **kwargs):
                            super().__init__(*args, **kwargs)
                            self.delay = lambda _: None
                            self.full_reads = 0
                        def read(self, address, length, check=lambda: None):
                            if length == mstar.CAPACITY:
                                self.full_reads += 1
                                if failure == "first-read":
                                    raise OSError("Injected first-read failure")
                            data = super().read(address, length, check)
                            if failure == "unequal-reads" and length == mstar.CAPACITY and self.full_reads == 2:
                                return data[:-1] + bytes([data[-1] ^ 1])
                            return data
                        def profile(self):
                            if failure == "final-profile" and self.full_reads == 2:
                                raise OSError("Injected final-profile failure")
                            return super().profile()
                    base = Path(temporary)
                    selected = base / "expected.bin"
                    selected.write_bytes(image())
                    args = types.SimpleNamespace(output=base / "result", device=profile().transport_locator,
                                                 state_directory=base / "state", input=selected, input_sha256=mstar.sha(image()))
                    cancellation = (patch.object(mstar.Cancellation, "check", side_effect=mstar.Cancelled("Injected cancellation"))
                                    if failure == "cancellation" else contextlib.nullcontext())
                    with patch.object(mstar, "LinuxI2C", return_value=wire), patch.object(mstar, "Bridge", FaultBridge), cancellation:
                        with self.assertRaises((mstar.MstarError, OSError)) as caught:
                            {"backup": mstar.backup, "verify": mstar.verify_image}[operation](args, profile())
                    self.assertNotIn(("write", 0x49, b"\x24"), wire.calls)
                    self.assertTrue(wire.entered)
                    self.assertTrue((args.state_directory / profile().key / "unresolved.json").exists())
                    result = json.loads((args.output / "result.json").read_text())
                    self.assertFalse(result["success"] or result["resetSent"])
                    self.assertEqual(result["error"], str(caught.exception))
                    if operation == "backup":
                        self.assertTrue((args.output / "backup-invalid.json").exists())
                        self.assertFalse((args.output / "backup.json").exists())
                        self.assertTrue((args.output / "flash-sfdp.bin").exists())
                    for number in (1, 2):
                        leaf = f"backup-read-{number}.bin" if operation == "backup" else f"read{number}.bin"
                        self.assertEqual((args.output / leaf).exists(), failure in ("unequal-reads", "final-profile"))

    def test_verified_read_operations_explicitly_exit_and_clear_marker(self):
        for operation in ("backup", "verify"):
            with self.subTest(operation=operation), tempfile.TemporaryDirectory() as temporary:
                base = Path(temporary)
                selected = base / "expected.bin"
                selected.write_bytes(image())
                wire = self.transport()
                args = types.SimpleNamespace(output=base / "result", device=profile().transport_locator,
                                             state_directory=base / "state", input=selected, input_sha256=mstar.sha(image()))
                with patch.object(mstar, "LinuxI2C", return_value=wire):
                    result = {"backup": mstar.backup, "verify": mstar.verify_image}[operation](args, profile())
                self.assertTrue(result["success"] and result["resetSent"])
                self.assertEqual(wire.calls.count(("write", 0x49, b"\x24")), 1)
                self.assertFalse(wire.entered)
                self.assertFalse((args.state_directory / profile().key / "unresolved.json").exists())

    def test_new_state_ancestors_are_synced_before_isp_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            state_directory = base / "new-state" / "nested"
            events = []
            original_sync = mstar.sync_directory
            def sync(path):
                events.append(("sync", Path(path).absolute()))
                original_sync(path)
            wire = self.transport()
            original_write = wire.write
            def write(address, data):
                if data == b"MSTAR":
                    events.append(("enter",))
                return original_write(address, data)
            wire.write = write
            with patch.object(mstar, "sync_directory", side_effect=sync), patch.object(mstar, "LinuxI2C", return_value=wire):
                with mstar.Session(profile(), "/dev/i2c-999", state_directory) as session:
                    session.bridge.enter()
                    session.finish(True)
            entry = events.index(("enter",))
            before = events[:entry]
            for path in (base, base / "new-state", state_directory, state_directory / profile().key):
                self.assertIn(("sync", path.absolute()), before)
            self.assertLess(before.index(("sync", base.absolute())),
                            before.index(("sync", state_directory.absolute())))

    def test_artifact_parent_synced_before_entry_and_erase(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            output = base / "new-run"
            source = image()
            target = bytearray(source)
            target[0x20000] ^= 1
            plan = mstar.make_plan(source, bytes(target))
            wire = WireTransport(source)
            events = []
            original_sync, original_write = mstar.sync_directory, wire.write
            def sync(path):
                events.append(("sync", Path(path).absolute()))
                original_sync(path)
            def write(address, data):
                if data == b"MSTAR" or data.startswith(b"\x10\x20"):
                    events.append(("hardware", bytes(data)))
                return original_write(address, data)
            wire.write = write
            bridge = mstar.Bridge(wire, delay=lambda _: None)
            args = types.SimpleNamespace(output=output, dry_run=False, acknowledge_erase=True,
                                         backup=base, device="/dev/i2c-999", state_directory=base)
            with patch.object(mstar, "sync_directory", side_effect=sync), \
                 patch.object(mstar, "selected_plan", return_value=plan), \
                 patch.object(mstar, "load_backup", return_value=(source, {"flash": {"sfdp_sha256": mstar.sha(mstar.SFDP + bytes(48))}})), \
                 patch.object(mstar, "Session", BackupAndCliTests().session(bridge)):
                result = mstar.write_image(args, profile())
            self.assertTrue(result["success"])
            first_hardware = next(i for i, event in enumerate(events) if event[0] == "hardware")
            self.assertIn(("sync", base.absolute()), events[:first_hardware])
            self.assertIn(("sync", output.absolute()), events[:first_hardware])
            self.assertTrue(any(event[0] == "hardware" and event[1].startswith(b"\x10\x20") for event in events))

    def test_native_adapter_without_raw_i2c_closes_before_io(self):
        with patch.object(mstar.sys, "platform", "linux"), \
             patch.object(mstar.os, "stat", return_value=types.SimpleNamespace(st_mode=stat.S_IFCHR)), \
             patch.object(mstar.os, "open", return_value=999), \
             patch.object(mstar.os, "close") as close, \
             patch("fcntl.flock"), patch("fcntl.ioctl", return_value=0), \
             patch.object(mstar.os, "write") as write, patch.object(mstar.os, "read") as read:
            with self.assertRaises(mstar.MstarError):
                mstar.LinuxI2C("/dev/i2c-999")
            close.assert_called_once_with(999)
            write.assert_not_called()
            read.assert_not_called()

    def test_native_short_read_and_write_are_fatal(self):
        import struct
        def ioctl(_fd, request, value, *rest):
            if request == 0x0705:
                value[:] = struct.pack("L", 1)
            return 0
        with patch.object(mstar.sys, "platform", "linux"), \
             patch.object(mstar.os, "stat", return_value=types.SimpleNamespace(st_mode=stat.S_IFCHR)), \
             patch.object(mstar.os, "open", return_value=999), \
             patch.object(mstar.os, "close") as close, \
             patch("fcntl.flock"), patch("fcntl.ioctl", side_effect=ioctl), \
             patch.object(mstar.os, "write", return_value=1), \
             patch.object(mstar.os, "read", return_value=b"x"):
            adapter = mstar.LinuxI2C("/dev/i2c-999")
            with self.assertRaises(mstar.MstarError):
                adapter.write(0x49, b"MSTAR")
            with self.assertRaises(mstar.MstarError):
                adapter.read(0x49, 3)
            adapter.close()
            close.assert_called_once_with(999)

    def transport(self):
        wire = WireTransport()
        wire.close = lambda: None
        return wire

    def test_session_marker_blocks_before_transport_construction(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / profile().key
            directory.mkdir()
            (directory / "unresolved.json").write_text("previous unresolved operation")
            with patch.object(mstar, "LinuxI2C", side_effect=AssertionError("adapter opened")):
                with self.assertRaises(mstar.MstarError):
                    with mstar.Session(profile(), "/dev/i2c-999", Path(temporary)):
                        pass

    def test_unresolved_marker_survives_route_edid_and_generic_identity_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            state = Path(temporary)
            original = profile()
            wire = self.transport()
            with patch.object(mstar, "LinuxI2C", return_value=wire):
                with mstar.Session(original, original.transport_locator, state) as session:
                    session.bridge.enter()
                    marker = session.marker
                # No verified finish: the actual marker stays after close.
            self.assertTrue(marker.exists())
            self.assertNotIn(("write", 0x49, b"\x24"), wire.calls)
            candidates = [
                dataclasses_replace(original, transport_locator="/dev/i2c-998", product_code=4321, edid_sha256="a" * 64),
                dataclasses_replace(original, serial="H1AK500000", model="operator-renamed model", transport_locator="/dev/i2c-997"),
                dataclasses_replace(original, serial="0", manufacturer="SAM", product_code=1, edid_sha256="b" * 64),
            ]
            for changed in candidates:
                with self.subTest(profile=changed):
                    self.assertEqual(changed.key, original.key)
                    with patch.object(mstar, "open_transport", side_effect=AssertionError("transport opened despite unresolved family marker")):
                        with self.assertRaisesRegex(mstar.MstarError, "Unresolved session"):
                            with mstar.Session(changed, changed.transport_locator, state):
                                pass
                    self.assertTrue(marker.exists())

    def test_session_lock_is_exclusive_and_exit_is_explicit(self):
        with tempfile.TemporaryDirectory() as temporary:
            wire = self.transport()
            with patch.object(mstar, "LinuxI2C", return_value=wire):
                with mstar.Session(profile(), "/dev/i2c-999", Path(temporary)) as first:
                    with self.assertRaises(BlockingIOError):
                        with mstar.Session(profile(), "/dev/i2c-999", Path(temporary)):
                            pass
                    first.bridge.enter()
                    with self.assertRaises(mstar.MstarError):
                        first.finish(False)
                    self.assertTrue(first.marker.exists())
                self.assertTrue(wire.entered)
                self.assertNotIn(("write", 0x49, b"\x24"), wire.calls)

    def test_profile_locator_mismatch_never_opens_adapter(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(mstar, "LinuxI2C", side_effect=AssertionError("adapter opened")):
                with self.assertRaises(mstar.MstarError):
                    with mstar.Session(profile(), "/dev/i2c-998", Path(temporary)):
                        pass


class ProviderContractTests(unittest.TestCase):
    """Reusable contract cases for a provider factory, here with an offline fake."""
    def backend(self):
        class FakeBackend:
            capabilities = mstar.TransportCapabilities(True, True, 256, 37)
            writes = []
            closed = False
            read_result = b"abc"
            write_result = 3
            write_error = None
            def write(self, address, data):
                self.writes.append((address, data))
                if self.write_error:
                    raise self.write_error
                return self.write_result
            def read(self, _address, _count):
                return self.read_result
            def close(self):
                self.closed = True
        return FakeBackend()

    def provider(self, backend):
        return mstar.TransportProvider("simulated-test", "Offline test provider", lambda _locator: backend,
                                       lambda: ["synthetic:0"], hardware=False)

    def test_selection_enumeration_never_construct_provider(self):
        def forbidden(_locator):
            raise AssertionError("Provider factory called during discovery")
        provider = dataclasses_replace(self.provider(None), factory=forbidden)
        with patch.dict(mstar.PROVIDERS, {provider.name: provider}), contextlib.redirect_stdout(io.StringIO()):
            self.assertIs(mstar.select_provider(provider.name), provider)
            self.assertEqual(mstar.main(["drivers"]), 0)
            self.assertEqual(mstar.main(["list", "--driver", provider.name]), 0)

    def test_simulated_provider_runs_unchanged_mstar_protocol(self):
        wire = WireTransport()
        wire.close = lambda: None
        provider = self.provider(wire)
        with patch.dict(mstar.PROVIDERS, {provider.name: provider}):
            transport = mstar.open_transport(provider.name, "synthetic:0")
            bridge = mstar.Bridge(transport, delay=lambda _: None)
            bridge.enter()
            self.assertEqual(bridge.profile()["jedec"], "ef4015")
            self.assertEqual(bridge.read(0, 256), image()[:256])
            bridge.exit()
            transport.close()

    def test_short_reads_and_writes_fail_without_padding_or_retry(self):
        backend = self.backend()
        provider = self.provider(backend)
        with patch.dict(mstar.PROVIDERS, {provider.name: provider}):
            transport = mstar.open_transport(provider.name, "synthetic:0")
            backend.read_result = b"ab"
            backend.write_result = 2
            with self.assertRaises(mstar.MstarError):
                transport.read(0x49, 3)
            with self.assertRaises(mstar.MstarError):
                transport.write(0x49, b"abc")
            self.assertEqual(len(backend.writes), 1)

    def test_write_error_propagates_without_retry(self):
        backend = self.backend()
        backend.write_error = OSError("Injected provider write failure")
        transport = mstar.CheckedTransport(backend)
        with self.assertRaises(OSError):
            transport.write(0x49, b"abc")
        self.assertEqual(len(backend.writes), 1)

    def test_capabilities_reject_and_close_before_any_i2c(self):
        for caps in (mstar.TransportCapabilities(False, True, 256, 37),
                     mstar.TransportCapabilities(True, False, 256, 37),
                     mstar.TransportCapabilities(True, True, 255, 37),
                     mstar.TransportCapabilities(True, True, 256, 36)):
            backend = self.backend()
            backend.capabilities = caps
            provider = self.provider(backend)
            with patch.dict(mstar.PROVIDERS, {provider.name: provider}):
                with self.assertRaises(mstar.MstarError):
                    mstar.open_transport(provider.name, "synthetic:0")
            self.assertTrue(backend.closed)
            self.assertFalse(backend.writes)

    def test_oversized_transfers_reject_before_provider_io(self):
        backend = self.backend()
        transport = mstar.CheckedTransport(backend)
        with self.assertRaises(mstar.MstarError):
            transport.write(0x49, bytes(38))
        with self.assertRaises(mstar.MstarError):
            transport.read(0x49, 257)
        self.assertFalse(backend.writes)


if __name__ == "__main__":
    unittest.main(verbosity=2)
