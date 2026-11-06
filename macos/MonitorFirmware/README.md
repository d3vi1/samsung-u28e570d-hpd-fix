# Monitor Firmware for macOS

Native SwiftUI application with an independent Swift firmware parser, backup/verification engine, transactional writer and provider interface. The application builds as an arm64 `.app`; all development verification is offline or uses in-memory devices.

**Hardware writes are not qualified or enabled on macOS.** A read-only experimental IOAV raw transport candidate is implemented, but requires explicit opt-in, an active selected display, an unambiguous complete EDID identity and an exact IOAV route. The explicit **Identify selected route** action binds the selected service using the fixed Samsung firmware query and bounded EDID reads; it does not guess a CoreGraphics-to-IOAV association. A successful backup is evidence, not permission to bypass write qualification.

## Build and test

Requires macOS 14 or later and an installed Xcode/Swift toolchain. Developed with Xcode 27.2 and Swift 6.4.2 on arm64. There are no downloaded package dependencies; the native bridge links system IOKit and zlib.

```sh
cd macos/MonitorFirmware
swift test
bash scripts/build-app.sh
```

The bundle is created at `build/Monitor Firmware.app`, with a metadata-free distribution archive at `build/MonitorFirmware-macos-arm64.zip`. The script ad-hoc signs and verifies the bundle. It does not provide Developer ID signing or notarization. File Provider may attach Finder metadata to an `.app` stored in a synced Documents folder after signing; the ZIP excludes that metadata. Open `Package.swift` in Xcode for development. Tests also validate the repository's unchanged `firmware/m-E5728UMDB-1005.0-experimental.1.bin`; retain that file when running this package's tests.

## Application workflow

1. Choose an I²C driver and press **Refresh**. Enumeration reads cached CoreGraphics/IORegistry data only. No IOAV service is opened and no DDC/ISP transaction runs on launch, refresh, selection or file browsing.
2. Select a cached external IOAV route and press **Identify selected route**. Confirm that its physical monitor is awake. This explicit action sends only the existing fixed `C3/0` query and two EDID reads at offset 0, plus offset 128 when the declared extension requires it. It validates checksums, supported Samsung identity and firmware family. A unique CoreGraphics vendor/product/numeric-serial match is recorded when present; otherwise awake state is explicitly operator-confirmed. Generic serials and duplicate matches remain ambiguous and cannot authorize ISP access. No route scanning or automatic fallback occurs.
3. Browse a Samsung package or full SPI image. **Verify file offline** checks bounds, header metadata, both CRC16 fields, both CRC32 fields and exact raw-DEFLATE input consumption/output length. A package is never described as a full SPI backup.
4. Choose the archive parent folder. **Download backup ×2** performs two complete 2 MiB reads, compares every byte, verifies unchanged flash status and saves both reads plus a common backup manifest. The experimental IOAV path additionally requires its explicit read-only opt-in. The UI does not silently enable it.
5. **Verify monitor ×2** compares two complete reads against a full expected image. A package needs the selected monitor's verified backup to compose that full expected image.
6. **Review upload / restore plan** prepares the immutable plan entirely in memory before showing a scrollable review sheet. It includes the unit/route, source full-image SHA-256, target full-image SHA-256, plan SHA-256 and exact sector order. Changing the input, source backup, restore provenance or target invalidates it. Execution consumes that same plan and runs transport/profile/identity/backup preflight before any erase. The current IOAV hardware-write gate rejects execution before opening the service. The complete transaction is exercised using mock devices in tests.

For a full-image restore, load a separate **current** own-unit backup, then choose **Load restore target** and select the older own-unit backup's manifest. The complete target image must match its two provenance files. A bare full BIN without target provenance is inspect/verify only.

The separate coding EEPROM is **not available**. There is no EEPROM read/write command, configuration change, inferred EEPROM backup, or misleading success stub.

## Providers and the raw transport boundary

`DisplayTransportProvider` owns enumeration, capability reporting and device creation. `RawI2CBus` defines exact raw transactions. `MStarISPDevice` supplies SPI commands. `FlashEngine` supplies full-image evidence and mutation/recovery policy. SwiftUI depends on the provider and engine, not a specific GPU/IOKit implementation.

| Provider | Enumeration | ISP reads | Flash writes |
| --- | --- | --- | --- |
| Apple Silicon IOAV/DCP | Explicit cached OS enumeration | Experimental candidate, opt-in plus complete target association required | Disabled pending independent write qualification |
| Intel IOFramebuffer/IOI2C | Unavailable implementation | Unavailable | Unavailable |

The Intel provider fails with its capability reason; it does not pretend to work or fall back to another monitor/driver. Future providers must implement the same raw transaction contract and pass the same command/state tests.

The candidate sends complete raw packets to seven-bit address `0x49` with `dataAddress = UINT32_MAX` for both IOAV read and write. Static analysis of this Mac's local `AppleDPTXController` identifies that value as the no-subaddress sentinel; DCPAVServiceProxy forwards the two scalar arguments unchanged. This does **not** prove behavior in the remote DCP firmware or on the wire. Using `dataAddress = 0` would not express raw mode in the examined local controller.

The private IOAV read wrapper returns status but discards the actual output length. Reads use independent randomized prefill and independent full-image comparisons to expose unfilled tails; this is detection evidence, not a claim that the private API guarantees the requested byte count. No zero-payload write is needed for the raw candidate: single-byte `0x11`, `0x12` and `0x24` packets are passed as complete one-byte buffers.

Experimental read-only transport accepts MSTAR entry, SPI `03/05/35/15/5A/9F` reads, transaction delimiters, WRDI and verified-session exit. It rejects WREN, erase, program, status-register writes and unknown packets before native service creation. The low-level C bridge is a candidate ABI implementation; the application never calls it outside an explicit operation through the selected provider.

The provider opens only the selected registry ID and requires the `DCPAVServiceProxy` class. A per-route process lock serializes identification and ISP sessions. The route is identified again immediately before ISP entry; DDC queries are not interleaved with an active ISP session. After verified exit, the provider waits three seconds for startup, then confirms identity with at most six attempts, two seconds apart, within a further 12-second retry window. Only transport/framing failures may be retried. Every attempt, failure and available invalid frame is archived. A valid wrong identity is immediately rejected; it is never treated as startup settling. A completed backup manifest is published only after this confirmation.

## Flash profiles and evidence format

Both implemented profiles require JEDEC `ef4015`, capacity `2097152`, 4096-byte sectors and physical 256-byte pages. Programming uses 32-byte chunks that do not cross a physical page boundary.

| Profile | Known SFDP first 16 bytes | Status registers / expected bytes |
| --- | --- | --- |
| Legacy | `53464450000100ff00000109800000ff` | `05/35` → `0000`; **never reads `15`** |
| JV | `53464450050100ff00050110800000ff` | `05/35/15` → `000260` |

The device reads JEDEC twice and SFDP64 twice and rejects instability. Only the known first 16 SFDP bytes select a profile; all 64 captured bytes are preserved in `flash-sfdp.bin` and covered by the manifest SHA-256. Tests use an explicitly synthetic 48-byte tail and make no claim that it came from hardware. Unknown profile, geometry or protection/status values fail before destructive commands. No status register is modified.

`backup.json` uses `monitorfw.backup.v1`, `image_kind: spi_full`, numeric `identity.product_code`, explicit ambiguity metadata, lowercase hexadecimal status strings, `flash.sfdp_bytes: 64`, `flash.sfdp_file: flash-sfdp.bin`, physical `page_size: 256`, and `programming_chunk_size: 32`. It records both full read files, byte counts and hashes, equality, UTC creation time, and `coding_eeprom.available: false`. Synthetic backups carry `simulated: true` and cannot be used with a hardware device. A manifest is checked against its actual files and fresh device evidence; it cannot enable a driver or override a live mismatch.

Import accepts other safe, distinct SFDP leaf filenames and valid bundles carrying explicit identity ambiguity. Those bundles remain usable for inspection/read-only comparison; ambiguous identity is rejected independently by write planning and live hardware gates. Manifest metadata is checked semantically, and regular-file sizes are bounded before reading JSON, SFDP or full-image evidence.

The emitted `model: LU28E570DS/EN` names the examined compatibility profile. EDID does not establish the `/EN` regional suffix; the separately queried firmware family is `m-E5728UMDB`.

## Transaction and recovery rules

The source and target package hashes must match the exact approved 1004.5 or 1005.0 packages before staging or ISP. Valid CRCs alone do not authorize a write; unknown packages remain available for offline inspection and read-only verification. The explicit test-only `simulationOnly` policy requires simulated provenance and is rejected before any hardware method when `device.isSimulated` is false.

A package is overlaid onto the current own-unit full backup. A full restore instead requires separate verified target-backup provenance and restores its exact bytes, including the gap between an older shorter package and the newer longer package. Both backups must belong to the same unambiguous identity and flash profile. Bytes at `0x90000` and above must remain identical. Bootloader, PM51 slot, invariant header bytes and loader bytes other than the embedded length must be preserved. This rejects unqualified cross-model changes and broad full-chip replacement.

Before erase, the engine archives source/target/provenance, verifies live identity/profile/status and compares **two fresh complete source reads** against the backup. It holds a per-unit filesystem lock and persists a recovery marker before the first potentially destructive command. Changed main sectors are written first and header sector `0x10000` last. Each sector receives full erased-state verification, 32-byte programming and immediate full-sector read-back. Final verification consists of **two complete target reads** plus unchanged status, even for a no-op plan.

Cancellation is checked between read blocks and sectors. An active sector finishes before cancellation triggers restoration. Recovery ignores cancellation, restores every touched sector in header-last order, and requires two complete source reads plus unchanged status. No reset is sent after unresolved writes, failed rollback, failed no-op final verification or an incomplete initial read. Reset permission is an in-memory opaque value created by the engine; it cannot be imported from JSON.

The application holds system/display idle-sleep activity assertions while an operation runs and refuses normal application termination until it finishes. Cancellation remains available; it does not interrupt required recovery.

After a full source/target/rollback image and status are verified, the orchestrator explicitly exits ISP. It never resets in `deinit`, unconditional cleanup or exception handling. An exit failure does not launch another destructive rollback. The recovery marker is retained until verified image state and successful ISP exit are established; a retained marker blocks a subsequent operation. Review its named archive instead of deleting it merely to retry.

Every operation gets a new archive directory. Partial reads, both mismatching complete reads, SFDP64, source/target images, operation logs and transaction results are retained as applicable. Incomplete or mismatching reads never produce a successful backup manifest. Failed hardware qualification is reported as failure, never as a successful simulated operation.

New archive/recovery directories and their parent links are synchronized before ISP work. New evidence files are synchronized together with their directory entries. The recovery marker's data and directory entry are synchronized before erase; marker removal is followed by a directory synchronization. Its stable key uses manufacturer and normalized unit serial, excluding product, EDID hash and route so changing inputs cannot bypass an unresolved write. Failures stop the operation before mutation. These are actual OS `fsync` operations, not a guarantee about cloud-provider durability, storage hardware caches or arbitrary power loss.

## Verification boundaries

XCTest covers structural integrity, the unchanged repository candidate, full backup/manifest reload, SFDP64 evidence, short/mismatched reads, exact raw MStar packets, both profile status selections, provider capability failures, destructive-command rejection, own-unit/tail checks, header-last writes, source mismatch, no-op final failure, cancellation, successful rollback, unresolved recovery markers and ISP-exit gating/failure. Tests never enumerate live displays, open IOAV, send DDC/ISP, change display power, or request UAC.

No live monitor backup, upload, hardware reset, or transport qualification was performed during development. A built application and passing simulations establish the software behavior tested here; they do not establish remote DCP or physical flash compatibility.
