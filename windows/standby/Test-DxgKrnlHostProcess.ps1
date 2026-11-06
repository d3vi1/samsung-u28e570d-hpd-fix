#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$HostScript
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([String]::IsNullOrWhiteSpace($HostScript)) {
    $HostScript = Join-Path $PSScriptRoot 'Run-DxgKrnlHost.ps1'
}

# Tests only two extracted host functions against an inline dummy controller.
# The host entry point, real ETW controller, display APIs and UAC are never invoked.
$HostScript = (Resolve-Path -LiteralPath $HostScript).Path
$tokens = $null; $parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($HostScript,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Host source did not parse.' }
$functions = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -in @('Save-HostJson','Invoke-OwnedController')
},$false))
if ($functions.Count -ne 2 -or @($functions | Where-Object Name -eq 'Save-HostJson').Count -ne 1 -or
    @($functions | Where-Object Name -eq 'Invoke-OwnedController').Count -ne 1) {
    throw 'Expected exactly the two named host function declarations.'
}
foreach ($function in $functions) { . ([ScriptBlock]::Create($function.Extent.Text)) }

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
$exitValue = if ($TestId -match 'exit7') { 7 } else { 0 }
[Console]::Out.WriteLine('dummy-stdout-begin-' + $exitValue)
[Console]::Error.WriteLine('dummy-stderr-begin-' + $exitValue)
if ($TestId -match 'large') {
    [Console]::Out.Write(('O' * 131072))
    [Console]::Error.Write(('E' * 131072))
}
[Console]::Out.WriteLine('dummy-stdout-end-' + $exitValue)
[Console]::Error.WriteLine('dummy-stderr-end-' + $exitValue)
# Deliberately claim success even for exit 7: the parent must also check the real exit code.
$result = [ordered]@{
    action=$Action; utc=[DateTime]::UtcNow.ToString('o')
    sessionName=('Samsung-Standby-DxgKrnl-' + $TestId); success=$true
    dummyPowerShellVersion=$PSVersionTable.PSVersion.ToString()
}
$name = 'dxgkrnl-' + $Action.ToLowerInvariant() + '-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ') + '-' + $PID + '.json'
[IO.File]::WriteAllText((Join-Path $OutputDirectory $name),($result | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
exit $exitValue
'@

$cases = @()
try {
    $wrapper = Join-Path $temporaryRoot 'DummyController.ps1'
    [IO.File]::WriteAllText($wrapper,$dummyController,[Text.UTF8Encoding]::new($false))
    $MaximumMegabytes = 32 # Passed through to the dummy only; no ETW session exists.
    foreach ($case in @('exit0','exit7','large-exit0','large-exit7')) {
        $TestId = 'dummy-' + $case
        $expectedSession = 'Samsung-Standby-DxgKrnl-' + $TestId
        $OutputDirectory = Join-Path $temporaryRoot $case
        [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
        $report = [ordered]@{ actions=@(); errors=@() }
        $caught = $false
        try { $null = Invoke-OwnedController 'Start' 15 } catch { $caught = $true }
        if ($report.actions.Count -ne 1) { throw "Unexpected action count: $case" }
        $entry = $report.actions[0]
        $expectedCode = if ($case -match 'exit7') { 7 } else { 0 }
        if ($null -eq $entry.exitCode -or $entry.exitCode -ne $expectedCode) {
            throw "Incorrect or missing process exit code: $case"
        }
        if ($caught -ne ($expectedCode -ne 0) -or $entry.verified -ne ($expectedCode -eq 0)) {
            throw "Incorrect acceptance/rejection: $case"
        }
        if ($entry.result.success -ne $true) { throw "Dummy success fixture was not loaded: $case" }
        if (-not $entry.stdoutComplete -or -not $entry.stderrComplete -or $report.errors.Count -ne 0) {
            throw "Incomplete output capture or persistence failure: $case"
        }
        $outText = [IO.File]::ReadAllText((Join-Path $OutputDirectory 'dxgkrnl-host-child-start.stdout.txt'))
        $errText = [IO.File]::ReadAllText((Join-Path $OutputDirectory 'dxgkrnl-host-child-start.stderr.txt'))
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
            stdoutChars=$outText.Length; stderrChars=$errText.Length
            childPowerShellVersion=$entry.result.dummyPowerShellVersion; passed=$true
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

# Public output intentionally excludes temporary paths, user names, PIDs and device identifiers.
[ordered]@{
    test='Test-DxgKrnlHostProcess'; parentPowerShellVersion=$PSVersionTable.PSVersion.ToString()
    hostScriptSha256=(Get-FileHash -LiteralPath $HostScript -Algorithm SHA256).Hash.ToLowerInvariant()
    passed=$cases.Count; cases=$cases; hostExecuted=$false; etwAccessed=$false
    displayAccessed=$false; uacRequested=$false; temporaryFixturesRemoved=$true
} | ConvertTo-Json -Depth 8
