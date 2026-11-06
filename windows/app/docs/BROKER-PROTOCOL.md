# Desktop broker protocol v1

The Windows 8 XAML frontend sends bounded file requests to a separate desktop process. The broker links the same transaction engine as the Win32 application. URI activation is an explicit user action, not an operation completion signal. The desktop transport has passed independent mock tests. The separate C++/CX frontend has passed x86 XAML generation, compilation, linking and unsigned APPX packaging on Windows 10 using the official Microsoft v110 and SDK 8.0 payloads extracted into the workspace. It has not been installed or run, and Windows 8 deployment and AppContainer integration remain untested. See [LEGACY-BUILD.md](LEGACY-BUILD.md) for the build procedure and [WINRT-FRONTEND.md](WINRT-FRONTEND.md) for artifact hashes and UI scope. These validations performed no hardware access.

## Installation boundary

The package identity is `MonitorFirmware.WinRT`, with publisher `CN=MonitorFirmware Development`. Its exact installed PackageFamilyName is registered for the current user under `HKCU\Software\MonitorFirmware\WinRTBroker\PackageFamilyName`. The value must match `MonitorFirmware.WinRT_` followed by 13 lowercase alphanumeric characters. The broker derives LocalState from the current user's LocalAppData and this family; JSON never supplies a path.

`Register-WinRTBroker.ps1` requires an already deployed exact package and an existing broker EXE. It supports `-WhatIf`, refuses to replace an existing protocol and writes only current-user registration. Its handler command quotes both the executable and `%1`. It neither installs an Appx/certificate nor requests elevation. The script was prepared but not executed during validation.

The only accepted production activation is:

```text
samsung_monitor_broker.exe --protocol monitorfw-bridge://request/<lowercase-UUID>
```

The broker accepts the canonical lowercase hexadecimal UUID shape; the frontend generates UUID v4. Query strings, fragments, extra segments and encoded paths are rejected.

## Request and fixed files

All files belong to `LocalState/requests/<UUID>/`. The frontend copies the selected image to `input.bin` using fail-if-exists semantics. For an own SPI dump it uses FolderPicker to select the bundle directory containing `backup.json` and the adjacent referenced artifacts; a single-file picker does not grant sibling-file access. The manifest is copied to `backup-evidence.json` at the request root. Both files referenced by `reads[0].file` and `reads[1].file`, and the file referenced by `flash.sfdp_file`, are copied into `evidence/` under their validated leaf names.

All three leaf names must be distinct ignoring case and contain 1–255 ASCII characters matching `[A-Za-z0-9][A-Za-z0-9._-]*`, with no trailing dot or reserved Windows device name. Each SPI read must be exactly 2 MiB and SFDP exactly 64 bytes. The frontend hashes the copied files, checks the evidence hashes and the input/read-hash match, then publishes `request.json` last by renaming a pending file. Preparation failure does not dispatch a request.

The broker reads inputs from the same validated handle, rejecting reparse points and concurrent write/delete sharing. It holds path components against rename and rejects reparse directories. It independently validates the full manifest and reloads both actual read files and SFDP through the core evidence loader, checking lengths, hashes, byte equality and SFDP contents. Manifest parsing alone does not produce validated evidence. The engine additionally checks live monitor binding, flash status/profile and the applicable firmware policy before accepting an own-dump operation.

The request must have exactly these ten fields:

| Field | Requirement |
| --- | --- |
| `schema` | `monitorfw.request.v1` |
| `request_id` | Matches the activation UUID |
| `created_unix` | Integer UTC seconds; at most 900 seconds old or 60 seconds in the future |
| `action` | `enumerate`, `download`, `upload` or `verify` |
| `mode` | `mock` or `hardware` |
| `provider_id` | Registry ID; lowercase identifier up to 32 characters |
| `endpoint_locator` | Explicit selected endpoint for every action except enumeration |
| `expected_edid_sha256` | Lowercase 64-digit hash for every action except enumeration |
| `image_kind` | `samsung_package` or `spi_full` |
| `firmware_sha256` | Lowercase 64-digit hash of copied input for upload/verify |

Empty unused strings are allowed for enumeration/download. Unknown providers are rejected by the registry. JSON parsing is bounded and rejects duplicate fields. Images are bounded to 2 MiB. The frontend limits the evidence manifest to 64 KiB; the broker independently accepts at most 1 MiB. The separate binary evidence files have the exact lengths above. A persistent `claimed.lock`, created exclusively before parsing, consumes the request even if parsing or the operation fails. Reusing its UUID cannot rerun it.

Hardware mode must be compiled in and deliberately requested. It does not fall back to another provider or mode. Before upload, the desktop broker requires confirmation of the concrete engine plan; JSON has no confirmation-bypass field. The self-test-only mock confirmation path cannot select hardware.

## Progress, result and artifacts

`progress.json` is an optional atomic snapshot with `phase`, `done` and `total`; zero total is indeterminate. It cannot establish completion or permission to disconnect a monitor.

`response.json` is the atomically published final response:

```json
{
  "schema": "monitorfw.response.v1",
  "request_id": "00000000-0000-4000-8000-000000000000",
  "mode": "mock",
  "provider_id": "nvidia",
  "success": true,
  "error": "",
  "providers": [],
  "endpoints": [],
  "result": null,
  "artifacts_directory": "artifacts"
}
```

Arrays contain provider metadata and selected-provider endpoints where applicable. `result` holds the core outcome for a completed transaction; enumeration uses null. A rejection before request parsing uses mode `unknown` and an empty provider. `artifacts/` may be absent for enumeration or early errors. The XAML frontend checks the request ID and context, displays the result and exports artifacts without overwriting files. Launch success, app resumption, an absent response or an exception is never interpreted as verified completion.

After the transaction engine starts executing a programming plan, broker exception handling cannot independently call backup cleanup/reset. The engine owns restoration and the verified-reset decision. Recovery markers are keyed by stable monitor identity rather than the port-specific EDID product code; unresolved state remains blocking across HDMI/DP changes. Ambiguous identities use a conservative shared key.

## Offline test entry point

`--self-test <fresh-TEMP-directory>` bypasses registration and AppContainer lookup, generates synthetic input, and runs 14 mock cases (six valid operations and eight invalid backup bundles), followed by 14 replay rejections. It cannot construct a hardware request. `tests/Run-BrokerSmoke.ps1` checks its artifacts/results and removes only its verified fresh TEMP directory. The standalone frontend test `winrt/Test-WinRtStatic.ps1` does not launch the broker.
