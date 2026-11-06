# Preparation results

The capture tools reached a preparation milestone, not an HDMI standby result. The notification logger was started, and one short real DxgKrnl ETW session was started and stopped with displays still active. No display-off request was made for the intended test and no standby observation interval was recorded.

## What was exercised

- Some elevation requests were cancelled at the UAC prompt. Those cancellations were not successful ETW starts and must not be counted as capture coverage.
- Notification-listener startup was exercised separately from its synthetic tests.
- A short ETW probe was started and stopped before display-off. It established that the provider/session path could run on the preparation system; it did not test HDMI standby, wake or flicker.
- The ETW host exposed a Windows PowerShell 5.1 process-handling bug: `Start-Process -PassThru` could leave a completed child's `ExitCode` null, causing the host to reject a successful child result.
- The host was changed to own child processes directly through `System.Diagnostics.Process`. It retains the process handle, drains stdout and stderr concurrently, reads the integer exit code, and verifies the matching action JSON. The outer elevation request still uses one ordinary UAC prompt.
- The subsequent live launch of the corrected host was cancelled at UAC. Its process handling passed dummy tests, but its complete live Start/wait/Stop/Export workflow has not been exercised end to end.

## Offline and dummy checks

The passive-listener suite passed 21 checks in PowerShell 7 and Windows PowerShell 5.1. The topology suite passed 20 checks in both shells. These cover synthetic decoding, ABI layouts and script compilation; they do not execute a standby interval. ETW layout checks also confirmed the reviewed x64 structures.

`Test-DxgKrnlHostProcess.ps1` tests the production host's process-handling functions against inline dummy controllers. It does not start the host's capture workflow, request elevation or call ETW/display APIs. The four cases are:

| Dummy child | Expected result |
|---|---|
| Exit 0 with a successful result JSON | Accepted |
| Exit 7 even though JSON declares success | Rejected |
| Exit 0 with 131072 payload characters plus markers on each output stream | Accepted; stdout and stderr captured completely |
| Exit 7 with the same large stdout/stderr payloads | Rejected; stdout and stderr still captured completely |

The sanitized machine-readable result accompanies the portable test. Temporary paths, process IDs, device identifiers and raw run artifacts are not part of this report.

## What remains untested

The measured HDMI standby duration is **zero**. These results do not show whether an HDMI monitor stays asleep, flickers, changes HPD state, reconnects or wakes correctly. They do not support a conclusion about a monitor firmware fix or regression.

A future test must record a verified capture-ready state, an explicit display-off boundary, an observation interval, deliberate wake, and confirmed cleanup. Interpretation also requires checking event coverage, logging errors and ETW loss/circular-overwrite limits. The notification and ETW tools observe Windows/driver events; neither measures electrical HPD or panel power directly.

See [listener instructions](README.md), [ETW instructions](DXGKRNL-NOTES.md) and [before/after topology snapshots](DISPLAY-TOPOLOGY.md) for the procedure and limitations.
