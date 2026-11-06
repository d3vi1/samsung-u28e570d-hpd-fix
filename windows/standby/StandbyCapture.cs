using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace WindowsStandby
{
    // Passive OS message listener. No display enumeration, DDC, NVAPI, power-policy
    // changes, execution-state inhibition or monitor-control calls are present.
    public static class EventDecoder
    {
        public static readonly Guid SessionDisplay = new Guid("2b84c20e-ad23-4ddf-93db-05ffbd7efca5");
        public static readonly Guid ConsoleDisplay = new Guid("6fe69556-704a-47a0-8f24-c28d936fda47");
        public static readonly Guid LegacyMonitor = new Guid("02731015-4510-4526-99e6-e5a17ebd1aea");
        public static readonly Guid MonitorInterface = new Guid("e6f07b5f-ee97-4a90-b076-33f57bf4eaa7");

        public static Dictionary<string, object> Power(Guid setting, byte[] data)
        {
            if (data == null) throw new ArgumentNullException("data");
            string name = setting == SessionDisplay ? "SESSION_DISPLAY_STATUS" :
                setting == ConsoleDisplay ? "CONSOLE_DISPLAY_STATE" :
                setting == LegacyMonitor ? "MONITOR_POWER_ON" : "UNKNOWN_POWER_SETTING";
            var fields = new Dictionary<string, object> {
                { "setting", name }, { "guid", setting.ToString() },
                { "dataLength", data.Length }, { "dataHex", BitConverter.ToString(data) }
            };
            if (data.Length == 4)
            {
                uint value = BitConverter.ToUInt32(data, 0);
                fields.Add("value", value);
                bool known = setting == SessionDisplay || setting == ConsoleDisplay || setting == LegacyMonitor;
                fields.Add("state", !known ? "unknown" : value == 0 ? "off" : value == 1 ? "on" :
                    value == 2 && setting != LegacyMonitor ? "dim" : "unknown");
            }
            return fields;
        }

        public static Dictionary<string, object> Display(long wParam, long lParam)
        {
            return new Dictionary<string, object> { { "bitsPerPixel", wParam },
                { "width", lParam & 0xffff }, { "height", (lParam >> 16) & 0xffff } };
        }

        public static Dictionary<string, object> Device(long code, byte[] payload)
        {
            string name;
            switch (code)
            {
                case 7: name = "DBT_DEVNODES_CHANGED"; break;
                case 0x17: name = "DBT_QUERYCHANGECONFIG"; break;
                case 0x18: name = "DBT_CONFIGCHANGED"; break;
                case 0x19: name = "DBT_CONFIGCHANGECANCELED"; break;
                case 0x8000: name = "DBT_DEVICEARRIVAL"; break;
                case 0x8001: name = "DBT_DEVICEQUERYREMOVE"; break;
                case 0x8002: name = "DBT_DEVICEQUERYREMOVEFAILED"; break;
                case 0x8003: name = "DBT_DEVICEREMOVEPENDING"; break;
                case 0x8004: name = "DBT_DEVICEREMOVECOMPLETE"; break;
                case 0x8005: name = "DBT_DEVICETYPESPECIFIC"; break;
                case 0x8006: name = "DBT_CUSTOMEVENT"; break;
                default: name = "UNKNOWN_DEVICE_EVENT"; break;
            }
            var fields = new Dictionary<string, object> { { "code", code }, { "name", name } };
            if (payload == null || payload.Length == 0) return fields;
            if (payload.Length < 12 || BitConverter.ToUInt32(payload, 0) != payload.Length)
                throw new InvalidDataException("Malformed device notification header.");
            uint type = BitConverter.ToUInt32(payload, 4);
            fields.Add("deviceType", type);
            fields.Add("payloadBytes", payload.Length);
            if (type == 5)
            {
                if (payload.Length < 28 || (payload.Length - 28) % 2 != 0)
                    throw new InvalidDataException("Malformed Unicode device interface notification.");
                byte[] guid = new byte[16]; Array.Copy(payload, 12, guid, 0, 16);
                fields.Add("interfaceClass", new Guid(guid).ToString());
                string deviceName = Encoding.Unicode.GetString(payload, 28, payload.Length - 28).TrimEnd('\0');
                using (var sha = SHA256.Create())
                    fields.Add("interfaceNameSha256", BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(deviceName))).Replace("-", "").ToLowerInvariant());
            }
            return fields;
        }

        public static string Json(IDictionary<string, object> fields)
        {
            var text = new StringBuilder("{"); bool first = true;
            foreach (var field in fields)
            {
                if (!first) text.Append(','); first = false;
                text.Append(Quote(field.Key)).Append(':');
                if (field.Value == null) text.Append("null");
                else if (field.Value is string) text.Append(Quote((string)field.Value));
                else if (field.Value is bool) text.Append((bool)field.Value ? "true" : "false");
                else if (field.Value is IFormattable) text.Append(((IFormattable)field.Value).ToString(null, CultureInfo.InvariantCulture));
                else throw new ArgumentException("Unsupported JSON field type.");
            }
            return text.Append('}').ToString();
        }

        private static string Quote(string value)
        {
            var text = new StringBuilder("\"");
            foreach (char c in value)
            {
                if (c == '"' || c == '\\') text.Append('\\').Append(c);
                else if (c < 32) text.Append("\\u").Append(((int)c).ToString("x4"));
                else text.Append(c);
            }
            return text.Append('"').ToString();
        }
    }

    public static class CaptureControl
    {
        public static string Send(string pipeName, string command, string text)
        {
            if (command != "PING" && command != "MARK" && command != "STOP")
                throw new ArgumentException("Only PING, MARK and STOP are accepted.");
            if (text != null && text.Length > 4096) throw new ArgumentException("Marker is too long.");
            using (var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut))
            {
                pipe.Connect(5000);
                var writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };
                var reader = new StreamReader(pipe, Encoding.UTF8);
                writer.WriteLine(command + " " + Convert.ToBase64String(Encoding.UTF8.GetBytes(text ?? "")));
                var response = reader.ReadLineAsync();
                if (!response.Wait(5000)) throw new TimeoutException("Capture control response timed out.");
                string result = response.Result;
                if (result != "OK") throw new IOException("Capture control failed: " + result);
                return result;
            }
        }
    }

    public sealed class CaptureHost
    {
        private readonly object logLock = new object();
        private readonly Stopwatch clock = Stopwatch.StartNew();
        private StreamWriter writer;
        private IntPtr window;
        private long sequence;
        private volatile bool logFailed;
        private long droppedRecords;
        private BlockingCollection<string> recordQueue;
        private Thread logThread;
        private WindowProcedure callback;
        private readonly List<IntPtr> powerNotifications = new List<IntPtr>();
        private IntPtr deviceNotification;

        public static void Run(string directory, string pipeName)
        {
            new CaptureHost().RunCore(Path.GetFullPath(directory), pipeName);
        }

        private void Write(string kind, Dictionary<string, object> fields)
        {
            lock (logLock)
            {
                var record = new Dictionary<string, object> {
                    { "utc", DateTimeOffset.UtcNow.ToString("O", CultureInfo.InvariantCulture) },
                    { "sequence", ++sequence }, { "elapsedMs", clock.Elapsed.TotalMilliseconds }, { "event", kind }
                };
                if (fields != null) foreach (var pair in fields) record.Add(pair.Key, pair.Value);
                if (!recordQueue.TryAdd(EventDecoder.Json(record))) Interlocked.Increment(ref droppedRecords);
            }
        }

        private void RunCore(string directory, string pipeName)
        {
            Directory.CreateDirectory(directory);
            string className = "WindowsStandbyPassive_" + Guid.NewGuid().ToString("N");
            IntPtr instance = GetModuleHandle(null);
            ushort atom = 0;
            using (var file = new FileStream(Path.Combine(directory, "events.jsonl"), FileMode.CreateNew, FileAccess.Write, FileShare.Read))
            using (writer = new StreamWriter(file, new UTF8Encoding(false)))
            {
                recordQueue = new BlockingCollection<string>(4096);
                logThread = new Thread(() => {
                    foreach (string record in recordQueue.GetConsumingEnumerable())
                    {
                        try { writer.WriteLine(record); writer.Flush(); }
                        catch { logFailed = true; }
                    }
                }) { IsBackground = true, Name = "Standby capture disk writer" };
                logThread.Start();
                try
                {
                    Write("capture_start", new Dictionary<string, object> {
                        { "pid", Process.GetCurrentProcess().Id }, { "osVersion", Environment.OSVersion.VersionString },
                        { "pointerBits", IntPtr.Size * 8 }, { "mode", "passive Windows notifications" },
                        { "sessionId", Process.GetCurrentProcess().SessionId },
                        { "activeConsoleSessionId", WTSGetActiveConsoleSessionId() },
                        { "remoteSession", GetSystemMetrics(0x1000) != 0 } });
                    callback = WindowProc;
                    var windowClass = new WindowClass { Procedure = callback, Instance = instance, ClassName = className };
                    atom = RegisterClass(ref windowClass);
                    if (atom == 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    // Parent NULL and no WS_VISIBLE: a hidden top-level window, not HWND_MESSAGE.
                    window = CreateWindowEx(0, className, "Passive standby notifications", 0, 0, 0, 0, 0,
                        IntPtr.Zero, IntPtr.Zero, instance, IntPtr.Zero);
                    if (window == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    foreach (Guid setting in new[] { EventDecoder.SessionDisplay, EventDecoder.ConsoleDisplay, EventDecoder.LegacyMonitor })
                    {
                        Guid value = setting;
                        IntPtr handle = RegisterPowerSettingNotification(window, ref value, 0);
                        if (handle == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                        powerNotifications.Add(handle);
                    }
                    var filter = new DeviceInterfaceFilter { Size = Marshal.SizeOf(typeof(DeviceInterfaceFilter)),
                        Type = 5, ClassGuid = EventDecoder.MonitorInterface };
                    deviceNotification = RegisterDeviceNotification(window, ref filter, 0);
                    if (deviceNotification == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    var control = new Thread(() => ControlLoop(pipeName)) { IsBackground = true, Name = "Standby capture control" };
                    control.Start();
                    Write("capture_ready", null);
                    File.WriteAllText(Path.Combine(directory, "ready.json"), EventDecoder.Json(new Dictionary<string, object> {
                        { "pid", Process.GetCurrentProcess().Id }, { "utc", DateTimeOffset.UtcNow.ToString("O") }, { "pipeName", pipeName } }), new UTF8Encoding(false));
                    Message message;
                    int status;
                    while ((status = GetMessage(out message, IntPtr.Zero, 0, 0)) > 0)
                    { TranslateMessage(ref message); DispatchMessage(ref message); }
                    if (status < 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                    Write("capture_stopped", new Dictionary<string, object> { { "logErrorObserved", logFailed }, { "droppedRecords", Interlocked.Read(ref droppedRecords) } });
                }
                catch (Exception error)
                {
                    try { Write("capture_error", new Dictionary<string, object> { { "error", error.ToString() } }); } catch { }
                    throw;
                }
                finally
                {
                    foreach (IntPtr registration in powerNotifications) UnregisterPowerSettingNotification(registration);
                    if (deviceNotification != IntPtr.Zero) UnregisterDeviceNotification(deviceNotification);
                    if (window != IntPtr.Zero) { DestroyWindow(window); window = IntPtr.Zero; }
                    if (atom != 0) UnregisterClass(className, instance);
                    recordQueue.CompleteAdding();
                    if (!logThread.Join(5000)) logFailed = true;
                    try { File.WriteAllText(Path.Combine(directory, "stopped.json"), EventDecoder.Json(new Dictionary<string, object> {
                        { "utc", DateTimeOffset.UtcNow.ToString("O") }, { "logErrorObserved", logFailed },
                        { "droppedRecords", Interlocked.Read(ref droppedRecords) } }), new UTF8Encoding(false)); } catch { }
                }
            }
        }

        private void ControlLoop(string pipeName)
        {
            while (true)
            {
                try
                {
                    using (var pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1))
                    {
                        pipe.WaitForConnection();
                        var reader = new StreamReader(pipe, Encoding.UTF8);
                        var response = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true };
                        string line = reader.ReadLine();
                        if (line == null || line.Length > 12000) { response.WriteLine("ERROR invalid control message"); continue; }
                        int separator = line.IndexOf(' ');
                        string command = separator < 0 ? line : line.Substring(0, separator);
                        string text = separator < 0 ? "" : Encoding.UTF8.GetString(Convert.FromBase64String(line.Substring(separator + 1)));
                        if (command == "MARK") Write("marker", new Dictionary<string, object> { { "text", text } });
                        else if (command == "STOP")
                        {
                            Write("stop_requested", null);
                            if (!PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero))
                                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                        }
                        else if (command != "PING") { response.WriteLine("ERROR unknown command"); continue; }
                        response.WriteLine("OK");
                        if (command == "STOP") return;
                    }
                }
                catch (Exception error)
                {
                    try { Write("control_error", new Dictionary<string, object> { { "error", error.Message } }); } catch { }
                }
            }
        }

        private IntPtr WindowProc(IntPtr handle, uint message, IntPtr wParam, IntPtr lParam)
        {
            try
            {
                if (message == 0x0218) // WM_POWERBROADCAST
                {
                    long code = wParam.ToInt64();
                    if (code == 0x8013 && lParam != IntPtr.Zero)
                    {
                        byte[] guidBytes = new byte[16]; Marshal.Copy(lParam, guidBytes, 0, 16);
                        int length = Marshal.ReadInt32(lParam, 16);
                        if (length < 0 || length > 4096) throw new InvalidDataException("Power payload is out of bounds.");
                        byte[] data = new byte[length]; Marshal.Copy(IntPtr.Add(lParam, 20), data, 0, length);
                        Write("power_setting", EventDecoder.Power(new Guid(guidBytes), data));
                    }
                    else Write("power_broadcast", new Dictionary<string, object> { { "code", code } });
                    return new IntPtr(1);
                }
                if (message == 0x007e)
                { Write("display_change", EventDecoder.Display(wParam.ToInt64(), lParam.ToInt64())); return IntPtr.Zero; }
                if (message == 0x0219)
                {
                    long code = wParam.ToInt64();
                    byte[] data = null;
                    // Only documented DBT device-event codes carry DEV_BROADCAST_HDR.
                    if (code >= 0x8000 && code <= 0x8006 && lParam != IntPtr.Zero)
                    {
                        int length = Marshal.ReadInt32(lParam);
                        if (length < 12 || length > 8192) throw new InvalidDataException("Device payload is out of bounds.");
                        data = new byte[length]; Marshal.Copy(lParam, data, 0, length);
                    }
                    Write("device_change", EventDecoder.Device(code, data));
                    return new IntPtr(1); // Never veto a device removal/configuration query.
                }
                if (message == 0x0010) { DestroyWindow(handle); window = IntPtr.Zero; return IntPtr.Zero; }
                if (message == 0x0002) { PostQuitMessage(0); return IntPtr.Zero; }
            }
            catch (Exception error)
            {
                try { Write("notification_error", new Dictionary<string, object> { { "message", message }, { "error", error.Message } }); } catch { }
                if (message == 0x0218 || message == 0x0219) return new IntPtr(1);
            }
            return DefWindowProc(handle, message, wParam, lParam);
        }

        private delegate IntPtr WindowProcedure(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WindowClass
        {
            public uint Style; public WindowProcedure Procedure; public int ClassExtra; public int WindowExtra;
            public IntPtr Instance; public IntPtr Icon; public IntPtr Cursor; public IntPtr Background;
            public string MenuName; public string ClassName;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct Message
        {
            public IntPtr Window; public uint Id; public UIntPtr WParam; public IntPtr LParam;
            public uint Time; public int X; public int Y; public uint Private;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct DeviceInterfaceFilter
        { public int Size; public int Type; public int Reserved; public Guid ClassGuid; public short Name; }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
        [DllImport("kernel32.dll")] private static extern uint WTSGetActiveConsoleSessionId();
        [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern ushort RegisterClass(ref WindowClass windowClass);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool UnregisterClass(string name, IntPtr instance);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateWindowEx(uint exStyle, string className, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr DefWindowProc(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);
        [DllImport("user32.dll")] private static extern void PostQuitMessage(int exitCode);
        [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)] private static extern int GetMessage(out Message message, IntPtr window, uint min, uint max);
        [DllImport("user32.dll")] private static extern bool TranslateMessage(ref Message message);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr DispatchMessage(ref Message message);
        [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr RegisterPowerSettingNotification(IntPtr recipient, ref Guid setting, uint flags);
        [DllImport("user32.dll")] private static extern bool UnregisterPowerSettingNotification(IntPtr registration);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr RegisterDeviceNotification(IntPtr recipient, ref DeviceInterfaceFilter filter, uint flags);
        [DllImport("user32.dll")] private static extern bool UnregisterDeviceNotification(IntPtr registration);
    }
}
