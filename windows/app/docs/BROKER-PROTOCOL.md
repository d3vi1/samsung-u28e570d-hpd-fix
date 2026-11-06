# Desktop broker protocol v1

The Windows 8 XAML frontend sends bounded file requests to a separate desktop process. The broker links the same transaction engine as the Win32 application. URI activation is an explicit user action, not an operation completion signal. This transport has passed independent mock tests; actual Windows 8 AppContainer integration remains untested.

## Installation boundary

The package identity is `MonitorFirmware.WinRT`, with publisher `CN=MonitorFirmware Development`. Its exact installed PackageFamilyName is registered for the current user under `HKCU\Software\MonitorFirmware\WinRTBroker\PackageFamilyName`. The value must match `MonitorFirmware.WinRT_` followed by 13 lowercase alphanumeric characters. The broker derives LocalState from the current user's LocalAppData and this family; JSON never supplies a path.

`Register-WinRTBroker.ps1` requires an already deployed exact package and an existing broker EXE. It supports `-WhatIf`, refuses to replace an existing protocol and writes only current-user registration. Its handler command quotes both the executable and `%1`. It neither installs an Appx/certificate nor requests elevation. The script was prepared but not executed during validation.

The only accepted production activation is:

```text
samsung_monitor_broker.exe --protocol monitorfw-bridge://request/<lowercase-UUID>
```

The broker accepts the canonical lowercase hexadecimal UUID shape; the frontend generates UUID v4. Query strings, fragments, extra segments and encoded paths are rejected.

## Request and fixed files

All files belong to `LocalState/requests/<UUID>/`. The frontend copies selected data to `input.bin` and, when needed, `backup-evidence.json`, using fail-if-exists semantics. It hashes the copied input and writes `request.json` last, by renaming a pending file. The broker reads inputs from the same validated handle, rejecting reparse points and concurrent write/delete sharing. It holds path components against rename and rejects reparse directories.

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

Empty unused strings are allowed for enumeration/download. Unknown providers are rejected by the registry. JSON parsing is bounded and rejects duplicate fields. Inputs are bounded to 2 MiB. The frontend limits backup evidence to 64 KiB; the broker independently accepts at most 1 MiB. A persistent `claimed.lock`, created exclusively before parsing, consumes the request even if parsing or the operation fails. Reusing its UUID cannot rerun it.

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

`--self-test <fresh-TEMP-directory>` bypasses registration and AppContainer lookup, generates synthetic input, and runs mock enumerate/download/upload/verify requests plus four replay rejections. It cannot construct a hardware request. `tests/Run-BrokerSmoke.ps1` checks its artifacts/results and removes only its verified fresh TEMP directory. The standalone frontend test `winrt/Test-WinRtStatic.ps1` does not launch the broker.
