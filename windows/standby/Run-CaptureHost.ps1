#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][ValidatePattern('^WindowsStandby-[0-9a-f]{32}$')][string]$PipeName
)
$ErrorActionPreference = 'Stop'
try {
    Add-Type -Path (Join-Path $PSScriptRoot 'StandbyCapture.cs')
    [WindowsStandby.CaptureHost]::Run($Directory, $PipeName)
} catch {
    $_ | Out-String | Set-Content -LiteralPath (Join-Path $Directory 'host-error.txt') -Encoding UTF8
    throw
}
