#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Validate','Start','Status','Stop','Export')][string]$Action,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidatePattern('^[A-Za-z0-9_-]{1,40}$')][string]$TestId = 'standby-test',
    [ValidateRange(32,512)][int]$MaximumMegabytes = 256
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ($OutputDirectory.StartsWith('\\')) { throw 'Use a local output directory for the ETL file.' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$sessionName = 'Samsung-Standby-DxgKrnl-' + $TestId
$statePath = Join-Path $OutputDirectory 'dxgkrnl-session.json'
$etlPath = Join-Path $OutputDirectory 'dxgkrnl.etl'
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')
$resultPath = Join-Path $OutputDirectory (('dxgkrnl-{0}-{1}-{2}.json' -f $Action.ToLowerInvariant(),$stamp,$PID))
$result = [ordered]@{
    action=$Action; utc=[DateTime]::UtcNow.ToString('o'); sessionName=$sessionName; success=$false; error=$null
    lossInterpretation='ETW loss counters do not count circular overwrite. Inspect exported first/last event UTC and test markers before inferring absence of events.'
}

function Write-NewJson([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 12
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + [Environment]::NewLine)
    $file = [IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try { $file.Write($bytes,0,$bytes.Length); $file.Flush($true) } finally { $file.Dispose() }
}

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace SamsungStandbyEtw {
    public sealed class Snapshot {
        public bool Running;
        public string SessionGuid, SessionName, LogFileName;
        public uint EventsLost, LogBuffersLost, RealTimeBuffersLost, BuffersWritten;
        public uint NumberOfBuffers, FreeBuffers, BufferSizeKb, MaximumFileSizeMb, LogFileMode;
        public ulong TraceHandle;
    }
    public static class Native {
        [StructLayout(LayoutKind.Sequential)] struct Wnode {
            public uint BufferSize, ProviderId;
            public ulong HistoricalContext, TimeStamp;
            public Guid Guid;
            public uint ClientContext, Flags;
        }
        [StructLayout(LayoutKind.Sequential)] struct Properties {
            public Wnode Wnode;
            public uint BufferSize, MinimumBuffers, MaximumBuffers, MaximumFileSize, LogFileMode, FlushTimer, EnableFlags;
            public int AgeLimit;
            public uint NumberOfBuffers, FreeBuffers, EventsLost, BuffersWritten, LogBuffersLost, RealTimeBuffersLost;
            public IntPtr LoggerThreadId;
            public uint LogFileNameOffset, LoggerNameOffset;
        }
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        static extern uint StartTraceW(out ulong handle, string name, IntPtr properties);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        static extern uint ControlTraceW(ulong handle, string name, IntPtr properties, uint code);
        [DllImport("advapi32.dll", ExactSpelling=true)]
        static extern uint EnableTraceEx2(ulong handle, ref Guid provider, uint code, byte level,
            ulong anyKeyword, ulong allKeyword, uint timeout, IntPtr parameters);
        const int Chars = 1024;
        static readonly Guid Provider = new Guid("802ec45a-1e99-4b83-9920-87c98277ba9d");
        public static int ValidateLayout() {
            if (IntPtr.Size != 8 || Marshal.SizeOf(typeof(Wnode)) != 48 || Marshal.SizeOf(typeof(Properties)) != 120
                || Marshal.OffsetOf(typeof(Properties), "LoggerThreadId").ToInt32() != 104)
                throw new InvalidOperationException("Unexpected x64 ETW structure layout.");
            return Marshal.SizeOf(typeof(Properties));
        }
        static IntPtr Allocate(string name, string path, Guid guid, uint maxMb, bool starting) {
            int size = ValidateLayout();
            if (name.Length >= Chars || path.Length >= Chars) throw new ArgumentException("ETW name/path too long.");
            int total = size + Chars * 4;
            IntPtr memory = Marshal.AllocHGlobal(total);
            try {
                Marshal.Copy(new byte[total],0,memory,total);
                Properties p = new Properties();
                p.Wnode.BufferSize = (uint)total; p.Wnode.Guid = guid; p.Wnode.Flags = 0x20000;
                p.LoggerNameOffset = (uint)size; p.LogFileNameOffset = (uint)(size + Chars * 2);
                if (starting) {
                    p.Wnode.ClientContext = 1; // QPC, converted by ETW consumers to system time.
                    p.BufferSize = 64; p.MinimumBuffers = 16; p.MaximumBuffers = 64;
                    p.MaximumFileSize = maxMb; p.LogFileMode = 2; p.FlushTimer = 5; // Circular file only.
                }
                Marshal.StructureToPtr(p,memory,false);
                byte[] text = Encoding.Unicode.GetBytes(name + "\0");
                Marshal.Copy(text,0,IntPtr.Add(memory,(int)p.LoggerNameOffset),text.Length);
                text = Encoding.Unicode.GetBytes(path + "\0");
                Marshal.Copy(text,0,IntPtr.Add(memory,(int)p.LogFileNameOffset),text.Length);
                return memory;
            } catch { Marshal.FreeHGlobal(memory); throw; }
        }
        static Snapshot Read(IntPtr memory, bool running) {
            Properties p = (Properties)Marshal.PtrToStructure(memory,typeof(Properties));
            return new Snapshot { Running=running, SessionGuid=p.Wnode.Guid.ToString(),
                SessionName=Marshal.PtrToStringUni(IntPtr.Add(memory,(int)p.LoggerNameOffset)),
                LogFileName=Marshal.PtrToStringUni(IntPtr.Add(memory,(int)p.LogFileNameOffset)),
                EventsLost=p.EventsLost, LogBuffersLost=p.LogBuffersLost, RealTimeBuffersLost=p.RealTimeBuffersLost,
                BuffersWritten=p.BuffersWritten, NumberOfBuffers=p.NumberOfBuffers, FreeBuffers=p.FreeBuffers,
                BufferSizeKb=p.BufferSize, MaximumFileSizeMb=p.MaximumFileSize, LogFileMode=p.LogFileMode,
                TraceHandle=p.Wnode.HistoricalContext };
        }
        static void Check(uint code, string operation) {
            if (code != 0) throw new Win32Exception((int)code,operation + " failed with ETW code " + code);
        }
        public static Snapshot Query(string name) {
            IntPtr memory = Allocate(name,"",Guid.Empty,0,false);
            try {
                uint code = ControlTraceW(0,name,memory,0);
                if (code == 4201) return new Snapshot { Running=false, SessionName=name };
                Check(code,"ControlTrace query"); return Read(memory,true);
            } finally { Marshal.FreeHGlobal(memory); }
        }
        public static Snapshot Stop(string name, Guid expectedGuid) {
            Snapshot current = Query(name);
            if (!current.Running) return current;
            if (new Guid(current.SessionGuid) != expectedGuid)
                throw new InvalidOperationException("Session GUID differs; refusing to stop it.");
            if (current.TraceHandle == 0)
                throw new InvalidOperationException("Verified session has no usable ETW handle; refusing name-only stop.");
            IntPtr memory = Allocate(name,"",expectedGuid,0,false);
            try {
                Check(ControlTraceW(current.TraceHandle,null,memory,1),"ControlTrace stop");
                return Read(memory,false); // Final loss counters, after the session flushes.
            } finally { Marshal.FreeHGlobal(memory); }
        }
        public static Snapshot Start(string name, string path, Guid guid, uint maxMb) {
            IntPtr memory = Allocate(name,path,guid,maxMb,true);
            ulong handle;
            try { Check(StartTraceW(out handle,name,memory),"StartTrace"); }
            finally { Marshal.FreeHGlobal(memory); }
            try {
                Guid provider = Provider;
                Check(EnableTraceEx2(handle,ref provider,1,4,0x10301,0,5000,IntPtr.Zero),"EnableTraceEx2");
                Snapshot current = Query(name);
                if (!current.Running || new Guid(current.SessionGuid) != guid)
                    throw new InvalidOperationException("Started session could not be verified.");
                return current;
            } catch (Exception original) {
                try { Stop(name,guid); }
                catch (Exception cleanup) { throw new AggregateException("ETW start failed and owned-session cleanup also failed.",original,cleanup); }
                throw;
            }
        }
    }
}
'@

try {
    if (-not ('SamsungStandbyEtw.Native' -as [type])) { Add-Type -TypeDefinition $nativeSource -Language CSharp }
    $result.nativeStructureBytes = [SamsungStandbyEtw.Native]::ValidateLayout()
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    $result.elevatedAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($Action -eq 'Validate') {
        $result.success = $true
        $result.note = 'Offline compilation/layout check only. No ETW API or monitor API was called.'
    } else {
        if (-not $result.elevatedAdministrator) {
            throw 'Run this action in an elevated process. Start-Process -Verb RunAs requests the normal user-approved UAC dialog; this script does not bypass it.'
        }
        if ($Action -eq 'Start') {
            if (Test-Path -LiteralPath $statePath) { throw 'Session manifest exists. Use Status/Stop/Export, or choose a fresh run directory.' }
            if (Test-Path -LiteralPath $etlPath) { throw 'ETL already exists; refusing overwrite.' }
            $existing = [SamsungStandbyEtw.Native]::Query($sessionName)
            if ($existing.Running) { throw 'The test session name is already active. No session was changed.' }
            $sessionGuid = [Guid]::NewGuid()
            $manifest = [ordered]@{
                schema='SamsungStandbyEtw/v1'; sessionName=$sessionName; sessionGuid=$sessionGuid.ToString()
                outputDirectory=$OutputDirectory; etlPath=$etlPath; requestedUtc=[DateTime]::UtcNow.ToString('o')
                provider='Microsoft-Windows-DxgKrnl'; providerGuid='802ec45a-1e99-4b83-9920-87c98277ba9d'
                matchAnyKeyword='0x10301'; level=4; maximumMegabytes=$MaximumMegabytes; fileMode='circular'; clock='QPC'
                note='Intent manifest; only a successful Start result confirms capture. No DDC/AUX/NVAPI or power commands.'
            }
            Write-NewJson $statePath $manifest
            $snapshot = [SamsungStandbyEtw.Native]::Start($sessionName,$etlPath,$sessionGuid,[uint32]$MaximumMegabytes)
            $result.snapshot = $snapshot
            $result.success = $true
        } else {
            if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { throw 'No session manifest in this run directory.' }
            $manifest = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
            if ($manifest.schema -ne 'SamsungStandbyEtw/v1' -or $manifest.sessionName -ne $sessionName -or
                [IO.Path]::GetFullPath($manifest.outputDirectory) -ne $OutputDirectory -or
                [IO.Path]::GetFullPath($manifest.etlPath) -ne $etlPath) { throw 'Session manifest does not match this test and directory.' }
            $expectedGuid = [Guid]::Parse($manifest.sessionGuid)
            $snapshot = [SamsungStandbyEtw.Native]::Query($sessionName)
            if ($snapshot.Running -and [Guid]::Parse($snapshot.SessionGuid) -ne $expectedGuid) {
                throw 'An active session has a different GUID. Refusing to control or export it.'
            }
            if ($Action -eq 'Stop') {
                $result.snapshotBeforeStop = $snapshot
                $result.snapshot = [SamsungStandbyEtw.Native]::Stop($sessionName,$expectedGuid)
                $result.finalLossCountersAvailable = [bool]$snapshot.Running
                $result.success = $true
                if (-not $snapshot.Running) { $result.note = 'Already stopped. No final live loss counters could be recovered by this action.' }
            } elseif ($Action -eq 'Status') {
                $result.snapshot = $snapshot; $result.success = $true
            } else {
                if ($snapshot.Running) { throw 'Stop this test session before offline export.' }
                if (-not (Test-Path -LiteralPath $etlPath -PathType Leaf)) { throw 'ETL file is missing.' }
                $xmlPath = Join-Path $OutputDirectory 'dxgkrnl-events.xml'
                $summaryPath = Join-Path $OutputDirectory 'dxgkrnl-summary.txt'
                $timelinePath = Join-Path $OutputDirectory 'dxgkrnl-key-events-utc.jsonl'
                foreach ($path in @($xmlPath,$summaryPath,$timelinePath)) { if (Test-Path -LiteralPath $path) { throw "Export already exists: $path" } }
                $tracerpt = Join-Path $env:SystemRoot 'System32\tracerpt.exe'
                $messages = & $tracerpt $etlPath -o $xmlPath -of XML -summary $summaryPath 2>&1
                $result.tracerptExitCode = $LASTEXITCODE
                $result.tracerptOutput = @($messages | ForEach-Object { $_.ToString() })
                if ($result.tracerptExitCode -ne 0) { throw 'tracerpt export failed; see this result for diagnostic output.' }
                $result.etlSha256 = (Get-FileHash -LiteralPath $etlPath -Algorithm SHA256).Hash.ToLowerInvariant()
                $timelineFile = [IO.FileStream]::new($timelinePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
                $timeline = [IO.StreamWriter]::new($timelineFile,[Text.UTF8Encoding]::new($false))
                $observedEvents = 0L; $keyEvents = 0L; $firstUtc = $null; $lastUtc = $null
                try {
                    Get-WinEvent -Path $etlPath -Oldest -ErrorAction Stop | ForEach-Object {
                        $observedEvents++
                        $utc = if ($null -ne $_.TimeCreated) { $_.TimeCreated.ToUniversalTime().ToString('o') } else { $null }
                        if ($utc) {
                            if (-not $firstUtc -or [String]::CompareOrdinal($utc,$firstUtc) -lt 0) { $firstUtc = $utc }
                            if (-not $lastUtc -or [String]::CompareOrdinal($utc,$lastUtc) -gt 0) { $lastUtc = $utc }
                        }
                        if ($_.ProviderName -eq 'Microsoft-Windows-DxgKrnl' -and
                            ($_.Id -in 148,149,150,151,272,430,1092,1098,1099,10000,10001,10002,10011 -or
                             $_.TaskDisplayName -eq 'RuntimePowerManagement')) {
                            $keyEvents++
                            $entry = [ordered]@{ utc=$utc; provider=$_.ProviderName; id=$_.Id; version=$_.Version
                                task=$_.TaskDisplayName; opcode=$_.Opcode; properties=@($_.Properties | ForEach-Object { $_.Value }) }
                            $timeline.WriteLine(($entry | ConvertTo-Json -Depth 8 -Compress))
                        }
                    }
                    $timeline.Flush(); $timelineFile.Flush($true)
                } finally { $timeline.Dispose(); $timelineFile.Dispose() }
                $result.observedEvents = $observedEvents; $result.keyEvents = $keyEvents
                $result.firstEventUtc = $firstUtc; $result.lastEventUtc = $lastUtc
                $result.success = $true
                $result.note = 'Raw ETL and full XML retained; selected display/connection/power events also exported with UTC timestamps. This is OS/driver telemetry, not electrical HPD measurement.'
            }
        }
    }
} catch {
    $result.error = $_.Exception.ToString()
} finally {
    $result.completedUtc = [DateTime]::UtcNow.ToString('o')
    try { Write-NewJson $resultPath $result }
    catch {
        $result.success = $false
        $result.error = [string]$result.error + "`nCannot save ETW result to $resultPath. Session may still be running; use Status with this same test directory. $($_.Exception.Message)"
        Write-Error $result.error -ErrorAction Continue
    }
    $result | ConvertTo-Json -Depth 12
}
if (-not $result.success) { exit 1 }
