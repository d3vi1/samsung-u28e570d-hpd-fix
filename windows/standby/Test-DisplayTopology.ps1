#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$topologySource = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'DisplayTopology.cs') -Raw
# Only pure decoder methods and early guard failures are called below.
# No QueryDisplayConfig, DisplayConfigGetDeviceInfo or other P/Invoke is executed.
$tests = @'
namespace WindowsStandbyTopologyTests {
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using WindowsStandbyTopology;
public static class DisplayTopologyOfflineTests
{
    private static int passed;
    private static void Check(bool ok, string name) { if (!ok) throw new Exception(name); passed++; }
    public static int Run()
    {
        TopologyDecoder.AssertAbi(); Check(true, "x64 native sizes and offsets");
        bool blocked = false;
        try { DisplayTopology.CaptureOnce("BeforeStandby", false); } catch (InvalidOperationException) { blocked = true; }
        Check(blocked, "Display-active guard rejects before native calls");
        blocked = false;
        try { DisplayTopology.CaptureOnce("DuringStandby", true); } catch (ArgumentException) { blocked = true; }
        Check(blocked, "Phase guard rejects before native calls");
        Check(TopologyDecoder.Technology(5) == "HDMI" && TopologyDecoder.Technology(11) == "DISPLAYPORT_EMBEDDED" &&
            TopologyDecoder.Technology(0x80000000) == "INTERNAL" && TopologyDecoder.Technology(0xFFFFFFFF) == "OTHER" &&
            TopologyDecoder.Technology(12345) == "UNKNOWN", "Technology values retain unknowns");
        var adapter = new Luid { LowPart = 0x12345678, HighPart = -1 };
        Check(TopologyDecoder.LuidText(adapter) == "FFFFFFFF:12345678", "LUID bit representation");
        var path = new PathInfo { Flags = 1,
            Source = new PathSource { AdapterId = adapter, Id = 3, ModeInfoIdx = 0 },
            Target = new PathTarget { AdapterId = adapter, Id = 9, ModeInfoIdx = 1, OutputTechnology = 5,
                TargetAvailable = 1, RefreshRate = new Rational { Numerator = 60000, Denominator = 1001 } } };
        var modes = new ModeInfo[2];
        modes[0] = new ModeInfo { InfoType = 1, Id = 3, AdapterId = adapter,
            Source = new SourceMode { Width = 3840, Height = 2160, X = 1920, Y = 0 } };
        modes[1] = new ModeInfo { InfoType = 2, Id = 9, AdapterId = adapter,
            Target = new VideoSignal { PixelRate = 594000000,
                ActiveSize = new Region { Width = 3840, Height = 2160 },
                TotalSize = new Region { Width = 4400, Height = 2250 },
                VSync = new Rational { Numerator = 60, Denominator = 1 } } };
        var sourceName = new SourceDeviceName { ViewGdiDeviceName = @"\\.\DISPLAY2" };
        string devicePath = @"\\?\DISPLAY#SAM0D71#synthetic#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
        var targetName = new TargetDeviceName { Flags = 5, OutputTechnology = 5,
            MonitorFriendlyDeviceName = "Synthetic Samsung", MonitorDevicePath = devicePath,
            EdidManufacturerId = 0x4C2D, EdidProductCodeId = 0x0D71 };
        var record = TopologyDecoder.DecodePath(path, modes, 2, sourceName, 0, targetName, 0);
        Check((uint)record["sourceWidth"] == 3840 && (uint)record["targetActiveHeight"] == 2160 &&
            (int)record["sourceX"] == 1920 && !(bool)record["primarySourceByPosition"], "Modes are joined to the same snapshot");
        Check(Math.Abs((double)record["pathRefreshHz"] - 59.94005994) < 0.00001, "Rational refresh decoding");
        Check((string)record["edidProductCodeIdHex"] == "0D71" && (bool)record["edidIdsValid"], "EDID flags gate IDs");
        string expectedHash;
        using (SHA256 sha = SHA256.Create()) expectedHash = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(devicePath))).Replace("-", "").ToLowerInvariant();
        Check((string)record["interfaceNameSha256"] == expectedHash && !record.ContainsKey("monitorDevicePath"), "Case-sensitive UTF-8 hash without raw path disclosure");
        Check(TopologyDecoder.InterfaceHash(devicePath + "\0\0") == expectedHash && TopologyDecoder.InterfaceHash("") == null,
            "Terminal NUL handling matches notifications");
        Check(TopologyDecoder.InterfaceHash(devicePath.ToLowerInvariant()) != expectedHash, "Case differences are not silently normalized");
        targetName.Flags = 0;
        record = TopologyDecoder.DecodePath(path, modes, 2, sourceName, 0, targetName, 0);
        Check(record["edidProductCodeIdHex"] == null, "Invalid EDID IDs omitted");
        record = TopologyDecoder.DecodePath(path, modes, 2, sourceName, 5, targetName, 31);
        Check(record["sourceGdiName"] == null && record["targetFriendlyName"] == null &&
            record["interfaceNameSha256"] == null && ((List<string>)record["warnings"]).Count == 2, "Name-query failures retained");
        var badPath = path; badPath.Source.ModeInfoIdx = UInt32.MaxValue; badPath.Target.ModeInfoIdx = 99;
        record = TopologyDecoder.DecodePath(badPath, modes, 2, sourceName, 0, targetName, 0);
        Check(!record.ContainsKey("sourceWidth") && !record.ContainsKey("targetActiveWidth") &&
            ((List<string>)record["warnings"]).Count == 2, "Invalid indices never dereferenced");
        var wrongModes = (ModeInfo[])modes.Clone(); wrongModes[0].AdapterId = new Luid(); wrongModes[1].Id = 100;
        record = TopologyDecoder.DecodePath(path, wrongModes, 2, sourceName, 0, targetName, 0);
        Check(!record.ContainsKey("sourceWidth") && !record.ContainsKey("targetActiveWidth"), "Adapter and target identity mismatch rejected");
        wrongModes = (ModeInfo[])modes.Clone(); wrongModes[0].InfoType = 2;
        record = TopologyDecoder.DecodePath(path, wrongModes, 2, sourceName, 0, targetName, 0);
        Check(!record.ContainsKey("sourceWidth"), "Mode type mismatch rejected");
        badPath = path; badPath.Flags |= 8;
        record = TopologyDecoder.DecodePath(badPath, modes, 2, sourceName, 0, targetName, 0);
        Check(!record.ContainsKey("sourceWidth") && ((List<string>)record["warnings"]).Count == 1, "Unexpected virtual layout is not guessed");
        badPath = path; badPath.Target.TargetAvailable = 0; badPath.Target.RefreshRate = new Rational();
        record = TopologyDecoder.DecodePath(badPath, modes, 2, sourceName, 0, targetName, 0);
        Check((bool)record["active"] && !(bool)record["targetAvailable"] && record["pathRefreshHz"] == null, "Availability and zero denominator preserved");
        blocked = false;
        try { TopologyDecoder.DecodePath(path, modes, 3, sourceName, 0, targetName, 0); } catch (ArgumentException) { blocked = true; }
        Check(blocked, "Returned table count bounded");
        targetName.OutputTechnology = 10;
        record = TopologyDecoder.DecodePath(path, modes, 2, sourceName, 0, targetName, 0);
        Check(((List<string>)record["warnings"]).Count == 1, "Connector change between metadata queries recorded");
        return passed;
    }
}
}
'@
Add-Type -TypeDefinition ($topologySource + [Environment]::NewLine + $tests)
$passed = [WindowsStandbyTopologyTests.DisplayTopologyOfflineTests]::Run()
[pscustomobject]@{passed=$passed; nativeDisplayCallsExecuted=$false; scope='Compilation, x64 ABI and synthetic decoding only'}
