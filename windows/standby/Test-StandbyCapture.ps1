#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -Path (Join-Path $PSScriptRoot 'StandbyCapture.cs')
$passed = [Collections.Generic.List[string]]::new()
function Assert([bool]$Condition,[string]$Name) {
    if (-not $Condition) { throw ('FAILED: ' + $Name) }
    $passed.Add($Name)
}
function Reject([scriptblock]$Code,[string]$Name) {
    $failed = $false
    try { & $Code | Out-Null } catch { $failed = $true }
    Assert $failed $Name
}
$off = [WindowsStandby.EventDecoder]::Power([WindowsStandby.EventDecoder]::SessionDisplay,[byte[]]@(0,0,0,0))
Assert ($off['state'] -eq 'off' -and $off['setting'] -eq 'SESSION_DISPLAY_STATUS') 'Session off notification decodes'
$dim = [WindowsStandby.EventDecoder]::Power([WindowsStandby.EventDecoder]::ConsoleDisplay,[byte[]]@(2,0,0,0))
Assert ($dim['state'] -eq 'dim') 'Console dim notification decodes'
$legacy = [WindowsStandby.EventDecoder]::Power([WindowsStandby.EventDecoder]::LegacyMonitor,[byte[]]@(2,0,0,0))
Assert ($legacy['state'] -eq 'unknown') 'Legacy power notification does not invent dim support'
$unknown = [WindowsStandby.EventDecoder]::Power([Guid]::Empty,[byte[]]@(1,0,0,0))
Assert ($unknown['state'] -eq 'unknown' -and $unknown['setting'] -eq 'UNKNOWN_POWER_SETTING') 'Unknown setting remains unknown'
$short = [WindowsStandby.EventDecoder]::Power([WindowsStandby.EventDecoder]::SessionDisplay,[byte[]]@(1))
Assert (-not $short.ContainsKey('state')) 'Short power payload is preserved without false state'
Reject { [WindowsStandby.EventDecoder]::Power([Guid]::Empty,$null) } 'Null power payload is rejected'
$display = [WindowsStandby.EventDecoder]::Display(32,([long]2160 -shl 16) -bor 3840)
Assert ($display['width'] -eq 3840 -and $display['height'] -eq 2160 -and $display['bitsPerPixel'] -eq 32) 'Display change words decode correctly'
$deviceName = '\\?\DISPLAY#TEST#PRIVATE-SERIAL' + [char]0
$nameBytes = [Text.Encoding]::Unicode.GetBytes($deviceName)
$payload = [byte[]]::new(28 + $nameBytes.Length)
[BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($payload,0)
[BitConverter]::GetBytes([uint32]5).CopyTo($payload,4)
[WindowsStandby.EventDecoder]::MonitorInterface.ToByteArray().CopyTo($payload,12)
$nameBytes.CopyTo($payload,28)
$device = [WindowsStandby.EventDecoder]::Device(0x8000,$payload)
Assert ($device['name'] -eq 'DBT_DEVICEARRIVAL' -and $device['interfaceClass'] -eq 'e6f07b5f-ee97-4a90-b076-33f57bf4eaa7') 'Monitor interface arrival decodes'
$deviceJson = [WindowsStandby.EventDecoder]::Json($device)
Assert ($device['interfaceNameSha256'].Length -eq 64 -and $deviceJson -notmatch 'PRIVATE-SERIAL') 'Interface name is hashed and not recorded in clear'
$repeat = [WindowsStandby.EventDecoder]::Device(0x8004,$payload)
Assert ($repeat['interfaceNameSha256'] -eq $device['interfaceNameSha256']) 'Arrival and removal have a stable interface correlation hash'
Reject { [WindowsStandby.EventDecoder]::Device(0x8000,[byte[]]@(1,2,3)) } 'Short device header is rejected'
$bad = [byte[]]$payload.Clone(); $bad[0] = $bad[0] -bxor 1
Reject { [WindowsStandby.EventDecoder]::Device(0x8000,$bad) } 'Mismatched device payload length is rejected'
$nodes = [WindowsStandby.EventDecoder]::Device(7,$null)
Assert ($nodes['name'] -eq 'DBT_DEVNODES_CHANGED' -and -not $nodes.ContainsKey('interfaceClass')) 'Generic devnode change stays generic'
$fields = [Collections.Generic.Dictionary[string,object]]::new()
$fields.Add('text',"quote`" backslash\ newline`n țară")
$fields.Add('value',[uint32]4294967295)
$fields.Add('flag',$false)
$roundtrip = [WindowsStandby.EventDecoder]::Json($fields) | ConvertFrom-Json
Assert ($roundtrip.text -eq $fields['text'] -and $roundtrip.value -eq 4294967295 -and -not $roundtrip.flag) 'JSON escaping and invariant scalars round-trip'
$flags = [Reflection.BindingFlags]::NonPublic
$messageType = [WindowsStandby.CaptureHost].GetNestedType('Message',$flags)
$classType = [WindowsStandby.CaptureHost].GetNestedType('WindowClass',$flags)
$filterType = [WindowsStandby.CaptureHost].GetNestedType('DeviceInterfaceFilter',$flags)
$expectedMessage = if ([IntPtr]::Size -eq 8) { 48 } else { 32 }
$expectedClass = if ([IntPtr]::Size -eq 8) { 72 } else { 40 }
Assert ([Runtime.InteropServices.Marshal]::SizeOf([Activator]::CreateInstance($messageType)) -eq $expectedMessage) 'MSG ABI size matches pointer width'
Assert ([Runtime.InteropServices.Marshal]::SizeOf([Activator]::CreateInstance($classType)) -eq $expectedClass) 'WNDCLASS ABI size matches pointer width'
Assert ([Runtime.InteropServices.Marshal]::SizeOf([Activator]::CreateInstance($filterType)) -eq 32) 'Device interface filter ABI is 32 bytes'
foreach ($name in @('Standby-Capture.ps1','Run-CaptureHost.ps1','Request-DisplayOff.ps1')) {
    $tokens = $null; $errors = $null
    $null = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
    Assert ($errors.Count -eq 0) ($name + ' parses without execution')
}
# Compile the separate active helper, but do not instantiate or invoke it.
$offSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Request-DisplayOff.ps1') -Raw
$match = [regex]::Match($offSource,"(?s)@'\r?\n(.*?)\r?\n'@")
Assert $match.Success 'Separate display-off source is present for compile-only validation'
Add-Type -TypeDefinition $match.Groups[1].Value
[pscustomobject]@{passed=$passed.Count;captureStarted=$false;nativeCallsExecuted=$false;cases=$passed.ToArray()} | ConvertTo-Json -Depth 3
