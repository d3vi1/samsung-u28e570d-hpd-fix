#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Compile the listener source, but call only its filesystem-only publication helper.
# The host entry point and all display/notification P/Invoke methods remain unused.
$listenerSource = Join-Path $PSScriptRoot 'StandbyCapture.cs'
$hostSource = Join-Path $PSScriptRoot 'Run-DxgKrnlHost.ps1'
$readerSource = Join-Path $PSScriptRoot 'Standby-Capture.ps1'
Add-Type -Path $listenerSource
$tokens = $null; $parseErrors = $null
$hostAst = [Management.Automation.Language.Parser]::ParseFile($hostSource,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Host source did not parse.' }
$saveFunctions = @($hostAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Save-HostJson'
},$false))
if ($saveFunctions.Count -ne 1) { throw 'Expected one Save-HostJson function.' }
. ([ScriptBlock]::Create($saveFunctions[0].Extent.Text))

$tokens = $null; $parseErrors = $null
$readerAst = [Management.Automation.Language.Parser]::ParseFile($readerSource,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'Capture launcher source did not parse.' }
$readerFunctions = @($readerAst.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Read-CaptureJson'
},$false))
if ($readerFunctions.Count -ne 1) { throw 'Expected one production Read-CaptureJson function.' }
$readerDefinition = $readerFunctions[0].Extent.Text
# Each observer runs the exact production function in a separate runspace.
# Neither the launcher body nor CaptureHost is executed.
$observerCode = 'param($Path,$ExpectedIndex,$ExpectedPayload,$Ready)' + [Environment]::NewLine +
    $readerDefinition + [Environment]::NewLine + @'
$ErrorActionPreference = 'Stop'
$timer = [Diagnostics.Stopwatch]::StartNew()
$null = $Ready.Set()
while (-not [IO.File]::Exists($Path)) {
    if ($timer.ElapsedMilliseconds -gt 10000) { throw 'Publication was not observed.' }
    [Threading.Thread]::Sleep(0)
}
$value = Read-CaptureJson $Path
if ($value.sequence -ne $ExpectedIndex -or $value.payload -cne $ExpectedPayload -or $value.complete -ne 'end-marker') {
    throw 'The first production read did not expose the complete expected JSON.'
}
$true
'@

$utf8 = [Text.UTF8Encoding]::new($false)
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$temporaryLeaf = 'standby-atomic-test-' + [Guid]::NewGuid().ToString('N')
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent $temporaryLeaf))
if (Test-Path -LiteralPath $temporaryRoot) { throw 'Temporary test directory already exists.' }
[IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
$cases = @()
$appearanceCount = 12
function Publish-TestValue([string]$Implementation,[string]$Path,$Value) {
    if ($Implementation -eq 'listener') {
        [WindowsStandby.AtomicPublication]::WriteJsonNew($Path,($Value | ConvertTo-Json -Depth 16))
    } else { Save-HostJson $Path $Value }
}
function Assert-NoTemporaryFiles([string]$Directory) {
    if ([IO.Directory]::GetFiles($Directory,'*.tmp').Length -ne 0) { throw 'Owned temporary files were not cleaned up.' }
}
try {
    foreach ($implementation in @('listener','host')) {
        $directory = Join-Path $temporaryRoot $implementation
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $path = Join-Path $directory 'complete.json'
        $value = [ordered]@{ text=('line one' + [char]10 + [char]0x2192); ready=$true; sequence=7 }
        Publish-TestValue $implementation $path $value
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) { throw 'Unexpected UTF-8 BOM.' }
        $decoded = $utf8.GetString($bytes) | ConvertFrom-Json
        if ($decoded.text -cne $value.text -or $decoded.ready -ne $true -or $decoded.sequence -ne 7) { throw 'Published JSON differed.' }
        $cases += [ordered]@{ implementation=$implementation; case='complete-utf8-json-without-bom'; passed=$true }

        $originalHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        $rejected = $false
        try { Publish-TestValue $implementation $path ([ordered]@{replacement=$true}) } catch { $rejected = $true }
        if (-not $rejected -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $originalHash) { throw 'Existing destination was overwritten.' }
        Assert-NoTemporaryFiles $directory
        $cases += [ordered]@{ implementation=$implementation; case='overwrite-rejected-original-preserved'; passed=$true }

        $occupied = Join-Path $directory 'occupied.json'
        [IO.Directory]::CreateDirectory($occupied) | Out-Null
        $sentinel = Join-Path $occupied 'sentinel.txt'
        [IO.File]::WriteAllText($sentinel,'preserve',$utf8)
        $foreignTemporary = Join-Path $directory 'foreign-sentinel.tmp'
        [IO.File]::WriteAllText($foreignTemporary,'foreign-preserve',$utf8)
        $rejected = $false
        try { Publish-TestValue $implementation $occupied $value } catch { $rejected = $true }
        if (-not $rejected -or [IO.File]::ReadAllText($sentinel) -ne 'preserve') { throw 'Failed rename damaged an existing destination.' }
        if ([IO.File]::ReadAllText($foreignTemporary) -ne 'foreign-preserve') { throw 'Failed rename removed a foreign temporary file.' }
        [IO.File]::Delete($foreignTemporary)
        Assert-NoTemporaryFiles $directory
        $cases += [ordered]@{ implementation=$implementation; case='failed-rename-cleans-only-owned-temporary'; passed=$true }

        for ($index=0; $index -lt $appearanceCount; $index++) {
            $path = Join-Path $directory ('appearance-' + $index + '.json')
            $value = [ordered]@{ sequence=$index; payload=('P' * 1048576); complete='end-marker' }
            $ready = [Threading.ManualResetEvent]::new($false)
            $observer = [PowerShell]::Create()
            $async = $null
            try {
                $null = $observer.AddScript($observerCode).AddArgument($path).AddArgument($index).AddArgument($value.payload).AddArgument($ready)
                $async = $observer.BeginInvoke()
                if (-not $ready.WaitOne(5000)) { throw 'Production-reader observer did not start.' }
                Publish-TestValue $implementation $path $value
                if (-not $async.AsyncWaitHandle.WaitOne(10000)) { throw 'Production-reader observer timed out.' }
                $observed = @($observer.EndInvoke($async))
                if ($observer.HadErrors -or $observed.Count -ne 1 -or $observed[0] -ne $true) {
                    throw ('Production-reader observation failed: ' + ($observer.Streams.Error | Out-String))
                }
            } finally {
                if ($null -ne $async -and -not $async.IsCompleted) { $observer.Stop() }
                $observer.Dispose(); $ready.Dispose()
            }
            $decoded = [IO.File]::ReadAllText($path) | ConvertFrom-Json
            if ($decoded.sequence -ne $index -or $decoded.payload.Length -ne 1048576 -or $decoded.complete -ne 'end-marker') { throw 'Final JSON did not parse completely.' }
        }
        Assert-NoTemporaryFiles $directory
        $cases += [ordered]@{ implementation=$implementation; case='first-appearance-is-complete'; publications=$appearanceCount; passed=$true }
    }
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
    $resolvedParent = [IO.Directory]::GetParent($resolvedRoot).FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
    if (-not [String]::Equals($resolvedParent,$temporaryParent,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedRoot) -ne $temporaryLeaf -or
        $temporaryLeaf -notmatch '^standby-atomic-test-[0-9a-f]{32}$') { throw 'Temporary cleanup target failed its containment check.' }
    [IO.Directory]::Delete($resolvedRoot,$true)
}
[ordered]@{
    test='Test-AtomicPublication'; parentPowerShellVersion=$PSVersionTable.PSVersion.ToString()
    listenerSourceSha256=(Get-FileHash -LiteralPath $listenerSource -Algorithm SHA256).Hash.ToLowerInvariant()
    hostSourceSha256=(Get-FileHash -LiteralPath $hostSource -Algorithm SHA256).Hash.ToLowerInvariant()
    readerSourceSha256=(Get-FileHash -LiteralPath $readerSource -Algorithm SHA256).Hash.ToLowerInvariant()
    passed=$cases.Count; cases=$cases; concurrentPublications=($appearanceCount*2)
    listenerStarted=$false; hostExecuted=$false; etwAccessed=$false; displayAccessed=$false; uacRequested=$false
    temporaryFixturesRemoved=$true
    firstAppearanceReader='Exact production Read-CaptureJson function in a concurrent runspace'
} | ConvertTo-Json -Depth 8
