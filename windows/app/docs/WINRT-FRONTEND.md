# Windows 8 XAML frontend

`winrt/MonitorFirmwareFrontend.sln` is a separate C++/CX Windows Store application targeting Visual Studio 2012 (`v110`) and Windows SDK 8.0, with Win32 and x64 configurations. It uses Windows.UI.Xaml, not WinUI 3 or XAML Islands. The C++14 firmware engine remains in the desktop broker; no engine ABI, NVAPI library or native display API is linked into the AppContainer application.

**Status: source prepared and statically checked only.** The available environment does not have the v110 compiler, Windows 8 SDK or XAML compiler. The project has not been compiled, deployed or run, and compatibility with any specific Windows 8 or later installation has not been demonstrated. XML well-formedness and the checks below are not SDK schema validation. The manifest's required `OSMaxVersionTested` field is a target declaration, not a statement of completed runtime testing.

## User flow

The initial mode is **Mock**, with NVIDIA selected as the simulated provider. Intel and AMD appear as unavailable providers. Provider metadata comes from the broker response; it does not authorize access by itself. Hardware mode requires explicit selection and a desktop broker built with hardware access enabled.

1. Select a mode and click **Refresh endpoints**. The frontend submits an enumeration request and opens the desktop bridge. Return to the frontend, click **Refresh result / progress**, and select a returned endpoint deliberately.
2. **Download backup** asks the broker to perform its complete double-read backup. **Browse firmware** selects a `.bin`; **Browse backup manifest** selects the evidence needed for an own SPI dump. The image-kind selector distinguishes a Samsung package from a full SPI dump.
3. **Upload firmware** and **Verify image** copy those inputs into a new request. Upload requires the separate desktop plan confirmation, including in mock mode. No request field bypasses that confirmation or the core policy.
4. Refresh manually to read a progress snapshot or the final response. Launch success, a progress counter and return to the app are never treated as operation completion. The frontend does not poll, cancel hardware work or automatically retry requests.
5. After a final response, **Export artifacts** uses FolderPicker to copy `artifacts/` and `response.json` into a new `monitorfw-<request-id>` directory. Existing files are not overwritten. If enumeration or an early failure produced no artifact directory, the final response can still be exported. An interrupted/failed export may leave a partial export directory; choose another destination to retry.

The last request identity and context are retained in LocalSettings before URI activation. If Windows suspends or terminates the app while the desktop window is active, reopening the app lets the user refresh that request. File-picker selections are not retained across termination; select inputs again for a new request. A pending request keeps new operations disabled until a matching final response is read. A missing response is not permission to assume that the monitor is idle. This frontend does not provide recovery, abandonment or forced-reset controls.

## File protocol

The package identity is `Name="MonitorFirmware.WinRT"`, `Publisher="CN=MonitorFirmware Development"`. After deployment, the desktop bridge must be configured for the actual PackageFamilyName. Registration is separate and explicit; building or opening this project does not register a protocol, request elevation or launch a broker.

Each request uses a fresh lowercase UUID v4 under the application's `LocalFolder/requests/<uuid>/`. Picker-selected files are copied with fail-if-exists semantics to `input.bin` and, for own-dump operations, `backup-evidence.json`. SHA256 is calculated from the copied input. Inputs are limited to 2 MiB and backup evidence to 64 KiB. Only after the copies and hash are complete is `request.pending` written and renamed to `request.json`; the app then calls `Launcher.LaunchUriAsync` with `monitorfw-bridge://request/<uuid>` on the UI thread.

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

Use Visual Studio 2012 with the Windows 8 SDK and the C++ Windows Store tools. Open the solution and build a Win32 or x64 configuration. The project references Microsoft.VCLibs 11.0 and disables automatic package signing. Deployment requires the developer to configure appropriate package signing and Windows deployment prerequisites separately; no certificate or signing key is included. The desktop CMake build does not compile this frontend. ARM/Windows RT desktop-broker operation is not supported by this project.

Run the static checks without launching the app or broker:

```powershell
powershell -NoProfile -File .\winrt\Test-WinRtStatic.ps1
```

The checks parse project/manifest/XAML XML, resolve declared source/asset files, compare XAML event handlers with source declarations, check the public package identity, mock default, protocol fields, size bounds, no-overwrite calls and PNG dimensions. They do not compile C++/CX, validate XAML types, check SDK schemas, register a URI, install an Appx, exercise UI behavior or access a monitor. Build and mock end-to-end tests with the actual SDK remain required before calling this frontend runnable.

## API references and scope

The frontend uses the original Windows Runtime storage, JSON, cryptography, XAML and URI-launch APIs. Microsoft's Windows 8-era material documents [app data folders and file/folder pickers](https://download.microsoft.com/download/6/6/5/665AF7A6-2184-45DC-B9DA-C89185B01937/Microsoft_Press_eBook_Programming_Windows_8_Apps_HTML_CSS_JavaScript_2E_PDF.pdf), [Windows.Data.Json](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/may/json-parse-json-strings-in-windows-runtime-components) and [C++ Windows Store URI activation and possible suspension](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/september/c-using-the-c-rest-sdk-in-windows-store-apps). Current API pages describe [Launcher](https://learn.microsoft.com/en-us/uwp/api/windows.system.launcher), [HashAlgorithmProvider](https://learn.microsoft.com/en-us/uwp/api/windows.security.cryptography.core.hashalgorithmprovider) and [StorageFile](https://learn.microsoft.com/en-us/uwp/api/windows.storage.storagefile); their modern UWP contract tables are not by themselves proof of Windows 8 compatibility.

This implementation does not depend on App Services, FullTrustProcessLauncher, LaunchUriForResults, brokered WinRT components, loopback exemptions or Windows 8.1-only IPC features. The custom-protocol desktop bridge is a separately installed component; its launch, package-directory access, file sharing and result flow still require an actual Windows 8 integration test. The main flash backup does not establish a backup of separate EEPROM, OTP/security storage or every calibration value. The frontend exposes no EEPROM coding or arbitrary chip-restore operation.
