# Validation record

Recorded on 2026-11-06, Windows 10 build 19045. All executed application tests used generated mock data. No NVAPI enumeration/open, monitor I2C/DDC, ISP, firmware programming, display API probe, UAC, protocol registration or Appx deployment was performed for this implementation.

## Compiled and executed

The portable w64devkit 2.11.0 x86 toolchain supplied GCC 16.2.0, CMake 4.4.4 and Ninja 1.13.2. Configuration: C++14, `SAMSUNG_TARGET_XP=ON`, `SAMSUNG_BUILD_GUI=ON`, `SAMSUNG_BUILD_TESTS=ON`, `SAMSUNG_ENABLE_HARDWARE=OFF`. All seven CTest targets passed:

| Target | Result |
| --- | --- |
| `core_tests` | 47 cases: dual reads, mismatches, identity/status constraints, immutable plans, sector verification/retry, rollback, reset gates, staging cancellation, persistence/progress failures, required manifest fields and complete evidence bundles |
| `transport_tests` | 33 cases: scripted I2C/MStar packets, EDID parsing, short reads, cleanup, capability limits and lazy provider registry; NVAPI and live stale-data detection are not exercised |
| `broker_tests` | 15 request/URI protocol cases |
| `package_conformance` | 17 synthetic parser/DEFLATE/checksum cases |
| `storage_tests` | 20 cases: durable no-overwrite artifacts, paths, UTF-8, bounded input, recovery identity across ports and evidence loading with missing/corrupt files, concurrent writers, hard links and a temporary junction fixture |
| `ui_mock_smoke` | Six operations: download, verify, package upload, verify, own-SPI restore and verify |
| `broker_mock_smoke` | 14 cases: six valid operations including evidenced restore/verify, eight rejected incomplete/corrupt bundles, plus 14 rejected replays; no registry access |

Four private local firmware packages (1002.0, 1003.1, 1004.5 and experimental 1005.0) were also inspected offline. The C++ parser matched the existing Python validator on all 12 compared fields for each package. These binaries and private comparison reports are not included in the source publication.

The XAML static suite passed 25 checks under Windows PowerShell 5.1 and PowerShell 7. These cover file references, XML parsing, handler names, protocol fields, bundle-copy ordering and asset dimensions. The static runner itself does not compile C++/CX or exercise the frontend.

Separately, `winrt/Build-Relocated.ps1` compiled and linked the final frontend and created an unsigned x86 Appx, using Microsoft cl 17.00.50727.1, XAML/Appx tasks 11.0.50727.1 and SDK 8.0 extracted from the official Express 2012 ISO. The Appx passed ZIP CRC/content inspection. See [legacy build provenance and reproduction](LEGACY-BUILD.md). The package was not signed, deployed or run; no generated installation script was executed.

## Compile-only and import inspection

The separate x86 `SAMSUNG_ENABLE_HARDWARE=ON` application and broker compiled successfully. Neither executable was launched. The application/broker PE imports in both build configurations were inspected: subsystem 5.1, MSVCRT/classic Win32 dependencies and no flagged modern static APIs. This is not proof of XP runtime or NVIDIA driver compatibility.

| x86 executable | SHA256 |
| --- | --- |
| Mock application | `74bfdabf86ca65b29a0f07677a5d685c89e0e3d131f7ddaf027d1431d08daa03` |
| Mock broker | `a239b8d02a44c1249b552c8884c64ca744635c276c38fdc24bb98e770baae301` |
| Hardware-enabled application, compile-only | `452374802b14ea89cfc1b0427b4dc8289fd7be510c304d23a5dfb0b369f9e1c9` |
| Hardware-enabled broker, compile-only | `12f4c7f1e88bf95b5a4ca136ddccb806075b828cc43b45b0f12160e5b69dd979` |
| WinRT/XAML executable, build-only | `4ff7fa3f6fbf59a2af74077c3cc7ccf81759c69ee56544d24bda321fbd3cf359` |
| WinRT/XAML unsigned Appx, build-only | `4fa0bb863b44aeb9d37732317b15deaf450038329d4643192b561c6b722f3230` |

Hashes identify these particular development binaries; reproducible builds have not been established. The development archive carries the MinGW-w64 runtime notices, GPLv3, GCC Runtime Library Exception and libstdc++ header notices. It includes the unsigned frontend Appx but not its separately licensed Microsoft VCLibs 11 framework dependency; no compiler, SDK, driver DLL, PDB or hardware-enabled executable is bundled.

## Remaining qualification

- The WinRT/XAML project is now compiled and packaged, but has not been signed, deployed or run. Its pickers, desktop URI and AppContainer file flow need an actual integration test.
- Execution on XP, Windows 7 and Windows 8/8.1 was not tested. The x86 build targeting XP was tested on Windows 10 only. No x64 build or runtime test was performed: the optional x64 toolchain acquisition was blocked before execution, so the x64 ABI assertion remains unexercised.
- No GPU, connector, monitor or flash profile was tested with this C++ implementation. Historical results from other tools do not transfer automatically.
- Intel/AMD providers and coding EEPROM access are unavailable.
- No claim is made that this application changes standby behavior, fixes HPD, or makes unqualified firmware safe to flash.
