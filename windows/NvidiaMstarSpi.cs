using System;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;

// Audited SPI transaction primitives used by the disabled experimental adapter.
// No per-unit profiles or official-upgrade entry point are included.
public sealed partial class NvidiaMstarRead
{
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SetThreadExecutionState(uint flags);

    private void SpiWriteCommand(byte[] command)
    {
        byte opcode = command[0];
        bool valid = ((opcode == 0x06 || opcode == 0x04) && command.Length == 1)
            || (opcode == 0x20 && command.Length == 4)
            || (opcode == 0x02 && command.Length >= 5 && command.Length <= 260);
        if (!valid) throw new Exception("Unsupported SPI update command");
        var packet = new byte[command.Length + 1];
        packet[0] = 0x10;
        Array.Copy(command, 0, packet, 1, command.Length);
        try { BridgeWrite(packet); }
        finally { BridgeWrite(new byte[] { 0x12 }); }
    }
    private byte WaitReady(int timeoutMilliseconds)
    {
        var timer = Stopwatch.StartNew();
        while (true)
        {
            byte status = SpiRead(new byte[] { 0x05 }, 1)[0];
            if ((status & 1) == 0) return status;
            if (timer.ElapsedMilliseconds >= timeoutMilliseconds) throw new Exception("Flash remained busy");
            Thread.Sleep(5);
        }
    }
    private void EnableWrite()
    {
        byte status = WaitReady(5000);
        if ((status & 0x1c) != 0) throw new Exception("Block protection is enabled");
        SpiWriteCommand(new byte[] { 0x06 });
        status = SpiRead(new byte[] { 0x05 }, 1)[0];
        if ((status & 3) != 2) throw new Exception("Write-enable latch did not set");
    }
    private byte[] ReadRange(int start, int length, string label, Action<string> log)
    {
        var result = new byte[length];
        for (int offset = 0; offset < length; offset += 256)
        {
            int count = Math.Min(256, length - offset);
            Array.Copy(ReadFlash(start + offset, count), 0, result, offset, count);
            if ((offset + count) % 65536 == 0 || offset + count == length)
                log(label + " " + (offset + count) + "/" + length + " bytes");
        }
        return result;
    }
    private void WriteSector(int address, byte[] image, int pageSize, Action<string> log)
    {
        for (int attempt = 1; attempt <= 2; attempt++)
        {
            try
            {
                EnableWrite();
                SpiWriteCommand(new byte[] { 0x20, (byte)(address >> 16), (byte)(address >> 8), (byte)address });
                byte status = WaitReady(5000);
                if ((status & 2) != 0) throw new Exception("Erase command was not accepted");
                if (ReadFlash(address, 16).Any(value => value != 0xff))
                    throw new Exception("Sector erase check failed");
                for (int offset = 0; offset < 4096; offset += pageSize)
                {
                    int target = address + offset;
                    byte[] data = image.Skip(target).Take(pageSize).ToArray();
                    if (data.All(value => value == 0xff)) continue;
                    var command = new byte[4 + pageSize];
                    command[0] = 0x02;
                    command[1] = (byte)(target >> 16);
                    command[2] = (byte)(target >> 8);
                    command[3] = (byte)target;
                    Array.Copy(data, 0, command, 4, pageSize);
                    EnableWrite();
                    SpiWriteCommand(command);
                    if ((WaitReady(5000) & 2) != 0) throw new Exception("Page program command was not accepted");
                }
                for (int offset = 0; offset < 4096; offset += 256)
                {
                    byte[] actual = ReadFlash(address + offset, 256);
                    if (!actual.SequenceEqual(image.Skip(address + offset).Take(256)))
                        throw new Exception("Sector read-back differs at 0x" + (address + offset).ToString("X6"));
                }
                return;
            }
            catch (Exception error)
            {
                log("Sector 0x" + address.ToString("X6") + ", attempt " + attempt + ": " + error.Message);
                if (attempt == 2) throw;
                WaitReady(5000);
            }
        }
    }
}
