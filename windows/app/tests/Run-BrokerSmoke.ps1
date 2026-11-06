#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$BrokerExe)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$BrokerExe = (Resolve-Path -LiteralPath $BrokerExe).Path
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$temporaryLeaf = 'monitorfw-broker-smoke-' + [Guid]::NewGuid().ToString('N')
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path $temporaryParent $temporaryLeaf))
if (Test-Path -LiteralPath $temporaryRoot) { throw 'The broker smoke directory must not exist.' }
$process = $null
try {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $BrokerExe
    # The generated root cannot contain quotes and has no trailing separator.
    $info.Arguments = '--self-test "' + $temporaryRoot + '"'
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $info
    if (-not $process.Start()) { throw 'The mock broker did not start.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(45000)) {
        $process.Kill(); [void]$process.WaitForExit(5000)
        throw 'The mock broker exceeded its test timeout.'
    }
    $outText = $stdout.GetAwaiter().GetResult(); $errText = $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw ('The mock broker failed with exit code ' + $process.ExitCode) }
    $result = Get-Content -LiteralPath (Join-Path $temporaryRoot 'self-test.json') -Raw | ConvertFrom-Json
    if ($result.passed -ne 14 -or $result.replaysRejected -ne 14 -or
        $result.validOwnBundleOperations -ne 2 -or $result.invalidBundlesRejected -ne 8 -or
        $result.hardwareAccessed -ne $false) {
        throw 'The broker self-test result did not match its mock-only contract.'
    }
    [ordered]@{
        test='broker_mock_smoke'; passed=$result.passed; replaysRejected=$result.replaysRejected
        validOwnBundleOperations=$result.validOwnBundleOperations; invalidBundlesRejected=$result.invalidBundlesRejected
        hardwareAccessed=$false; registryAccessed=$false; uacRequested=$false
        brokerSha256=(Get-FileHash -LiteralPath $BrokerExe -Algorithm SHA256).Hash.ToLowerInvariant()
        stdoutChars=$outText.Length; stderrChars=$errText.Length
    } | ConvertTo-Json
} finally {
    if ($null -ne $process) { $process.Dispose() }
    if (Test-Path -LiteralPath $temporaryRoot) {
        $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
        $resolvedParent = [IO.Directory]::GetParent($resolvedRoot).FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
        if (-not [String]::Equals($resolvedParent,$temporaryParent,[StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedRoot) -ne $temporaryLeaf -or
            $temporaryLeaf -notmatch '^monitorfw-broker-smoke-[0-9a-f]{32}$') {
            throw 'Broker smoke cleanup target failed containment checks.'
        }
        $pending = [Collections.Generic.Queue[string]]::new(); $pending.Enqueue($resolvedRoot)
        while ($pending.Count -gt 0) {
            $directory = $pending.Dequeue()
            $item = Get-Item -LiteralPath $directory -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in mock cleanup target.' }
            foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
                if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in mock cleanup child.' }
                if ($child.PSIsContainer) { $pending.Enqueue($child.FullName) }
            }
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
