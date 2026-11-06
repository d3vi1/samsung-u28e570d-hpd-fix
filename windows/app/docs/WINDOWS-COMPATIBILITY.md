# Windows compatibility

The legacy frontend uses a Unicode Win32 desktop window and the operating system's native common controls, fonts and colors. It is intended for XP through Windows 7; it can also be developed and tested on newer Windows. A separate WinRT/XAML frontend is required for the requested Windows 8+ interface. The Win32 build is not a substitute for that frontend. A common-controls v6 manifest selects the installed visual style. The legacy frontend has no browser, .NET, UWP or Windows App SDK dependency. Long operations run on a Win32 worker thread; UI updates are posted to the main window.

The development build starts in **MOCK / OFFLINE** mode. `SAMSUNG_ENABLE_HARDWARE` defaults to `OFF`; NVAPI transport source is compiled and tested with scripted I2C mocks, but the GUI cannot enumerate or open real devices. Enabling that option is a build decision, not evidence of compatibility or hardware validation.

The provider selector reads registry metadata only. NVIDIA is implemented; Intel and AMD are listed explicitly as unavailable. No provider fallback or automatic probe occurs when the selection changes. In a hardware-enabled build, only explicit Refresh or an operation on the selected endpoint can enumerate/open a provider. The UI's full model string names the reviewed firmware profile; EDID alone does not prove its regional SKU suffix.

| Target | Build approach | Validation status |
| --- | --- | --- |
| Windows XP SP3 x86 | Compatible x86 compiler/runtime; `SAMSUNG_TARGET_XP=ON`; XP API declarations and subsystem 5.1 | Target configuration only; not executed on XP |
| Windows 7 | Native Win32 shell, compatible compiler/runtime and NVIDIA driver | Not executed on that system |
| Windows 8 / 8.1 | Separate WinRT/XAML frontend and its broker; legacy Win32 build remains useful for development | This document does not claim that frontend has been built or validated |
| Current Windows host | Portable x86 development build and offline mock tests | Recorded separately in build/test results |

Defining `_WIN32_WINNT` and changing the PE subsystem version do not by themselves make a binary compatible with XP. Compiler runtimes, imported APIs, dynamically loaded dependencies and the driver must also support the target. The XP profile rejects a non-x86 configuration and an MSVC toolset without an `_xp` suffix. Newer Win32 APIs must not become unconditional imports in that profile.

## Toolchains

The local development build uses [w64devkit 2.11.0](https://github.com/skeeto/w64devkit/releases/tag/v2.11.0), x86 archive `w64devkit-x86-2.11.0.7z.exe`, SHA-256 `f7e8c79eacc4cbdd718a4cbb6519f2f370311017dc08f7750abedb4340d1b65d`. The digest was compared with the asset digest from the primary GitHub release API before task-local extraction. No global installation or elevation is required. The kit contains GCC 16.2.0, CMake 4.4.4 and Ninja 1.13.2.

The [upstream compatibility statement](https://github.com/skeeto/w64devkit#operating-system-support) says the x86 kit targets XP with an SSE2-capable CPU and uses MSVCRT with static compiler runtimes. CMake/Ninja themselves require Windows 7 or later, which affects the build host rather than the application target. Do not introduce its C11 threading API into the XP profile. Distribute the kit's `COPYING.MinGW-w64-runtime.txt` with generated binaries, as described in its licensing section.

The Microsoft alternative is [Visual Studio's v141_xp toolset](https://learn.microsoft.com/en-us/cpp/build/configuring-programs-for-windows-xp), based on the Windows 7 SDK and VS 2017 compiler. Microsoft lists XP SP3 x86 and XP SP2 x64 as minimum legacy targets. Current VC++ redistributables no longer support XP; the last compatible redistributable has a 14.27 file version. That legacy runtime/toolset combination requires its own validation.

Example build from a shell with the portable kit's `bin` directory on that process's PATH:

```powershell
cmake -S . -B build-x86 -G Ninja -DSAMSUNG_TARGET_XP=ON -DSAMSUNG_ENABLE_HARDWARE=OFF
cmake --build build-x86
ctest --test-dir build-x86 --output-on-failure
```

## Native appearance and modern frameworks

Microsoft documents [common-controls v6 visual styles from XP onward](https://learn.microsoft.com/en-us/windows/win32/sbscs/enabling-updated-common-controls-visual-styles-and-themes). Standard controls use the installed theme; accessibility and high-contrast behavior remain under the system's control.

[WinUI 3](https://learn.microsoft.com/en-us/windows/apps/winui) requires Windows 10 version 1809 or later. [UWP](https://learn.microsoft.com/en-us/windows/uwp/get-started/universal-application-platform-guide) was introduced in Windows 10. Neither is a Windows 8 UI solution. The separate WinRT/XAML frontend must state its actual SDK/OS floor and broker requirements; its validation is independent of the Win32 binary.

## Offline validation

The Win32 `--smoke-test` entry point creates a hidden window and exercises six operations against generated mock memory: backup/download, package verification, package upload with a synthetic plan, verification, own-SPI restoration with its saved manifest, and verification again. Only this explicit mock entry point approves synthetic plans automatically. Normal uploads require an owned UI confirmation dialog. Smoke artifacts remain in a fresh TEMP directory for diagnosis; they contain synthetic data.

The storage tests use only fresh TEMP directories and `mockScope=true`. They cover non-overwriting atomic publication, rejected path/device names, bounded reads, strict UTF-8 conversion, durable journals and the persistence/explicit clearing of mock recovery markers. They do not create hardware recovery markers in LocalAppData.

An initial x86 PE import inspection found subsystem 5.1 and classic Win32/MSVCRT dependencies in the legacy application and transport/storage test binaries. No static dependency on UCRT, `api-ms-win-*`, WinRT, `GetTickCount64`, SRW locks or condition variables was present. This check does not replace execution on an XP machine or validate runtime fallback paths and NVIDIA drivers.

The recorded x86 run on Windows 10 build 19045 passed all seven CTest targets: 34 core cases, 33 transport cases, 15 broker protocol cases, 17 package conformance cases, 10 storage cases, six UI mock operations, and four broker mock operations with four rejected replays. The separate hardware-enabled legacy application and broker also compiled successfully; neither executable was launched. No hardware operation, protocol registration, UAC prompt or AppContainer deployment was part of these tests.

## NVAPI and hardware support

NVIDIA's current [NVAPI requirements](https://docs.nvidia.com/nvapi/sysreq.html) and [I2C API documentation](https://docs.nvidia.com/nvapi/group__i2capi.html) specify Windows 10 and newer. Loading an API dynamically and preserving historical structure versions does not establish support on XP, 7 or 8. Those targets need appropriate historical drivers and an explicit compatibility test. A successful offline build never implies that a GPU, connector, monitor or firmware operation was tested.
