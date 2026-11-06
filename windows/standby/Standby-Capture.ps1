#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Start','Mark','Stop','Status')][string]$Action,
    [Parameter(Mandatory=$true)][string]$Directory,
    [string]$Text
)
$ErrorActionPreference = 'Stop'
$Directory = [IO.Path]::GetFullPath($Directory)
$manifestPath = Join-Path $Directory 'session.json'
if ($Action -eq 'Start') {
    if (Test-Path -LiteralPath $Directory) { throw 'Use a new capture directory.' }
    $null = New-Item -ItemType Directory -Path $Directory
    $pipeName = 'WindowsStandby-' + [Guid]::NewGuid().ToString('N')
    $hostScript = Join-Path $PSScriptRoot 'Run-CaptureHost.ps1'
    $shell = (Get-Process -Id $PID).Path
    if ([IO.Path]::GetFileName($shell) -notin @('pwsh.exe','powershell.exe')) {
        $shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    }
    # Encode the command to preserve literal paths, including spaces and apostrophes.
    $command = "& '" + $hostScript.Replace("'","''") + "' -Directory '" +
        $Directory.Replace("'","''") + "' -PipeName '" + $pipeName + "'"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    # Persist the control address before spawning, so a later filesystem error
    # cannot leave an otherwise running listener without its Stop endpoint.
    [ordered]@{utc=[DateTimeOffset]::UtcNow.ToString('O');pid=$null;pipeName=$pipeName;mode='passive notifications'} |
        ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $child = Start-Process -FilePath $shell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-EncodedCommand',$encoded) -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath (Join-Path $Directory 'ready.json'))) {
        if ($child.HasExited -or (Test-Path -LiteralPath (Join-Path $Directory 'host-error.txt'))) {
            throw ('Capture did not start. Inspect: ' + $Directory)
        }
        if ([DateTime]::UtcNow -ge $deadline) { throw ('Capture startup timed out; inspect or Stop it using: ' + $Directory) }
        Start-Sleep -Milliseconds 100
    }
    Get-Content -LiteralPath (Join-Path $Directory 'ready.json') -Raw | ConvertFrom-Json
    return
}
if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'No capture session manifest exists.' }
$session = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($session.pipeName -notmatch '^WindowsStandby-[0-9a-f]{32}$') { throw 'Invalid session pipe name.' }
if ($Action -eq 'Status' -and (Test-Path -LiteralPath (Join-Path $Directory 'stopped.json'))) {
    Get-Content -LiteralPath (Join-Path $Directory 'stopped.json') -Raw | ConvertFrom-Json
    return
}
if ($Action -eq 'Mark' -and [string]::IsNullOrWhiteSpace($Text)) { throw 'Mark requires -Text.' }
if (-not ('WindowsStandby.CaptureControl' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'StandbyCapture.cs') }
$command = switch ($Action) { 'Mark' { 'MARK' }; 'Stop' { 'STOP' }; 'Status' { 'PING' } }
$null = [WindowsStandby.CaptureControl]::Send($session.pipeName, $command, $Text)
if ($Action -eq 'Stop') {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath (Join-Path $Directory 'stopped.json'))) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Stop was requested, but clean shutdown is not yet confirmed.' }
        Start-Sleep -Milliseconds 100
    }
    Get-Content -LiteralPath (Join-Path $Directory 'stopped.json') -Raw | ConvertFrom-Json
} else {
    [pscustomobject]@{utc=[DateTimeOffset]::UtcNow.ToString('O');action=$Action;result='OK'}
}
