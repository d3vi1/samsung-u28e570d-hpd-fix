using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace WindowsStandbyTopology
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Luid { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Rational { public uint Numerator, Denominator; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Region { public uint Width, Height; }
    [StructLayout(LayoutKind.Sequential)]
    public struct SourceMode { public uint Width, Height, PixelFormat; public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct VideoSignal
    {
        public ulong PixelRate;
        public Rational HSync, VSync;
        public Region ActiveSize, TotalSize;
        public uint AdditionalSignalInfo, ScanlineOrdering;
    }
    [StructLayout(LayoutKind.Explicit, Size = 64)]
    public struct ModeInfo
    {
        [FieldOffset(0)] public uint InfoType;
        [FieldOffset(4)] public uint Id;
        [FieldOffset(8)] public Luid AdapterId;
        [FieldOffset(16)] public SourceMode Source;
        [FieldOffset(16)] public VideoSignal Target;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct PathSource { public Luid AdapterId; public uint Id, ModeInfoIdx, StatusFlags; }
    [StructLayout(LayoutKind.Sequential)]
    public struct PathTarget
    {
        public Luid AdapterId;
        public uint Id, ModeInfoIdx, OutputTechnology, Rotation, Scaling;
        public Rational RefreshRate;
        public uint ScanlineOrdering;
        public int TargetAvailable; // Native BOOL is four bytes.
        public uint StatusFlags;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct PathInfo { public PathSource Source; public PathTarget Target; public uint Flags; }
    [StructLayout(LayoutKind.Sequential)]
    public struct DeviceInfoHeader { public uint Type, Size; public Luid AdapterId; public uint Id; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SourceDeviceName
    {
        public DeviceInfoHeader Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string ViewGdiDeviceName;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct TargetDeviceName
    {
        public DeviceInfoHeader Header;
        public uint Flags, OutputTechnology;
        public ushort EdidManufacturerId, EdidProductCodeId;
        public uint ConnectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string MonitorFriendlyDeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string MonitorDevicePath;
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct AdapterDeviceName
    {
        public DeviceInfoHeader Header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string AdapterDevicePath;
    }

    // Pure formatting/ABI checks are independently testable without a display call.
    public static class TopologyDecoder
    {
        public static void AssertAbi()
        {
            if (IntPtr.Size != 8) throw new PlatformNotSupportedException("This reviewed layout requires a 64-bit process.");
            Size(typeof(Luid), 8); Size(typeof(Rational), 8); Size(typeof(SourceMode), 20);
            Size(typeof(VideoSignal), 48); Size(typeof(ModeInfo), 64);
            Size(typeof(PathSource), 20); Size(typeof(PathTarget), 48); Size(typeof(PathInfo), 72);
            Size(typeof(DeviceInfoHeader), 20); Size(typeof(SourceDeviceName), 84);
            Size(typeof(TargetDeviceName), 420); Size(typeof(AdapterDeviceName), 276);
            Offset(typeof(PathInfo), "Target", 20); Offset(typeof(PathInfo), "Flags", 68);
            Offset(typeof(PathTarget), "RefreshRate", 28); Offset(typeof(PathTarget), "TargetAvailable", 40);
            Offset(typeof(ModeInfo), "Source", 16); Offset(typeof(ModeInfo), "Target", 16);
            Offset(typeof(TargetDeviceName), "MonitorFriendlyDeviceName", 36);
            Offset(typeof(TargetDeviceName), "MonitorDevicePath", 164);
        }
        private static void Size(Type type, int expected)
        {
            if (Marshal.SizeOf(type) != expected) throw new InvalidOperationException("Unexpected native size: " + type.Name);
        }
        private static void Offset(Type type, string field, int expected)
        {
            if (Marshal.OffsetOf(type, field).ToInt64() != expected)
                throw new InvalidOperationException("Unexpected native offset: " + type.Name + "." + field);
        }
        public static string LuidText(Luid value)
        {
            return unchecked((uint)value.HighPart).ToString("X8", CultureInfo.InvariantCulture) + ":" +
                value.LowPart.ToString("X8", CultureInfo.InvariantCulture);
        }
        public static string InterfaceHash(string name)
        {
            // Same case-sensitive UTF-8 hash as the passive notification logger.
            if (String.IsNullOrEmpty(name)) return null;
            name = name.TrimEnd('\0');
            if (name.Length == 0) return null;
            using (SHA256 sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(name))).Replace("-", "").ToLowerInvariant();
        }
        public static string Technology(uint value)
        {
            switch (value)
            {
                case 0: return "HD15/VGA"; case 1: return "SVIDEO"; case 2: return "COMPOSITE_VIDEO";
                case 3: return "COMPONENT_VIDEO"; case 4: return "DVI"; case 5: return "HDMI";
                case 6: return "LVDS"; case 8: return "D_JPN"; case 9: return "SDI";
                case 10: return "DISPLAYPORT_EXTERNAL"; case 11: return "DISPLAYPORT_EMBEDDED";
                case 12: return "UDI_EXTERNAL"; case 13: return "UDI_EMBEDDED";
                case 14: return "SDTVDONGLE"; case 15: return "MIRACAST";
                case 16: return "INDIRECT_WIRED"; case 17: return "INDIRECT_VIRTUAL";
                case 18: return "DISPLAYPORT_USB_TUNNEL"; case 0x80000000: return "INTERNAL";
                case 0xFFFFFFFF: return "OTHER"; default: return "UNKNOWN";
            }
        }
        public static object Rate(Rational rate)
        {
            return rate.Denominator == 0 ? null : (object)((double)rate.Numerator / rate.Denominator);
        }
        private static bool SameLuid(Luid a, Luid b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
        private static ModeInfo? FindMode(ModeInfo[] modes, int count, uint index, uint type, Luid adapter, uint id, List<string> warnings)
        {
            if (index == UInt32.MaxValue) { warnings.Add("Mode index is unavailable for type " + type + "."); return null; }
            if (index >= count) { warnings.Add("Mode index is outside the returned table for type " + type + "."); return null; }
            ModeInfo mode = modes[(int)index];
            if (mode.InfoType != type || mode.Id != id || !SameLuid(mode.AdapterId, adapter))
            { warnings.Add("Mode type/adapter/id does not match its path for type " + type + "."); return null; }
            return mode;
        }
        public static Dictionary<string, object> DecodePath(PathInfo path, ModeInfo[] modes, int validModeCount,
            SourceDeviceName sourceName, int sourceNameStatus, TargetDeviceName targetName, int targetNameStatus)
        {
            if (modes == null || validModeCount < 0 || validModeCount > modes.Length)
                throw new ArgumentException("Invalid returned mode count.");
            var warnings = new List<string>();
            var record = new Dictionary<string, object> {
                { "sourceAdapterId", LuidText(path.Source.AdapterId) }, { "sourceId", path.Source.Id },
                { "targetAdapterId", LuidText(path.Target.AdapterId) }, { "targetId", path.Target.Id },
                { "pathFlags", path.Flags }, { "active", (path.Flags & 1) != 0 },
                { "sourceStatusFlags", path.Source.StatusFlags }, { "targetStatusFlags", path.Target.StatusFlags },
                { "targetAvailable", path.Target.TargetAvailable != 0 },
                { "outputTechnologyValue", path.Target.OutputTechnology }, { "outputTechnology", Technology(path.Target.OutputTechnology) },
                { "sourceModeIndexRaw", path.Source.ModeInfoIdx }, { "targetModeIndexRaw", path.Target.ModeInfoIdx },
                { "rotationValue", path.Target.Rotation }, { "scalingValue", path.Target.Scaling },
                { "pathRefreshNumerator", path.Target.RefreshRate.Numerator },
                { "pathRefreshDenominator", path.Target.RefreshRate.Denominator }, { "pathRefreshHz", Rate(path.Target.RefreshRate) },
                { "sourceNameStatus", sourceNameStatus }, { "targetNameStatus", targetNameStatus },
                { "sourceGdiName", sourceNameStatus == 0 ? sourceName.ViewGdiDeviceName : null },
                { "targetFriendlyName", targetNameStatus == 0 ? targetName.MonitorFriendlyDeviceName : null },
                { "interfaceNameSha256", targetNameStatus == 0 ? InterfaceHash(targetName.MonitorDevicePath) : null },
                { "warnings", warnings }
            };
            if (sourceNameStatus != 0) warnings.Add("Source device-name query failed: " + sourceNameStatus);
            if (targetNameStatus != 0) warnings.Add("Target device-name query failed: " + targetNameStatus);
            else
            {
                record.Add("targetNameFlags", targetName.Flags);
                record.Add("friendlyNameFromEdid", (targetName.Flags & 1) != 0);
                record.Add("friendlyNameForced", (targetName.Flags & 2) != 0);
                record.Add("edidIdsValid", (targetName.Flags & 4) != 0);
                record.Add("edidManufacturerIdHex", (targetName.Flags & 4) != 0 ? targetName.EdidManufacturerId.ToString("X4") : null);
                record.Add("edidProductCodeIdHex", (targetName.Flags & 4) != 0 ? targetName.EdidProductCodeId.ToString("X4") : null);
                record.Add("connectorInstance", targetName.ConnectorInstance);
                record.Add("targetNameTechnologyValue", targetName.OutputTechnology);
                if (targetName.OutputTechnology != path.Target.OutputTechnology)
                    warnings.Add("Target-name connector differs from the earlier path query; topology may have changed.");
            }
            // We deliberately requested the legacy index layout (flags=2 only).
            // Do not interpret a virtual-mode union as a normal 32-bit index.
            if ((path.Flags & 8) != 0)
            {
                warnings.Add("Virtual-mode path returned to a non-virtual-aware query; mode decoding omitted.");
                return record;
            }
            ModeInfo? source = FindMode(modes, validModeCount, path.Source.ModeInfoIdx, 1, path.Source.AdapterId, path.Source.Id, warnings);
            if (source.HasValue)
            {
                SourceMode value = source.Value.Source;
                record.Add("sourceWidth", value.Width); record.Add("sourceHeight", value.Height);
                record.Add("sourceX", value.X); record.Add("sourceY", value.Y); record.Add("pixelFormatValue", value.PixelFormat);
                record.Add("primarySourceByPosition", value.X == 0 && value.Y == 0);
            }
            ModeInfo? target = FindMode(modes, validModeCount, path.Target.ModeInfoIdx, 2, path.Target.AdapterId, path.Target.Id, warnings);
            if (target.HasValue)
            {
                VideoSignal value = target.Value.Target;
                record.Add("targetActiveWidth", value.ActiveSize.Width); record.Add("targetActiveHeight", value.ActiveSize.Height);
                record.Add("targetTotalWidth", value.TotalSize.Width); record.Add("targetTotalHeight", value.TotalSize.Height);
                record.Add("targetPixelRate", value.PixelRate); record.Add("targetVSyncHz", Rate(value.VSync));
                record.Add("targetVSyncNumerator", value.VSync.Numerator); record.Add("targetVSyncDenominator", value.VSync.Denominator);
                record.Add("targetScanlineOrdering", value.ScanlineOrdering);
            }
            return record;
        }
    }

    public static class DisplayTopology
    {
        private const uint QueryFlags = 2; // QDC_ONLY_ACTIVE_PATHS; no expensive ALL_PATHS or polling.
        private const int MaxPaths = 128, MaxModes = 512, MaxAttempts = 3;

        public static Dictionary<string, object> CaptureOnce(string phase, bool displaysAreActive)
        {
            if (!displaysAreActive) throw new InvalidOperationException("This helper may run only before standby or after deliberate wake, with displays active.");
            if (phase != "BeforeStandby" && phase != "AfterWake") throw new ArgumentException("Unknown capture phase.");
            TopologyDecoder.AssertAbi();
            string started = DateTimeOffset.UtcNow.ToString("O");
            var timer = Stopwatch.StartNew();
            PathInfo[] paths = null; ModeInfo[] modes = null;
            uint pathCount = 0, modeCount = 0;
            int attempts = 0, status = 122;
            for (attempts = 1; attempts <= MaxAttempts; attempts++)
            {
                status = GetDisplayConfigBufferSizes(QueryFlags, out pathCount, out modeCount);
                if (status != 0) throw new Win32Exception(status, "GetDisplayConfigBufferSizes failed; no fallback or display mutation was attempted.");
                if (pathCount > MaxPaths || modeCount > MaxModes) throw new InvalidOperationException("Display topology exceeds bounded buffer limits.");
                pathCount = Math.Max(pathCount, 1U); modeCount = Math.Max(modeCount, 1U);
                paths = new PathInfo[(int)pathCount]; modes = new ModeInfo[(int)modeCount];
                status = QueryDisplayConfig(QueryFlags, ref pathCount, paths, ref modeCount, modes, IntPtr.Zero);
                if (status == 122) continue; // Resize only for a documented topology-size race.
                if (status != 0) throw new Win32Exception(status, "QueryDisplayConfig failed; no fallback or display mutation was attempted.");
                if (pathCount > paths.Length || modeCount > modes.Length) throw new InvalidOperationException("Native returned counts exceed allocated buffers.");
                break;
            }
            if (status != 0) throw new Win32Exception(status, "Topology kept changing during three bounded buffer attempts.");
            string queryCompleted = DateTimeOffset.UtcNow.ToString("O");
            var records = new List<Dictionary<string, object>>();
            var adapters = new Dictionary<string, Dictionary<string, object>>();
            for (int i = 0; i < (int)pathCount; i++)
            {
                PathInfo path = paths[i];
                var source = new SourceDeviceName { Header = Header(1, 84, path.Source.AdapterId, path.Source.Id), ViewGdiDeviceName = "" };
                var target = new TargetDeviceName { Header = Header(2, 420, path.Target.AdapterId, path.Target.Id),
                    MonitorFriendlyDeviceName = "", MonitorDevicePath = "" };
                int sourceStatus = GetSourceName(ref source);
                int targetStatus = GetTargetName(ref target);
                records.Add(TopologyDecoder.DecodePath(path, modes, (int)modeCount, source, sourceStatus, target, targetStatus));
                AddAdapter(adapters, path.Source.AdapterId); AddAdapter(adapters, path.Target.AdapterId);
            }
            return new Dictionary<string, object> {
                { "schema", "WindowsStandby.DisplayTopology/v1" }, { "phase", phase },
                { "utcStarted", started }, { "utcQueryCompleted", queryCompleted }, { "utcCompleted", DateTimeOffset.UtcNow.ToString("O") },
                { "elapsedMs", timer.Elapsed.TotalMilliseconds }, { "queryFlags", QueryFlags }, { "queryAttempts", attempts },
                { "processId", Process.GetCurrentProcess().Id }, { "sessionId", Process.GetCurrentProcess().SessionId },
                { "operatorConfirmedDisplaysActive", displaysAreActive }, { "pathCount", pathCount }, { "modeCount", modeCount },
                { "paths", records }, { "adapters", new List<Dictionary<string, object>>(adapters.Values) },
                { "scope", "One OS topology snapshot; not a passive standby event, electrical measurement or proof of monitor wake/AUX health." }
            };
        }
        private static DeviceInfoHeader Header(uint type, uint size, Luid adapter, uint id)
        { return new DeviceInfoHeader { Type = type, Size = size, AdapterId = adapter, Id = id }; }
        private static void AddAdapter(Dictionary<string, Dictionary<string, object>> adapters, Luid id)
        {
            string key = TopologyDecoder.LuidText(id);
            if (adapters.ContainsKey(key)) return;
            var value = new AdapterDeviceName { Header = Header(4, 276, id, 0), AdapterDevicePath = "" };
            int status = GetAdapterName(ref value);
            adapters.Add(key, new Dictionary<string, object> {
                { "adapterId", key }, { "deviceInfoStatus", status },
                { "adapterDevicePathSha256", status == 0 ? TopologyDecoder.InterfaceHash(value.AdapterDevicePath) : null }
            });
        }
        [DllImport("user32.dll", ExactSpelling = true)]
        private static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
        [DllImport("user32.dll", ExactSpelling = true)]
        private static extern int QueryDisplayConfig(uint flags, ref uint pathCount, [Out] PathInfo[] paths,
            ref uint modeCount, [Out] ModeInfo[] modes, IntPtr topologyId);
        [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int GetSourceName(ref SourceDeviceName name);
        [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int GetTargetName(ref TargetDeviceName name);
        [DllImport("user32.dll", EntryPoint = "DisplayConfigGetDeviceInfo", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int GetAdapterName(ref AdapterDeviceName name);
    }
}
