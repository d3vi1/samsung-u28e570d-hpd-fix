#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CandidatePath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('Validate','Forward','Rollback')][string]$Mode = 'Validate',
    [string]$BaselinePath,
    [string]$OfficialPath
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
if (-not $BaselinePath) { $BaselinePath = Join-Path $workspace 'inputs\baseline-1004.5-full.bin' }
if (-not $OfficialPath) { $OfficialPath = Join-Path $workspace 'inputs\official-1004.5.bin' }
$sources = @('FirmwarePackage.cs','FlashTransition.cs','ExperimentalPreparation.cs') | ForEach-Object { Join-Path $PSScriptRoot $_ }
if ($Mode -ne 'Validate') {
    $sources += Join-Path $workspace 'NvidiaMstarRead.cs'
    $sources += Join-Path $workspace 'NvidiaMstarSpi.cs'
    $sources += Join-Path $PSScriptRoot 'NvidiaExperimentalAdapter.cs'
}
Add-Type -Path $sources
$prepared = [SamsungExperimental.ExperimentPreparation]::PrepareFiles(
    (Resolve-Path -LiteralPath $BaselinePath).Path,
    (Resolve-Path -LiteralPath $OfficialPath).Path,
    (Resolve-Path -LiteralPath $CandidatePath).Path)
if ($Mode -eq 'Validate') {
    $prepared.SaveImages([IO.Path]::GetFullPath($OutputDirectory))
    $summary = [ordered]@{
        mode = 'offline validation'
        hardwareEnabled = [SamsungExperimental.ExperimentalPolicy]::HardwareEnabled
        candidatePackageSha256 = [SamsungExperimental.ExperimentalPolicy]::CandidatePackageSha256
        targetFullSha256 = $prepared.TargetSha256
        package = $prepared.PackageReport
        forward = $prepared.ForwardPlan.ToSummary()
        reverse = $prepared.ReversePlan.ToSummary()
    }
    $json = $summary | ConvertTo-Json -Depth 12
    $json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'validation-and-plans.json') -Encoding utf8
    $json
    return
}
# The compiled policy gate is checked before the NVIDIA object is constructed.
[SamsungExperimental.ExperimentalPolicy]::RequireHardwareApproval($prepared)
$result = [NvidiaMstarRead]::RunExperimental10050($prepared, ($Mode -eq 'Rollback'), $OutputDirectory)
$result | ConvertTo-Json -Depth 6
if (-not $result.TransitionSucceeded -or -not $result.PostBootHealthy -or $result.Error) {
    throw 'Experimental transition did not complete with verified boot health. Inspect its result and recovery state.'
}
