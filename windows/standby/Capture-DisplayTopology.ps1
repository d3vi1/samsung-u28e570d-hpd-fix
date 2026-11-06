#requires -Version 5.1
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)][ValidateSet('BeforeStandby','AfterWake')][string]$Phase,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [switch]$DisplaysAreActive
)
$ErrorActionPreference = 'Stop'
if (-not $DisplaysAreActive) { throw 'Run only before standby or after deliberate wake, with displays active; acknowledge this using -DisplaysAreActive.' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $OutputPath) { throw 'Use a new topology output file; snapshots are never overwritten.' }
if (-not $PSCmdlet.ShouldProcess('Windows display topology', "Take one $Phase snapshot while displays are active")) { return }
if (-not ('WindowsStandbyTopology.DisplayTopology' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'DisplayTopology.cs') }
$snapshot = [WindowsStandbyTopology.DisplayTopology]::CaptureOnce($Phase, $true)
$json = $snapshot | ConvertTo-Json -Depth 10
$null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputPath))
$stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush($true)
} finally { $stream.Dispose() }
$snapshot
