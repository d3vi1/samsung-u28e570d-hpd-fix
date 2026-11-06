#requires -Version 5.1
[CmdletBinding()]
param([string]$HostScript)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([String]::IsNullOrWhiteSpace($HostScript)) {
    $HostScript = Join-Path $PSScriptRoot 'Run-DxgKrnlHost.ps1'
}

# Import only the controller launcher and its dependencies, never the host entry point.
# All launched scripts below are generated dummy fixtures, without ETW or display access.
$HostScript = (Resolve-Path -LiteralPath $HostScript).Path
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($HostScript,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Host source did not parse.' }
$requiredFunctions = @('Save-HostJson','Initialize-OwnedControllerJobType','Invoke-OwnedController')
$functions = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -in $requiredFunctions
},$false))
foreach ($name in $requiredFunctions) {
    if (@($functions | Where-Object Name -eq $name).Count -ne 1) {
        throw "Expected exactly one host function declaration: $name"
    }
}
foreach ($function in $functions) { . ([ScriptBlock]::Create($function.Extent.Text)) }

# Retain real process handles before cleanup, so assertions do not rely on reused PIDs.
# This witness accesses only dummy processes/files and performs failure cleanup itself.
if (-not ('SamsungStandbyDummyTests.ProcessWitness' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading;
namespace SamsungStandbyDummyTests {
    public sealed class ProcessWitness : IDisposable {
        public readonly ManualResetEvent Ready = new ManualResetEvent(false);
        public Process Controller { get; private set; }
        public Process Descendant { get; private set; }
        public string Failure { get; private set; }
        private readonly string metadata, acknowledgement;
        private readonly Thread thread;
        private volatile bool stopping;
        public ProcessWitness(string metadata, string acknowledgement) {
            this.metadata = metadata; this.acknowledgement = acknowledgement;
            thread = new Thread(Observe); thread.IsBackground = true; thread.Start();
        }
        private static Process OpenExact(string id, string ticks) {
            Process process = Process.GetProcessById(Int32.Parse(id, CultureInfo.InvariantCulture));
            try {
                IntPtr retainedHandle = process.Handle;
                if (process.StartTime.ToUniversalTime().Ticks != Int64.Parse(ticks, CultureInfo.InvariantCulture))
                    throw new InvalidOperationException("Dummy process identity changed.");
                return process;
            } catch { process.Dispose(); throw; }
        }
        private void Observe() {
            Stopwatch timer = Stopwatch.StartNew();
            try {
                while (!stopping && timer.Elapsed.TotalSeconds < 15) {
                    if (File.Exists(metadata)) {
                        string[] lines = File.ReadAllLines(metadata);
                        if (lines.Length == 4) {
                            Controller = OpenExact(lines[0],lines[1]);
                            Descendant = OpenExact(lines[2],lines[3]);
                            File.WriteAllText(acknowledgement,"handles-retained");
                            Ready.Set(); return;
                        }
                    }
                    Thread.Sleep(10);
                }
                if (!stopping) Failure = "Dummy process witness timed out.";
            } catch (Exception error) { Failure = error.GetType().Name; }
            finally { Ready.Set(); }
        }
        public void Dispose() {
            stopping = true; thread.Join(1000);
            foreach (Process process in new Process[] { Descendant, Controller }) {
                if (process == null) continue;
                try { if (!process.HasExited) { process.Kill(); process.WaitForExit(5000); } }
                finally { process.Dispose(); }
            }
            Ready.Dispose();
        }
    }
}
'@
}

# Both parent runtimes exercise the production host's Windows PowerShell child runtime.
$childPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $childPowerShell -PathType Leaf)) {
    throw 'This Windows-only test requires Windows PowerShell 5.1.'
}
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$temporaryLeaf = 'samsung-standby-dummy-' + [Guid]::NewGuid().ToString('N')
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent $temporaryLeaf))
if (Test-Path -LiteralPath $temporaryRoot) { throw 'Temporary test directory already exists.' }
[IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null

$dummyController = @'
param([string]$Action,[string]$OutputDirectory,[string]$TestId,[int]$MaximumMegabytes)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$expected = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'expected-arguments.json') -Raw | ConvertFrom-Json
if (-not [String]::Equals($OutputDirectory,$expected.outputDirectory,[StringComparison]::Ordinal) -or
    -not [String]::Equals($TestId,$expected.testId,[StringComparison]::Ordinal) -or
    $Action -cne 'Start' -or $MaximumMegabytes -ne 32) {
    throw 'Dummy controller received changed arguments.'
}
$exitValue = if ($expected.mode -match 'exit7') { 7 } else { 0 }
[Console]::Out.WriteLine('dummy-stdout-begin-' + $exitValue)
[Console]::Error.WriteLine('dummy-stderr-begin-' + $exitValue)
if ($expected.mode -match 'large') {
    [Console]::Out.Write(('O' * 131072))
    [Console]::Error.Write(('E' * 131072))
}
if ($expected.mode -in @('timeout-tree','exit0-tree')) {
    $descendantScript = Join-Path $PSScriptRoot 'Dummy Descendant.ps1'
    $code = '& ' + "'" + $descendantScript.Replace("'","''") + "'"
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $info.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    # Request explicit standard handles while leaving stdout/stderr inherited from
    # the controller. This makes the descendant keep both captured pipes open.
    $info.RedirectStandardInput = $true
    $descendant = [Diagnostics.Process]::new(); $descendant.StartInfo = $info
    if (-not $descendant.Start()) { throw 'Dummy descendant did not start.' }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $heartbeat = Join-Path $PSScriptRoot 'descendant-heartbeat.txt'
    while (-not (Test-Path -LiteralPath $heartbeat) -or (Get-Item -LiteralPath $heartbeat).Length -lt 5) {
        if ($clock.Elapsed.TotalSeconds -gt 10) { throw 'Dummy descendant did not produce heartbeats.' }
        Start-Sleep -Milliseconds 20
    }
    $self = [Diagnostics.Process]::GetCurrentProcess()
    $identity = @($PID,$self.StartTime.ToUniversalTime().Ticks,$descendant.Id,$descendant.StartTime.ToUniversalTime().Ticks)
    $metadataPath = Join-Path $PSScriptRoot 'dummy-processes.txt'
    $metadataTemporary = $metadataPath + '.tmp'
    [IO.File]::WriteAllLines($metadataTemporary,[string[]]$identity)
    [IO.File]::Move($metadataTemporary,$metadataPath)
    $ack = Join-Path $PSScriptRoot 'witness-ready.txt'
    while (-not (Test-Path -LiteralPath $ack)) {
        if ($clock.Elapsed.TotalSeconds -gt 10) { throw 'Dummy process witness did not acknowledge.' }
        Start-Sleep -Milliseconds 20
    }
    if ($expected.mode -eq 'timeout-tree') {
        # Bounded even if the implementation under test regresses.
        while ($clock.Elapsed.TotalSeconds -lt 60) {
            [IO.File]::AppendAllText((Join-Path $PSScriptRoot 'controller-heartbeat.txt'),'C')
            Start-Sleep -Milliseconds 50
        }
    }
}
[Console]::Out.WriteLine('dummy-stdout-end-' + $exitValue)
[Console]::Error.WriteLine('dummy-stderr-end-' + $exitValue)
# Claim success even for exit 7: the launcher must also verify the real exit code.
$result = [ordered]@{
    action=$Action; utc=[DateTime]::UtcNow.ToString('o')
    sessionName=('Samsung-Standby-DxgKrnl-' + $TestId); success=$true
    argumentsExact=$true; dummyPowerShellVersion=$PSVersionTable.PSVersion.ToString()
}
$name = 'dxgkrnl-' + $Action.ToLowerInvariant() + '-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ') + '-' + $PID + '.json'
[IO.File]::WriteAllText((Join-Path $OutputDirectory $name),($result | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
exit $exitValue
'@

$dummyDescendant = @'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::Out.WriteLine('dummy-descendant-stdout-open')
[Console]::Error.WriteLine('dummy-descendant-stderr-open')
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($clock.Elapsed.TotalSeconds -lt 60) {
    [IO.File]::AppendAllText((Join-Path $PSScriptRoot 'descendant-heartbeat.txt'),'D')
    Start-Sleep -Milliseconds 30
}
'@

$cases = @()
try {
    $MaximumMegabytes = 32 # Dummy argument only; no ETW session exists.
    foreach ($case in @('exit0','exit7','large-exit0','large-exit7',
        'arguments-spaces-trailing-separator','arguments-apostrophe-trailing-separator','timeout-tree','exit0-tree')) {
        $fixtureDirectory = Join-Path $temporaryRoot $case
        if ($case -eq 'arguments-apostrophe-trailing-separator') {
            $fixtureDirectory = Join-Path $temporaryRoot "fixture with spaces and ' apostrophe"
        }
        [IO.Directory]::CreateDirectory($fixtureDirectory) | Out-Null
        $wrapper = Join-Path $fixtureDirectory 'Dummy Controller.ps1'
        [IO.File]::WriteAllText($wrapper,$dummyController,[Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText((Join-Path $fixtureDirectory 'Dummy Descendant.ps1'),$dummyDescendant,[Text.UTF8Encoding]::new($false))
        $TestId = 'dummy-' + ($cases.Count + 1) + '_Case'
        $expectedSession = 'Samsung-Standby-DxgKrnl-' + $TestId
        $OutputDirectory = Join-Path $fixtureDirectory 'output'
        if ($case -like 'arguments-*') {
            $OutputDirectory = (Join-Path $fixtureDirectory 'output with spaces') + [IO.Path]::DirectorySeparatorChar
        }
        [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
        $expected = [ordered]@{outputDirectory=$OutputDirectory;testId=$TestId;mode=$case}
        [IO.File]::WriteAllText((Join-Path $fixtureDirectory 'expected-arguments.json'),($expected | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
        $report = [ordered]@{ actions=@(); errors=@() }
        $witness = $null
        try {
            if ($case -in @('timeout-tree','exit0-tree')) {
                $witness = [SamsungStandbyDummyTests.ProcessWitness]::new(
                    (Join-Path $fixtureDirectory 'dummy-processes.txt'),(Join-Path $fixtureDirectory 'witness-ready.txt'))
            }
            $caught = $false
            $timeout = if ($case -eq 'timeout-tree') { 5 } else { 15 }
            try { $null = Invoke-OwnedController 'Start' $timeout } catch { $caught = $true }
            if ($report.actions.Count -ne 1) { throw "Unexpected action count: $case" }
            $entry = $report.actions[0]
            if (-not $entry.jobAssigned -or -not $entry.gateReleased -or -not $entry.treeTerminationConfirmed -or
                $entry.remainingJobProcesses -ne 0 -or $entry.jobCleanupError) {
                throw "Job ownership or complete process-tree cleanup was not confirmed: $case"
            }
            if (-not $entry.stdoutComplete -or -not $entry.stderrComplete -or $report.errors.Count -ne 0) {
                throw "Incomplete output capture or persistence failure: $case"
            }
            $outText = [IO.File]::ReadAllText((Join-Path $OutputDirectory 'dxgkrnl-host-child-start.stdout.txt'))
            $errText = [IO.File]::ReadAllText((Join-Path $OutputDirectory 'dxgkrnl-host-child-start.stderr.txt'))
            if ($case -in @('timeout-tree','exit0-tree')) {
                if (-not $witness.Ready.WaitOne(1000) -or $witness.Failure -or
                    $null -eq $witness.Controller -or $null -eq $witness.Descendant) {
                    throw "Dummy witness did not retain both process identities: $case"
                }
                if ($entry.pid -ne $witness.Controller.Id -or -not $witness.Controller.WaitForExit(2000) -or
                    -not $witness.Descendant.WaitForExit(2000)) {
                    throw "Controller or descendant survived launcher cleanup: $case"
                }
                $controllerExit = $witness.Controller.ExitCode
                $descendantExit = $witness.Descendant.ExitCode
                if ($null -eq $entry.exitCode -or $entry.exitCode -ne $controllerExit) {
                    throw "Controller exit code was not recorded exactly: $case"
                }
                $heartbeatPath = Join-Path $fixtureDirectory 'descendant-heartbeat.txt'
                $before = [IO.File]::ReadAllText($heartbeatPath)
                Start-Sleep -Milliseconds 400
                $after = [IO.File]::ReadAllText($heartbeatPath)
                if ($before.Length -lt 5 -or $after -cne $before) {
                    throw "Descendant did not run, or kept writing after cleanup: $case"
                }
                if (-not $outText.Contains('dummy-descendant-stdout-open') -or
                    -not $errText.Contains('dummy-descendant-stderr-open')) {
                    throw "Descendant did not exercise the inherited output streams: $case"
                }
                if ($case -eq 'timeout-tree') {
                    if (-not $caught -or -not $entry.timedOut -or $entry.verified -or
                        -not $entry.childTerminationConfirmed -or -not $entry.treeTerminationAttempted) {
                        throw 'Timeout did not reject and confirm termination of the dummy tree.'
                    }
                    $controllerHeartbeat = [IO.File]::ReadAllText((Join-Path $fixtureDirectory 'controller-heartbeat.txt'))
                    if ($controllerHeartbeat.Length -lt 2) { throw 'Timed-out controller was not active before termination.' }
                } elseif (-not $caught -or $entry.timedOut -or $entry.verified -or $controllerExit -ne 0 -or
                    -not $entry.unexpectedDescendants -or -not $entry.treeTerminationAttempted) {
                    throw 'Premature successful controller exit was not rejected and cleaned up.'
                }
                $cases += [ordered]@{
                    case=$case; exitCode=$controllerExit; accepted=$entry.verified; timedOut=$entry.timedOut
                    controllerExited=$true; descendantExited=$true; descendantExitCode=$descendantExit
                    identitiesRetainedBeforeExit=$true; writesStableAfterCleanup=$true
                    descendantHeldOutputStreams=$true; remainingJobProcesses=$entry.remainingJobProcesses
                    argumentsExact=$true; passed=$true
                }
            } else {
                $expectedCode = if ($case -match 'exit7') { 7 } else { 0 }
                if ($null -eq $entry.exitCode -or $entry.exitCode -ne $expectedCode) {
                    throw "Incorrect or missing process exit code: $case"
                }
                if ($caught -ne ($expectedCode -ne 0) -or $entry.verified -ne ($expectedCode -eq 0)) {
                    throw "Incorrect acceptance/rejection: $case"
                }
                if ($entry.result.success -ne $true -or $entry.result.argumentsExact -ne $true) {
                    throw "Dummy success/argument fixture was not loaded: $case"
                }
                $outPayload = ''; $errPayload = ''
                if ($case -match 'large') { $outPayload = 'O' * 131072; $errPayload = 'E' * 131072 }
                $expectedOut = 'dummy-stdout-begin-' + $expectedCode + [Environment]::NewLine + $outPayload +
                    'dummy-stdout-end-' + $expectedCode + [Environment]::NewLine
                $expectedErr = 'dummy-stderr-begin-' + $expectedCode + [Environment]::NewLine + $errPayload +
                    'dummy-stderr-end-' + $expectedCode + [Environment]::NewLine
                if ($outText -cne $expectedOut -or $errText -cne $expectedErr) {
                    throw "Output content differed or was truncated: $case"
                }
                $cases += [ordered]@{
                    case=$case; exitCode=$entry.exitCode; accepted=$entry.verified
                    stdoutChars=$outText.Length; stderrChars=$errText.Length; argumentsExact=$true
                    childPowerShellVersion=$entry.result.dummyPowerShellVersion
                    remainingJobProcesses=$entry.remainingJobProcesses; passed=$true
                }
            }
        } finally {
            if ($null -ne $witness) { $witness.Dispose() }
        }
    }
} finally {
    # Delete only this invocation's generated temporary fixture directory.
    $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
    $resolvedParent = [IO.Directory]::GetParent($resolvedRoot).FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not [String]::Equals($resolvedParent,$temporaryParent,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedRoot) -ne $temporaryLeaf -or
        $temporaryLeaf -notmatch '^samsung-standby-dummy-[0-9a-f]{32}$') {
        throw 'Temporary cleanup target failed its containment check.'
    }
    [IO.Directory]::Delete($resolvedRoot,$true)
}

# Public output excludes temporary paths, user names, PIDs and device identifiers.
[ordered]@{
    test='Test-DxgKrnlHostProcess'; parentPowerShellVersion=$PSVersionTable.PSVersion.ToString()
    hostScriptSha256=(Get-FileHash -LiteralPath $HostScript -Algorithm SHA256).Hash.ToLowerInvariant()
    passed=$cases.Count; cases=$cases; hostExecuted=$false; etwAccessed=$false
    displayAccessed=$false; uacRequested=$false; temporaryFixturesRemoved=$true
} | ConvertTo-Json -Depth 8
