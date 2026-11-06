#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$TestId = 'standby-test',
    [ValidateRange(32,512)][int]$MaximumMegabytes = 256,
    [ValidateRange(1,20)][int]$MaximumMinutes = 20
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($OutputDirectory.StartsWith('\\')) { throw 'Use a local run directory.' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$wrapper = Join-Path $PSScriptRoot 'Capture-DxgKrnl.ps1'
$childPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$manifestPath = Join-Path $OutputDirectory 'dxgkrnl-session.json'
$stopPath = Join-Path $OutputDirectory 'etw-stop.request'
$hostPath = Join-Path $OutputDirectory 'dxgkrnl-host.json'
$resultPath = Join-Path $OutputDirectory 'dxgkrnl-host-result.json'
$readyPath = Join-Path $OutputDirectory 'dxgkrnl-host-ready.json'
$expectedSession = 'Samsung-Standby-DxgKrnl-' + $TestId
$report = [ordered]@{
    schema='SamsungStandbyEtwHost/v1'; pid=$PID; requestedUtc=[DateTime]::UtcNow.ToString('o')
    sessionName=$expectedSession; outputDirectory=$OutputDirectory; stopRequestPath=$stopPath
    maximumMinutes=$MaximumMinutes; startVerified=$false; stopVerified=$false; exportVerified=$false
    stopReason=$null; success=$false; errors=@(); actions=@()
}
$ownsRun = $false
$startAttempted = $false
$sessionGuid = $null
$timer = [Diagnostics.Stopwatch]::new()

function Save-HostJson([string]$Path, $Value) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 16) + [Environment]::NewLine)
    $file = [IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Write($bytes,0,$bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
}

function Invoke-OwnedController([ValidateSet('Start','Stop','Export')][string]$Phase, [int]$TimeoutSeconds) {
    $entry = [ordered]@{ action=$Phase; requestedUtc=[DateTime]::UtcNow.ToString('o'); pid=$null; exitCode=$null
        timedOut=$false; resultPath=$null; result=$null; verified=$false; error=$null
        stdoutComplete=$false; stderrComplete=$false; streamCaptureError=$null }
    $report.actions += $entry
    $stdout = Join-Path $OutputDirectory ('dxgkrnl-host-child-' + $Phase.ToLowerInvariant() + '.stdout.txt')
    $stderr = Join-Path $OutputDirectory ('dxgkrnl-host-child-' + $Phase.ToLowerInvariant() + '.stderr.txt')
    foreach ($path in @($stdout,$stderr)) { if (Test-Path -LiteralPath $path) { throw "Child output already exists: $path" } }
    $argsForChild = @('-NoProfile','-File',('"{0}"' -f $wrapper),'-Action',$Phase,
        '-OutputDirectory',('"{0}"' -f $OutputDirectory),'-TestId',$TestId,
        '-MaximumMegabytes',$MaximumMegabytes.ToString())
    $process = $null
    $stdoutFile = $null; $stderrFile = $null
    $stdoutTask = $null; $stderrTask = $null
    try {
        # Already elevated host: no RunAs here and therefore no additional UAC dialog.
        # Own the Process handle directly: Start-Process -PassThru can lose ExitCode in Windows PowerShell 5.1.
        $stdoutFile = [IO.FileStream]::new($stdout,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $stderrFile = [IO.FileStream]::new($stderr,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $childPowerShell
        $startInfo.Arguments = $argsForChild -join ' '
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw "$Phase controller process did not start." }
        $entry.pid = $process.Id
        # Drain both streams concurrently before waiting, so a full stderr/stdout pipe cannot deadlock the child.
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $childTimer = [Diagnostics.Stopwatch]::StartNew()
        while (-not $process.WaitForExit(500)) {
            if ($childTimer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $entry.timedOut = $true
                $entry.childTerminationConfirmed = $false
                try { $process.Kill(); $entry.childTerminationConfirmed = $process.WaitForExit(5000) }
                catch { $entry.childTerminationError = $_.Exception.Message }
                throw "$Phase controller exceeded $TimeoutSeconds seconds; its session state must be checked."
            }
        }
        $entry.exitCode = [int]$process.ExitCode
        $pattern = 'dxgkrnl-' + $Phase.ToLowerInvariant() + '-*-' + $process.Id + '.json'
        $files = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter $pattern -File)
        if ($files.Count -ne 1) { throw "$Phase controller did not produce exactly one PID-bound result JSON." }
        $entry.resultPath = $files[0].FullName
        $childResult = Get-Content -LiteralPath $entry.resultPath -Raw | ConvertFrom-Json
        $entry.result = $childResult
        if ($entry.exitCode -ne 0 -or $childResult.success -ne $true -or
            $childResult.action -ne $Phase -or $childResult.sessionName -ne $expectedSession) {
            throw "$Phase controller failed its exit-code/result/identity check."
        }
        $entry.verified = $true
        return $childResult
    } catch {
        $entry.error = $_.Exception.ToString()
        throw
    } finally {
        try {
            if ($null -ne $stdoutTask -and $null -ne $stderrTask) {
                $streamsDone = [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($stdoutTask,$stderrTask),5000)
                if (-not $streamsDone) { throw 'Child output streams did not close within five seconds after process exit/timeout.' }
                $stdoutBytes = [Text.UTF8Encoding]::new($false).GetBytes($stdoutTask.GetAwaiter().GetResult())
                $stderrBytes = [Text.UTF8Encoding]::new($false).GetBytes($stderrTask.GetAwaiter().GetResult())
                $stdoutFile.Write($stdoutBytes,0,$stdoutBytes.Length); $stdoutFile.Flush($true)
                $stderrFile.Write($stderrBytes,0,$stderrBytes.Length); $stderrFile.Flush($true)
                $entry.stdoutComplete = $true; $entry.stderrComplete = $true
            }
        } catch {
            $entry.streamCaptureError = $_.Exception.ToString()
            $report.errors += 'Child stream capture failed: ' + $_.Exception.Message
        }
        if ($null -ne $stdoutFile) { $stdoutFile.Dispose() }
        if ($null -ne $stderrFile) { $stderrFile.Dispose() }
        $entry.completedUtc = [DateTime]::UtcNow.ToString('o')
        try { Save-HostJson (Join-Path $OutputDirectory ('dxgkrnl-host-action-' + $Phase.ToLowerInvariant() + '.json')) $entry }
        catch { $report.errors += 'Action report persistence failed: ' + $_.Exception.Message }
        if ($null -ne $process) { $process.Dispose() }
    }
}

function Read-OwnedManifest {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $null }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 'SamsungStandbyEtw/v1' -or $manifest.sessionName -ne $expectedSession -or
        [IO.Path]::GetFullPath($manifest.outputDirectory) -ne $OutputDirectory -or
        [IO.Path]::GetFullPath($manifest.etlPath) -ne (Join-Path $OutputDirectory 'dxgkrnl.etl')) {
        throw 'Session manifest does not belong to this host run.'
    }
    [void][Guid]::Parse($manifest.sessionGuid)
    return $manifest
}

try {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Launch this host once with Start-Process -Verb RunAs -WindowStyle Hidden. The user must approve the normal UAC dialog.'
    }
    if (-not (Test-Path -LiteralPath $wrapper -PathType Leaf)) { throw 'ETW controller wrapper is missing.' }
    foreach ($path in @($hostPath,$manifestPath,$resultPath,$readyPath,$stopPath,(Join-Path $OutputDirectory 'dxgkrnl.etl'))) {
        if (Test-Path -LiteralPath $path) { throw "Use a fresh run directory; an ETW run artifact already exists: $path" }
    }
    Save-HostJson $hostPath $report
    $ownsRun = $true
    # The deadline includes controller startup, so the requested quiet capture is not extended by startup work.
    $timer.Start()
    $startAttempted = $true
    $started = Invoke-OwnedController 'Start' 45
    $manifest = Read-OwnedManifest
    if ($null -eq $manifest -or $started.snapshot.Running -ne $true -or
        $started.snapshot.SessionName -ne $expectedSession -or
        [Guid]::Parse($started.snapshot.SessionGuid) -ne [Guid]::Parse($manifest.sessionGuid)) {
        throw 'Start reported success but active-session identity did not match its manifest.'
    }
    $sessionGuid = $manifest.sessionGuid
    $report.sessionGuid = $sessionGuid
    $report.startVerified = $true
    $report.readyUtc = [DateTime]::UtcNow.ToString('o')
    Save-HostJson $readyPath ([ordered]@{ utc=$report.readyUtc; startVerified=$true; sessionName=$expectedSession
        sessionGuid=$sessionGuid; stopRequestPath=$stopPath; maximumMinutes=$MaximumMinutes
        note='Capture ready. Display-off must be requested separately; this host never controls power.' })
    while ($true) {
        if (Test-Path -LiteralPath $stopPath -PathType Leaf) { $report.stopReason = 'stop-request-file'; break }
        if ($timer.Elapsed.TotalMinutes -ge $MaximumMinutes) { $report.stopReason = 'maximum-duration'; break }
        Start-Sleep -Milliseconds 500 # Only local stop-file polling; no display/device queries.
    }
} catch {
    $report.errors += $_.Exception.ToString()
    if (-not $report.stopReason) { $report.stopReason = 'host-error' }
} finally {
    # Never stop based solely on a guessed name. This run must have attempted Start and created its own manifest.
    if ($ownsRun -and $startAttempted) {
        try {
            $ownedManifest = Read-OwnedManifest
            if ($null -eq $ownedManifest) {
                $report.errors += 'No owned session manifest was created; Stop and Export were skipped.'
            } else {
                if ($sessionGuid -and [Guid]::Parse($ownedManifest.sessionGuid) -ne [Guid]::Parse($sessionGuid)) {
                    throw 'Session GUID changed after Start; refusing cleanup of a different session.'
                }
                $stopped = Invoke-OwnedController 'Stop' 60
                if ($stopped.snapshot.Running -ne $false) { throw 'Stop result did not confirm that the owned session stopped.' }
                if ($stopped.snapshot.SessionGuid -and
                    [Guid]::Parse($stopped.snapshot.SessionGuid) -ne [Guid]::Parse($ownedManifest.sessionGuid)) {
                    throw 'Stop returned a different session identity.'
                }
                $report.stopVerified = $true
                $report.stopVerifiedUtc = [DateTime]::UtcNow.ToString('o')
                $report.finalLossCountersAvailable = $stopped.finalLossCountersAvailable
                if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'dxgkrnl.etl') -PathType Leaf) {
                    try {
                        $exported = Invoke-OwnedController 'Export' 180
                        $report.exportVerified = $true
                        $report.export = $exported
                    } catch { $report.errors += 'Offline export failed: ' + $_.Exception.ToString() }
                } else { $report.errors += 'Owned session is stopped, but no ETL exists to export.' }
            }
        } catch { $report.errors += 'Owned-session cleanup failed; inspect session status: ' + $_.Exception.ToString() }
    }
    $report.elapsedSeconds = $timer.Elapsed.TotalSeconds
    $report.completedUtc = [DateTime]::UtcNow.ToString('o')
    $report.success = $report.startVerified -and $report.stopVerified -and $report.exportVerified -and $report.errors.Count -eq 0
    if ($ownsRun) {
        try { Save-HostJson $resultPath $report }
        catch {
            $report.success = $false
            Write-Error ('Host result persistence failed: ' + $_.Exception.Message) -ErrorAction Continue
        }
    } else {
        try { Save-HostJson (Join-Path $OutputDirectory ('dxgkrnl-host-rejected-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ') + '-' + $PID + '.json')) $report }
        catch { Write-Error ('Cannot save rejected-host diagnostic: ' + $_.Exception.Message) -ErrorAction Continue }
    }
    $report | ConvertTo-Json -Depth 16
}
if (-not $report.success) { exit 1 }
