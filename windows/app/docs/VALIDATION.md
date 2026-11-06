# Validation record

Recorded on 2026-11-06, Windows 10 build 19045. All executed application tests used generated mock data. No NVAPI enumeration/open, monitor I2C/DDC, ISP, firmware programming, display API probe, UAC, protocol registration or Appx deployment was performed for this implementation.

## Compiled and executed

The portable w64devkit 2.11.0 x86 toolchain supplied GCC 16.2.0, CMake 4.4.4 and Ninja 1.13.2. Configuration: C++14, `SAMSUNG_TARGET_XP=ON`, `SAMSUNG_BUILD_GUI=ON`, `SAMSUNG_BUILD_TESTS=ON`, `SAMSUNG_ENABLE_HARDWARE=OFF`. All seven CTest targets passed:

| Target | Result |
| --- | --- |
| `core_tests` | 34 cases: dual reads, mismatches, identity/status constraints, immutable plans, sector verification/retry, rollback, reset gates, cancellation and persistence/progress failures |
| `transport_tests` | 33 cases: scripted I2C/MStar packets, EDID parsing, short reads, cleanup, capability limits and lazy provider registry; NVAPI and live stale-data detection are not exercised |
| `broker_tests` | 15 request/URI protocol cases |
| `package_conformance` | 17 synthetic parser/DEFLATE/checksum cases |
| `storage_tests` | 10 cases: durable no-overwrite artifacts, paths, UTF-8, bounded input and recovery identity across ports |
| `ui_mock_smoke` | Six operations: download, verify, package upload, verify, own-SPI restore and verify |
| `broker_mock_smoke` | Four operations and four rejected replays; no registry access |

Four private local firmware packages (1002.0, 1003.1, 1004.5 and experimental 1005.0) were also inspected offline. The C++ parser matched the existing Python validator on all 12 compared fields for each package. These binaries and private comparison reports are not included in the source publication.

The XAML static suite passed 20 checks under Windows PowerShell 5.1 and PowerShell 7. These cover file references, XML parsing, handler names, protocol fields and asset dimensions. They do not compile C++/CX, validate SDK schemas or exercise the frontend.

## Compile-only and import inspection

The separate x86 `SAMSUNG_ENABLE_HARDWARE=ON` application and broker compiled successfully. Neither executable was launched. The application/broker PE imports in both build configurations were inspected: subsystem 5.1, MSVCRT/classic Win32 dependencies and no flagged modern static APIs. This is not proof of XP runtime or NVIDIA driver compatibility.

| x86 executable | SHA256 |
| --- | --- |
| Mock application | `34241591dbcd89dc29d08109ecdebfcc600834496425ceb039677886c190a13e` |
| Mock broker | `44d85c4a67a8feff1f14d00589e5c774c415bce5424015a6bafc6e0d121b072f` |
| Hardware-enabled application, compile-only | `64ae7edece9c44e888ee661fdee8848935bcb092d86f4dd5c9a0ce5f755e59b8` |
| Hardware-enabled broker, compile-only | `0be3c1c600fd3afa3a444133b087839f1f2e60ea4bac474a2bdaa69c7dac252b` |

Hashes identify these particular development binaries; reproducible builds have not been established. Distribute the MinGW-w64 runtime license with them.

## Remaining qualification

- The WinRT/XAML project has not been compiled, signed, deployed or run. Visual Studio 2012/v110 and Windows SDK 8.0 were unavailable. Its desktop URI and AppContainer file flow need an actual integration test.
- Execution on XP, Windows 7 and Windows 8/8.1 was not tested. The x86 build targeting XP was tested on Windows 10 only. No x64 build or runtime test was performed: the optional x64 toolchain acquisition was blocked before execution, so the x64 ABI assertion remains unexercised.
- No GPU, connector, monitor or flash profile was tested with this C++ implementation. Historical results from other tools do not transfer automatically.
- Intel/AMD providers and coding EEPROM access are unavailable.
- No claim is made that this application changes standby behavior, fixes HPD, or makes unqualified firmware safe to flash.
