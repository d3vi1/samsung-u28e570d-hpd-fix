# Linux monitor firmware CLI

`mstar.py` is a Python 3.10+ standard-library implementation for an explicit Linux `i2c-dev` adapter and the reviewed MStar ISP / EF4015 legacy and JV 2 MiB flash profiles. It can back up the main SPI flash, verify a selected full image, plan a firmware change, and program it with readback. **This Linux implementation has only offline/mock validation; no live Linux adapter or monitor has been qualified.** The Windows transport evidence does not qualify Linux adapter behavior.

No command runs at import or CLI startup. `drivers` lists registered providers; `list` lists filesystem device paths without opening adapters. `inspect`, `inspect-edid`, `plan`, `write --dry-run`, `simulate` and `eeprom` do no hardware I/O. Actual backup/verify/write require an explicit `/dev/i2c-N`, a matching local profile and `--acknowledge-isp`. Writing additionally requires `--acknowledge-erase` and the selected input's SHA-256. Select the access driver with `--driver i2c-dev` (the default).

## Scope and transport

- Seven-bit ISP address `0x49`; ASCII `MSTAR` entry; `0x10` SPI command prefix, separate `0x11` read request and raw read, then `0x12` release. `0x24` exits ISP and can reboot the monitor.
- Every bridge transfer uses a separate `i2c-dev` read/write syscall. No repeated-start concatenation, SMBus emulation, bus scan, address probe loop or `I2C_SLAVE_FORCE` is used. Raw-I2C adapter capability is required. Short reads and writes are fatal, not padded or silently retried.
- Both variants have JEDEC `EF4015`. SFDP is read **before selecting status opcodes**. Legacy SFDP prefix `53464450000100ff00000109800000ff` requires status `05/35 = 00/00`; opcode `0x15` is never sent. JV prefix `53464450050100ff00050110800000ff` requires status `05/35/15 = 00/02/60`. The detected variant must match the explicit profile. No status/protection register writes are implemented. A 256-byte read is checked against sixteen independent 16-byte reads before a bulk operation.
- The main array is exactly 2 MiB. The write unit is a 4 KiB erase sector; physical flash pages are 256 bytes, but program chunks are always 32 bytes, with immediate chunk readback and full-sector readback. There is no unqualified larger-chunk mode. Backup metadata records `page_size=256` and `programming_chunk_size=32` separately.
- This does **not** back up configuration EEPROM, coding data, OTP, security registers or TCON storage. `eeprom` explains the unsupported route without trying vendor commands. A firmware change can still alter separate configuration after boot.

## Explicit local profile

Copy `profile.example.json` (JV) or `profile.legacy.example.json` (legacy) to a private local file and replace its placeholders using the unit's known flash profile. `inspect-edid` reads a previously saved EDID, reports its SHA-256, manufacturer, product code and text serial, and makes no monitor calls:

```sh
python3 linux/mstar.py list
python3 linux/mstar.py drivers
python3 linux/mstar.py inspect-edid /path/to/saved-edid.bin
python3 linux/mstar.py inspect /path/to/firmware.bin
python3 linux/mstar.py eeprom
```

Select the adapter yourself. Do not discover it by running MSTAR entry against every bus. The profile's `transport_locator` must equal the explicit device argument. Before ISP entry, the client reads EDID from that one adapter and checks its header/checksums, complete hash, manufacturer, product code and text serial against the profile. It supports a base EDID and at most one extension.

An EDID serial or hash can be shared by multiple physical monitors. The metadata explicitly records this ambiguity. The profile and transport locator express the operator's physical selection; before any erase, two complete live reads must also match the exact backup bytes. Keep the connection fixed during an operation and do not substitute another unit's backup. The `firmware` profile label is operator-supplied expected context; a recognized firmware label extracted from the SPI data is recorded separately in the backup.

## Double-read backup

The example arguments below refer to your selected adapter and private profile, not a preconfigured device. The output directory must not already exist. Use a parent directory you control.

```sh
sudo python3 linux/mstar.py backup \
  --driver i2c-dev --device /dev/i2c-N --profile /path/to/local-profile.json \
  --output /path/to/new-backup --acknowledge-isp
```

Artifacts are `backup-read-1.bin`, `backup-read-2.bin`, `flash-sfdp.bin`, `backup.json` and `result.json`. Both passes must contain all 2,097,152 bytes and match exactly; flash identity, status and SFDP must remain stable. On failure or mismatch, completed passes are retained and the manifest is named `backup-invalid.json`, with `reads_equal=false`; no valid-looking `backup.json` is produced. No partial or mismatched backup is accepted as a write source. An incomplete read, mismatch, cancellation or final profile failure withholds ISP exit/reset and retains the unresolved-session marker; the original error and completed evidence files remain available.

`backup.json` uses `monitorfw.backup.v1` and contains `image_kind=spi_full`, model/firmware, explicit identity/transport locator and ambiguity, flash profile/status/SFDP evidence, both filenames/lengths/SHA-256 values, equality status, UTC timestamp and the unavailable coding-EEPROM declaration. SFDP evidence is all 64 bytes (`sfdp_bytes=64`), retained as `flash-sfdp.bin`, named by `flash.sfdp_file` and hashed in the manifest; the known first 16 bytes select the profile. The backup reader requires a safe local filename and a regular, non-symlink file, checks the actual 64-byte length and SHA-256, and verifies that the saved prefix matches the explicit known flash profile. Additional platform evidence fields remain accepted. The reader validates every required field, type, constant and date-time in `apps/backup.schema.json` before consuming the files. It accepts extra evidence fields and distinct safe local read filenames from other platforms, then recomputes both image hashes and compares their bytes. Missing fields, unsafe paths, symlinks and aliased read filenames are rejected. Keep these artifacts private; they may contain monitor-specific data.

## Verify a selected full image

```sh
sudo python3 linux/mstar.py verify \
  --device /dev/i2c-N --profile /path/to/local-profile.json \
  --input /path/to/expected-full.bin --input-sha256 YOUR_FULL_IMAGE_SHA256 \
  --output /path/to/new-verification --acknowledge-isp
```

Verification performs two independent complete reads, retains both as `read1.bin` and `read2.bin`, compares them to each other and the selected image, and checks final status/SFDP. It does not program flash. ISP exit/reset and marker removal occur only after both complete reads match the selected image and the final profile is unchanged. Failure or cancellation retains the marker and completed reads without sending `0x24`. A nonzero exit or `success=false` is failure; inspect `result.json`.

## Plan and write

The first release programs only these exact reviewed package SHA-256 values, for both the current source and selected target:

- Official 1004.5: `60e0b2322236b03673d1bd8b5182b36462824baada9a8ec9bff6c724d30d060b`.
- Experimental 1005.0: `3d5cda5eb49c6c0bb0378742aa1767becd31d3b3e85a6c0b9c023bd6ada2dac4`.

The operator's `--input-sha256` additionally catches an unintended file selection; it cannot authorize another package. A repacked image with valid CRCs but another hash is inspectable and its full-array image can be verified read-only, but it cannot produce a programming plan. The restriction also applies to `write --dry-run` and embedded packages in full dumps.

`--input-type package` overlays an approved package onto the exact full backup, preserving the remainder. `--input-type full` is a separate same-unit restoration path: it requires `--restore-backup` pointing to a verified dual-read bundle whose complete bytes equal the selected 2 MiB image. Its identity, explicit transport locator and flash evidence must match the current profile. The source and target must preserve the bootstrap, PM51 and bytes outside the source/target package extents. Package CRCs and lengths are also checked using the public container tool. Older 1002/1003 uploads are rejected, although their raw main-array backup can still be read.

```sh
python3 linux/mstar.py plan \
  --profile /path/to/local-profile.json --backup /path/to/verified-backup \
  --input /path/to/selected-package.bin --input-type package \
  --input-sha256 YOUR_SELECTED_FILE_SHA256

sudo python3 linux/mstar.py write \
  --device /dev/i2c-N --profile /path/to/local-profile.json \
  --backup /path/to/verified-backup \
  --input /path/to/selected-package.bin --input-type package \
  --input-sha256 YOUR_SELECTED_FILE_SHA256 \
  --output /path/to/new-write-run --acknowledge-isp --acknowledge-erase
```

Adding `--dry-run` to `write` returns the same offline plan without opening the adapter or creating the run directory; acknowledgements are then unnecessary. `plan` is the shorter offline form.

To plan a full restoration, keep `--backup` pointed at the verified **current** image and use the historical same-unit bundle separately:

```sh
python3 linux/mstar.py plan \
  --profile /path/to/local-profile.json --backup /path/to/current-backup \
  --input-type full --restore-backup /path/to/historical-backup \
  --input /path/to/historical-backup/backup-read-1.bin \
  --input-sha256 YOUR_HISTORICAL_FULL_IMAGE_SHA256
```

Before ISP entry, the writer validates and snapshots all files, then durably stages `source-full.bin`, `target-full.bin` and `plan.json`. Newly created artifact and session directories, including missing session ancestors, are individually fsynced together with their parent directory entries before ISP. Before erase, two full live reads must equal the staged source, and live SFDP must match the backup evidence. Only changed sectors are written; sector `0x10000`, containing the main header, is last. Every programmed chunk and sector is read back. After programming, write-disable is confirmed, two full target reads are compared byte for byte, and the final status and profile are checked. The exact target SHA-256 is in the plan; the final result records whether this target was verified.

## Cancellation, ownership and recovery boundary

The tool holds an advisory exclusive lock on the adapter descriptor and a shared recovery lock for the supported `m-E5728UMDB` family. Linux EDID identities are currently all treated as ambiguous, so the recovery key deliberately excludes the route, EDID hash/product code, serial and descriptive model label. A bus renumbering, input change or generic serial cannot bypass an unresolved marker. This conservatively blocks other monitors of the same family until the unresolved session is recovered. This prevents cooperating instances from colliding, not kernel display-driver or unrelated `i2c-tools` activity. Stop other firmware/control applications and avoid concurrent bus traffic. A durable unresolved-session marker is created before attempting ISP entry. All invocations for this firmware family use the same state directory, default `/var/lib/samsung-mstar`; do not change it to bypass a marker.

SIGINT/SIGTERM request cancellation. The signal handler does not interrupt a sector halfway through programming. Cancellation is acted on before the next sector and enters restoration of every touched sector. Ordinary I/O/verification failures also attempt restoration, including a sector whose erase may have succeeded before an exception. Restoration ignores cancellation, uses 32-byte chunks, then requires two full source reads and unchanged status. Diagnostic journal failures are recorded and do not abort restoration.

**ISP exit/reset is withheld after an unresolved partial write.** Closing the Python transport never sends `0x24` implicitly. A failed restoration leaves the marker in place, and subsequent operations refuse adapter access. Deleting the marker is not recovery. This CLI does not yet provide an automatic rescue entry for an arbitrary partial image; review the saved source/target/journal and use an independently qualified recovery method. Power loss, SIGKILL, cable removal, kernel I2C hangs and a fully verified image that does not boot are outside the in-process rollback guarantee. Individual read calls use the adapter/kernel's timing; the 30-minute bulk-read deadline is checked between transactions.

## Offline tests and simulation

```sh
python3 linux/test_mstar.py
python3 linux/test_policy.py /path/to/official-1004.5.bin
python3 linux/mstar.py simulate /path/to/source-full.bin /path/to/target-full.bin
python3 linux/mstar.py simulate /path/to/source-full.bin /path/to/target-full.bin \
  --fail-after-erase 0x20000
python3 linux/mstar.py simulate /path/to/source-full.bin /path/to/target-full.bin \
  --fail-after-erase 0x20000 --fail-restore
```

The simulator exercises the transaction engine against memory, including arbitrary synthetic images; it cannot access a hardware provider. Tests separately check byte-level bridge transfer ordering and 32-byte programming, native-call short-transfer handling, EDID checks, session locks/markers, directory-sync order, schema/equality evidence, dual preflight/final verification, cancellation, post-erase failure, final-read failure, rollback failure and reset inhibition. The policy suite mutates an application byte and repacks valid CRCs, then checks that unknown package/full-image programming remains rejected before plan creation or hardware access. These tests do not qualify a real Linux adapter.

## Adding an I2C access driver

The named `TransportProvider` registry separates access drivers from `Bridge` (MStar/SPI), the image planner and `execute` (the flash transaction engine). The implemented `i2c-dev` provider uses the Linux kernel interface and is vendor-neutral; there are no pretend Intel/AMD/NVIDIA provider entries or claims of tested GPU support. The test suite registers an in-memory `simulated-test` provider and runs the same bridge against it. The separate `simulate` CLI exercises the flash engine without a transport.

A contributor adds a backend and one `TransportProvider` registration, without modifying the firmware planner or transaction engine. The provider has a name, description, factory, hardware flag and a **passive** locator enumerator. Lookup/enumeration must not open adapters, probe addresses, enter ISP or issue vendor commands. Construction occurs only inside the explicitly requested hardware session, after the durable session lock and unresolved-marker check.

The backend contract is:

- `capabilities: TransportCapabilities(raw_i2c, separate_transfers, max_read, max_write)`. This implementation requires raw, separate STOP-delimited transfers, reads of at least 256 bytes and writes of at least 37 bytes. Capability declarations do not replace native capability checks or live read-size qualification.
- `write(address_7bit, data) -> int` performs exactly one transfer and returns the actual count; `read(address_7bit, count) -> bytes` performs exactly one transfer. Never pad, retry, combine, or treat errors as success. The common `CheckedTransport` rejects short transfers and requests exceeding declared limits.
- The factory validates its locator, acquires exclusive cooperating-process ownership and closes resources if construction fails. `close()` releases resources only; it must never enter/exit ISP, reset or mutate the monitor. Session code owns those decisions.

Add the provider to `PROVIDERS` to make `--driver NAME` available. Use `ProviderContractTests` as a shared fault-test template: no I/O during selection/enumeration, short reads/writes, propagated write errors, missing raw/separate-transfer support, insufficient transfer limits and oversized requests. Also supply native API mocks and separate hardware qualification evidence for the new backend. A kernel or vendor API lacking the required separate-transfer behavior must be rejected, not silently approximated.
