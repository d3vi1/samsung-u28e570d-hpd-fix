#!/usr/bin/env python3
"""Offline programming-policy regressions; supply the official stock BIN path."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import mstar
from test_mstar import profile

STOCK_PATH = None


class ProgrammingPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if STOCK_PATH is None:
            raise unittest.SkipTest("Run this suite with the official stock BIN argument")
        cls.stock = STOCK_PATH.read_bytes()
        cls.candidate = (Path(__file__).resolve().parents[1] / "firmware" / "m-E5728UMDB-1005.0-experimental.1.bin").read_bytes()
        cls.fmt = mstar.container_tool()
        _, body, _ = cls.fmt.inspect(cls.stock)
        mutated = bytearray(body)
        mutated[0x1000] ^= 1
        cls.unknown = cls.fmt.repack(cls.stock, bytes(mutated))
        cls.fmt.inspect(cls.unknown)  # All container CRCs remain valid.
        assert mstar.sha(cls.unknown) not in mstar.PROGRAM_PACKAGE_SHA256
        cls.source = cls.stock + b"\xff" * (mstar.CAPACITY - len(cls.stock))
        cls.upgraded = mstar.prepare_target(cls.source, cls.candidate, "package")

    def test_known_packages_support_forward_and_reverse_package_plans(self):
        self.assertEqual(mstar.prepare_target(self.source, self.candidate, "package"), self.upgraded)
        reversed_target = mstar.prepare_target(self.upgraded, self.stock, "package")
        self.assertEqual(mstar.package_prefix(reversed_target), self.stock)

    def test_crc_valid_repacked_application_is_read_only(self):
        with self.assertRaisesRegex(mstar.MstarError, "not approved"):
            mstar.prepare_target(self.source, self.unknown, "package")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unknown.bin"
            path.write_bytes(self.unknown)
            with contextlib.redirect_stdout(io.StringIO()), patch.object(mstar, "LinuxI2C", side_effect=AssertionError("adapter opened")):
                self.assertEqual(mstar.main(["inspect", str(path)]), 0)

    def test_self_pinned_unknown_input_fails_before_plan_or_hardware(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unknown.bin"
            path.write_bytes(self.unknown)
            args = types.SimpleNamespace(backup=Path(temporary), input=path,
                                         input_sha256=mstar.sha(self.unknown), input_type="package",
                                         output=Path(temporary) / "must-not-exist", dry_run=False)
            with patch.object(mstar, "load_backup", return_value=(self.source, {})), \
                 patch.object(mstar, "make_plan", side_effect=AssertionError("unknown plan created")), \
                 patch.object(mstar, "Session", side_effect=AssertionError("hardware opened")):
                with self.assertRaisesRegex(mstar.MstarError, "not approved"):
                    mstar.write_image(args, profile())
            self.assertFalse(args.output.exists())

    def test_unknown_source_is_not_programmable(self):
        source = self.unknown + self.source[len(self.unknown):]
        with self.assertRaisesRegex(mstar.MstarError, "Source package"):
            mstar.prepare_target(source, self.stock, "package")

    def test_full_dump_cannot_bypass_known_package_policy(self):
        unknown = self.unknown + self.source[len(self.unknown):]
        with self.assertRaisesRegex(mstar.MstarError, "cannot bypass"):
            mstar.prepare_target(self.source, unknown, "full", verified_restore=unknown)

    def test_full_restore_requires_exact_separate_backup_bytes(self):
        with self.assertRaisesRegex(mstar.MstarError, "explicit verified"):
            mstar.prepare_target(self.upgraded, self.source, "full")
        with self.assertRaisesRegex(mstar.MstarError, "explicit verified"):
            mstar.prepare_target(self.upgraded, self.source, "full", verified_restore=self.upgraded)
        self.assertEqual(mstar.prepare_target(self.upgraded, self.source, "full", verified_restore=self.source), self.source)

    def test_restore_requires_same_unit_bundle_and_protected_tail(self):
        changed = self.source[:-1] + bytes([self.source[-1] ^ 1])
        with self.assertRaisesRegex(mstar.MstarError, "same-unit tail"):
            mstar.prepare_target(self.upgraded, changed, "full", verified_restore=changed)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            selected = directory / "restore.bin"
            selected.write_bytes(self.source)
            args = types.SimpleNamespace(backup=directory / "current", restore_backup=directory / "restore",
                                         input=selected, input_sha256=mstar.sha(self.source), input_type="full")
            calls = []
            def load(path, supplied_profile):
                calls.append((path, supplied_profile))
                if path == args.backup:
                    return self.upgraded, {}
                raise mstar.MstarError("Backup belongs to a different identity/profile/transport locator")
            with patch.object(mstar, "load_backup", side_effect=load):
                with self.assertRaisesRegex(mstar.MstarError, "different identity"):
                    mstar.selected_plan(args, profile())
            self.assertEqual([call[0] for call in calls], [args.backup, args.restore_backup])

    def test_cli_full_input_requires_restore_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            selected = directory / "full.bin"
            selected.write_bytes(self.source)
            args = types.SimpleNamespace(backup=directory, input=selected, input_sha256=mstar.sha(self.source), input_type="full")
            with patch.object(mstar, "load_backup", return_value=(self.upgraded, {})):
                with self.assertRaisesRegex(mstar.MstarError, "--restore-backup"):
                    mstar.selected_plan(args, profile())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 linux/test_policy.py /path/to/official-1004.5.bin")
    STOCK_PATH = Path(sys.argv[1])
    unittest.main(argv=[sys.argv[0]], verbosity=2)
