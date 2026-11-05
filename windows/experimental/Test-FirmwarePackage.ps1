[CmdletBinding()]
param([string]$Workspace = '')
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Workspace)) { $Workspace = Split-Path -Parent $PSScriptRoot }
if (-not ('SamsungExperimental.FirmwarePackage' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'FirmwarePackage.cs')
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Get-PackagePrefix([string]$RelativePath) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $Workspace $RelativePath))
    $length = [BitConverter]::ToUInt32($bytes, 0x10020) + 4
    Assert-True ($length -le $bytes.Length) "Invalid fixture package length: $RelativePath"
    $prefix = New-Object byte[] $length
    [Array]::Copy($bytes, $prefix, $length)
    return ,$prefix
}
function Assert-Rejected([byte[]]$Bytes, [string]$ErrorPrefix, [string]$Label) {
    $report = [SamsungExperimental.FirmwarePackage]::Validate($Bytes, $script:official)
    Assert-True (-not $report.IsValid) "$Label was unexpectedly accepted."
    Assert-True (@($report.Errors | Where-Object { $_.StartsWith($ErrorPrefix) }).Count -gt 0) "$Label did not report $ErrorPrefix; errors: $($report.Errors -join '; ')"
}
function Set-Le32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 4)
}
function Set-Be16([byte[]]$Bytes, [int]$Offset, [uint16]$Value) {
    $Bytes[$Offset] = [byte]($Value -shr 8)
    $Bytes[$Offset + 1] = [byte]($Value -band 255)
}
function Set-Be32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    $Bytes[$Offset] = [byte](($Value -shr 24) -band 255)
    $Bytes[$Offset + 1] = [byte](($Value -shr 16) -band 255)
    $Bytes[$Offset + 2] = [byte](($Value -shr 8) -band 255)
    $Bytes[$Offset + 3] = [byte]($Value -band 255)
}
function Repair-Integrity([byte[]]$Bytes) {
    $mainEnd = [int][BitConverter]::ToUInt32($Bytes, 0x1000C)
    $crcOffset = [int][BitConverter]::ToUInt32($Bytes, 0x10020)
    Set-Be16 $Bytes ($mainEnd - 2) ([SamsungExperimental.FirmwarePackage]::ComputeSamsungCrc16($Bytes, 0x10000, $mainEnd - 2 - 0x10000))
    Set-Be16 $Bytes ($crcOffset - 2) ([SamsungExperimental.FirmwarePackage]::ComputeSamsungCrc16($Bytes, 0x10000, $crcOffset - 2 - 0x10000))
    Set-Le32 $Bytes 0x10014 ([SamsungExperimental.FirmwarePackage]::ComputeCrc32($Bytes, 0x20080, $mainEnd - 0x20080))
    Set-Be32 $Bytes $crcOffset ([SamsungExperimental.FirmwarePackage]::ComputeCrc32($Bytes, 0, $crcOffset))
}

$script:official = Get-PackagePrefix 'inputs\official-1004.5.bin'
$fixtures = @(
    @{ Version = '1002.0'; Path = 'inputs\fixtures\1002.0.bin'; Expanded = 1257000; Hash = 'c3b9b12a721858cfd99aaa0ae55a07a30b7cee11f16da4ad683ddb60c9df3186'; Inner = 0x5C2B; Sum = 0x5DD3 },
    @{ Version = '1003.1'; Path = 'inputs\fixtures\1003.1.bin'; Expanded = 1257000; Hash = '627c2089d0c49f3641e51c6b518fa68ed79df49f34987560f25876fe50d1ae9b'; Inner = 0x2025; Sum = 0x78FA },
    @{ Version = '1004.5'; Path = 'inputs\official-1004.5.bin'; Expanded = 1276176; Hash = '43d99a1e97a11735a95b5547fa0cbe297100753481c453876d970d747bc442d4'; Inner = 0x95D5; Sum = 0x48D0 }
)
foreach ($fixture in $fixtures) {
    $bytes = Get-PackagePrefix $fixture.Path
    $report = [SamsungExperimental.FirmwarePackage]::Validate($bytes, $script:official)
    Assert-True $report.IsValid "$($fixture.Version): $($report.Errors -join '; ')"
    Assert-True ($report.Version -eq "m-E5728UMDB-$($fixture.Version)") 'Version mismatch.'
    Assert-True ($report.ExpandedLength -eq $fixture.Expanded -and $report.ExpandedSha256 -eq $fixture.Hash) 'Expanded fixture mismatch.'
    Assert-True ($report.InnerCrcExpected -eq $fixture.Inner -and $report.OuterCrcExpected -eq 0xBBB7) 'CRC16 fixture mismatch.'
    Assert-True ($report.Sum16 -eq $fixture.Sum -and $report.PackageCrcRemainder -eq 0) 'Package checksum fixture mismatch.'
    Assert-True ($report.BootPreserved -and $report.Pm51Preserved) 'Boot/PM51 preservation mismatch.'
    "PASS known firmware $($fixture.Version)"
}

$changed = [byte[]]$script:official.Clone(); $changed[0x10014] = $changed[0x10014] -bxor 1
Assert-Rejected $changed 'MainCRC32:' 'AP CRC mutation'
$report = [SamsungExperimental.FirmwarePackage]::Validate($changed, $script:official)
Assert-True ($report.InnerCrcExpected -eq $report.InnerCrcComputed -and $report.OuterCrcExpected -eq $report.OuterCrcComputed) 'CRC16 must zero AP CRC bytes.'
$changed = [byte[]]$script:official.Clone(); $changed[0x8D4A8] = $changed[0x8D4A8] -bxor 1
Assert-Rejected $changed 'InnerCRC16:' 'Inner CRC mutation'
$changed = [byte[]]$script:official.Clone(); $changed[0x8D4B6] = $changed[0x8D4B6] -bxor 1
Assert-Rejected $changed 'OuterCRC16:' 'Outer CRC mutation'
$changed = [byte[]]$script:official.Clone(); $changed[$changed.Length - 1] = $changed[$changed.Length - 1] -bxor 1
Assert-Rejected $changed 'PackageCRC32:' 'Final CRC mutation'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x1000C ([uint32]::MaxValue)
Assert-Rejected $changed 'MainBounds:' 'Overflowed main end'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x10020 ([uint32]::MaxValue)
Assert-Rejected $changed 'PackageLength:' 'Overflowed package end'
$changed = New-Object byte[] ($script:official.Length - 1); [Array]::Copy($script:official, $changed, $changed.Length)
Assert-Rejected $changed 'PackageLength:' 'Truncated package'
$changed = [byte[]]$script:official.Clone(); $changed[0x8D4AA] = 1; Repair-Integrity $changed
Assert-Rejected $changed 'AlignmentPadding:' 'CRC-valid nonzero alignment padding'
$changed = [byte[]]$script:official.Clone(); $changed[0x8D4B0] = 0; Repair-Integrity $changed
Assert-Rejected $changed 'PackageMarker:' 'CRC-valid bad marker'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x211CA 1; Repair-Integrity $changed
Assert-Rejected $changed 'EmbeddedMainLength:' 'CRC-valid incorrect embedded main length'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x8D4A4 ([uint32]::MaxValue); Repair-Integrity $changed
Assert-Rejected $changed 'ExpandedSizeLimit:' 'CRC-valid excessive expanded length'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x8D4A4 1; Repair-Integrity $changed
Assert-Rejected $changed 'Deflate:' 'CRC-valid understated expanded length'
$changed = [byte[]]$script:official.Clone(); Set-Le32 $changed 0x8D4A4 1276177; Repair-Integrity $changed
Assert-Rejected $changed 'ExpandedLength:' 'CRC-valid overstated expanded length'
$changed = [byte[]]$script:official.Clone(); $changed[0x23880] = ($changed[0x23880] -band 248) -bor 7; Repair-Integrity $changed
Assert-Rejected $changed 'Deflate:' 'CRC-valid reserved DEFLATE block type'
$changed = [byte[]]$script:official.Clone()
[Array]::Copy($changed, 0x8D4A4, $changed, 0x8D4A5, 6); $changed[0x8D4A4] = 0
Set-Le32 $changed 0x1000C 0x8D4AB; Set-Le32 $changed 0x10008 0x26D42B; Set-Le32 $changed 0x211CA 0x6D42B
Repair-Integrity $changed
Assert-Rejected $changed 'DeflateBoundary:' 'CRC-valid unused byte before DEFLATE trailer'
$changed = [byte[]]$script:official.Clone()
[Array]::Copy($changed, 0x8D4A4, $changed, 0x8D4A3, 6); $changed[0x8D4A9] = 0
Set-Le32 $changed 0x1000C 0x8D4A9; Set-Le32 $changed 0x10008 0x26D429; Set-Le32 $changed 0x211CA 0x6D429
Repair-Integrity $changed
Assert-Rejected $changed 'DeflateTruncated:' 'CRC-valid truncated DEFLATE stream'
$changed = [byte[]]$script:official.Clone(); $changed[0] = $changed[0] -bxor 1; Repair-Integrity $changed
Assert-Rejected $changed 'BootChanged:' 'CRC-valid bootstrap mutation'
$changed = [byte[]]$script:official.Clone(); $changed[0x10080] = $changed[0x10080] -bxor 1; Repair-Integrity $changed
Assert-Rejected $changed 'Pm51Changed:' 'CRC-valid PM51 mutation'
$badReference = [byte[]]$script:official.Clone(); $badReference[0] = $badReference[0] -bxor 1
$report = [SamsungExperimental.FirmwarePackage]::Validate($script:official, $badReference)
Assert-True (-not $report.IsValid -and @($report.Errors | Where-Object { $_.StartsWith('OfficialReference:') }).Count -gt 0) 'Unpinned reference accepted.'
'PASS 19 corruption/bounds/reference cases'
'All tests are offline; no monitor or flash interfaces are instantiated.'
