using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// NVAPI ABI from NVIDIA/nvapi, pack(8). Transport commands are restricted
// to EDID address selection and MStar read/identification/ISP lifecycle.
public sealed partial class NvidiaMstarRead
{
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct I2cInfo
    {
        public uint Version, Mask;
        public byte Ddc, Address;
        public IntPtr Register;
        public uint RegisterLength;
        public IntPtr Data;
        public uint Length, Speed, SpeedKhz;
        public byte Port;
        public uint PortSet;
    }

    [DllImport("nvapi64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr nvapi_QueryInterface(uint id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Init();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Gpus([Out] IntPtr[] handles, out uint count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Displays(IntPtr gpu, IntPtr ids, ref uint count, uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Output(uint display, out IntPtr gpu, out uint mask);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Edid(IntPtr gpu, uint mask, IntPtr data);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int I2c(IntPtr gpu, ref I2cInfo info);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Error(int code, StringBuilder text);

    private readonly IntPtr gpu;
    private readonly uint mask;
    private readonly uint speedKhz;
    public uint DisplayId { get; private set; }
    public string EdidSerial { get; private set; }
    private readonly I2c read = Api<I2c>(0x2fde12c5);
    private readonly I2c write = Api<I2c>(0xe812eb07);
    private static T Api<T>(uint id) where T : Delegate
    {
        IntPtr pointer = nvapi_QueryInterface(id);
        if (pointer == IntPtr.Zero) throw new Exception("NVAPI function unavailable: " + id.ToString("X8"));
        return Marshal.GetDelegateForFunctionPointer<T>(pointer);
    }
    private static void Check(int status, string operation)
    {
        if (status == 0) return;
        var text = new StringBuilder(64);
        Api<Error>(0x6c2d048c)(status, text);
        throw new Exception(operation + ": " + status + " " + text);
    }
    private static byte[] DriverEdid(IntPtr gpu, uint mask)
    {
        IntPtr buffer = Marshal.AllocHGlobal(272);
        try
        {
            Marshal.Copy(new byte[272], 0, buffer, 272);
            Marshal.WriteInt32(buffer, 0x30000 | 272);
            Check(Api<Edid>(0x37d32e69)(gpu, mask, buffer), "GetEDID");
            var bytes = new byte[256];
            Marshal.Copy(IntPtr.Add(buffer, 4), bytes, 0, bytes.Length);
            return bytes;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    public NvidiaMstarRead(string expectedEdidSerial, uint speedKhz = 0)
    {
        if (speedKhz != 0 && speedKhz != 4) throw new ArgumentException("Use default or 100 kHz NVAPI speed");
        this.speedKhz = speedKhz;
        if (IntPtr.Size != 8 || Marshal.SizeOf<I2cInfo>() != 64) throw new Exception("Unexpected ABI");
        Check(Api<Init>(0x0150e828)(), "Initialize");
        var handles = new IntPtr[64];
        uint gpuCount;
        Check(Api<Gpus>(0xe5ac921f)(handles, out gpuCount), "EnumPhysicalGPUs");
        int matches = 0;
        for (int index = 0; index < gpuCount; index++)
        {
            uint count = 0;
            Check(Api<Displays>(0x0078dba2)(handles[index], IntPtr.Zero, ref count, 0), "Display count");
            if (count == 0) continue;
            IntPtr ids = Marshal.AllocHGlobal(checked((int)count * 16));
            try
            {
                Marshal.Copy(new byte[count * 16], 0, ids, (int)count * 16);
                for (int n = 0; n < count; n++) Marshal.WriteInt32(ids, n * 16, 0x30010);
                Check(Api<Displays>(0x0078dba2)(handles[index], ids, ref count, 0), "Display IDs");
                for (int n = 0; n < count; n++)
                {
                    uint display = (uint)Marshal.ReadInt32(ids, n * 16 + 8);
                    uint outputMask;
                    IntPtr outputGpu;
                    Check(Api<Output>(0x112ba1a5)(display, out outputGpu, out outputMask), "Output ID");
                    byte[] edid = DriverEdid(outputGpu, outputMask);
                    Console.WriteLine("Display 0x{0:X}, output 0x{1:X}, EDID {2}", display, outputMask, BitConverter.ToString(edid, 0, 16));
                    string serial = "";
                    for (int descriptor = 54; descriptor <= 108; descriptor += 18)
                    {
                        if (edid[descriptor] == 0 && edid[descriptor + 1] == 0 && edid[descriptor + 3] == 0xff)
                            serial = Encoding.ASCII.GetString(edid, descriptor + 5, 13).Trim('\0', '\n', '\r', ' ');
                    }
                    Console.WriteLine("EDID serial: " + serial);
                    if (edid[8] == 0x4c && edid[9] == 0x2d && serial == expectedEdidSerial && serial.Length > 0)
                    {
                        matches++;
                        gpu = outputGpu;
                        mask = outputMask;
                        DisplayId = display;
                        EdidSerial = serial;
                    }
                }
            }
            finally { Marshal.FreeHGlobal(ids); }
        }
        if (matches != 1) throw new Exception("Expected exactly one Samsung with serial " + expectedEdidSerial + ", found " + matches);
    }
    private byte[] Transfer(bool isRead, byte address, byte[] prefix, byte[] payload, int length)
    {
        IntPtr register = IntPtr.Zero;
        IntPtr data = Marshal.AllocHGlobal(Math.Max(length, 1));
        try
        {
            byte[] bytes = Enumerable.Repeat((byte)0xcc, length).ToArray();
            if (!isRead) bytes = payload;
            Marshal.Copy(bytes, 0, data, length);
            if (prefix != null && prefix.Length > 0)
            {
                register = Marshal.AllocHGlobal(prefix.Length);
                Marshal.Copy(prefix, 0, register, prefix.Length);
            }
            var info = new I2cInfo {
                // NVAPI uses LSB=1 for a direct read, which ignores a supplied
                // register prefix. Combined write/repeated-START/read needs LSB=0.
                Version = 0x30040, Mask = mask, Ddc = 1,
                Address = (byte)((address << 1) | (isRead && (prefix == null || prefix.Length == 0) ? 1 : 0)),
                Register = register, RegisterLength = (uint)(prefix == null ? 0 : prefix.Length),
                Data = data, Length = (uint)length, Speed = 0xffff, SpeedKhz = speedKhz
            };
            int status = (isRead ? read : write)(gpu, ref info);
            Check(status, (isRead ? "I2C read " : "I2C write ") + address.ToString("X2"));
            if (isRead) Marshal.Copy(data, bytes, 0, length);
            return bytes;
        }
        finally
        {
            Marshal.FreeHGlobal(data);
            if (register != IntPtr.Zero) Marshal.FreeHGlobal(register);
        }
    }
    public void VerifyTransport()
    {
        byte[] expected = DriverEdid(gpu, mask);
        foreach (byte offset in new byte[] { 0, 8, 0 })
        {
            byte[] actual = Transfer(true, 0x50, new byte[] { offset }, null, 16);
            bool matches = actual.SequenceEqual(expected.Skip(offset).Take(16));
            Console.WriteLine("Combined EDID offset {0}: {1}, match={2}", offset, BitConverter.ToString(actual), matches);
            if (!matches) throw new Exception("Combined I2C transport validation failed");
        }
        foreach (byte offset in new byte[] { 0, 8, 0 })
        {
            Transfer(false, 0x50, null, new byte[] { offset }, 1);
            byte[] actual = Transfer(true, 0x50, null, null, 16);
            Console.WriteLine("Separate EDID offset {0}: {1}, match={2}", offset, BitConverter.ToString(actual), actual.SequenceEqual(expected.Skip(offset).Take(16)));
        }
    }
    public void DiagnoseResponses()
    {
        foreach (byte offset in new byte[] { 0, 8, 10 })
        {
            Transfer(false, 0x50, null, new byte[] { offset }, 1);
            byte[] edid = Transfer(true, 0x50, null, null, 1);
            byte[] isp = Transfer(true, 0x49, null, null, 3);
            Console.WriteLine("EDID offset {0} -> {1}; ISP -> {2}", offset, BitConverter.ToString(edid), BitConverter.ToString(isp));
        }
        CheckDdc();
    }
    public void CheckDdc()
    {
        Console.WriteLine("Raw DDC C8 response: " + BitConverter.ToString(GetVcpResponse(0xc8)));
    }
    private byte[] GetVcpResponse(byte code)
    {
        byte[] request = { 0x51, 0x82, 0x01, code, 0 };
        byte checksum = 0x6e;
        for (int i = 0; i < request.Length - 1; i++) checksum ^= request[i];
        request[request.Length - 1] = checksum;
        Transfer(false, 0x37, null, request, request.Length);
        Thread.Sleep(100);
        byte[] response = Transfer(true, 0x37, null, null, 11);
        byte responseChecksum = 0x50;
        foreach (byte b in response) responseChecksum ^= b;
        if (responseChecksum != 0 || response[2] != 2 || response[3] != 0 || response[4] != code)
            throw new Exception("DDC reply validation failed");
        return response;
    }
    public System.Collections.Generic.Dictionary<string, string> CaptureSettings()
    {
        var settings = new System.Collections.Generic.Dictionary<string, string>();
        settings["edidSerial"] = EdidSerial;
        foreach (byte code in new byte[] { 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a, 0x60, 0xc8, 0xc9, 0xd6 })
        {
            string key = "VCP_" + code.ToString("X2");
            try
            {
                byte[] response = GetVcpResponse(code);
                settings[key] = ((response[8] << 8) | response[9]).ToString();
            }
            catch (Exception error) { settings[key] = "unavailable: " + error.Message; }
        }
        return settings;
    }
    public byte[] ReadLiveEdid()
    {
        var bytes = new byte[256];
        for (int offset = 0; offset < bytes.Length; offset += 16)
        {
            Transfer(false, 0x50, null, new byte[] { (byte)offset }, 1);
            Array.Copy(Transfer(true, 0x50, null, null, 16), 0, bytes, offset, 16);
        }
        byte[] header = { 0, 255, 255, 255, 255, 255, 255, 0 };
        if (!bytes.Take(8).SequenceEqual(header) || bytes[126] != 1)
            throw new Exception("Unexpected live EDID structure");
        for (int start = 0; start < bytes.Length; start += 128)
            if ((bytes.Skip(start).Take(128).Sum(b => (int)b) & 255) != 0)
                throw new Exception("Live EDID checksum mismatch");
        if (!Encoding.ASCII.GetString(bytes).Contains(EdidSerial))
            throw new Exception("Live EDID serial differs from the selected monitor");
        return bytes;
    }
    private void BridgeWrite(byte[] packet)
    {
        Transfer(false, 0x49, null, packet, packet.Length);
    }
    private byte[] SpiRead(byte[] command, int length)
    {
        if (command.Length == 0 || (command[0] != 0x9f && command[0] != 0x05 && command[0] != 0x35 && command[0] != 0x15 && command[0] != 0x03 && command[0] != 0x5a))
            throw new Exception("Only SPI read opcodes are allowed");
        var packet = new byte[command.Length + 1];
        packet[0] = 0x10;
        Array.Copy(command, 0, packet, 1, command.Length);
        try
        {
            BridgeWrite(packet);
            // The validated SPI bridge sequence uses separate raw command/read transfers.
            BridgeWrite(new byte[] { 0x11 });
            return Transfer(true, 0x49, null, null, length);
        }
        finally { BridgeWrite(new byte[] { 0x12 }); }
    }
    public void Identify()
    {
        bool entered = false;
        try
        {
            BridgeWrite(Encoding.ASCII.GetBytes("MSTAR"));
            entered = true;
            Console.WriteLine("ISP entry transaction completed");
            Thread.Sleep(50);
            byte[] first = SpiRead(new byte[] { 0x9f }, 3);
            byte[] second = SpiRead(new byte[] { 0x9f }, 3);
            Console.WriteLine("JEDEC 1: " + BitConverter.ToString(first));
            Console.WriteLine("JEDEC 2: " + BitConverter.ToString(second));
            if (!first.SequenceEqual(second) || first.Distinct().Count() == 1 || first[2] < 0x10 || first[2] > 0x1a)
                throw new Exception("No stable plausible JEDEC ID, stopping identification");
            Console.WriteLine("Status: " + BitConverter.ToString(SpiRead(new byte[] { 0x05 }, 1)));
            foreach (int address in new int[] { 0, 0x100, 0 })
            {
                byte[] command = { 0x03, (byte)(address >> 16), (byte)(address >> 8), (byte)address };
                Console.WriteLine("Flash 0x{0:X6}: {1}", address, BitConverter.ToString(SpiRead(command, 16)));
            }
        }
        finally
        {
            if (entered)
            {
                BridgeWrite(new byte[] { 0x24 });
                Console.WriteLine("ISP exit/reset transaction completed");
            }
        }
    }
    public void InspectFlashStatus()
    {
        BridgeWrite(Encoding.ASCII.GetBytes("MSTAR"));
        try
        {
            Thread.Sleep(100);
            Console.WriteLine("JEDEC: " + BitConverter.ToString(SpiRead(new byte[] { 0x9f }, 3)));
            foreach (byte opcode in new byte[] { 0x05, 0x35, 0x15 })
                Console.WriteLine("Status opcode {0:X2}: {1}", opcode, BitConverter.ToString(SpiRead(new byte[] { opcode }, 1)));
            Console.WriteLine("SFDP: " + BitConverter.ToString(SpiRead(new byte[] { 0x5a, 0, 0, 0, 0 }, 64)));
        }
        finally { BridgeWrite(new byte[] { 0x24 }); }
    }
    public void DumpTwice(string directory)
    {
        const int capacity = 2 * 1024 * 1024;
        const int blockSize = 256;
        Directory.CreateDirectory(directory);
        bool entered = false;
        try
        {
            BridgeWrite(Encoding.ASCII.GetBytes("MSTAR"));
            entered = true;
            Thread.Sleep(100);
            byte[] expectedId = { 0xef, 0x40, 0x15 };
            if (!SpiRead(new byte[] { 0x9f }, 3).SequenceEqual(expectedId) ||
                !SpiRead(new byte[] { 0x9f }, 3).SequenceEqual(expectedId))
                throw new Exception("This dump routine requires verified JEDEC EF-40-15 (2 MiB)");
            byte[] page = ReadFlash(0, blockSize);
            byte[] smallReads = new byte[blockSize];
            for (int offset = 0; offset < blockSize; offset += 16)
                Array.Copy(ReadFlash(offset, 16), 0, smallReads, offset, 16);
            if (!page.SequenceEqual(smallReads) || page.Distinct().Count() < 8)
                throw new Exception("256-byte read validation failed");
            Console.WriteLine("Validated 256-byte transport against sixteen independent reads");

            var paths = new string[2];
            for (int pass = 0; pass < paths.Length; pass++)
            {
                paths[pass] = Path.Combine(directory, "spi-read" + (pass + 1) + ".bin");
                string partial = paths[pass] + ".partial";
                var timer = System.Diagnostics.Stopwatch.StartNew();
                using (var output = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.Read))
                {
                    for (int address = 0; address < capacity; address += blockSize)
                    {
                        if (timer.Elapsed.TotalMinutes > 30) throw new Exception("Read timeout");
                        byte[] data = ReadFlash(address, blockSize);
                        output.Write(data, 0, data.Length);
                        if ((address + blockSize) % 65536 == 0)
                        {
                            output.Flush();
                            Console.WriteLine("Pass {0}: {1}/{2} bytes ({3:F1}%), {4:F1}s",
                                pass + 1, address + blockSize, capacity,
                                100.0 * (address + blockSize) / capacity, timer.Elapsed.TotalSeconds);
                        }
                    }
                    output.Flush(true);
                }
                if (!SpiRead(new byte[] { 0x9f }, 3).SequenceEqual(expectedId))
                    throw new Exception("JEDEC ID changed after dump");
                File.Move(partial, paths[pass]);
            }
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                byte[] hash1, hash2;
                using (var file = File.OpenRead(paths[0])) hash1 = sha.ComputeHash(file);
                using (var file = File.OpenRead(paths[1])) hash2 = sha.ComputeHash(file);
                Console.WriteLine("SHA256 read1: " + BitConverter.ToString(hash1).Replace("-", "").ToLowerInvariant());
                Console.WriteLine("SHA256 read2: " + BitConverter.ToString(hash2).Replace("-", "").ToLowerInvariant());
                if (!hash1.SequenceEqual(hash2)) throw new Exception("Dump hashes differ");
                Console.WriteLine("VERIFIED: two complete 2097152-byte reads have identical SHA256");
            }
        }
        finally
        {
            if (entered)
            {
                BridgeWrite(new byte[] { 0x24 });
                Console.WriteLine("ISP exit/reset transaction completed");
            }
        }
    }
    private byte[] ReadFlash(int address, int length)
    {
        return SpiRead(new byte[] { 0x03, (byte)(address >> 16), (byte)(address >> 8), (byte)address }, length);
    }
}
