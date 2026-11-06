# Cross-platform firmware applications

These applications share a backup format and operation contract. Their drivers and
runtime qualification are independent: tests of a Windows/NVIDIA transport do not
qualify Linux i2c-dev or macOS IOAV.

The [validation record](VALIDATION.md) separates compiled/mock-tested components
from unperformed operating-system and monitor qualification.

The [macOS IOAV analysis](MACOS-IOAV.md) records the raw-addressing candidate and
the boundary between local kernel evidence and unqualified DCP hardware behavior.

| Platform | Source | Initial access provider |
|---|---|---|
| Windows | [C++ Win32 and separate WinRT/XAML frontend](../windows/app/README.md) | NVIDIA NVAPI; Intel/AMD extension interfaces, not implemented |
| Linux | [Python CLI](../linux/README.md) | Linux i2c-dev, independent of GPU vendor when the kernel exposes a suitable adapter |
| macOS | [SwiftUI application](../macos/MonitorFirmware/README.md) | Apple Silicon IOAV experimental candidate; Intel IOFramebuffer/IOI2C extension point |

Each implementation separates provider discovery, raw I2C access, the MStar ISP
protocol, the flash transaction engine, artifact storage and the UI/CLI. Selecting
an unavailable driver must not fall back to another driver or open hardware.
Enumeration/selection uses cached OS information where available; active identity
queries and ISP entry are explicit operations.

## Access-provider contract

A provider supplies a stable ID, human-readable name, availability reason,
endpoints and transfer capabilities. It opens only an explicitly selected endpoint.
The raw interface uses seven-bit chip addresses, exact byte arrays and exact read
counts. Writes do not add a subaddress implicitly. MStar ISP uses separate
STOP-delimited transfers; combined transactions are a different capability.

A provider must reject unsupported transfer sizes before sending, propagate native
errors, detect short transfers where the API reports them, serialize its own
operations, and never implement an implicit reset, wake, write-enable or address
scan. APIs that hide actual read length must disclose that limitation and require
additional protocol/data validation. Limits cannot be guessed from a successful
brightness query.

Provider contract tests must cover selection without I/O, unavailable providers,
transfer bounds, short reads/writes, errors and the absence of vendor-command
fallbacks. Firmware compatibility is enforced above the driver; adding Intel/AMD
access does not expand the supported monitor or firmware list automatically.

Native API references for provider contributors:

| Platform/provider | API and qualification boundary |
|---|---|
| Windows NVIDIA | [NVAPI I²C](https://docs.nvidia.com/nvapi/group__i2capi.html); current implementation, independent runtime qualification required |
| Windows Intel | [IGCL `ctlI2CAccess`](https://intel.github.io/drivers.gpu.control-library/Control/api.html#ctli2caccess); future provider; the documented driver-override flags are HDMI-specific |
| Windows AMD | [ADL I²C/DDC APIs](https://gpuopen-librariesandsdks.github.io/adl/group__I2CDDCEDIDAPI.html); future provider; verify raw-mode support and limits before adapting MStar packets |
| Linux | [Kernel i2c-dev](https://docs.kernel.org/i2c/dev-interface.html); available adapters and capabilities depend on the loaded hardware drivers |
| macOS legacy framebuffer | [Apple's IOI2C implementation](https://github.com/apple-oss-distributions/IOKitUser/blob/main/graphics.subproj/IOGraphicsLib.c); future provider where the matching framebuffer interface exists |
| macOS Apple Silicon | Private IOAV candidate described in [the local analysis](MACOS-IOAV.md); not a public compatibility guarantee |

An API's existence does not establish its support for this monitor's ISP framing.
The current Windows MStar adapter needs at least 256-byte reads and 37-byte writes.
For example, AMD's [published ADL constants](https://github.com/GPUOpen-LibrariesAndSDKs/display-library/blob/master/include/adl_defines.h)
include smaller limits for particular I²C entry points. A future provider must
reject an insufficient transport or implement and test a suitable bounded
packetization path; it must not simply advertise NVIDIA's limits.

## Shared backup format

The formal structure is [backup.schema.json](backup.schema.json), with
`schema = monitorfw.backup.v1` and `image_kind = spi_full`. It describes a completed
2 MiB main SPI backup. It is not an authenticated certificate or permission to
program a monitor.

For the examined hardware profile, the canonical `model` is `LU28E570DS/EN`.
This names the compatibility profile; it does not claim that EDID independently
establishes the full regional suffix. Keep firmware-family/version text in separate
fields instead of changing this identifier between implementations. Schema-valid
artifacts with an ambiguous identity remain readable for inspection; hardware
eligibility is a separate operation-specific decision.

- Read the complete flash twice through independent passes. Save both raw files as
  `backup-read-1.bin` and `backup-read-2.bin` before comparing them. Do not discard
  mismatch evidence or publish a successful `backup.json` for an unequal pair.
- Each pass must be exactly 2,097,152 bytes. Record its own SHA-256 and compare the
  actual bytes, not only a caller-supplied hash or `reads_equal` field.
- Record the exact selected transport locator, manufacturer, numeric product code,
  EDID serial/hash and identity ambiguity. A generic EDID serial is not proof of a
  physical unit's identity. Platform-specific extra fields are permitted.
- Read and save 64 SFDP bytes as `flash-sfdp.bin`, record their SHA-256 and
  `sfdp_bytes = 64`. Known profiles validate only the observed 16-byte header;
  do not invent or substitute the remaining 48 bytes.
- `status_before` and `status_after` are lowercase hexadecimal strings in the
  profile's opcode order. `page_size = 256` is the flash's physical page size;
  `programming_chunk_size = 32` is the conservative transfer size used by these apps.
- The coding EEPROM is separate. Set `coding_eeprom.available = false` with the
  reason; do not pad EDID, counters or a RAM fragment into a purported full backup.

Before consuming a backup, recompute and validate its saved evidence, reject unsafe
file references, and enforce live identity/profile and firmware policy. Extra
JSON fields must not invalidate otherwise compatible evidence; they also cannot
weaken required checks. A manifest alone cannot authorize writing to another unit.

The observed flash profiles are in [flash-profiles.json](flash-profiles.json).
The legacy profile reads only status opcodes 0x05/0x35; it must not assume that
0x15 exists. Neither profile authorizes rewriting status/configuration registers.

## Programming and verification contract

The first programming release is restricted to the exact public packages below.
Unknown images may be inspected and compared read-only; valid CRCs alone do not
establish firmware compatibility.

| Package | SHA-256 |
|---|---|
| Official 1004.5 | `60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b` |
| Experimental 1005.0 | `3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4` |

A package overlay preserves the same monitor's full-image tail. A full-image
restore requires separate own-backup provenance plus loader/PM51 and protected-tail
compatibility; a full dump must not bypass the package policy. Persist source,
target, plan and recovery state before the first erase.

Preflight and final verification use two full read passes. Programming operates on
changed 4 KiB sectors with 32-byte program chunks, verifies each sector immediately,
and writes the header sector at 0x10000 last. The exact immutable plan is shown
before programming. Cancellation is accepted before the first mutation; after
mutation begins, verification/restoration owns the process and must not be cut short
by closing a UI window.

An unresolved write prevents reset and another ordinary operation. Reset/ISP exit
requires verified source, target or restored-image evidence and unchanged status.
Recovery markers and staged files must be persisted before mutation. These measures
do not guarantee recovery from every power loss, lost transport or failed device.

Verification reports must distinguish comparing a package's byte range from
comparing all 2 MiB against a full-image reference. Dual reads establish consistency
of the observed bytes; they are not electrical bus measurements or proof that
separate coding EEPROM contents are included.
