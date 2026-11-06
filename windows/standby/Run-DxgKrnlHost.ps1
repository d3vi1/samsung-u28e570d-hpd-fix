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
    $destination = [IO.Path]::GetFullPath($Path)
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($destination)) ('.' + [IO.Path]::GetFileName($destination) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $temporaryCreated = $false
    try {
        $file = [IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $temporaryCreated = $true
        try { $file.Write($bytes,0,$bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
        # Same-directory rename publishes only a closed complete file; this overload never overwrites.
        [IO.File]::Move($temporary,$destination)
    } finally {
        if ($temporaryCreated -and [IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Initialize-OwnedControllerJobType {
    if ('SamsungStandbyHostJob.OwnedJob' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace SamsungStandbyHostJob {
    // Anonymous, non-inheritable job. Only KILL_ON_JOB_CLOSE is enabled: no breakaway flags.
    public sealed class OwnedJob : IDisposable {
        sealed class JobHandle : SafeHandleZeroOrMinusOneIsInvalid {
            internal JobHandle(IntPtr value) : base(true) { SetHandle(value); }
            protected override bool ReleaseHandle() { return CloseHandle(handle); }
        }
        [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
            public long ProcessTime, JobTime;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }
        [StructLayout(LayoutKind.Sequential)] struct IoCounters {
            public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
        }
        [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
            public BasicLimits Basic;
            public IoCounters Io;
            public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
        }
        [StructLayout(LayoutKind.Sequential)] struct Accounting {
            public long TotalUserTime, TotalKernelTime, PeriodUserTime, PeriodKernelTime;
            public uint PageFaults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
        }
        readonly JobHandle handle;
        public OwnedJob() {
            if (IntPtr.Size != 8 || Marshal.SizeOf(typeof(BasicLimits)) != 64 ||
                Marshal.SizeOf(typeof(ExtendedLimits)) != 144 || Marshal.SizeOf(typeof(Accounting)) != 48 ||
                Marshal.OffsetOf(typeof(Accounting), "ActiveProcesses").ToInt32() != 40)
                throw new InvalidOperationException("Unexpected x64 Job Object layout.");
            IntPtr value = CreateJobObject(IntPtr.Zero, null);
            if (value == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateJobObject failed.");
            handle = new JobHandle(value);
            try {
                ExtendedLimits limits = new ExtendedLimits();
                limits.Basic.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if (!SetInformationJobObject(handle, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "SetInformationJobObject failed.");
            } catch { handle.Dispose(); throw; }
        }
        public void Assign(IntPtr processHandle) {
            if (!AssignProcessToJobObject(handle, processHandle))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "AssignProcessToJobObject failed; wrapper gate remains closed.");
        }
        public uint ActiveProcessCount {
            get {
                Accounting accounting;
                uint returned;
                if (!QueryInformationJobObject(handle, 1, out accounting, (uint)Marshal.SizeOf(typeof(Accounting)), out returned))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "QueryInformationJobObject failed.");
                if (returned != Marshal.SizeOf(typeof(Accounting)))
                    throw new InvalidOperationException("Incomplete Job Object accounting result.");
                return accounting.ActiveProcesses;
            }
        }
        public void Terminate(uint exitCode) {
            if (!TerminateJobObject(handle, exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "TerminateJobObject failed.");
        }
        public bool WaitForEmpty(int milliseconds) {
            Stopwatch timer = Stopwatch.StartNew();
            do {
                if (ActiveProcessCount == 0) return true;
                if (timer.ElapsedMilliseconds >= milliseconds) return false;
                Thread.Sleep(20);
            } while (true);
        }
        public void Dispose() { handle.Dispose(); }
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool SetInformationJobObject(JobHandle job, int informationClass, ref ExtendedLimits information, uint length);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool QueryInformationJobObject(JobHandle job, int informationClass, out Accounting information, uint length, out uint returnedLength);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool AssignProcessToJobObject(JobHandle job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool TerminateJobObject(JobHandle job, uint exitCode);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool CloseHandle(IntPtr handle);
    }
}
'@
}

function Invoke-OwnedController([ValidateSet('Start','Stop','Export')][string]$Phase, [int]$TimeoutSeconds) {
    $entry = [ordered]@{ action=$Phase; requestedUtc=[DateTime]::UtcNow.ToString('o'); pid=$null; exitCode=$null
        timedOut=$false; resultPath=$null; result=$null; verified=$false; error=$null
        stdoutComplete=$false; stderrComplete=$false; streamCaptureError=$null
        jobAssigned=$false; gateReleased=$false; treeTerminationAttempted=$false
        childTerminationConfirmed=$false; treeTerminationConfirmed=$false; remainingJobProcesses=$null
        unexpectedDescendants=$false; jobCleanupError=$null; jobClosed=$false }
    $report.actions += $entry
    $stdout = Join-Path $OutputDirectory ('dxgkrnl-host-child-' + $Phase.ToLowerInvariant() + '.stdout.txt')
    $stderr = Join-Path $OutputDirectory ('dxgkrnl-host-child-' + $Phase.ToLowerInvariant() + '.stderr.txt')
    foreach ($path in @($stdout,$stderr)) { if (Test-Path -LiteralPath $path) { throw "Child output already exists: $path" } }
    $process = $null
    $job = $null; $gate = $null
    $stdoutFile = $null; $stderrFile = $null
    $stdoutTask = $null; $stderrTask = $null
    try {
        Initialize-OwnedControllerJobType
        $job = [SamsungStandbyHostJob.OwnedJob]::new()
        $gateName = 'Local\SamsungStandbyGate-' + [Guid]::NewGuid().ToString('N')
        $createdNew = $false
        $gate = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$gateName,[ref]$createdNew)
        if (-not $createdNew) { throw 'Controller gate already exists; refusing to launch.' }
        # The initial process executes only this bounded gate wait. Wrapper code cannot
        # run or spawn tracerpt until Assign succeeds and the parent signals the gate.
        # Literal PowerShell strings preserve spaces, apostrophes and trailing backslashes.
        $wrapperLiteral = "'" + $wrapper.Replace("'","''") + "'"
        $directoryLiteral = "'" + $OutputDirectory.Replace("'","''") + "'"
        $testIdLiteral = "'" + $TestId.Replace("'","''") + "'"
        $phaseLiteral = "'" + $Phase.Replace("'","''") + "'"
        $bootstrap = @(
            '$ErrorActionPreference = ''Stop'''
            '$gate = $null'
            'try {'
            ('$gate = [Threading.EventWaitHandle]::OpenExisting(''' + $gateName + ''')')
            'if (-not $gate.WaitOne(30000)) { exit 125 }'
            '$gate.Dispose(); $gate = $null'
            '$global:LASTEXITCODE = 0'
            ('& ' + $wrapperLiteral + ' -Action ' + $phaseLiteral + ' -OutputDirectory ' + $directoryLiteral +
                ' -TestId ' + $testIdLiteral + ' -MaximumMegabytes ' + $MaximumMegabytes.ToString([Globalization.CultureInfo]::InvariantCulture))
            '$controllerSucceeded = $?'
            'if (-not $controllerSucceeded -and $LASTEXITCODE -eq 0) { exit 1 }'
            'exit $LASTEXITCODE'
            '} catch { [Console]::Error.WriteLine($_.Exception.ToString()); exit 126 }'
            'finally { if ($null -ne $gate) { $gate.Dispose() } }'
        ) -join [Environment]::NewLine
        $encodedBootstrap = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($bootstrap))
        # Already elevated host: no RunAs here and therefore no additional UAC dialog.
        # Own the Process handle directly: Start-Process -PassThru can lose ExitCode in Windows PowerShell 5.1.
        $stdoutFile = [IO.FileStream]::new($stdout,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $stderrFile = [IO.FileStream]::new($stderr,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $childPowerShell
        $startInfo.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + $encodedBootstrap
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
        $job.Assign($process.Handle)
        $entry.jobAssigned = $true
        if (-not $gate.Set()) { throw 'Could not release the assigned controller gate.' }
        $entry.gateReleased = $true
        $childTimer = [Diagnostics.Stopwatch]::StartNew()
        while (-not $process.WaitForExit(500)) {
            if ($childTimer.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                $entry.timedOut = $true
                throw "$Phase controller exceeded $TimeoutSeconds seconds; its session state must be checked."
            }
        }
        $entry.exitCode = [int]$process.ExitCode
        $entry.childTerminationConfirmed = $true
        if (-not $job.WaitForEmpty(250)) {
            $entry.unexpectedDescendants = $true
            throw "$Phase controller exited while descendants were still running; the action is not complete."
        }
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
        # Job lifetime covers the whole action, including stream/error paths. Killing
        # only the PowerShell controller would leave native children such as tracerpt.
        $cleanupFailed = $false
        try {
            # Do not let an accounting-query failure bypass the direct cleanup of
            # a bootstrap that could not be assigned. Its gate is still closed.
            if ($null -ne $entry.pid -and -not $entry.jobAssigned) {
                try { if (-not $process.HasExited) { $process.Kill() } }
                finally { $entry.childTerminationConfirmed = $process.WaitForExit(5000) }
            }
            if ($null -ne $job) {
                if ($job.ActiveProcessCount -ne 0) {
                    $entry.treeTerminationAttempted = $true
                    $job.Terminate(124)
                }
                $jobEmpty = $job.WaitForEmpty(5000)
                $entry.remainingJobProcesses = $job.ActiveProcessCount
                if ($null -ne $entry.pid -and $entry.jobAssigned) {
                    $entry.childTerminationConfirmed = $process.WaitForExit(5000)
                }
                $entry.treeTerminationConfirmed = $jobEmpty -and $entry.remainingJobProcesses -eq 0 -and
                    ($null -eq $entry.pid -or $entry.childTerminationConfirmed)
                if (-not $entry.treeTerminationConfirmed) { throw 'Controller/job termination could not be confirmed.' }
            }
        } catch {
            $cleanupFailed = $true; $entry.verified = $false
            $entry.jobCleanupError = $_.Exception.ToString()
            $report.errors += 'Controller job cleanup failed: ' + $_.Exception.Message
        } finally {
            if ($null -ne $entry.pid -and $entry.childTerminationConfirmed) {
                try { $entry.exitCode = [int]$process.ExitCode }
                catch { $cleanupFailed = $true; $entry.verified = $false; $report.errors += 'Terminated child exit-code read failed: ' + $_.Exception.Message }
            }
            if ($null -ne $job) {
                try { $job.Dispose(); $entry.jobClosed = $true }
                catch { $cleanupFailed = $true; $entry.verified = $false; $report.errors += 'Job close failed: ' + $_.Exception.Message }
            }
            if ($null -ne $gate) { $gate.Dispose() }
        }
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
        foreach ($stream in @($stdoutFile,$stderrFile)) {
            if ($null -ne $stream) {
                try { $stream.Dispose() } catch { $report.errors += 'Child output close failed: ' + $_.Exception.Message }
            }
        }
        $entry.completedUtc = [DateTime]::UtcNow.ToString('o')
        try { Save-HostJson (Join-Path $OutputDirectory ('dxgkrnl-host-action-' + $Phase.ToLowerInvariant() + '.json')) $entry }
        catch { $report.errors += 'Action report persistence failed: ' + $_.Exception.Message }
        if ($null -ne $process) { $process.Dispose() }
        if ($cleanupFailed) { throw 'Controller process-tree cleanup was not verified; inspect the action result.' }
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
