# Samsung monitor firmware application for Windows

Development application for the reviewed Samsung LU28E570DS/EN firmware profile. It reads a full SPI backup, verifies an image and prepares/programs a reviewed firmware package or an evidenced backup from the same monitor. The default build uses simulated memory and cannot access a monitor.

Two frontends share the C++ transaction engine:

| Component | Status |
| --- | --- |
| Native Win32 frontend, intended for XP through Windows 7 | x86 compiled and mock-tested on Windows 10; XP/7 runtime and drivers unqualified |
| Separate Windows 8 WinRT/XAML frontend | C++/CX v110/SDK 8.0 source and static checks only; not compiled or deployed |
| Desktop broker for the XAML frontend | x86 compiled; file protocol and mock operations tested independently of XAML |
| NVIDIA I2C provider | Implemented; scripted transport tests and compile-only hardware build; no live validation of this C++ implementation |
| Intel and AMD providers | Explicitly unavailable; extension interfaces and documentation included |

No firmware BIN, device backup, calibration data, signing key or NVAPI DLL is distributed here. The full model string denotes the reviewed compatibility profile; it is not inferred completely from EDID.

## Build and try the offline application

Use an x86 Windows C++14 toolchain with CMake and Ninja on its process-local PATH. The tested toolchain and legacy limitations are in [Windows compatibility](docs/WINDOWS-COMPATIBILITY.md).

```powershell
cmake -S . -B build-x86 -G Ninja -DSAMSUNG_TARGET_XP=ON -DSAMSUNG_ENABLE_HARDWARE=OFF
cmake --build build-x86
ctest --test-dir build-x86 --output-on-failure
```

Launch `build-x86/samsung_monitor_firmware.exe`. Keep **MOCK / OFFLINE** selected, refresh the simulated endpoint and choose an output directory. **Read / download** creates a fresh directory containing both independent reads, their manifest and transaction evidence. **Browse** selects a Samsung package or a full SPI image; select the matching backup manifest for an own SPI image. **Verify** compares against the selected image. **Upload / restore** first creates a new backup and displays the exact immutable programming plan for confirmation.

The mock device contains synthetic firmware. Real packages may correctly fail its loader/profile checks. The hidden `--smoke-test` mode generates compatible synthetic inputs and exercises the full sequence automatically; it never opens a GPU. Normal uploads, including normal mock uploads, require the plan dialog.

Provider selection reads metadata only. Hardware access is excluded from the default application and broker entry points. `SAMSUNG_ENABLE_HARDWARE=ON` produces an experimental hardware-capable build; successful compilation is not qualification to use it. No application operation requires an automatic UAC prompt.

For the separate XAML project and its explicit deployment/registration steps, see [WinRT frontend](docs/WINRT-FRONTEND.md) and [broker protocol](docs/BROKER-PROTOCOL.md). The desktop CMake build does not build XAML. The registration script is not part of the test or build process.

## Transaction behavior

- The selected live identity is checked before ISP entry. JEDEC, all 64 SFDP bytes, capacity and the reviewed status-register profile must agree.
- Every complete backup consists of two independent 2 MiB reads. Both completed files are retained even on mismatch. A mismatch produces invalid evidence and does not authorize reset or programming. A read interrupted by an exception may be incomplete; no valid manifest is emitted for it.
- The explicit verification, immediate programming preflight and final read-back each use two full passes. Both completed read files are retained; transaction evidence records the source and target hashes.
- Programming uses an immutable list of changed 4 KiB sectors, packets carrying up to 32 data bytes and full read-back after each sector. If the firmware header sector at `0x10000` changes, it is written last. No chip erase or flash-status rewrite is used.
- Cancellation is accepted before the first mutation. Once a sector may have changed, the engine completes verification or attempts restoration of every touched sector; it does not interrupt midway because the UI was closed.
- Reset is attempted only after the target or restored original image and status have been verified. Persistent recovery markers block another normal operation after an unresolved interruption. There is no automatic reset in destructors and no blind recovery override in the UI.
- A Samsung package is overlaid on the current monitor's complete backup, preserving trailing bytes. Restoring a full SPI dump requires an unambiguous matching monitor identity and valid dual-read evidence. Hash, identity, bootstrap, PM51 and protected-tail checks reject images outside the reviewed constraints. The manifest is checked for consistency; it is not an authenticated certificate of origin.

The backup format is `monitorfw.backup.v1`; it includes both read files/hashes, identity, flash geometry/status and the SHA256 of `flash-sfdp.bin` (64 bytes). The physical page size is 256 bytes and the programming chunk size is 32 bytes. Completed artifacts are published without overwriting existing files, with a durable journal and recovery marker.

**Coding EEPROM is unavailable.** A main SPI dump does not establish a backup of separate EEPROM, OTP/security storage or every calibration value. No safe complete external EEPROM read route has been demonstrated, and a RAM mirror would not establish one. This application does not expose EEPROM coding or offset `0x8E` writes.

## Development references

- [Validation record](docs/VALIDATION.md): actual tests, binary hashes and untested targets.
- [Package format](docs/PACKAGE-FORMAT.md): bounded decompression, checksums and firmware policy.
- [Adding providers](docs/ADDING-PROVIDERS.md): I2C capabilities, identity and error contracts.
- [Windows compatibility](docs/WINDOWS-COMPATIBILITY.md): compiler/runtime and driver limitations.

The package parser can inspect historical images, while the hardware write policy accepts only reviewed package hashes and compatible loaders. Parsing an image does not authorize flashing it. Extending that policy is a reviewed code change, not a file-picker option.
