#requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$sources = @('FirmwarePackage.cs','FlashTransition.cs','ExperimentalPreparation.cs','NvidiaExperimentalAdapter.cs') |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
$sources += Join-Path $workspace 'NvidiaMstarRead.cs'
$sources += Join-Path $workspace 'NvidiaMstarSpi.cs'
Add-Type -Path $sources
Add-Type -TypeDefinition 'public sealed class ThrowingExperimentalDisposable : System.IDisposable { public void Dispose() { throw new System.IO.IOException("injected close failure"); } }'
$baseline = [IO.File]::ReadAllBytes((Join-Path $workspace 'inputs\baseline-1004.5-full.bin'))
$official = [IO.File]::ReadAllBytes((Join-Path $workspace 'inputs\official-1004.5.bin'))
$candidate = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'artifacts\candidate-1005.0-zlib-1.3.2.bin'))
$passed = [Collections.Generic.List[string]]::new()
function New-Prepared { [SamsungExperimental.ExperimentPreparation]::Prepare($baseline,$official,$candidate) }
function Assert-Case([bool]$condition,[string]$name) {
    if (-not $condition) { throw "FAILED: $name" }
    $passed.Add($name)
}
function Assert-Rejected([scriptblock]$action,[string]$name) {
    $rejected = $false
    try { & $action | Out-Null } catch { $rejected = $true }
    Assert-Case $rejected $name
}
$prepared = New-Prepared
$plans = [SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared)
Assert-Case ($prepared.PackageReport.IsValid -and $prepared.PackageReport.Warnings.Count -eq 0) 'Exact reproduced candidate validates independently'
Assert-Case ($plans[0].ChangedSectors.Count -eq 109 -and $plans[1].ChangedSectors.Count -eq 109 -and
    $plans[0].ChangedSectors[-1].Address -eq 0x10000 -and $plans[1].ChangedSectors[-1].Address -eq 0x10000) 'Both pinned plans contain 109 sectors with header last'
$prepared.ForwardPlan.TargetFull[0x25000] = $prepared.ForwardPlan.TargetFull[0x25000] -bxor 1
Assert-Rejected { [SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared) } 'Altered target array is rejected before a hardware plan is returned'
Assert-Case ([SamsungExperimental.FlashTransition]::Sha256($plans[0].TargetFull) -eq [SamsungExperimental.ExperimentalPolicy]::ApprovedTargetSha256) 'Returned execution snapshots are isolated from later DTO changes'
$prepared = New-Prepared
$prepared.ForwardPlan.ChangedSectors[0].Address = 0
Assert-Rejected { [SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared) } 'Altered sector list is rejected before hardware'
$prepared = New-Prepared
$prepared.ReversePlan.SourceFull[0x1FFFFF] = 0
Assert-Rejected { [SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared) } 'Altered reverse source tail is rejected before hardware'
$prepared = New-Prepared
$prepared.ForwardPlan.ApprovedRanges = @([SamsungExperimental.AddressRange]::new(0,2097152))
Assert-Rejected { [SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared) } 'Expanded allowed range changes the pin and is rejected'
$externalBaseline = [byte[]]$baseline.Clone()
$prepared = [SamsungExperimental.ExperimentPreparation]::Prepare($externalBaseline,$official,$candidate)
$externalBaseline[0] = $externalBaseline[0] -bxor 1
Assert-Case (([SamsungExperimental.ExperimentalPolicy]::ValidatePreparedPlans($prepared)).Count -eq 2) 'Preparation owns independent snapshots of caller arrays'
$badCandidate = [byte[]]$candidate.Clone()
$badCandidate[-1] = $badCandidate[-1] -bxor 1
Assert-Rejected { [SamsungExperimental.ExperimentPreparation]::Prepare($baseline,$official,$badCandidate) } 'Candidate identity mismatch is rejected'
if ([SamsungExperimental.ExperimentalPolicy]::HardwareEnabled -or [SamsungExperimental.ExperimentalPolicy]::FirstEdidSerial -ne 'LOCAL_PROFILE_REQUIRED') { throw 'Repository tests require the disabled placeholder hardware profile.' }
$unusedOutput = Join-Path ([IO.Path]::GetTempPath()) ('samsung-no-hardware-' + [Guid]::NewGuid().ToString('N'))
Assert-Rejected { [NvidiaMstarRead]::RunExperimental10050($null,$false,$unusedOutput) } 'Disabled repository policy rejects entry before NVIDIA construction'
Assert-Case (-not (Test-Path -LiteralPath $unusedOutput)) 'Rejected production entry creates no run output'
$result = [SamsungExperimental.ExperimentalRunResult]::new()
$flags = [Reflection.BindingFlags]::NonPublic -bor [Reflection.BindingFlags]::Static
$dispose = [NvidiaMstarRead].GetMethod('SafeExperimentalDispose',$flags)
$null = $dispose.Invoke($null,@([ThrowingExperimentalDisposable]::new(),$result,'injected disposal'))
Assert-Case ($result.PersistenceErrors.Count -eq 1) 'Journal disposal failure remains diagnostic and does not escape'
Assert-Case (-not ([SamsungExperimental.ExecutionState]::new()).ResetAllowed) 'A fresh operation cannot reset an unverified image'
[pscustomobject]@{hardwareAccess=$false; passed=$passed.Count; cases=$passed.ToArray()} | ConvertTo-Json -Depth 3
