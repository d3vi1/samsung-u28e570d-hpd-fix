# Windows 8 XAML frontend

`winrt/MonitorFirmwareFrontend.sln` is a separate C++/CX Windows Store application targeting Visual Studio 2012 (`v110`) and Windows SDK 8.0, with Win32 and x64 configurations. It uses Windows.UI.Xaml, not WinUI 3 or XAML Islands. The C++14 firmware engine remains in the desktop broker; no engine ABI, NVAPI library or native display API is linked into the AppContainer application.

**Status: x86 XAML generation, compilation, linking and unsigned APPX packaging passed on Windows 10.** The build used Microsoft Visual Studio 2012 Express (`v110`) and Windows SDK 8.0 payloads extracted from the official Microsoft ISO into the workspace. No installer, elevation, registry change or GAC registration was used. The application has not been installed or run; deployment on Windows 8, UI behavior and the AppContainer-to-desktop round trip remain unvalidated. The x64 project configuration has not been built. The manifest's required `OSMaxVersionTested` field is a target declaration, not a statement of completed runtime testing. See [the relocated legacy build procedure](LEGACY-BUILD.md).

## User flow

The initial mode is **Mock**, with NVIDIA selected as the simulated provider. Intel and AMD appear as unavailable providers. Provider metadata comes from the broker response; it does not authorize access by itself. Hardware mode requires explicit selection and a desktop broker built with hardware access enabled.

1. Select a mode and click **Refresh endpoints**. The frontend submits an enumeration request and opens the desktop bridge. Return to the frontend, click **Refresh result / progress**, and select a returned endpoint deliberately.
2. **Download backup** asks the broker to perform its complete double-read backup. **Browse firmware** selects a `.bin`. For an own SPI dump, **Browse backup bundle folder** selects the folder containing `backup.json`, both referenced SPI read files and the referenced SFDP capture. Select the actual artifact folder when the exported bundle contains an outer directory. The image-kind selector distinguishes a Samsung package from a full SPI dump.
3. **Upload firmware** and **Verify image** copy those inputs into a new request and check their lengths and hashes. A manifest alone is insufficient for an own-dump request. Upload requires the separate desktop plan confirmation, including in mock mode. No request field bypasses that confirmation or the core policy.
4. Refresh manually to read a progress snapshot or the final response. Launch success, a progress counter and return to the app are never treated as operation completion. The frontend does not poll, cancel hardware work or automatically retry requests.
5. After a final response, **Export artifacts** uses FolderPicker to copy `artifacts/` and `response.json` into a new `monitorfw-<request-id>` directory. Existing files are not overwritten. If enumeration or an early failure produced no artifact directory, the final response can still be exported. An interrupted/failed export may leave a partial export directory; choose another destination to retry.

The last request identity and context are retained in LocalSettings before URI activation. If Windows suspends or terminates the app while the desktop window is active, reopening the app lets the user refresh that request. File-picker selections are not retained across termination; select inputs again for a new request. A pending request keeps new operations disabled until a matching final response is read. A missing response is not permission to assume that the monitor is idle. This frontend does not provide recovery, abandonment or forced-reset controls.

## File protocol

The package identity is `Name="MonitorFirmware.WinRT"`, `Publisher="CN=MonitorFirmware Development"`. After deployment, the desktop bridge must be configured for the actual PackageFamilyName. Registration is separate and explicit; building or opening this project does not register a protocol, request elevation or launch a broker.

Each request uses a fresh lowercase UUID v4 under the application's `LocalFolder/requests/<uuid>/`. The selected image is copied to `input.bin` with fail-if-exists semantics and is limited to 2 MiB.

For an own SPI dump, FolderPicker grants access to the selected bundle folder and its files. The frontend copies `backup.json` to `backup-evidence.json` at the request root, with a 64 KiB manifest limit. It then copies the files named by `reads[0].file`, `reads[1].file` and `flash.sfdp_file` into `request/evidence/`. The two read files must each be exactly 2 MiB; SFDP must be exactly 64 bytes. All three names must be distinct ignoring case, 1–255 ASCII characters, start with an alphanumeric character and otherwise contain only alphanumerics, `.`, `_` or `-`. Paths, trailing dots and reserved Windows device names are rejected. Selecting a manifest through a single-file picker would not grant access to its siblings, so this flow requires the folder picker.

The frontend checks SHA256 from each copied evidence file against the copied manifest, requires matching read hashes, and checks the copied input against that read hash. Only after all copies and checks succeed is `request.pending` written and renamed to `request.json`; the app then calls `Launcher.LaunchUriAsync` with `monitorfw-bridge://request/<uuid>` on the UI thread. A failed preparation leaves no published request or automatic broker launch. The broker independently reloads and fully validates the manifest, both read files and SFDP, including actual byte equality, monitor binding, flash status/profile and firmware policy. Frontend checks do not replace backend validation.

The request object has these fields, with no filesystem paths supplied in JSON:

```json
{
  "schema": "monitorfw.request.v1",
  "request_id": "00000000-0000-4000-8000-000000000000",
  "created_unix": 0,
  "action": "enumerate",
  "mode": "mock",
  "provider_id": "nvidia",
  "endpoint_locator": "",
  "expected_edid_sha256": "",
  "image_kind": "samsung_package",
  "firmware_sha256": ""
}
```

The zero timestamp and illustrative UUID above are documentation placeholders. The frontend creates real values. Actions are `enumerate`, `download`, `upload` and `verify`; kinds are `samsung_package` and `spi_full`. Enumeration and download carry no input image/hash. The broker independently validates the request, copied bytes, endpoint identity and applicable firmware policy. Local files and the URI are an IPC mechanism, not proof of trusted input or authorization.

The final file is `response.json`, with schema `monitorfw.response.v1`, matching `request_id`, echoed `mode` and `provider_id`, `success`, `error`, provider/endpoint arrays, a `result` object or null, and the literal `artifacts_directory: "artifacts"`. The UI explicitly labels mock results as simulated and hardware results as hardware. A broker rejection before parsing can use mode `unknown` and an empty provider. Optional `progress.json` contains `phase`, `done` and `total`; zero total means indeterminate progress. JSON reads are bounded to 1 MiB. Export is limited to eight directory levels, 1,024 entries and 64 MiB. Only a matching, parseable final response releases the pending state after successful dispatch. The final response remains visible as JSON so the detailed broker outcome can be reviewed.

## Building and validation

Use Visual Studio 2012 with the Windows 8 SDK and the C++ Windows Store tools, or follow [LEGACY-BUILD.md](LEGACY-BUILD.md) for the demonstrated workspace-local toolchain. The successful build used `winrt/Build-Relocated.ps1` with `-CreateUnsignedPackage`, targeting Win32. The project references Microsoft.VCLibs 11.0 and disables automatic package signing. Deployment requires the developer to configure appropriate package signing and Windows deployment prerequisites separately; no certificate or signing key is included. The desktop CMake build does not compile this separate C++/CX frontend. ARM/Windows RT desktop-broker operation is not supported by this project.

The validated build artifacts have these SHA256 values:

| Artifact | SHA256 |
| --- | --- |
| `MonitorFirmwareFrontend.exe` | `4ff7fa3f6fbf59a2af74077c3cc7ccf81759c69ee56544d24bda321fbd3cf359` |
| Unsigned Win32 APPX | `4fa0bb863b44aeb9d37732317b15deaf450038329d4643192b561c6b722f3230` |

Run the static checks without launching the app or broker:

```powershell
powershell -NoProfile -File .\winrt\Test-WinRtStatic.ps1
```

The 25 static checks passed in Windows PowerShell 5.1 and PowerShell 7. They parse project/manifest/XAML XML, resolve declared source/asset files, compare XAML event handlers with source declarations, and check the package identity, mock default, protocol fields, bundle-copy ordering, bounds, no-overwrite calls and PNG dimensions. The static script does not compile or launch anything; the successful XAML/compiler/linker/packager run is separate evidence. No validation performed deployment, URI registration, a frontend-to-broker runtime round trip or monitor access. Mock end-to-end testing from an installed frontend, including on Windows 8, remains required.

## API references and scope

The frontend uses the original Windows Runtime storage, JSON, cryptography, XAML and URI-launch APIs. Microsoft's Windows 8-era material documents [app data folders and file/folder pickers](https://download.microsoft.com/download/6/6/5/665AF7A6-2184-45DC-B9DA-C89185B01937/Microsoft_Press_eBook_Programming_Windows_8_Apps_HTML_CSS_JavaScript_2E_PDF.pdf), [Windows.Data.Json](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/may/json-parse-json-strings-in-windows-runtime-components) and [C++ Windows Store URI activation and possible suspension](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/september/c-using-the-c-rest-sdk-in-windows-store-apps). Current API pages describe [Launcher](https://learn.microsoft.com/en-us/uwp/api/windows.system.launcher), [HashAlgorithmProvider](https://learn.microsoft.com/en-us/uwp/api/windows.security.cryptography.core.hashalgorithmprovider) and [StorageFile](https://learn.microsoft.com/en-us/uwp/api/windows.storage.storagefile); their modern UWP contract tables are not by themselves proof of Windows 8 compatibility.

This implementation does not depend on App Services, FullTrustProcessLauncher, LaunchUriForResults, brokered WinRT components, loopback exemptions or Windows 8.1-only IPC features. The custom-protocol desktop bridge is a separately installed component; its launch, package-directory access, file sharing and result flow still require an actual Windows 8 integration test. The main flash backup does not establish a backup of separate EEPROM, OTP/security storage or every calibration value. The frontend exposes no EEPROM coding or arbitrary chip-restore operation.
