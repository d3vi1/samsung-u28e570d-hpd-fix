# Before/after display topology snapshots

`Capture-DisplayTopology.ps1` is a separate, one-shot Windows topology query. Run it only **before standby or after deliberate wake, while the displays are active**. It must not run during the observation interval. It is not part of the passive notification listener and performs no polling.

From the repository root, with displays active:

```powershell
pwsh -NoProfile -File .\windows\standby\Capture-DisplayTopology.ps1 `
  -Phase BeforeStandby -DisplaysAreActive `
  -OutputPath '.\windows\standby\runs\test-01\topology-before.json'

# Run only after the planned standby interval and deliberate wake:
pwsh -NoProfile -File .\windows\standby\Capture-DisplayTopology.ps1 `
  -Phase AfterWake -DisplaysAreActive `
  -OutputPath '.\windows\standby\runs\test-01\topology-after.json'
```

If the listener will use the same run directory, start it first: its Start action requires a directory that does not yet exist. Topology snapshots may then be added while displays remain active.

The switch records an operator assertion, not an automatic measurement of display power. Without it the helper rejects the request before calling native display APIs. `-WhatIf` also returns before the query. Existing output files are never overwritten. Use a 64-bit PowerShell 7 or Windows PowerShell 5.1 process.

The helper uses `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` and `DisplayConfigGetDeviceInfo` source, target and adapter-name requests. It retrieves OS/driver topology information; it does not issue DDC, NVAPI, EDID bus reads, `SetDisplayConfig`, display-off, power-policy or execution-state commands. These OS queries are still excluded from standby to avoid disturbing the measurement. They are not guaranteed to be a physical monitor-health probe or a cache-only operation in every graphics driver.

Each path records source/target IDs and adapter LUIDs, the source GDI name, target friendly name, connector technology, availability, path flags, source desktop dimensions/position and target signal dimensions/refresh. The target's EDID manufacturer/product IDs are reported only when the API marks them valid. The names and connector can distinguish an external HDMI path from an integrated panel when the driver provides those details; they do not authenticate a monitor serial number.

`interfaceNameSha256` hashes the monitor interface path using the same case-sensitive UTF-8 SHA256 algorithm as the passive `device_change` logger. Raw monitor/adapter interface paths are not written. Matching hashes support correlation between a topology path and a device-interface notification. A mismatch is not proof of a different monitor: casing or interface-path changes can prevent matching. Global power/display-change events do not carry a per-monitor interface identity.

Compare paths by interface hash and adapter/target IDs, then inspect names, connector, availability and dimensions. LUIDs and interface identities need not remain stable across reboot/re-enumeration. The path/mode arrays come from one query; device-name requests happen afterwards, so the snapshot is not atomic across all metadata. UTC start/query/end timestamps bound that interval. Query errors are surfaced; partial device-name failures and invalid mode references are retained as warnings instead of fabricated names or resolutions. Remote-session access can be denied by Windows.

Only active paths are requested. Disconnected/inactive targets are outside this snapshot. If topology changes between buffer sizing and retrieval, the helper retries that sizing race at most three times, with bounded arrays; it does not start a sampling loop. An unexpected virtual-mode path is retained with a warning and its mode union is not decoded as an ordinary index.

## Offline verification

```powershell
pwsh -NoProfile -File .\windows\standby\Test-DisplayTopology.ps1
powershell.exe -NoProfile -File .\windows\standby\Test-DisplayTopology.ps1
```

All 20 checks passed under PowerShell 7 and Windows PowerShell 5.1. These tests compile the helper, check native x64 sizes/offsets and decode synthetic data. They do not call `QueryDisplayConfig`, `DisplayConfigGetDeviceInfo`, the capture entry point with an enabled request, or any display P/Invoke. Hash compatibility with the notification logger was also checked using a synthetic interface path.

These checks do not validate standby behavior. No HDMI standby interval or before/after standby comparison was completed; see [Preparation results](PREPARATION-RESULT.md).

## Primary API references

- [QueryDisplayConfig](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig) and [buffer sizing](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdisplayconfigbuffersizes): active-path scope, non-atomic sizing and buffer retry.
- [DisplayConfigGetDeviceInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-displayconfiggetdeviceinfo), [source name](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_source_device_name) and [target name](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_target_device_name): name requests and returned fields.
- [Path layout](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_info), [mode union](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_mode_info), [signal layout](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info) and [LUID](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-luid): ABI definitions.
- [Target-name flags](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_target_device_name_flags) and [connector technologies](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ne-wingdi-displayconfig_video_output_technology): validity and interpretation.
