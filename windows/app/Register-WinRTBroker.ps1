#requires -Version 3.0
[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [Parameter(Mandatory=$true)][string]$BrokerPath,
    [Parameter(Mandatory=$true)][ValidatePattern('^MonitorFirmware\.WinRT_[a-z0-9]{13}$')][string]$PackageFamilyName
)
$ErrorActionPreference = 'Stop'
$resolvedBroker = (Resolve-Path -LiteralPath $BrokerPath).Path
if ([IO.Path]::GetExtension($resolvedBroker) -ne '.exe' -or $resolvedBroker.Contains('"')) { throw 'Choose the compiled desktop broker executable.' }
$installed = @(Get-AppxPackage -Name 'MonitorFirmware.WinRT' | Where-Object { $_.PackageFamilyName -ceq $PackageFamilyName })
if ($installed.Count -ne 1) { throw 'The exact WinRT package must already be explicitly deployed for this user.' }
$scheme = 'HKCU:\Software\Classes\monitorfw-bridge'
if (Test-Path -LiteralPath $scheme) { throw 'Protocol already exists. Review its owner before changing registration.' }
if ($PSCmdlet.ShouldProcess($PackageFamilyName,'Register the current-user monitorfw-bridge desktop handler')) {
    New-Item -Path 'HKCU:\Software\MonitorFirmware\WinRTBroker' -Force | Out-Null
    New-ItemProperty -Path 'HKCU:\Software\MonitorFirmware\WinRTBroker' -Name PackageFamilyName -Value $PackageFamilyName -PropertyType String -Force | Out-Null
    New-Item -Path $scheme | Out-Null
    Set-Item -LiteralPath $scheme -Value 'URL:Monitor firmware desktop broker'
    New-ItemProperty -Path $scheme -Name 'URL Protocol' -Value '' -PropertyType String | Out-Null
    New-Item -Path ($scheme + '\shell\open\command') -Force | Out-Null
    Set-Item -LiteralPath ($scheme + '\shell\open\command') -Value ('"' + $resolvedBroker + '" --protocol "%1"')
}
