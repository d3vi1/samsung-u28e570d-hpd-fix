# Windows standby observation

This Windows lab tool records display-power, resolution and monitor-interface notifications. It can support a controlled HDMI standby investigation, but it has not yet been validated across an HDMI standby interval. Listener startup and a short ETW session were exercised with displays active; see [Preparation results](PREPARATION-RESULT.md) for the exact scope.

Preparing or compiling the sources does not start a listener. Start capture explicitly with the displays active before making a separate display-off request.

`StandbyCapture.cs` has no NVIDIA, DDC, EDID, ISP, QueryDisplayConfig, display-enumeration, power-policy or sleep-inhibition calls. It creates a hidden top-level window only when `CaptureHost.Run` is explicitly invoked. It subscribes to Windows notifications and waits for messages. A local named pipe accepts `PING`, `MARK` and `STOP`; no command on that pipe controls a monitor. No elevation is required for this listener.

## Using the listener

From the repository root, use 64-bit PowerShell 7 or Windows PowerShell 5.1 and a new directory for every run. Start the listener before another tool creates the same run directory:

```powershell
$run = Join-Path $PWD 'windows\standby\runs\test-001'
& .\windows\standby\Standby-Capture.ps1 -Action Start -Directory $run
& .\windows\standby\Standby-Capture.ps1 -Action Mark -Directory $run -Text 'Baseline active; HDMI; observed timing recorded separately.'
& .\windows\standby\Standby-Capture.ps1 -Action Status -Directory $run
```

`Start` launches a hidden PowerShell child and waits for `ready.json`. Startup failure is reported through `host-error.txt`; a timeout is not a success indication. The control endpoint is saved in `session.json` before launching the child, so `Stop` remains possible if startup acknowledgement fails. `ready.json` contains the actual child PID. Both `ready.json` and `stopped.json` are published by a same-directory rename only after the temporary file has been flushed and closed; existing records are never overwritten. Their reader uses file sharing compatible with the publisher's brief rename handle, then parses the complete JSON. Do not reuse an old run directory or delete it while its listener is alive.

Optional [topology snapshots](DISPLAY-TOPOLOGY.md) belong before standby and after deliberate wake, while displays are active. Optional [DxgKrnl ETW capture](DXGKRNL-NOTES.md) uses one elevated host: approve its single UAC prompt and require its ready record before display-off. The host's Start/Stop/Export children inherit elevation and use direct `System.Diagnostics.Process` handling; they do not create additional UAC prompts.

The following is a separate active Windows request, never called by the passive listener:

```powershell
# Explicitly requests that Windows turn displays off; it does not request system sleep.
& .\windows\standby\Request-DisplayOff.ps1 -RequestDisplayOff -CaptureDirectory $run
```

That script accepts `-WhatIf`. It sends `WM_SYSCOMMAND / SC_MONITORPOWER=2` only after the explicit switch and ShouldProcess check. Its completion means Windows received a request, not that physical monitor standby has been proven. Wake is left to the operator; there is no wake timer, automatic key injection or scheduled sleep. The request affects Windows displays generally, not one selected monitor. Microsoft defines the command and its `lParam` values in [WM_SYSCOMMAND](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand).

Add observational markers and stop after the operator's planned observation interval:

```powershell
& .\windows\standby\Standby-Capture.ps1 -Action Mark -Directory $run -Text 'Visible flicker observed; approximate timing.'
& .\windows\standby\Standby-Capture.ps1 -Action Mark -Directory $run -Text 'Operator completed wake.'
& .\windows\standby\Standby-Capture.ps1 -Action Stop -Directory $run
```

Markers themselves are log records, but typing or moving a mouse to issue one can wake a display. Record how each observation was made. `Stop` requests orderly cleanup and waits for `stopped.json`. The host allows five seconds to drain its log worker; a timeout sets `logErrorObserved=true`, so a stopped marker with that flag does not prove a complete flush. Stop does not kill a process or change monitor power.

## Recorded data and interpretation

Every `events.jsonl` record includes UTC, a sequence number and monotonic elapsed milliseconds. These times are assigned when the process receives/handles a notification, before disk I/O; scheduling latency separates them from the underlying event. A dedicated worker writes records. The bounded queue reports `droppedRecords`; `stopped.json` also reports any observed logging error. A nonzero drop count or logging error makes a gap inconclusive.

The listener registers these power settings: `GUID_SESSION_DISPLAY_STATUS`, `GUID_CONSOLE_DISPLAY_STATE`, and legacy `GUID_MONITOR_POWER_ON`. Session status is the recommended setting for interactive applications; session/console values distinguish off, on and dim, while the legacy setting describes the primary monitor and has only off/on values. The capture-start record includes process session ID, active console session ID, and the remote-session flag. These are aggregate Windows/session notifications, not reliable per-panel power measurements. See [Power Setting GUIDs](https://learn.microsoft.com/en-us/windows/win32/power/power-setting-guids) and [RegisterPowerSettingNotification](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerpowersettingnotification).

`WM_DISPLAYCHANGE` records reported bits per pixel and resolution. It signals a resolution change, so absence of this message does not exclude a transient HDMI signal or a physical panel flash. See [WM_DISPLAYCHANGE](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-displaychange).

Monitor arrival/removal notifications are explicitly registered using `GUID_DEVINTERFACE_MONITOR`. Generic `WM_DEVICECHANGE` events are also retained; a generic devnode event alone does not identify a monitor. Monitor-interface records contain the class GUID and SHA-256 of the interface name for correlation, not the raw device path. Microsoft documents the [monitor interface GUID](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/guid-devinterface-monitor) and [device notification registration](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerdevicenotificationw). The handler copies event payloads promptly and never vetoes device-removal queries.

No recorded notification directly measures HDMI HPD voltage, backlight power or monitor-internal standby. A visible flicker with no Windows event remains useful evidence, but it cannot by itself prove a firmware fault. Compare the exact UTC interval with the optional ETW trace and the existing Kernel-PnP/Configuration log if collected. Do not enable channels or poll topology during the passive interval. The optional ETW controller has different elevation and overhead requirements; those do not apply to this listener.

## Validation and sharing

`Test-StandbyCapture.ps1` compiles both the listener and separate display-off helper but calls neither. It tests synthetic power/PnP/display payloads, JSON escaping, identifier hashing, script parsing and the Win32 structure sizes. All 21 checks passed under PowerShell 7 and Windows PowerShell 5.1. Those tests execute no listener, native notification API or display-off request. Separate preparation runs exercised listener startup and a short ETW start/stop; they did not include display-off or a standby interval. This distinction is recorded in [Preparation results](PREPARATION-RESULT.md).

Source files contain no per-unit serial or personal path. Run artifacts are private: marker text, error stacks, ETW and exported OS events can contain identifiers even though interface names in this listener are hashed. Review and sanitize artifacts before sharing. The code-only `.gitignore` excludes run outputs.

`Test-DxgKrnlHostProcess.ps1` and `Test-AtomicPublication.ps1` each pass eight cases per shell. The publication test invokes the actual ready/stopped reader concurrently with both writers, covering 48 first-appearance reads of 1 MiB JSON across both shells. These tests use only dummy processes, runspaces and temporary files. See the [preparation results](PREPARATION-RESULT.md) and accompanying sanitized JSON for source hashes and case details.

One control-channel limitation is that the pipe server checks the line length after reading a complete line; an abnormal local client that connects without completing its request can temporarily prevent Mark/Stop service. The provided client always sends one newline-terminated request and uses timeouts. This control endpoint is a local lab interface, not a hardened service for untrusted clients.
