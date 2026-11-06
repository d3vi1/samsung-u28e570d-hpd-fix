# Building the Windows 8 XAML frontend without a system installation

A Release Win32 build and unsigned Appx package were produced on Windows 10 using the original Microsoft v110 compiler and Windows 8 SDK, extracted into a task-local directory. No setup program, MSI installation/custom action, registry/GAC registration, UAC prompt, package deployment or application launch was used. This demonstrates this particular build route; it does not establish runtime compatibility with Windows 8 or AppContainer/broker integration.

## Official source and recorded provenance

The Microsoft CDN still served [Visual Studio Express 2012 for Windows 8](https://download.microsoft.com/download/6/e/c/6ec5b3cf-cc0d-448a-9846-8af059de7f72/vs2012_winexp_enu.iso) when checked on 2026-11-06:

- File: `vs2012_winexp_enu.iso`, 422,924,288 bytes.
- Observed SHA256: `4f81953edc092b1ee8576f992723de953f429795a926476231560175bb1b8a8d`.
- The compiler, library, Win8 Express core and SDK MSI packages had valid Microsoft Corporation Authenticode signatures. The extracted `cl.exe` and XAML task DLL also had valid Microsoft signatures.
- Compiler: `17.00.50727.1`; XAML/Appx tasks: `11.0.50727.1`; MakePRI: `6.2.9200.16384`.

The digest identifies the downloaded artifact, rather than a separately published Microsoft checksum. Microsoft's [C++ team announcement](https://devblogs.microsoft.com/cppblog/directx-graphics-development-with-visual-studio-2012/) confirms that Visual Studio 2012 includes the Windows 8 SDK. The SDK by itself does not supply the compiler in this ISO: compiler payloads are in `vc_compilercore`.

No Microsoft compiler/SDK payload is checked into this repository or included in the application source archive. Use the official source and the applicable Microsoft license. The public helpers consume an already extracted package tree; they do not download or install software.

## Data-only extraction

Use a Windows host with Python's standard `_msi` module, the built-in `expand.exe`, and a tool that reads the ISO as data. The validated host used Python 3.12 and Windows `tar.exe` for ISO extraction. `expand.exe -R` was necessary for CABs with a single file; Windows tar could not read these signed CABs.

After downloading the ISO from Microsoft, verify its size/hash and inspect its signed MSI files. Extract the ISO into an otherwise empty workspace directory. Do not run `win8express_full.exe`, `msiexec`, the preparation/finalizer programs or the generated deployment scripts.

From `windows/app`, where the ISO contents are already under `work/iso`:

```powershell
python .\winrt\Extract-LegacyToolchain.py `
  --packages-root .\work\iso\packages `
  --output-root .\work\toolchain `
  --cache-root .\work\cab-cache
```

The extractor opens MSI databases read-only and resolves `File`, `Component`, `Directory` and `Media` tables. It expands CAB data and copies files into the selected output root; it does not run install sequences or custom actions. File names, destination containment, sizes and existing-file equality are checked. Its default package list is exactly:

1. `vc_compilercore/vc_compilercore.msi`
2. `vc_compilercoreres/vc_compilercoreres.msi`
3. `vc_librarycore/vc_librarycore.msi`
4. `Windows_SDK/Windows Software Development Kit for Metro style Apps-x86_en-us.msi`
5. `win8express/core/win8express_coremsi.msi`
6. `vcRuntimeMinimum_x86/vc_runtimeMinimum_x86.msi`

The resulting layout has `Program Files/Microsoft Visual Studio 11.0`, `Program Files/MSBuild`, `Program Files/Microsoft SDKs` and `Windows Kits/8.0` directly below the output root. Keep the root path reasonably short for the legacy tools. The extractor may materialize long unrelated IDE data paths using Windows extended path syntax, but the build uses the ordinary shorter compiler/SDK paths. A cache belongs to the same verified source ISO; use a fresh cache when changing the source. Cache reuse checks extracted files, not a cryptographic binding of the cached content to its CAB.

An idempotent extraction check covered all six MSI packages and 4,470 file references: zero files were created or rewritten, and 8,963 output/cache files retained identical sizes, modification times and SHA256 hashes. Fourteen additional synthetic checks covered unsafe names, conflicting outputs and preservation of a pre-existing temporary file.

## Compile and optionally create an unsigned package

```powershell
.\winrt\Build-Relocated.ps1 `
  -ToolchainRoot .\work\toolchain `
  -OutputDirectory .\work\xaml-build `
  -CreateUnsignedPackage
```

Omit `-CreateUnsignedPackage` for compilation/linking only. The helper runs the existing x86 .NET Framework MSBuild, sets process-local compiler/SDK paths, and restores the environment afterward. It emits a local `UsingTask` override through `ForceImportBeforeCppTargets`; it does not edit Microsoft's targets or install assemblies into the GAC. XAML/Appx tasks already load their assemblies by file path. The [MSBuild UsingTask documentation](https://learn.microsoft.com/en-us/visualstudio/msbuild/usingtask-element-msbuild) describes the first-registration behavior used here.

Microsoft documents a [relocatable build environment for desktop applications](https://learn.microsoft.com/en-us/visualstudio/ide/walkthrough-creating-a-multiple-computer-build-environment), but explicitly excludes Store/UWP applications from that walkthrough's supported result. The successful XAML build recorded here is empirical evidence for this source/toolset/host combination, not a claim that Microsoft supports a general portable Store build installation.

The real build exposed two source/project defects that static XML checks missed: chained C++/CX property assignments (setters return void), and absent `DependentUpon` metadata on XAML header items. Both were corrected. Generated XAML files now stay below the intermediate output directory. The linker uses `/PDBALTPATH:MonitorFirmwareFrontend.pdb` so the executable's debug record contains only a filename, not the local build path. PDB files are not included in the development archive.

## What this build proves

The final frontend with complete backup-bundle copying compiled, linked and packaged successfully. The unsigned Appx passed a ZIP CRC/content inspection and contains the executable, WinMD, XAML, all four image assets, resource index and manifest. No signature or certificate was added. The generated manifest declares x86, the original Windows 8 platform floor and the Microsoft VCLibs 11 dependency; these declarations are not runtime test results.

Signing/deployment, launching the frontend, file-picker behavior, protocol activation and the end-to-end AppContainer/desktop-broker flow remain untested. No generated `Add-AppDevPackage.ps1` script was run. The separate desktop engine was tested with mocks; the XAML frontend was built, not exercised against either a mock broker or a monitor.
