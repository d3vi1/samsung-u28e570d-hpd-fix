# Optional passive DxgKrnl capture

This controller collects Windows display-driver events for a controlled standby investigation. A short real ETW session was started and stopped during preparation while displays remained active. No HDMI standby interval was tested; see [Preparation results](PREPARATION-RESULT.md). It does not call NVAPI, DDC, AUX, EDID reads, WMI polling, display enumeration, or power-setting APIs. Starting ETW enables diagnostic event production and incurs CPU/disk overhead; it is not an electrical probe.

Use the separate Win32 notification listener as the primary low-overhead observation. This ETW capture adds driver/OS evidence. It does not replace visual observations, the listener's UTC/QPC markers, or a physical HPD analyzer.

## Requirements

Use a 64-bit Windows process with Windows PowerShell 5.1 or PowerShell 7. The host launches its controller children in Windows PowerShell 5.1. Offline export uses the Windows `tracerpt.exe` utility and `Get-WinEvent`. The scripts use a selected DxgKrnl provider mask rather than a broad WPR profile.

The wrapper requires an elevated administrator for Start, Status, Stop and Export. Validate requires no elevation and makes no ETW API calls. Elevation through `Start-Process -Verb RunAs` displays the normal UAC consent dialog. The user must approve that dialog; there is no bypass. `-WindowStyle Hidden` hides the controller's ordinary window, not UAC.

## Controller behavior

`Capture-DxgKrnl.ps1` supports explicit `Validate`, `Start`, `Status`, `Stop`, and `Export` actions. Nothing starts automatically.

- Default session: `Samsung-Standby-DxgKrnl-standby-test`. `-TestId` changes only the suffix and must be identical for all actions on a run. Use a descriptive test ID; keep any private serial-to-test mapping separately.
- Provider: `Microsoft-Windows-DxgKrnl`, GUID `802ec45a-1e99-4b83-9920-87c98277ba9d`.
- Match-any mask: `0x10301`; match-all: zero; level: 4. In the Windows 10 provider manifest inspected during preparation, these correspond to Base, StatusChangeNotify, Power and DxgKrnl categories. Provider metadata and event availability can vary by Windows/driver version. No stack capture or explicit provider-state rundown is requested.
- QPC clock, circular ETL, default limit 256 MiB; `-MaximumMegabytes` accepts 32–512. Buffers are 64 KiB, requested minimum 16/maximum 64, flush interval five seconds. Windows may adjust buffer allocation. The direct controller bounds ETL file size but has no automatic duration limit. The host described below requests Stop after its configured duration.
- Before starting, it saves an immutable intent manifest. Only a successful `dxgkrnl-start-*.json` confirms that Start and provider enablement completed.
- Start refuses an existing manifest, ETL, or active session with this name. It never reuses or replaces a previous session.
- Status/Stop/Export verify the manifest's output path, session name and session GUID. Stop targets the verified session handle, not a wildcard or another session name.
- Each action writes a separate result JSON with UTC timestamps and errors. A failure to save its result produces a nonzero exit status; a successfully started session may still be running, so use Status with the same directory.
- Stop records final native `EventsLost`, `LogBuffersLost`, `RealTimeBuffersLost`, buffer counts, and file parameters. An already stopped session cannot supply final live loss counters; the result says so.
- Export is allowed only after the owned session is stopped. It retains the ETL, creates full XML and a tracerpt summary, and writes selected connection/power events to `dxgkrnl-key-events-utc.jsonl`. It records ETL SHA256 and the minimum/maximum observed event UTC. Existing export files are never overwritten.

## One UAC prompt through the host

Launch `Run-DxgKrnlHost.ps1` once with elevation. Its hidden Start/Stop/Export children inherit that elevation, so they do not prompt for UAC again. The host checks each child's exit code and PID-bound result JSON, then waits for a local stop-request file or a monotonic deadline of at most 20 minutes. The deadline requests cleanup; Stop and offline Export have their own bounded execution times. No power setting is changed.

The host owns each child directly through `System.Diagnostics.Process`, with `UseShellExecute=false`, `CreateNoWindow=true` and concurrent stdout/stderr reads. It does not use `Start-Process -PassThru` for those children. This fixes an observed Windows PowerShell 5.1 path where a completed child's `ExitCode` was null and its successful result was rejected. The outer `Start-Process -Verb RunAs` below is used only to request elevation for the host.

Each controller waits at a gate until the host assigns it to an anonymous, non-inheritable Windows Job Object. The job allows no breakaway and uses kill-on-close; ordinary child processes such as `tracerpt.exe` stay in the same job. On timeout the host terminates the job and checks both controller exit and zero active job processes. A controller that exits while descendants remain is rejected, even if its JSON declares success. This owns the process tree, not the persistent kernel ETW session: stopping the session still requires the separate manifest-checked Stop action. The encoded bootstrap preserves paths containing spaces, apostrophes and trailing backslashes.

Host JSON records are written to a unique temporary file in the destination directory, flushed and closed, then renamed without overwriting an existing record. Readers cannot observe the final name during the write. Dummy-process and temporary-file regressions cover these behaviors without starting ETW or requesting elevation; they do not establish an end-to-end standby result.

```powershell
$workspace = (Get-Location).Path # Repository root.
$hostScript = Join-Path $workspace 'windows\standby\Run-DxgKrnlHost.ps1'
$runDir = Join-Path $workspace 'windows\standby\runs\standby-test01'
$testId = 'standby-test01'
$powerShell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
function ConvertTo-PowerShellLiteral([string]$Value) { "'" + $Value.Replace("'", "''") + "'" }
$hostCommand = '& {0} -OutputDirectory {1} -TestId {2} -MaximumMinutes 20' -f
    (ConvertTo-PowerShellLiteral $hostScript), (ConvertTo-PowerShellLiteral $runDir), (ConvertTo-PowerShellLiteral $testId)
$hostArgs = @('-NoProfile','-NonInteractive','-EncodedCommand',
    [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($hostCommand)))
$hostProcess = Start-Process -FilePath $powerShell -ArgumentList $hostArgs -Verb RunAs -WindowStyle Hidden -PassThru
# The user approves the ordinary UAC dialog once.
# Require dxgkrnl-host-ready.json with startVerified=true before display-off.
# Run the separately controlled standby observation, then intentionally wake.
# Record the listener's wake/end markers before requesting the ETW stop:
[IO.File]::WriteAllText((Join-Path $runDir 'etw-stop.request'),[DateTime]::UtcNow.ToString('o'))
# Wait for dxgkrnl-host-result.json; require startVerified/stopVerified/exportVerified.
```

Do not run the start and stop-file lines back-to-back: the explicit observation belongs between them. The host writes `dxgkrnl-host.json` before attempting Start, `dxgkrnl-host-ready.json` only after verifying the active session, separate `dxgkrnl-host-action-*.json` records, child stdout/stderr, and `dxgkrnl-host-result.json` after cleanup. A cancelled UAC dialog yields no new host record. The run directory must not contain previous ETW run artifacts or a preexisting stop request. If sharing a directory, start the Win32 listener first because its Start action requires a new directory; the ETW host can then add its artifacts.

The host starts its timeout before launching the Start child. On a stop request, timeout, or ordinary exception, its `finally` block attempts Stop for its own manifest/session and then Export only after a verified stop. It does not stop any session if its Start attempt never produced an owned manifest. Failed Start, Stop, export, persistence, or child timeouts are recorded and produce a nonzero host exit. Child deadlines are 45 seconds for Start, 60 for Stop, and 180 for offline Export. A failed Stop needs inspection; do not assume the timer successfully stopped the session when the final result says otherwise. Forcefully killing the host or losing power prevents a guaranteed `finally`; use the direct Status/Stop flow below with the same run directory and TestId if recovery is needed. Raw ETL/XML and detailed payloads remain private.

The direct controller flow below is available for deliberate diagnostics or recovery. It requests UAC for each action; prefer the host above for the actual measurement.

The following is an example diagnostic sequence, not a standby test result. Choose one fresh local run directory. It can also contain the Win32 listener's files.

```powershell
$workspace = (Get-Location).Path # Run from the repository root.
$script = Join-Path $workspace 'windows\standby\Capture-DxgKrnl.ps1'
$runDir = Join-Path $workspace 'windows\standby\runs\standby-test01'
$testId = 'standby-test01'
$powerShell = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"

# Pure compilation and layout validation; no ETW calls.
& $powerShell -NoProfile -File $script -Action Validate -OutputDirectory $runDir -TestId $testId

function ConvertTo-PowerShellLiteral([string]$Value) { "'" + $Value.Replace("'", "''") + "'" }
function Invoke-TestEtw([ValidateSet('Start','Status','Stop','Export')][string]$Phase) {
    $command = '& {0} -Action {1} -OutputDirectory {2} -TestId {3}' -f
        (ConvertTo-PowerShellLiteral $script), (ConvertTo-PowerShellLiteral $Phase),
        (ConvertTo-PowerShellLiteral $runDir), (ConvertTo-PowerShellLiteral $testId)
    $argsForChild = @('-NoProfile','-NonInteractive','-EncodedCommand',
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command)))
    Start-Process -FilePath $powerShell -ArgumentList $argsForChild -Verb RunAs -WindowStyle Hidden -PassThru
}

# With displays active and normal UAC approval:
Invoke-TestEtw Start
# Inspect the newly written dxgkrnl-start-*.json and require success=true.
# Mark baseline, request display-off separately, observe the quiet interval,
# then intentionally wake and mark the end using the Win32 listener.

Invoke-TestEtw Status
# Wait for and inspect the matching status result before the next action.
Invoke-TestEtw Stop
# Require success=true and inspect final native loss counters.
Invoke-TestEtw Export
# Inspect export result, coverage UTC, summary, and listener markers together.
```

Do not press keys or run interactive commands during the quiet interval: input can wake the display. The controller does not change display or system sleep timeouts and does not inhibit sleep. If Windows itself sleeps, there can be no assumption of continuous OS observation during that interval. This experiment should separate display-only standby from system sleep.

If UAC is cancelled, the requested elevated process does not run and no new action-result file should be treated as its result. Do not reuse an old success JSON: match the action, session, timestamp and, where recorded, PID. For the preferred host flow, inspect `dxgkrnl-host-ready.json` and the final host/action JSON files. Do not rely solely on the object returned by an outer `Start-Process -PassThru` invocation to establish successful child execution.

## Interpreting evidence

The Windows 10 DxgKrnl metadata inspected during preparation declared these events. Treat this as the basis for the selected mask, not a universal event contract:

| Event ID | Task / useful payload | Selected keyword |
|---|---|---|
| 272 | `DxgkCbIndicateChildStatus`: `ChildUid`, `Type`, `Connected`, adapter object | Base `0x1` |
| 10000–10002 | `PerfTrack_DMM_Hotplug_Monitor_Set_VideoMode`; correlation ID | DxgKrnl `0x10000` |
| 10011 | `PerfTrack_Dxgkrnl_StateChangeNotify`; type/source | StatusChange `0x100` or DxgKrnl |
| Various | `RuntimePowerManagement` | Power `0x200` |
| 148–151, 1098–1099 | Driver child/connection queries that Windows initiated | Base `0x1` |

Mask `0x300` alone would miss event 272 and the hotplug mode-setting events. Including Base increases event volume. Driver/OS event availability can vary; declaration in a manifest does not guarantee emission for every HDMI event. The listener's monitor-interface arrival/removal notifications complement these traces.

`Connected` is a state reported by the display driver. It does not expose HDMI/DP wire voltage or exact HPD pulse widths. Windows callbacks can represent logical changes such as laptop lid state. A disconnect/reconnect sequence supports an OS-observed transition, not a direct electrical measurement. Conversely, no events cannot prove no electrical pulses occurred.

Power-setting notifications describe session/console state, not an individual panel's electrical state. `WM_DISPLAYCHANGE` specifically indicates resolution change and can miss a transient disconnect that preserves the desktop mode. PnP interface events reflect configured devices, not every raw HPD transition.

The ETL is circular: old data can be overwritten without increasing `EventsLost`. Zero loss counters alone are therefore insufficient. Compare exported first/last UTC against the listener's start/off/wake/end markers and ensure the required interval is retained. Inspect nonzero loss counters, decoding failures and missing metadata as limitations. QPC provides stable relative timing; UTC conversion can differ from wall-clock markers if the system clock is adjusted mid-capture. Keep both UTC and QPC markers.

The raw ETL/XML can contain process identifiers and driver/device paths. Keep them with the private test artifacts; produce redacted summaries when sharing. The selected JSONL includes positional payload values whose names are available from the local provider template or full XML.

## Primary Microsoft references

- [StartTrace: explicit session start, naming, resource scope and privilege rules](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracew)
- [EnableTraceEx2: level/keyword semantics and provider enablement](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-enabletraceex2)
- [ControlTrace: query/stop and final properties/statistics](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-controltracew)
- [EVENT_TRACE_PROPERTIES: buffer/file limits and loss counters](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_properties)
- [WNODE_HEADER: QPC, clock conversion and output session handle](https://learn.microsoft.com/en-us/windows/win32/etw/wnode-header)
- [DxgkCbIndicateChildStatus: driver-reported connection and lid transitions](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dispmprt/nc-dispmprt-dxgkcb_indicate_child_status)
- [Power-setting GUIDs and their session/console/legacy scopes](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids)
- [WM_DISPLAYCHANGE: resolution-change notification](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-displaychange)
- [Monitor device-interface GUID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-monitor)
- [RegisterDeviceNotification: notification scope and nonblocking handlers](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerdevicenotificationw)
- [tracerpt: offline XML and summary export](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/tracerpt)
- [Get-WinEvent: reading ETL with -Oldest](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.diagnostics/get-winevent)
- [Job Objects: child inheritance, breakaway and kill-on-close](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [TerminateJobObject: terminating the assigned process tree](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject)
- [Job accounting: active-process count](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information)
- [logman trace options](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/logman-create-trace), [wevtutil provider metadata and log export](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/wevtutil), [WPR profile/start/stop options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)

Offline checks in PowerShell 7 and Windows PowerShell 5.1 confirmed x64 `WNODE_HEADER=48`, `EVENT_TRACE_PROPERTIES=120`, and `LoggerThreadId` offset 104. Separate preparation activity exercised a short ETW start/stop with displays active. The portable host-process and atomic-publication tests use dummy processes and temporary files, making no ETW, UAC or monitor calls. See [Preparation results](PREPARATION-RESULT.md) for what these checks establish and what remains untested.
