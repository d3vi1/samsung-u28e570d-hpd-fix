#!/usr/bin/env python3
"""Explicit-bus Linux MStar ISP laboratory client. No discovery-time I2C I/O."""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import stat
import struct
import sys
import time
from datetime import datetime, timezone
from typing import Callable

CAPACITY = 2 * 1024 * 1024
SECTOR = 4096
PAGE = 32
PHYSICAL_PAGE = 256
HEADER = 0x10000
JEDEC = bytes.fromhex("ef4015")
STATUS = bytes.fromhex("000260")
SFDP = bytes.fromhex("53464450050100ff00050110800000ff")
SFDP_FILE = "flash-sfdp.bin"
DEFAULT_FLASH_PROFILE = "EF4015-2MiB-SR000260-SFDP1.5"
# Container integrity does not authorize a program image. This first release
# programs only these independently reviewed, exact distributed packages.
PROGRAM_PACKAGE_SHA256 = frozenset((
    "60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b",
    "3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4",
))
FLASH_PROFILES = {
    DEFAULT_FLASH_PROFILE: {"sfdp": SFDP, "status": STATUS, "status_opcodes": (0x05, 0x35, 0x15)},
    "EF4015-2MiB-legacy-SR0000-SFDP1.0": {
        "sfdp": bytes.fromhex("53464450000100ff00000109800000ff"),
        "status": bytes.fromhex("0000"), "status_opcodes": (0x05, 0x35)},
}


class MstarError(Exception):
    pass


class Cancelled(MstarError):
    pass


def require(ok: bool, message: str):
    if not ok:
        raise MstarError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def utc() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def json_bytes(value) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def sync_directory(directory: Path):
    fd = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def durable_directory(directory: Path, *, parents=False, exist_ok=False):
    """Persist each newly created directory's entry in its own parent."""
    directory = directory.absolute()
    pending = []
    cursor = directory
    while not cursor.exists():
        pending.append(cursor)
        if not parents:
            break
        cursor = cursor.parent
    if not pending:
        if not exist_ok:
            raise FileExistsError(str(directory))
        require(directory.is_dir(), "Directory path is not a directory: " + str(directory))
    for child in reversed(pending):
        try:
            child.mkdir(mode=0o700)
        except FileExistsError:
            if child == directory and not exist_ok:
                raise
            require(child.is_dir(), "Directory path is not a directory: " + str(child))
        sync_directory(child)
        sync_directory(child.parent)


def save_new(path: Path, data: bytes):
    """Exclusive file creation and durable bytes; never overwrite an artifact."""
    with path.open("xb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    sync_directory(path.parent)


def save_json(path: Path, value):
    save_new(path, json_bytes(value))


@dataclasses.dataclass(frozen=True)
class Profile:
    model: str
    serial: str
    edid_sha256: str
    expected_firmware: str
    manufacturer: str
    product_code: int
    transport_locator: str
    flash_profile: str = DEFAULT_FLASH_PROFILE

    @classmethod
    def load(cls, path: Path) -> "Profile":
        value = json.loads(path.read_text())
        require(value.get("schema") == "monitorfw.profile.v1", "Unsupported profile schema")
        identity = value["identity"]
        for key in ("model", "edid_serial", "edid_sha256", "firmware", "manufacturer", "transport_locator"):
            require(isinstance(identity.get(key), str) and identity[key].strip(), "Missing identity field: " + key)
            require("REPLACE" not in identity[key], "Replace the example profile placeholders first")
        require(re.fullmatch(r"[0-9a-f]{64}", identity["edid_sha256"]) is not None, "EDID SHA256 must be lowercase hex")
        require(type(identity.get("product_code")) is int and 0 <= identity["product_code"] <= 65535, "Invalid EDID product code")
        flash = value["flash"]
        name = flash.get("profile")
        require(name in FLASH_PROFILES, "Unknown flash profile; choose the explicit legacy or JV profile")
        spec = FLASH_PROFILES[name]
        require(flash == {"profile": name, "capacity": CAPACITY, "jedec": JEDEC.hex(),
                          "status": spec["status"].hex(), "sfdpPrefix": spec["sfdp"].hex()},
                "Only the exact reviewed EF4015 legacy/JV profiles are supported; no status-register writes")
        return cls(identity["model"], identity["edid_serial"], identity["edid_sha256"], identity["firmware"],
                   identity["manufacturer"], identity["product_code"], identity["transport_locator"], name)

    def identity(self):
        return {"manufacturer": self.manufacturer, "product_code": self.product_code,
                "edid_serial": self.serial, "edid_sha256": self.edid_sha256,
                "transport_locator": self.transport_locator, "ambiguous": True,
                "ambiguity_reason": "EDID serial/hash may be shared by multiple units; transport locator and current full-byte preflight are also required."}

    @property
    def key(self):
        # No Linux profile currently carries a validated physical-unit ID:
        # identity() explicitly marks every EDID identity ambiguous. Use one
        # conservative family interlock, including generic/changed serials.
        # An input change, bus renumbering or descriptive profile edit must
        # never make an unresolved monitor appear to be a fresh session.
        return sha(json_bytes({"recovery_family": "m-E5728UMDB"}))


def decode_edid(edid: bytes) -> dict:
    require(len(edid) in (128, 256) and edid[:8] == bytes.fromhex("00ffffffffffff00"), "Invalid EDID size/header")
    require(edid[126] <= 1 and len(edid) == 128 * (edid[126] + 1), "Only base EDID plus at most one extension is supported")
    require(all(sum(edid[i:i + 128]) % 256 == 0 for i in range(0, len(edid), 128)), "EDID checksum failed")
    fields = {}
    for offset in (54, 72, 90, 108):
        descriptor = edid[offset:offset + 18]
        if descriptor[:3] == b"\0\0\0" and descriptor[3] in (0xFF, 0xFC):
            fields["serial" if descriptor[3] == 0xFF else "name"] = descriptor[5:18].decode("ascii", errors="strict").rstrip("\n\r\0 ")
    manufacturer_code = int.from_bytes(edid[8:10], "big")
    manufacturer = "".join(chr(64 + ((manufacturer_code >> shift) & 31)) for shift in (10, 5, 0))
    return {"sha256": sha(edid), "length": len(edid), "manufacturer": manufacturer,
            "product_code": int.from_bytes(edid[10:12], "little"), **fields}


@dataclasses.dataclass(frozen=True)
class TransportCapabilities:
    raw_i2c: bool
    separate_transfers: bool
    max_read: int
    max_write: int


class CheckedTransport:
    """Common provider boundary: no padding, retries, hidden reset or coalescing."""
    def __init__(self, backend):
        self.backend = backend
        caps = backend.capabilities
        require(isinstance(caps, TransportCapabilities) and caps.raw_i2c and caps.separate_transfers,
                "Driver must support raw I2C and separate STOP-delimited transfers")
        require(caps.max_read >= 256 and caps.max_write >= PAGE + 5,
                "Driver cannot carry required 256-byte reads and 37-byte writes")
        self.capabilities = caps

    def write(self, address: int, data: bytes):
        require(0 <= address <= 0x7f and 0 < len(data) <= self.capabilities.max_write,
                "Write exceeds driver address/transfer limits")
        count = self.backend.write(address, data)
        require(type(count) is int and count == len(data),
                f"Driver returned short/invalid write count {count!r}/{len(data)}; side effects are possible")
        return count

    def read(self, address: int, count: int) -> bytes:
        require(0 <= address <= 0x7f and 0 < count <= self.capabilities.max_read,
                "Read exceeds driver address/transfer limits")
        data = self.backend.read(address, count)
        require(isinstance(data, bytes) and len(data) == count, "Driver returned a short/invalid read")
        return data

    def close(self):
        self.backend.close()  # Must not enter/exit ISP or reset the monitor.


class LinuxI2C:
    """Separate write/read syscalls, each a STOP-delimited raw i2c-dev transfer."""
    # Client limits, not a claim that every kernel/GPU adapter supports them.
    # I2C_FUNCS and live read-size qualification are separate checks.
    capabilities = TransportCapabilities(True, True, 256, PAGE + 5)
    def __init__(self, device: str):
        import fcntl
        require(sys.platform.startswith("linux"), "Live transport requires Linux")
        require(re.fullmatch(r"/dev/i2c-[0-9]+", device) is not None, "Select an explicit /dev/i2c-N device; no scanning")
        require(stat.S_ISCHR(os.stat(device).st_mode), "Adapter path must be a character device")
        self.device = device
        self.fd = os.open(device, os.O_RDWR | os.O_CLOEXEC)
        self.fcntl = fcntl
        try:
            # Advisory lock shared by this tool's users of the same device inode.
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            capabilities = bytearray(struct.calcsize("L"))
            fcntl.ioctl(self.fd, 0x0705, capabilities, True)  # I2C_FUNCS
            require(struct.unpack("L", capabilities)[0] & 1 != 0, "Adapter lacks raw I2C transfers")
        except BaseException:
            os.close(self.fd)
            raise

    def write(self, address: int, data: bytes):
        self.fcntl.ioctl(self.fd, 0x0703, address)  # I2C_SLAVE, never I2C_SLAVE_FORCE
        count = os.write(self.fd, data)
        require(count == len(data), f"Short I2C write: {count}/{len(data)}; transaction may have side effects")
        return count

    def read(self, address: int, count: int) -> bytes:
        self.fcntl.ioctl(self.fd, 0x0703, address)
        data = os.read(self.fd, count)
        require(len(data) == count, f"Short I2C read: {len(data)}/{count}")
        return data

    def close(self):
        os.close(self.fd)


@dataclasses.dataclass(frozen=True)
class TransportProvider:
    name: str
    description: str
    factory: Callable[[str], object]
    enumerate_locators: Callable[[], list[str]]
    hardware: bool = True


PROVIDERS = {
    "i2c-dev": TransportProvider(
        "i2c-dev", "Linux kernel i2c-dev; GPU-vendor-neutral, not hardware-qualified here",
        lambda locator: LinuxI2C(locator),
        lambda: sorted(str(path) for path in Path("/dev").glob("i2c-[0-9]*"))),
}


def select_provider(name: str) -> TransportProvider:
    """Pure registry lookup. Selecting a driver must not construct/open it."""
    require(name in PROVIDERS, "Unknown driver: " + name)
    return PROVIDERS[name]


def open_transport(name: str, locator: str) -> CheckedTransport:
    backend = select_provider(name).factory(locator)
    try:
        return CheckedTransport(backend)
    except BaseException:
        backend.close()
        raise


class Bridge:
    def __init__(self, transport, delay: Callable[[float], None] = time.sleep, expected_profile=None):
        self.transport = transport
        self.delay = delay
        self.entered = False
        self.expected_profile = expected_profile
        self.flash_profile = None

    def write(self, data: bytes):
        self.transport.write(0x49, data)

    def enter(self):
        self.entered = True  # A failed transfer may nevertheless have entered ISP.
        self.write(b"MSTAR")
        self.delay(0.1)

    def exit(self):
        self.write(b"\x24")  # Exits ISP and can reset/reboot the monitor.
        self.entered = False

    def read_edid(self) -> bytes:
        data = bytearray()
        for offset in range(0, 128, 16):
            self.transport.write(0x50, bytes([offset]))
            part = self.transport.read(0x50, 16)
            require(len(part) == 16, "Short EDID read")
            data.extend(part)
        require(data[126] <= 1, "More than one EDID extension is unsupported")
        if data[126]:
            for offset in range(128, 256, 16):
                self.transport.write(0x50, bytes([offset]))
                part = self.transport.read(0x50, 16)
                require(len(part) == 16, "Short EDID read")
                data.extend(part)
        decode_edid(bytes(data))
        return bytes(data)

    def spi_read(self, command: bytes, count: int) -> bytes:
        require(command and command[0] in (0x9F, 0x05, 0x35, 0x15, 0x03, 0x5A), "Unsupported SPI read command")
        require(command[0] != 0x15 or self.flash_profile == DEFAULT_FLASH_PROFILE,
                "Status opcode 0x15 is allowed only after SFDP identifies the reviewed JV variant")
        require(1 <= count <= 256, "SPI reads are restricted to 1..256 bytes")
        try:
            self.write(b"\x10" + command)
            self.write(b"\x11")
            result = self.transport.read(0x49, count)
            require(len(result) == count, "Short bridge response")
            return result
        finally:
            self.write(b"\x12")

    def spi_write(self, command: bytes):
        require((command[:1] in (b"\x06", b"\x04") and len(command) == 1)
                or (command[:1] == b"\x20" and len(command) == 4)
                or (command[:1] == b"\x02" and len(command) == 4 + PAGE), "Unsupported SPI mutation")
        try:
            self.write(b"\x10" + command)
        finally:
            self.write(b"\x12")

    def ready(self, timeout: float = 5) -> int:
        deadline = time.monotonic() + timeout
        while True:
            status = self.spi_read(b"\x05", 1)[0]
            if not status & 1:
                return status
            require(time.monotonic() < deadline, "SPI busy timeout")
            self.delay(0.005)

    def status(self) -> bytes:
        # Select via SFDP before considering opcode 0x15. It is never sent to
        # the legacy EF4015 variant, including first entry and final checks.
        if self.flash_profile is None:
            self.detect_sfdp()
        spec = FLASH_PROFILES[self.flash_profile]
        return bytes([self.ready()] + [self.spi_read(bytes([opcode]), 1)[0] for opcode in spec["status_opcodes"][1:]])

    def detect_sfdp(self) -> bytes:
        sfdp = self.spi_read(b"\x5a\0\0\0\0", 64)
        matches = [name for name, spec in FLASH_PROFILES.items() if sfdp.startswith(spec["sfdp"])]
        require(len(matches) == 1, "SFDP does not identify a reviewed legacy/JV variant")
        detected = matches[0]
        require(self.expected_profile is None or detected == self.expected_profile, "SFDP variant differs from explicit profile")
        require(self.flash_profile is None or detected == self.flash_profile, "SFDP variant changed during the session")
        self.flash_profile = detected
        return sfdp

    def profile(self) -> dict:
        ids = [self.spi_read(b"\x9f", 3), self.spi_read(b"\x9f", 3)]
        require(ids[0] == ids[1] == JEDEC, "JEDEC identity differs from EF4015")
        sfdp = self.detect_sfdp()
        status = self.status()
        require(status == FLASH_PROFILES[self.flash_profile]["status"], "Status registers differ from reviewed variant; refusing status changes")
        return {"profile": self.flash_profile, "jedec": ids[0].hex(),
                "status": status.hex(), "sfdp": sfdp.hex(), "sfdpSHA256": sha(sfdp),
                "qualification": "protocol reference only; Linux hardware has not been qualified"}

    def read(self, address: int, length: int, check: Callable = lambda: None) -> bytes:
        require(0 <= address <= CAPACITY and 0 <= length <= CAPACITY - address, "Read outside flash")
        result = bytearray()
        deadline = time.monotonic() + 1800
        for start in range(address, address + length, 256):
            check()
            require(time.monotonic() < deadline, "Flash read exceeded 30 minutes")
            amount = min(256, address + length - start)
            result.extend(self.spi_read(b"\x03" + start.to_bytes(3, "big"), amount))
        return bytes(result)

    def validate_read_size(self):
        block = self.read(0, 256)
        small = b"".join(self.spi_read(b"\x03" + offset.to_bytes(3, "big"), 16) for offset in range(0, 256, 16))
        require(block == small and len(set(block)) >= 8, "256-byte transport qualification against independent 16-byte reads failed")

    def write_disable(self):
        self.spi_write(b"\x04")
        require(self.ready() & 3 == 0, "Write-disable or ready state could not be confirmed")

    def enable_write(self):
        require(self.ready() == STATUS[0], "Unexpected status before write-enable")
        self.spi_write(b"\x06")
        require(self.spi_read(b"\x05", 1)[0] == 2, "Write-enable latch did not set exactly")

    def write_sector(self, address: int, data: bytes):
        require(address % SECTOR == 0 and 0 <= address <= CAPACITY - SECTOR and len(data) == SECTOR, "Invalid sector request")
        self.enable_write()
        self.spi_write(b"\x20" + address.to_bytes(3, "big"))
        require(self.ready() == STATUS[0], "Erase was not accepted or status changed")
        require(self.read(address, SECTOR) == b"\xff" * SECTOR, "Full-sector blank check failed")
        # Cancellation is deferred until the complete sector has been programmed
        # and read back, or an I/O error has entered the restoration path.
        for offset in range(0, SECTOR, PAGE):
            part = data[offset:offset + PAGE]
            if part == b"\xff" * PAGE:
                continue
            self.enable_write()
            self.spi_write(b"\x02" + (address + offset).to_bytes(3, "big") + part)
            require(self.ready() == STATUS[0], "Program was not accepted or status changed")
            require(self.read(address + offset, PAGE) == part, "Program-chunk readback failed")
        require(self.read(address, SECTOR) == data, "Sector readback failed")


@dataclasses.dataclass(frozen=True)
class Plan:
    source: bytes
    target: bytes
    sectors: tuple[int, ...]

    def summary(self):
        value = {"schema": "SamsungMstar.Plan/v1", "capacity": CAPACITY,
                 "sectorBytes": SECTOR, "programBytes": PAGE, "sourceSHA256": sha(self.source),
                 "targetSHA256": sha(self.target), "changedSectors": list(self.sectors), "headerLast": True}
        value["planSHA256"] = sha(json_bytes(value))
        return value


def make_plan(source: bytes, target: bytes) -> Plan:
    require(len(source) == len(target) == CAPACITY, "Source and target must both be exactly 2 MiB")
    sectors = [offset for offset in range(0, CAPACITY, SECTOR) if source[offset:offset + SECTOR] != target[offset:offset + SECTOR]]
    sectors.sort(key=lambda value: (value == HEADER, value))
    return Plan(bytes(source), bytes(target), tuple(sectors))


@dataclasses.dataclass
class Transition:
    reset_allowed: bool = False
    source_verified: bool = False
    target_verified: bool = False
    rollback_verified: bool = False
    mutation_started: bool = False
    completed: bool = False
    touched: list[int] = dataclasses.field(default_factory=list)
    error: str | None = None
    rollback_errors: list[str] = dataclasses.field(default_factory=list)
    log_errors: list[str] = dataclasses.field(default_factory=list)


def execute(device, plan: Plan, state: Transition, check=lambda: None, log=lambda value: None):
    """Full-source preflight, bounded mutation units, exact rollback on failure."""
    require(plan == make_plan(plan.source, plan.target), "Plan contents/order were changed")
    require(not state.touched and not state.source_verified and not state.completed, "State cannot be reused")
    def record(value):
        try:
            log(value)
        except Exception as error:
            state.log_errors.append(str(error))
    initial = None
    try:
        check()
        initial = device.status()
        require(initial in [spec["status"] for spec in FLASH_PROFILES.values()], "Preflight status mismatch")
        for _ in range(2):
            require(device.read(0, CAPACITY, check) == plan.source, "Full source differs from the verified backup; no erase performed")
        require(device.status() == initial, "Status changed during source verification")
        state.source_verified = True
        state.reset_allowed = True
        for address in plan.sectors:
            check()
            # Mark touched before erase: an exception can follow a side effect.
            state.reset_allowed = False
            state.mutation_started = True
            state.touched.append(address)
            record({"event": "sector-begin", "address": address})
            device.write_sector(address, plan.target[address:address + SECTOR])
            require(device.read(address, SECTOR) == plan.target[address:address + SECTOR], "Independent sector verification failed")
            record({"event": "sector-verified", "address": address})
        device.write_disable()
        state.reset_allowed = False
        for _ in range(2):
            require(device.read(0, CAPACITY) == plan.target, "Final full target verification failed")
        require(device.status() == initial, "Final status changed")
        state.target_verified = state.reset_allowed = state.completed = True
        record({"event": "target-verified", "sha256": sha(plan.target)})
    except BaseException as error:
        state.error = str(error)
        if state.mutation_started:
            state.reset_allowed = False
            # Ignore cancellation during restoration; attempt every touched sector.
            for address in state.touched:
                try:
                    device.write_sector(address, plan.source[address:address + SECTOR])
                    require(device.read(address, SECTOR) == plan.source[address:address + SECTOR], "Restored sector mismatch")
                    record({"event": "sector-restored", "address": address})
                except BaseException as restore_error:
                    state.rollback_errors.append(str(restore_error))
            try:
                device.write_disable()
                for _ in range(2):
                    require(device.read(0, CAPACITY) == plan.source, "Restored full source mismatch")
                require(device.status() == initial, "Restored status mismatch")
                state.rollback_verified = state.reset_allowed = True
                record({"event": "rollback-verified", "sha256": sha(plan.source)})
            except BaseException as restore_error:
                state.rollback_errors.append(str(restore_error))
        raise


class Cancellation:
    def __init__(self):
        self.requested = False
        self.previous = {}

    def __enter__(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            self.previous[sig] = signal.signal(sig, lambda *_: setattr(self, "requested", True))
        return self

    def check(self):
        if self.requested:
            raise Cancelled("Cancellation requested; stopping at a safe boundary")

    def __exit__(self, *_):
        for sig, previous in self.previous.items():
            signal.signal(sig, previous)


class Session:
    """Own the adapter and durable unresolved-session marker; never implicit reset."""
    def __init__(self, profile: Profile, device: str, state_directory: Path, context=None, driver="i2c-dev"):
        self.profile = profile
        self.device = device
        self.directory = state_directory / profile.key
        self.marker = self.directory / "unresolved.json"
        self.lock = None
        self.transport = None
        self.bridge = None
        self.evidence = None
        self.context = context or {}
        self.driver = select_provider(driver).name  # Selection alone has no I/O.

    def __enter__(self):
        import fcntl
        require(self.device == self.profile.transport_locator, "Device path differs from the explicit profile locator")
        durable_directory(self.directory, parents=True, exist_ok=True)
        self.lock = (self.directory / "session.lock").open("a+b")
        try:
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            require(not self.marker.exists(), f"Unresolved session blocks hardware access: {self.marker}. Review recovery evidence; deleting it is not recovery.")
            self.transport = open_transport(self.driver, self.device)
            self.bridge = Bridge(self.transport, expected_profile=self.profile.flash_profile)
            edid = self.bridge.read_edid()
            decoded = decode_edid(edid)
            require(decoded["sha256"] == self.profile.edid_sha256 and decoded.get("serial") == self.profile.serial
                    and decoded["manufacturer"] == self.profile.manufacturer and decoded["product_code"] == self.profile.product_code,
                    "Live EDID differs from the explicit same-unit profile; ISP not entered")
            self.evidence = decoded
            save_json(self.marker, {"schema": "SamsungMstar.Unresolved/v1", "createdUTC": utc(),
                                   "identity": self.profile.identity(), "transport": self.device,
                                   "driver": self.driver, "context": self.context})
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        try:
            if self.transport:
                self.transport.close()
                self.transport = None
        finally:
            if self.lock:
                self.lock.close()
                self.lock = None

    def finish(self, reset_allowed: bool) -> bool:
        require(reset_allowed, f"Reset withheld; unresolved marker retained at {self.marker}")
        if self.bridge.entered:
            self.bridge.exit()
        self.marker.unlink()
        sync_directory(self.directory)
        return True

    def __exit__(self, *_):
        self.close()  # No automatic 0x24 here; caller must prove reset permission.


def container_tool():
    path = Path(__file__).resolve().parents[1] / "tools" / "firmware_container.py"
    spec = importlib.util.spec_from_file_location("samsung_container_linux", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def package_prefix(image: bytes) -> bytes:
    require(len(image) == CAPACITY, "Full image must be 2 MiB")
    end = struct.unpack_from("<I", image, 0x10020)[0] + 4
    require(0x23880 < end <= 0x90000, "Unsupported full-image package boundary")
    return image[:end]


def prepare_target(source: bytes, selected: bytes, input_type: str, *, verified_restore: bytes | None = None) -> bytes:
    require(input_type in ("package", "full"), "Unsupported programming input type")
    fmt = container_tool()
    source_package = package_prefix(source)
    require(sha(source_package) in PROGRAM_PACKAGE_SHA256,
            "Source package is not an approved exact 1004.5/1005.0 image; inspection/backup/verification only")
    fmt.inspect(source_package)
    if input_type == "full":
        require(verified_restore is not None and selected == verified_restore,
                "Full-image programming requires exact bytes from an explicit verified same-unit restore backup")
    else:
        require(verified_restore is None, "A restore backup applies only to full-image restoration")
        require(sha(selected) in PROGRAM_PACKAGE_SHA256,
                "Selected package SHA256 is not approved for programming; CRC-valid unknown packages are read-only")
    target = selected if input_type == "full" else selected + source[len(selected):]
    require(len(target) == CAPACITY, "Target must resolve to exactly 2 MiB")
    target_package = package_prefix(target)
    require(sha(target_package) in PROGRAM_PACKAGE_SHA256,
            "Target package SHA256 is not approved for programming; a full dump cannot bypass package policy")
    fmt.inspect(target_package)
    if input_type == "package":
        require(target_package == selected, "Selected package contains trailing bytes")
    require(source[:0x10000] == target[:0x10000] and source[0x10080:0x20080] == target[0x10080:0x20080],
            "This writer preserves bootstrap and PM51; selected image changes them")
    end = max(len(source_package), len(target_package))
    require(source[end:] == target[end:], "Full target changes bytes outside the two firmware package extents; same-unit tail must be preserved")
    return bytes(target)


def validate_backup_manifest(manifest):
    """Validate every assertion in the shared schema, with no third-party dependency.

    This intentionally supports only the schema's current keyword set. A new
    assertion keyword fails closed until its validation is implemented here.
    Extra evidence fields are accepted as the shared contract permits.
    """
    schema_path = Path(__file__).resolve().parents[1] / "apps" / "backup.schema.json"
    schema = json.loads(schema_path.read_text())
    supported = {"$schema", "$id", "title", "description", "type", "required", "properties",
                 "const", "enum", "minimum", "maximum", "minLength", "pattern", "format",
                 "minItems", "maxItems", "items"}

    def validate(value, rule, location):
        def check(ok, reason):
            require(ok, f"Invalid backup manifest {location}: {reason}")
        check(not set(rule) - supported, "unsupported schema assertion")
        kinds = {"object": isinstance(value, dict), "array": isinstance(value, list),
                 "string": isinstance(value, str), "boolean": type(value) is bool,
                 "integer": type(value) is int or type(value) is float and value.is_integer()}
        if "type" in rule:
            check(kinds.get(rule["type"], False), "expected " + rule["type"])
        if "const" in rule:
            expected = rule["const"]
            check(value == expected and (type(value) is bool) == (type(expected) is bool), "wrong constant")
        if "enum" in rule:
            check(value in rule["enum"], "value outside enum")
        for key, compare in (("minimum", lambda a, b: a >= b), ("maximum", lambda a, b: a <= b)):
            if key in rule:
                check(type(value) in (int, float) and compare(value, rule[key]), "violates " + key)
        if "minLength" in rule:
            check(isinstance(value, str) and len(value) >= rule["minLength"], "string too short")
        if "pattern" in rule:
            check(isinstance(value, str) and re.fullmatch(rule["pattern"], value) is not None, "pattern mismatch")
        if "format" in rule:
            check(rule["format"] == "date-time", "unsupported format")
            check(isinstance(value, str) and re.fullmatch(
                r"\d{4}-\d\d-\d\d[Tt]\d\d:\d\d:\d\d(?:\.\d+)?(?:[Zz]|[+-](?:[01]\d|2[0-3]):[0-5]\d)", value) is not None,
                "invalid date-time")
            try:
                parsed = datetime.fromisoformat(value.replace("t", "T").replace("z", "+00:00").replace("Z", "+00:00"))
            except ValueError as error:
                raise MstarError(f"Invalid backup manifest {location}: invalid date-time") from error
            check(parsed.tzinfo is not None, "date-time lacks timezone")
        for key, compare in (("minItems", lambda a, b: a >= b), ("maxItems", lambda a, b: a <= b)):
            if key in rule:
                check(isinstance(value, list) and compare(len(value), rule[key]), "violates " + key)
        if isinstance(value, dict):
            for key in rule.get("required", []):
                check(key in value, "missing required " + key)
            for key, child in rule.get("properties", {}).items():
                if key in value:
                    validate(value[key], child, location + "." + key)
        if "items" in rule:
            check(isinstance(value, list), "expected array items")
            for index, item in enumerate(value):
                validate(item, rule["items"], f"{location}[{index}]")

    validate(manifest, schema, "$")


def read_backup_artifact(directory: Path, leaf: str, length: int, digest: str) -> bytes:
    require(isinstance(leaf, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", leaf) is not None,
            "Backup artifact must have a safe local filename")
    path = directory / leaf
    require(not path.is_symlink() and path.is_file(), "Backup artifact is missing or is not a regular local file: " + leaf)
    require(path.stat().st_size == length, "Backup artifact length mismatch: " + leaf)
    data = path.read_bytes()
    require(len(data) == length and sha(data) == digest, "Backup artifact length or SHA256 mismatch: " + leaf)
    return data


def load_backup(directory: Path, profile: Profile) -> tuple[bytes, dict]:
    manifest = json.loads((directory / "backup.json").read_text())
    validate_backup_manifest(manifest)
    identity = manifest["identity"]
    require(all(identity.get(key) == profile.identity()[key] for key in
                ("manufacturer", "product_code", "edid_serial", "edid_sha256", "transport_locator"))
            and manifest.get("model") == profile.model,
            "Backup belongs to a different identity/profile/transport locator")
    reads = manifest["reads"]
    require(reads[0]["file"] != reads[1]["file"], "Backup requires two distinct read artifact filenames")
    first, second = [read_backup_artifact(directory, record["file"], CAPACITY, record["sha256"]) for record in reads]
    require(first == second, "Backup passes differ or are incomplete")
    flash = manifest["flash"]
    require(flash.get("jedec") == JEDEC.hex() and flash.get("status_before") == flash.get("status_after") == FLASH_PROFILES[profile.flash_profile]["status"].hex()
            and flash.get("sector_size") == SECTOR and flash.get("page_size") == PHYSICAL_PAGE
            and flash.get("sfdp_bytes") == 64
            and re.fullmatch(r"[0-9a-f]{64}", flash.get("sfdp_sha256", "")) is not None,
            "Backup lacks matching flash/status/SFDP evidence")
    sfdp = read_backup_artifact(directory, flash["sfdp_file"], 64, flash["sfdp_sha256"])
    matched_profiles = [name for name, spec in FLASH_PROFILES.items() if sfdp.startswith(spec["sfdp"])]
    require(matched_profiles == [profile.flash_profile], "Backup SFDP does not match the explicit known flash profile")
    return first, manifest


def firmware_label(image: bytes) -> str:
    try:
        _, body, _ = container_tool().inspect(package_prefix(image))
        match = re.search(rb"m-E5728UMDB-[0-9]+\.[0-9]+", body)
        return match.group().decode() if match else "unknown"
    except (ValueError, MstarError, struct.error):
        return "unknown"


def backup(args, profile):
    durable_directory(args.output)
    outcome = {"schema": "SamsungMstar.Operation/v1", "operation": "backup", "createdUTC": utc(), "success": False, "resetSent": False}
    with Cancellation() as cancellation, Session(profile, args.device, args.state_directory,
                                                  {"operation": "backup", "artifacts": str(args.output.resolve())},
                                                  driver=getattr(args, "driver", "i2c-dev")) as session:
        passes = []
        evidence = None
        final_evidence = None
        try:
            session.bridge.enter()
            evidence = session.bridge.profile()
            save_new(args.output / SFDP_FILE, bytes.fromhex(evidence["sfdp"]))
            session.bridge.validate_read_size()
            for number in (1, 2):
                image = session.bridge.read(0, CAPACITY, cancellation.check)
                save_new(args.output / f"backup-read-{number}.bin", image)
                passes.append(image)
                final_evidence = session.bridge.profile()
                require(final_evidence == evidence, "Flash profile changed during backup")
            require(passes[0] == passes[1], "Two full reads differ; backup not verified")
            outcome["success"] = True
        except BaseException as error:
            outcome["error"] = str(error)
            raise
        finally:
            try:
                # Even a read-only session needs complete, stable evidence
                # before rebooting the monitor and clearing its recovery marker.
                if outcome["success"]:
                    outcome["resetSent"] = session.finish(True)
            except BaseException as error:
                outcome["success"] = False
                outcome["error"] = str(error)
                raise
            finally:
                manifest = {"schema": "monitorfw.backup.v1", "image_kind": "spi_full", "created_utc": utc(),
                            "identity": profile.identity(),
                            "model": profile.model, "firmware": firmware_label(passes[0]) if passes else "unknown",
                            "expected_firmware": profile.expected_firmware, "transport": {
                                "driver": getattr(args, "driver", "i2c-dev"), "locator": args.device,
                                "isp_address_7bit": 0x49, "separate_transfers": True},
                            "flash": {"jedec": evidence["jedec"] if evidence else None, "capacity": CAPACITY,
                                      "profile": evidence["profile"] if evidence else None,
                                      "sfdp_file": SFDP_FILE if evidence else None,
                                      "sfdp_sha256": evidence["sfdpSHA256"] if evidence else None,
                                      "sfdp_bytes": 64 if evidence else None,
                                      "sector_size": SECTOR, "page_size": PHYSICAL_PAGE, "programming_chunk_size": PAGE,
                                      "status_before": evidence["status"] if evidence else None,
                                      "status_after": final_evidence["status"] if final_evidence else None},
                            "reads": [{"file": f"backup-read-{i}.bin", "bytes": len(data), "sha256": sha(data)} for i, data in enumerate(passes, 1)],
                            "reads_equal": len(passes) == 2 and passes[0] == passes[1] and outcome["success"],
                            "coding_eeprom": {"available": False, "reason": "No safe arbitrary EEPROM export route is qualified; SPI backup excludes separate coding EEPROM/OTP/TCON."},
                            "profile_evidence": evidence, "hardware_qualified": False}
                save_json(args.output / ("backup.json" if manifest["reads_equal"] else "backup-invalid.json"), manifest)
                save_json(args.output / "result.json", outcome)
    return outcome


def selected_plan(args, profile):
    source, _ = load_backup(args.backup, profile)
    selected = args.input.read_bytes()
    require(sha(selected) == args.input_sha256, "Selected input SHA256 mismatch")
    restore_directory = getattr(args, "restore_backup", None)
    if args.input_type == "full":
        require(restore_directory is not None, "Full restoration requires --restore-backup with a verified same-unit bundle")
        verified_restore, _ = load_backup(restore_directory, profile)
    else:
        require(restore_directory is None, "--restore-backup is only valid with --input-type full")
        verified_restore = None
    target = prepare_target(source, selected, args.input_type, verified_restore=verified_restore)
    return make_plan(source, target)


def write_image(args, profile):
    plan = selected_plan(args, profile)  # All input validation occurs before opening the adapter.
    if args.dry_run:
        return {"dryRun": True, "hardwareAccessed": False, **plan.summary()}
    require(args.acknowledge_erase, "write requires --acknowledge-erase after reviewing the dry-run plan")
    durable_directory(args.output)
    # Durable exact source and target are available even after power/process loss.
    save_new(args.output / "source-full.bin", plan.source)
    save_new(args.output / "target-full.bin", plan.target)
    save_json(args.output / "plan.json", plan.summary())
    state = Transition()
    outcome = {"schema": "SamsungMstar.Operation/v1", "operation": "write", "createdUTC": utc(), "resetSent": False}
    with (args.output / "journal.jsonl").open("x") as journal:
        def log(value):
            journal.write(json.dumps({"utc": utc(), **value}) + "\n")
            journal.flush()
            os.fsync(journal.fileno())
        with Cancellation() as cancellation, Session(profile, args.device, args.state_directory,
                                                      {"operation": "write", "artifacts": str(args.output.resolve()), **plan.summary()},
                                                      driver=getattr(args, "driver", "i2c-dev")) as session:
            try:
                session.bridge.enter()
                evidence = session.bridge.profile()
                _, baseline_manifest = load_backup(args.backup, profile)
                require(evidence["sfdpSHA256"] == baseline_manifest["flash"]["sfdp_sha256"], "Live SFDP differs from backup evidence")
                session.bridge.validate_read_size()
                outcome["profileEvidence"] = evidence
                execute(session.bridge, plan, state, cancellation.check, log)
                require(session.bridge.profile() == evidence, "Flash profile changed after transition")
            except BaseException as error:
                outcome["error"] = str(error)
                # A post-transition identity/SFDP failure revokes reset permission.
                if state.completed:
                    state.reset_allowed = False
                raise
            finally:
                try:
                    if state.reset_allowed:
                        outcome["resetSent"] = session.finish(True)
                finally:
                    outcome["transition"] = dataclasses.asdict(state)
                    outcome["success"] = state.completed and outcome["resetSent"]
                    save_json(args.output / "result.json", outcome)
    return outcome


def verify_image(args, profile):
    expected = args.input.read_bytes()
    require(len(expected) == CAPACITY and sha(expected) == args.input_sha256, "Verification requires an exact pinned 2 MiB image")
    durable_directory(args.output)
    outcome = {"schema": "SamsungMstar.Operation/v1", "operation": "verify", "createdUTC": utc(), "success": False, "resetSent": False}
    with Cancellation() as cancellation, Session(profile, args.device, args.state_directory,
                                                  {"operation": "verify", "artifacts": str(args.output.resolve())},
                                                  driver=getattr(args, "driver", "i2c-dev")) as session:
        try:
            session.bridge.enter()
            evidence = session.bridge.profile()
            session.bridge.validate_read_size()
            reads = []
            for number in (1, 2):
                actual = session.bridge.read(0, CAPACITY, cancellation.check)
                save_new(args.output / f"read{number}.bin", actual)
                reads.append(actual)
            outcome.update({"sha256": [sha(data) for data in reads], "expectedSHA256": sha(expected),
                            "reads_equal": reads[0] == reads[1], "profileEvidence": evidence})
            require(reads[0] == reads[1] == expected, "Full readback passes differ from each other or selected image")
            require(session.bridge.profile() == evidence, "Profile changed during verification")
            outcome["success"] = True
        except BaseException as error:
            outcome["error"] = str(error)
            raise
        finally:
            try:
                if outcome["success"]:
                    outcome["resetSent"] = session.finish(True)
            except BaseException as error:
                outcome["success"] = False
                outcome["error"] = str(error)
                raise
            finally:
                save_json(args.output / "result.json", outcome)
    return outcome


class SimulatedFlash:
    """Deterministic transaction-engine device, never accesses an adapter."""
    def __init__(self, image: bytes, fail_after_erase: int | None = None, fail_restore=False):
        require(len(image) == CAPACITY, "Simulation source must be 2 MiB")
        self.image = bytearray(image)
        self.calls = []
        self.fail_after_erase = fail_after_erase
        self.failed = False
        self.fail_restore = fail_restore

    def status(self):
        return STATUS

    def read(self, address, length, check=lambda: None):
        check()
        return bytes(self.image[address:address + length])

    def write_disable(self):
        self.calls.append(("disable",))

    def write_sector(self, address, data):
        self.calls.append(("erase", address))
        self.image[address:address + SECTOR] = b"\xff" * SECTOR
        if (self.fail_after_erase == address and not self.failed) or (self.failed and self.fail_restore):
            self.failed = True
            raise MstarError("Injected failure after sector erase")
        self.image[address:address + SECTOR] = data


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="List driver locator paths only; no adapter opens or I2C transactions")
    listing.add_argument("--driver", choices=tuple(PROVIDERS), default="i2c-dev")
    commands.add_parser("drivers", help="List registered access drivers without constructing or opening one")
    inspect = commands.add_parser("inspect", help="Offline firmware container validation")
    inspect.add_argument("image", type=Path)
    edid = commands.add_parser("inspect-edid", help="Decode a previously saved EDID without hardware access")
    edid.add_argument("image", type=Path)
    commands.add_parser("eeprom", help="Explain why configuration EEPROM backup is currently unsupported")
    simulate = commands.add_parser("simulate", help="Offline full-image transition with optional fault injection")
    simulate.add_argument("source", type=Path)
    simulate.add_argument("target", type=Path)
    simulate.add_argument("--fail-after-erase", type=lambda value: int(value, 0))
    simulate.add_argument("--fail-restore", action="store_true")
    for name in ("backup", "verify", "write", "plan"):
        command = commands.add_parser(name)
        command.add_argument("--profile", required=True, type=Path)
        if name != "plan":
            command.add_argument("--driver", choices=tuple(PROVIDERS), default="i2c-dev")
            command.add_argument("--device", required=True)
            command.add_argument("--output", required=True, type=Path, help="New, nonexistent artifact directory")
            command.add_argument("--state-directory", type=Path, default=Path("/var/lib/samsung-mstar"))
            command.add_argument("--acknowledge-isp", action="store_true", help="Acknowledge ISP interrupts the monitor and exit can reboot it")
        if name in ("write", "plan"):
            command.add_argument("--backup", required=True, type=Path)
            command.add_argument("--input-type", choices=("package", "full"), required=True)
            command.add_argument("--restore-backup", type=Path,
                                 help="Required for full restoration: verified same-unit bundle containing the exact selected full image")
        if name != "backup":
            command.add_argument("--input", required=True, type=Path)
            command.add_argument("--input-sha256", required=True)
        if name == "write":
            command.add_argument("--dry-run", action="store_true")
            command.add_argument("--acknowledge-erase", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "list":
            provider = select_provider(args.driver)
            result = {"hardwareAccessed": False, "driver": provider.name, "adapters": provider.enumerate_locators()}
        elif args.command == "drivers":
            result = {"hardwareAccessed": False, "drivers": [
                {"name": provider.name, "description": provider.description, "hardware": provider.hardware}
                for provider in PROVIDERS.values()]}
        elif args.command == "inspect":
            data = args.image.read_bytes()
            result = container_tool().inspect(package_prefix(data) if len(data) == CAPACITY else data)[0]
        elif args.command == "inspect-edid":
            result = decode_edid(args.image.read_bytes())
        elif args.command == "eeprom":
            result = {"supported": False, "hardwareAccessed": False,
                      "reason": "No safe arbitrary configuration-EEPROM read route is qualified. Main SPI backup does not include it. C3 exposes only fixed windows; CB/C2/C4 and speculative GPIO/SERDB sequences are not attempted."}
        elif args.command == "simulate":
            plan = make_plan(args.source.read_bytes(), args.target.read_bytes())
            device = SimulatedFlash(plan.source, args.fail_after_erase, args.fail_restore)
            state = Transition()
            try:
                execute(device, plan, state)
            except BaseException:
                pass
            result = {"hardwareAccessed": False, "plan": plan.summary(), "transition": dataclasses.asdict(state), "finalSHA256": sha(bytes(device.image))}
        else:
            profile = Profile.load(args.profile)
            if args.command == "plan":
                result = {"hardwareAccessed": False, **selected_plan(args, profile).summary()}
            else:
                require(getattr(args, "dry_run", False) or args.acknowledge_isp, "Explicit --acknowledge-isp is required before opening any adapter")
                result = {"backup": backup, "verify": verify_image, "write": write_image}[args.command](args, profile)
        print(json.dumps(result, indent=2))
        return 0
    except (Exception, KeyboardInterrupt) as error:
        print(json.dumps({"success": False, "error": str(error)}, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
