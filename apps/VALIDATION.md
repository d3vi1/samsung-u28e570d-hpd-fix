# Application validation — 2026-11-06

These results concern the new applications. The earlier physical firmware flashes
used the separate C# laboratory tools; they do not qualify these implementations.
No live monitor I²C, ISP entry, erase/program, reset, display-power experiment or
coding-EEPROM operation was performed during this application development.

| Component | Executed evidence | Remaining boundary |
|---|---|---|
| Windows C++ / Win32 x86 | 7/7 CTest targets on Windows 10; 47 core, 33 transport, 15 broker-protocol, 17 parser and 20 storage cases; UI and broker mock workflows | New NVAPI backend not run on hardware; XP/7 runtime not tested; no x64 build |
| Windows WinRT/XAML | Compiled and linked using Microsoft v110 / Windows SDK 8 payloads; unsigned Appx packaged; 25 static checks on PowerShell 5.1 and 7 | Not signed, deployed or run; AppContainer-to-desktop integration not exercised; VCLibs 11 remains an external dependency |
| Windows independent review | Scoped review of the core/transport/parser and subsequent backup-ingestion diff; 47 portable core cases passed on macOS with AddressSanitizer and UndefinedBehaviorSanitizer | Win32 filesystem, broker and XAML behavior reviewed statically on this second host |
| Linux CLI | 46 general tests and 8 firmware-policy tests, run on macOS using mocked native calls and in-memory transport | Linux kernel adapter and monitor behavior not exercised |
| macOS SwiftUI/core | Release `.app` built and ad-hoc signed; 37 offline XCTest cases; independent native-route and transaction-core reviews; focused final model-metadata regression | IOAV raw transport and remote DCP remain unqualified; hardware writes disabled; Intel IOI2C provider unavailable |
| Shared backup contract | 16/16 test methods, including actual Windows and Linux readers consuming a Swift-generated mock bundle and each other's bundles | Synthetic data only; this tests format/evidence handling, not physical-unit provenance |

The shared suite recomputes both 2 MiB image hashes and SFDP64 evidence. It tests
both flash-profile variants, missing/corrupt/truncated evidence, required fields,
extra fields, distinct safe filenames and byte equality. The optional Swift bundle
is generated locally; no monitor dump or identity is checked into the repository.

```sh
python3 linux/test_mstar.py
python3 linux/test_policy.py /path/to/official-1004.5.bin
python3 apps/test_backup_contract.py
# Include a separately generated Swift mock bundle in the shared suite:
MONITORFW_MAC_BACKUP=/absolute/path/to/mock-bundle python3 apps/test_backup_contract.py
cd macos/MonitorFirmware
swift test
bash scripts/build-app.sh
```

Without `MONITORFW_MAC_BACKUP`, the shared suite reports one explicit skip. The
Windows portable-core cases require an existing C++14 compiler. The Swift tests
use the repository's public 1005.0 package and synthetic identities/tails; no
private full-chip backup is needed.

## Review fixes covered by regressions

- Windows cancellation after staging now stops before the first erase.
- Linux and macOS enforce the exact approved package policy before programming;
  a changed application repacked with valid CRCs remains read-only.
- Complete backup ingestion requires both independent images and actual SFDP
  evidence; a parsed JSON manifest alone cannot authorize restoration.
- New recovery directories and their parent entries are synchronized before ISP
  or mutation. Unresolved markers survive input/route changes.
- Linux backup/verify failures withhold ISP exit and retain their marker.
- macOS distinguishes schema-valid ambiguous evidence from hardware eligibility,
  recognizes the known generic serial, preserves rollback/reset restrictions and
  allows bounded post-exit DDC settling without accepting a different identity.

The Windows build record, toolchains and executable hashes are documented in
[its validation report](../windows/app/docs/VALIDATION.md). Current platform
limitations and operating instructions are in each application's README.
